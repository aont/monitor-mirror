#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

/*
    Build with `make` from the repository root. The output binary is
    bin/monitor_mirror.exe.

    Usage:
      ./bin/monitor_mirror.exe --filter bilinear 0
      ./bin/monitor_mirror.exe -f lanczos 0

    Filters:
      nearest, bilinear, bicubic, lanczos
*/

#define SAFE_RELEASE(p)                                                     \
    do {                                                                    \
        if ((p) != NULL) {                                                  \
            IUnknown_Release((IUnknown *)(p));                              \
            (p) = NULL;                                                     \
        }                                                                   \
    } while (0)

typedef enum ResizeFilter {
    FILTER_NEAREST = 0,
    FILTER_BILINEAR = 1,
    FILTER_BICUBIC = 2,
    FILTER_LANCZOS = 3
} ResizeFilter;

typedef struct FilterConstants {
    float tex_size[2];
    float inv_tex_size[2];
    int filter_mode;
    float lanczos_a;
    float pad[2];
} FilterConstants;

typedef struct Vertex2D {
    float pos[2];
    float uv[2];
} Vertex2D;

static HWND g_hwnd = NULL;
static int g_running = 1;
static int g_cursor_was_inside_source = 0;
static HCURSOR g_mirror_window_cursor = NULL;

static IDXGIAdapter1 *g_adapter = NULL;
static IDXGIOutput1 *g_output1 = NULL;
static IDXGIOutputDuplication *g_dup = NULL;

static ID3D11Device *g_device = NULL;
static ID3D11DeviceContext *g_context = NULL;
static IDXGISwapChain *g_swapchain = NULL;
static ID3D11Texture2D *g_staging_tex = NULL;
static ID3D11Texture2D *g_frame_gpu_tex = NULL;
static ID3D11ShaderResourceView *g_frame_srv = NULL;
static ID3D11RenderTargetView *g_backbuffer_rtv = NULL;
static ID3D11VertexShader *g_vertex_shader = NULL;
static ID3D11PixelShader *g_pixel_shader = NULL;
static ID3D11InputLayout *g_input_layout = NULL;
static ID3D11Buffer *g_vertex_buffer = NULL;
static ID3D11Buffer *g_filter_cbuffer = NULL;
static ID3D11SamplerState *g_linear_sampler = NULL;
static ID3D11SamplerState *g_point_sampler = NULL;

static ResizeFilter g_resize_filter = FILTER_BILINEAR;
static double g_fps_log_interval_sec = 0.0;
static int g_fps_log_enabled = 0;
static LARGE_INTEGER g_fps_qpc_freq;
static LARGE_INTEGER g_fps_period_start;
static uint64_t g_fps_period_present_count = 0;
static uint64_t g_fps_total_present_count = 0;
static int g_fps_counter_initialized = 0;

static UINT g_capture_width = 0;
static UINT g_capture_height = 0;
static DXGI_OUTPUT_DESC g_output_desc;

static UINT g_backbuffer_width = 0;
static UINT g_backbuffer_height = 0;
static UINT g_client_width = 0;
static UINT g_client_height = 0;
static int g_need_resize_present = 0;

static uint8_t *g_frame_pixels = NULL;
static UINT g_frame_pitch = 0;
static UINT g_frame_width = 0;
static UINT g_frame_height = 0;

static uint8_t *g_pointer_shape = NULL;
static UINT g_pointer_shape_capacity = 0;
static DXGI_OUTDUPL_POINTER_SHAPE_INFO g_pointer_shape_info;
static int g_have_pointer_shape = 0;
static int g_pointer_visible = 0;
static POINT g_pointer_pos_output;

static void die_hr(const char *msg, HRESULT hr)
{
    fprintf(stderr, "%s failed: HRESULT=0x%08lx\n", msg, (unsigned long)hr);
    ExitProcess(1);
}

static HWND normalize_top_level(HWND hwnd)
{
    HWND root;

    if (!hwnd) return NULL;

    root = GetAncestor(hwnd, GA_ROOT);
    if (root && IsWindow(root)) return root;

    return hwnd;
}

static bool same_top_level(HWND a, HWND b)
{
    return normalize_top_level(a) == normalize_top_level(b);
}

static bool is_foreground(HWND hwnd)
{
    HWND fg = GetForegroundWindow();
    return fg && same_top_level(fg, hwnd);
}

static void tap_alt_key(void)
{
    INPUT inputs[2];

    ZeroMemory(inputs, sizeof(inputs));

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_MENU;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_MENU;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

static bool activate_window(HWND hwnd)
{
    DWORD current_tid;
    DWORD foreground_tid = 0;
    DWORD target_tid = 0;
    DWORD dummy_pid = 0;
    HWND foreground_hwnd;
    BOOL attached_foreground = FALSE;
    BOOL attached_target = FALSE;
    MSG msg;

    hwnd = normalize_top_level(hwnd);

    if (!hwnd) return false;
    if (!IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;

    if (is_foreground(hwnd)) return true;

    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);

    current_tid = GetCurrentThreadId();

    foreground_hwnd = GetForegroundWindow();
    if (foreground_hwnd && IsWindow(foreground_hwnd)) {
        foreground_tid = GetWindowThreadProcessId(foreground_hwnd, &dummy_pid);
    }

    target_tid = GetWindowThreadProcessId(hwnd, &dummy_pid);

    if (foreground_tid && foreground_tid != current_tid) {
        attached_foreground = AttachThreadInput(current_tid, foreground_tid, TRUE);
    }

    if (target_tid && target_tid != current_tid && target_tid != foreground_tid) {
        attached_target = AttachThreadInput(current_tid, target_tid, TRUE);
    }

    if (IsIconic(hwnd)) {
        ShowWindowAsync(hwnd, SW_RESTORE);
    } else {
        ShowWindowAsync(hwnd, SW_SHOW);
    }

    SetWindowPos(
        hwnd,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_SHOWWINDOW |
        SWP_ASYNCWINDOWPOS
    );

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (attached_target) {
        AttachThreadInput(current_tid, target_tid, FALSE);
    }

    if (attached_foreground) {
        AttachThreadInput(current_tid, foreground_tid, FALSE);
    }

    if (is_foreground(hwnd)) {
        return true;
    }

    tap_alt_key();

    if (IsIconic(hwnd)) {
        ShowWindowAsync(hwnd, SW_RESTORE);
    } else {
        ShowWindowAsync(hwnd, SW_SHOW);
    }

    SetWindowPos(
        hwnd,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_SHOWWINDOW |
        SWP_ASYNCWINDOWPOS
    );

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    return is_foreground(hwnd);
}

static int point_in_rect_exclusive(const RECT *r, POINT p)
{
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

static void maybe_activate_when_cursor_enters_source_display(void)
{
    POINT cursor;
    int inside;

    if (!g_hwnd || !IsWindow(g_hwnd)) return;

    if (!GetCursorPos(&cursor)) return;

    inside = point_in_rect_exclusive(&g_output_desc.DesktopCoordinates, cursor);

    if (inside && !g_cursor_was_inside_source) {
        activate_window(g_hwnd);
    }

    g_cursor_was_inside_source = inside;
}

static void initialize_cursor_inside_source_state(void)
{
    POINT cursor;

    if (GetCursorPos(&cursor)) {
        g_cursor_was_inside_source = point_in_rect_exclusive(&g_output_desc.DesktopCoordinates, cursor);
    } else {
        g_cursor_was_inside_source = 0;
    }
}

static const char *filter_name(ResizeFilter filter)
{
    switch (filter) {
    case FILTER_NEAREST: return "nearest";
    case FILTER_BILINEAR: return "bilinear";
    case FILTER_BICUBIC: return "bicubic";
    case FILTER_LANCZOS: return "lanczos";
    default: return "unknown";
    }
}

static int parse_filter_name(const char *name, ResizeFilter *filter_out)
{
    if (!name || !filter_out) return 0;

    if (strcmp(name, "nearest") == 0 || strcmp(name, "point") == 0) {
        *filter_out = FILTER_NEAREST;
        return 1;
    }
    if (strcmp(name, "bilinear") == 0 || strcmp(name, "linear") == 0) {
        *filter_out = FILTER_BILINEAR;
        return 1;
    }
    if (strcmp(name, "bicubic") == 0 || strcmp(name, "cubic") == 0) {
        *filter_out = FILTER_BICUBIC;
        return 1;
    }
    if (strcmp(name, "lanczos") == 0 || strcmp(name, "lanczos3") == 0) {
        *filter_out = FILTER_LANCZOS;
        return 1;
    }

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options] <display-index>\n"
            "       %s [options]\n\n"
            "Options:\n"
            "  -f, --filter <name>        Resize filter: nearest, bilinear, bicubic, lanczos\n"
            "      --fps-log              Log FPS every 1.0 second\n"
            "      --fps-log-interval <s> Log FPS every <s> seconds; 0 disables logging\n"
            "  -h, --help                 Show this help\n\n"
            "If <display-index> is omitted, available DXGI outputs are listed.\n",
            prog, prog);
}

static int parse_positive_double_or_zero(const char *text, double *value_out)
{
    char *end = NULL;
    double value;

    if (!text || !*text || !value_out) return 0;

    value = strtod(text, &end);
    if (end == text || *end != '\0') return 0;
    if (value < 0.0) return 0;

    *value_out = value;
    return 1;
}

static double qpc_elapsed_sec(LARGE_INTEGER start, LARGE_INTEGER end)
{
    return (double)(end.QuadPart - start.QuadPart) / (double)g_fps_qpc_freq.QuadPart;
}

static void fps_log_initialize(void)
{
    if (!g_fps_log_enabled || g_fps_counter_initialized) return;

    QueryPerformanceFrequency(&g_fps_qpc_freq);
    QueryPerformanceCounter(&g_fps_period_start);
    g_fps_period_present_count = 0;
    g_fps_total_present_count = 0;
    g_fps_counter_initialized = 1;
}

static void fps_log_on_present(void)
{
    LARGE_INTEGER now;
    double elapsed;
    double fps;

    if (!g_fps_log_enabled || g_fps_log_interval_sec <= 0.0) return;

    fps_log_initialize();

    g_fps_period_present_count++;
    g_fps_total_present_count++;

    QueryPerformanceCounter(&now);
    elapsed = qpc_elapsed_sec(g_fps_period_start, now);

    if (elapsed >= g_fps_log_interval_sec) {
        fps = (double)g_fps_period_present_count / elapsed;
        fprintf(stderr,
                "fps: %.2f  frames=%llu  interval=%.3f sec  total=%llu  filter=%s\n",
                fps,
                (unsigned long long)g_fps_period_present_count,
                elapsed,
                (unsigned long long)g_fps_total_present_count,
                filter_name(g_resize_filter));

        g_fps_period_start = now;
        g_fps_period_present_count = 0;
    }
}

static void make_process_dpi_aware(void)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");

    if (user32) {
        typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
        PFN_SetProcessDPIAware pSetProcessDPIAware =
            (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");

        if (pSetProcessDPIAware) {
            pSetProcessDPIAware();
        }
    }
}

static void get_window_frame_delta(HWND hwnd, LONG *dx, LONG *dy)
{
    RECT rc;
    DWORD style = (DWORD)GetWindowLongPtrA(hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtrA(hwnd, GWL_EXSTYLE);

    rc.left = 0;
    rc.top = 0;
    rc.right = 100;
    rc.bottom = 100;
    AdjustWindowRectEx(&rc, style, FALSE, ex_style);

    *dx = (rc.right - rc.left) - 100;
    *dy = (rc.bottom - rc.top) - 100;
}

static void constrain_sizing_rect_to_capture_aspect(HWND hwnd, WPARAM edge, RECT *r)
{
    LONG frame_dx = 0;
    LONG frame_dy = 0;
    LONG win_w;
    LONG win_h;
    LONG client_w;
    LONG client_h;
    LONG new_client_w;
    LONG new_client_h;
    double aspect;
    int width_driven = 0;

    if (!g_capture_width || !g_capture_height) return;

    get_window_frame_delta(hwnd, &frame_dx, &frame_dy);

    win_w = r->right - r->left;
    win_h = r->bottom - r->top;
    client_w = win_w - frame_dx;
    client_h = win_h - frame_dy;

    if (client_w < 64) client_w = 64;
    if (client_h < 64) client_h = 64;

    aspect = (double)g_capture_width / (double)g_capture_height;

    switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_RIGHT:
        width_driven = 1;
        break;
    case WMSZ_TOP:
    case WMSZ_BOTTOM:
        width_driven = 0;
        break;
    default:
        width_driven = ((double)client_w / (double)client_h > aspect) ? 1 : 0;
        break;
    }

    if (width_driven) {
        new_client_w = client_w;
        new_client_h = (LONG)((double)new_client_w / aspect + 0.5);
    } else {
        new_client_h = client_h;
        new_client_w = (LONG)((double)new_client_h * aspect + 0.5);
    }

    if (new_client_w < 64) new_client_w = 64;
    if (new_client_h < 64) new_client_h = 64;

    win_w = new_client_w + frame_dx;
    win_h = new_client_h + frame_dy;

    switch (edge) {
    case WMSZ_LEFT:
    case WMSZ_TOPLEFT:
    case WMSZ_BOTTOMLEFT:
        r->left = r->right - win_w;
        break;
    default:
        r->right = r->left + win_w;
        break;
    }

    switch (edge) {
    case WMSZ_TOP:
    case WMSZ_TOPLEFT:
    case WMSZ_TOPRIGHT:
        r->top = r->bottom - win_h;
        break;
    default:
        r->bottom = r->top + win_h;
        break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZING:
        constrain_sizing_rect_to_capture_aspect(hwnd, wp, (RECT *)lp);
        return TRUE;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (!g_mirror_window_cursor) {
                g_mirror_window_cursor = LoadCursor(NULL, IDC_CROSS);
            }
            SetCursor(g_mirror_window_cursor);
            return TRUE;
        }
        break;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_client_width = LOWORD(lp);
            g_client_height = HIWORD(lp);
            if (g_client_width && g_client_height) {
                g_need_resize_present = 1;
            }
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static void print_outputs(void)
{
    HRESULT hr;
    IDXGIFactory1 *factory = NULL;
    UINT flat_index = 0;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateDXGIFactory1 failed: 0x%08lx\n", (unsigned long)hr);
        return;
    }

    for (UINT ai = 0;; ai++) {
        IDXGIAdapter1 *adapter = NULL;

        hr = IDXGIFactory1_EnumAdapters1(factory, ai, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) break;

        for (UINT oi = 0;; oi++) {
            IDXGIOutput *output = NULL;
            DXGI_OUTPUT_DESC odesc;

            hr = IDXGIAdapter1_EnumOutputs(adapter, oi, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) break;

            ZeroMemory(&odesc, sizeof(odesc));
            IDXGIOutput_GetDesc(output, &odesc);

            wprintf(
                L"[%u] Adapter=%u Output=%u DeviceName=%ls "
                L"Desktop=(%ld,%ld)-(%ld,%ld) Attached=%d Rotation=%u\n",
                flat_index,
                ai,
                oi,
                odesc.DeviceName,
                odesc.DesktopCoordinates.left,
                odesc.DesktopCoordinates.top,
                odesc.DesktopCoordinates.right,
                odesc.DesktopCoordinates.bottom,
                odesc.AttachedToDesktop,
                (UINT)odesc.Rotation
            );

            flat_index++;
            SAFE_RELEASE(output);
        }

        SAFE_RELEASE(adapter);
    }

    SAFE_RELEASE(factory);
}

static HRESULT select_output(UINT wanted_flat_index)
{
    HRESULT hr;
    IDXGIFactory1 *factory = NULL;
    UINT flat_index = 0;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr)) return hr;

    for (UINT ai = 0;; ai++) {
        IDXGIAdapter1 *adapter = NULL;

        hr = IDXGIFactory1_EnumAdapters1(factory, ai, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) {
            SAFE_RELEASE(factory);
            return hr;
        }

        for (UINT oi = 0;; oi++) {
            IDXGIOutput *output = NULL;
            IDXGIOutput1 *output1 = NULL;
            DXGI_OUTPUT_DESC desc;

            hr = IDXGIAdapter1_EnumOutputs(adapter, oi, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) {
                SAFE_RELEASE(adapter);
                SAFE_RELEASE(factory);
                return hr;
            }

            ZeroMemory(&desc, sizeof(desc));
            hr = IDXGIOutput_GetDesc(output, &desc);
            if (FAILED(hr)) {
                SAFE_RELEASE(output);
                SAFE_RELEASE(adapter);
                SAFE_RELEASE(factory);
                return hr;
            }

            if (flat_index == wanted_flat_index) {
                hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
                if (FAILED(hr)) {
                    SAFE_RELEASE(output);
                    SAFE_RELEASE(adapter);
                    SAFE_RELEASE(factory);
                    return hr;
                }

                g_adapter = adapter;
                IDXGIAdapter1_AddRef(g_adapter);
                g_output1 = output1;
                g_output_desc = desc;
                g_capture_width = (UINT)(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                g_capture_height = (UINT)(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

                SAFE_RELEASE(output);
                SAFE_RELEASE(adapter);
                SAFE_RELEASE(factory);
                return S_OK;
            }

            flat_index++;
            SAFE_RELEASE(output);
        }

        SAFE_RELEASE(adapter);
    }

    SAFE_RELEASE(factory);
    return DXGI_ERROR_NOT_FOUND;
}

static void create_window_for_capture(HINSTANCE inst)
{
    WNDCLASSA wc;
    RECT rc;
    DWORD style = WS_OVERLAPPEDWINDOW;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "DDA_CAPTURE_WINDOW_AUTOACTIVATE_C";
    g_mirror_window_cursor = LoadCursor(NULL, IDC_CROSS);
    wc.hCursor = g_mirror_window_cursor;

    if (!RegisterClassA(&wc)) {
        fprintf(stderr, "RegisterClassA failed\n");
        ExitProcess(1);
    }

    rc.left = 0;
    rc.top = 0;
    rc.right = (LONG)g_capture_width;
    rc.bottom = (LONG)g_capture_height;
    AdjustWindowRect(&rc, style, FALSE);

    g_hwnd = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Desktop Duplication API - auto activate mirror window",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        NULL,
        NULL,
        inst,
        NULL
    );

    if (!g_hwnd) {
        fprintf(stderr, "CreateWindowExA failed\n");
        ExitProcess(1);
    }

    g_client_width = g_capture_width;
    g_client_height = g_capture_height;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
}

static void create_d3d_and_swapchain(void)
{
    HRESULT hr;
    DXGI_SWAP_CHAIN_DESC sd;

    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = g_client_width;
    sd.BufferDesc.Height = g_client_height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        (IDXGIAdapter *)g_adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &sd,
        &g_swapchain,
        &g_device,
        NULL,
        &g_context
    );

    if (FAILED(hr)) die_hr("D3D11CreateDeviceAndSwapChain", hr);

    g_backbuffer_width = g_client_width;
    g_backbuffer_height = g_client_height;
}

static HRESULT create_duplication(void)
{
    SAFE_RELEASE(g_dup);
    return IDXGIOutput1_DuplicateOutput(g_output1, (IUnknown *)g_device, &g_dup);
}

static HRESULT recreate_duplication(void)
{
    SAFE_RELEASE(g_dup);
    Sleep(100);
    return create_duplication();
}

static HRESULT ensure_staging_and_frame_buffer(UINT width, UINT height)
{
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    size_t required_size;
    uint8_t *new_pixels;

    if (g_staging_tex && g_frame_pixels && g_frame_width == width && g_frame_height == height) {
        return S_OK;
    }

    SAFE_RELEASE(g_staging_tex);

    ZeroMemory(&td, sizeof(td));
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateTexture2D(g_device, &td, NULL, &g_staging_tex);
    if (FAILED(hr)) return hr;

    required_size = (size_t)width * (size_t)height * 4u;
    new_pixels = (uint8_t *)realloc(g_frame_pixels, required_size);
    if (!new_pixels) {
        free(g_frame_pixels);
        g_frame_pixels = NULL;
        SAFE_RELEASE(g_staging_tex);
        return E_OUTOFMEMORY;
    }

    g_frame_pixels = new_pixels;
    g_frame_pitch = width * 4u;
    g_frame_width = width;
    g_frame_height = height;
    return S_OK;
}

static HRESULT copy_desktop_texture_to_cpu(ID3D11Texture2D *desktop_tex)
{
    HRESULT hr;
    D3D11_TEXTURE2D_DESC src_desc;
    D3D11_MAPPED_SUBRESOURCE mapped;

    ZeroMemory(&src_desc, sizeof(src_desc));
    ID3D11Texture2D_GetDesc(desktop_tex, &src_desc);

    if (src_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) return E_FAIL;

    hr = ensure_staging_and_frame_buffer(src_desc.Width, src_desc.Height);
    if (FAILED(hr)) return hr;

    ID3D11DeviceContext_CopyResource(g_context, (ID3D11Resource *)g_staging_tex, (ID3D11Resource *)desktop_tex);

    ZeroMemory(&mapped, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(g_context, (ID3D11Resource *)g_staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;

    for (UINT y = 0; y < src_desc.Height; y++) {
        const uint8_t *src_row = (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch;
        uint8_t *dst_row = g_frame_pixels + (size_t)y * g_frame_pitch;
        memcpy(dst_row, src_row, (size_t)src_desc.Width * 4u);
    }

    ID3D11DeviceContext_Unmap(g_context, (ID3D11Resource *)g_staging_tex, 0);
    return S_OK;
}

static HRESULT ensure_pointer_shape_buffer(UINT required)
{
    uint8_t *new_buffer;

    if (required <= g_pointer_shape_capacity) return S_OK;

    new_buffer = (uint8_t *)realloc(g_pointer_shape, required);
    if (!new_buffer) {
        free(g_pointer_shape);
        g_pointer_shape = NULL;
        g_pointer_shape_capacity = 0;
        g_have_pointer_shape = 0;
        return E_OUTOFMEMORY;
    }

    g_pointer_shape = new_buffer;
    g_pointer_shape_capacity = required;
    return S_OK;
}

static HRESULT update_pointer_state(const DXGI_OUTDUPL_FRAME_INFO *frame_info)
{
    HRESULT hr;
    UINT required = 0;

    /*
        DXGI_OUTDUPL_POINTER_POSITION::Position is relative to the top-left of
        the duplicated adapter output, not to the virtual desktop.  AcquireNextFrame
        gives the current pointer position for the frame, so update this on every
        acquired frame.  Position is valid only when Visible is TRUE.
    */
    g_pointer_visible = frame_info->PointerPosition.Visible ? 1 : 0;
    if (g_pointer_visible) {
        g_pointer_pos_output = frame_info->PointerPosition.Position;
    }

    if (frame_info->PointerShapeBufferSize == 0) return S_OK;

    hr = ensure_pointer_shape_buffer(frame_info->PointerShapeBufferSize);
    if (FAILED(hr)) return hr;

    ZeroMemory(&g_pointer_shape_info, sizeof(g_pointer_shape_info));

    hr = IDXGIOutputDuplication_GetFramePointerShape(
        g_dup,
        g_pointer_shape_capacity,
        g_pointer_shape,
        &required,
        &g_pointer_shape_info
    );

    if (hr == DXGI_ERROR_MORE_DATA) {
        hr = ensure_pointer_shape_buffer(required);
        if (FAILED(hr)) return hr;

        hr = IDXGIOutputDuplication_GetFramePointerShape(
            g_dup,
            g_pointer_shape_capacity,
            g_pointer_shape,
            &required,
            &g_pointer_shape_info
        );
    }

    if (FAILED(hr)) {
        g_have_pointer_shape = 0;
        return hr;
    }

    g_have_pointer_shape = 1;
    return S_OK;
}

static void alpha_blend_bgra(uint8_t *dst, const uint8_t *src)
{
    uint32_t a = src[3];

    if (a == 0) return;
    if (a == 255) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        return;
    }

    {
        uint32_t inv = 255 - a;
        dst[0] = (uint8_t)((src[0] * a + dst[0] * inv + 127) / 255);
        dst[1] = (uint8_t)((src[1] * a + dst[1] * inv + 127) / 255);
        dst[2] = (uint8_t)((src[2] * a + dst[2] * inv + 127) / 255);
        dst[3] = 255;
    }
}

static int get_1bpp_bit(const uint8_t *base, UINT pitch, UINT x, UINT y)
{
    const uint8_t *row = base + (size_t)y * pitch;
    uint8_t b = row[x >> 3];
    return (b >> (7 - (x & 7))) & 1;
}

static void draw_monochrome_pointer(int left, int top)
{
    UINT width = g_pointer_shape_info.Width;
    UINT height = g_pointer_shape_info.Height / 2;
    UINT pitch = g_pointer_shape_info.Pitch;
    const uint8_t *and_mask = g_pointer_shape;
    const uint8_t *xor_mask = NULL;

    /*
        Monochrome pointer shape data is AND mask followed by XOR mask.
        The official desktop duplication sample treats ShapeInfo.Height as the
        total scan-line count for both masks and draws Height / 2 rows.
    */
    if (height == 0) return;

    xor_mask = g_pointer_shape + (size_t)pitch * height;

    for (UINT y = 0; y < height; y++) {
        int dy = top + (int)y;
        if (dy < 0 || dy >= (int)g_frame_height) continue;

        for (UINT x = 0; x < width; x++) {
            int dx = left + (int)x;
            int and_bit;
            int xor_bit;
            uint8_t *dst;

            if (dx < 0 || dx >= (int)g_frame_width) continue;

            and_bit = get_1bpp_bit(and_mask, pitch, x, y);
            xor_bit = get_1bpp_bit(xor_mask, pitch, x, y);

            if (and_bit == 1 && xor_bit == 0) continue;

            dst = g_frame_pixels + (size_t)dy * g_frame_pitch + (size_t)dx * 4u;

            if (and_bit == 0) {
                dst[0] = 0; dst[1] = 0; dst[2] = 0;
            }
            if (xor_bit) {
                dst[0] ^= 0xFF; dst[1] ^= 0xFF; dst[2] ^= 0xFF;
            }
            dst[3] = 255;
        }
    }
}

static void draw_color_pointer(int left, int top)
{
    UINT width = g_pointer_shape_info.Width;
    UINT height = g_pointer_shape_info.Height;
    UINT pitch = g_pointer_shape_info.Pitch;

    for (UINT y = 0; y < height; y++) {
        int dy = top + (int)y;
        const uint8_t *src_row;
        if (dy < 0 || dy >= (int)g_frame_height) continue;
        src_row = g_pointer_shape + (size_t)y * pitch;

        for (UINT x = 0; x < width; x++) {
            int dx = left + (int)x;
            if (dx < 0 || dx >= (int)g_frame_width) continue;
            alpha_blend_bgra(
                g_frame_pixels + (size_t)dy * g_frame_pitch + (size_t)dx * 4u,
                src_row + (size_t)x * 4u
            );
        }
    }
}

static void draw_masked_color_pointer(int left, int top)
{
    UINT width = g_pointer_shape_info.Width;
    UINT height = g_pointer_shape_info.Height;
    UINT pitch = g_pointer_shape_info.Pitch;

    for (UINT y = 0; y < height; y++) {
        int dy = top + (int)y;
        const uint8_t *src_row;
        if (dy < 0 || dy >= (int)g_frame_height) continue;
        src_row = g_pointer_shape + (size_t)y * pitch;

        for (UINT x = 0; x < width; x++) {
            int dx = left + (int)x;
            const uint8_t *src;
            uint8_t *dst;

            if (dx < 0 || dx >= (int)g_frame_width) continue;

            src = src_row + (size_t)x * 4u;
            dst = g_frame_pixels + (size_t)dy * g_frame_pitch + (size_t)dx * 4u;

            if (src[3] == 0) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
            } else if (src[3] == 255) {
                dst[0] ^= src[0]; dst[1] ^= src[1]; dst[2] ^= src[2]; dst[3] = 255;
            } else {
                alpha_blend_bgra(dst, src);
            }
        }
    }
}

static void draw_win32_cursor_fallback_on_frame(void)
{
    CURSORINFO ci;
    ICONINFO ii;
    BITMAP bm;
    BITMAPINFO bi;
    HDC screen_dc = NULL;
    HDC mem_dc = NULL;
    HBITMAP dib = NULL;
    HGDIOBJ old_obj = NULL;
    void *dib_bits = NULL;
    LONG width = 0;
    LONG height = 0;
    int left;
    int top;

    if (!g_pointer_visible || g_have_pointer_shape || !g_frame_pixels) return;

    ZeroMemory(&ci, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ZeroMemory(&ii, sizeof(ii));
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    ZeroMemory(&bm, sizeof(bm));
    if (ii.hbmColor) {
        GetObject(ii.hbmColor, sizeof(bm), &bm);
        width = bm.bmWidth;
        height = bm.bmHeight;
    } else if (ii.hbmMask) {
        GetObject(ii.hbmMask, sizeof(bm), &bm);
        width = bm.bmWidth;
        height = bm.bmHeight / 2;
    }

    if (width <= 0 || height <= 0) goto cleanup;

    left = ci.ptScreenPos.x - (int)ii.xHotspot - (int)g_output_desc.DesktopCoordinates.left;
    top = ci.ptScreenPos.y - (int)ii.yHotspot - (int)g_output_desc.DesktopCoordinates.top;

    if (left >= (int)g_frame_width || top >= (int)g_frame_height ||
        left + width <= 0 || top + height <= 0) {
        goto cleanup;
    }

    screen_dc = GetDC(NULL);
    if (!screen_dc) goto cleanup;

    mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) goto cleanup;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; /* top-down BGRA */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dib = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    if (!dib || !dib_bits) goto cleanup;

    old_obj = SelectObject(mem_dc, dib);

    for (LONG y = 0; y < height; y++) {
        int dy = top + (int)y;
        uint8_t *dst_row = (uint8_t *)dib_bits + (size_t)y * (size_t)width * 4u;

        for (LONG x = 0; x < width; x++) {
            int dx = left + (int)x;
            uint8_t *dst = dst_row + (size_t)x * 4u;

            if (dx >= 0 && dx < (int)g_frame_width && dy >= 0 && dy < (int)g_frame_height) {
                const uint8_t *src = g_frame_pixels + (size_t)dy * g_frame_pitch + (size_t)dx * 4u;
                memcpy(dst, src, 4u);
            } else {
                dst[0] = 0;
                dst[1] = 0;
                dst[2] = 0;
                dst[3] = 255;
            }
        }
    }

    if (!DrawIconEx(mem_dc, 0, 0, ci.hCursor, width, height, 0, NULL, DI_NORMAL)) {
        goto cleanup;
    }

    for (LONG y = 0; y < height; y++) {
        int dy = top + (int)y;
        const uint8_t *src_row;

        if (dy < 0 || dy >= (int)g_frame_height) continue;
        src_row = (const uint8_t *)dib_bits + (size_t)y * (size_t)width * 4u;

        for (LONG x = 0; x < width; x++) {
            int dx = left + (int)x;
            uint8_t *dst;

            if (dx < 0 || dx >= (int)g_frame_width) continue;

            dst = g_frame_pixels + (size_t)dy * g_frame_pitch + (size_t)dx * 4u;
            memcpy(dst, src_row + (size_t)x * 4u, 4u);
        }
    }

cleanup:
    if (old_obj && mem_dc) SelectObject(mem_dc, old_obj);
    if (dib) DeleteObject(dib);
    if (mem_dc) DeleteDC(mem_dc);
    if (screen_dc) ReleaseDC(NULL, screen_dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

static void draw_pointer_on_frame(void)
{
    int left;
    int top;

    if (!g_pointer_visible) return;

    if (!g_have_pointer_shape || !g_pointer_shape) {
        draw_win32_cursor_fallback_on_frame();
        return;
    }

    /*
        Position is already output-local.  Do not subtract DesktopCoordinates here.
        Subtracting it makes the cursor move outside the captured surface for
        monitors whose virtual-desktop origin is not (0, 0).
    */
    left = (int)g_pointer_pos_output.x;
    top = (int)g_pointer_pos_output.y;

    switch (g_pointer_shape_info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        draw_monochrome_pointer(left, top);
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        draw_color_pointer(left, top);
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        draw_masked_color_pointer(left, top);
        break;
    default:
        break;
    }
}

static HRESULT compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **blob_out)
{
    HRESULT hr;
    ID3DBlob *errors = NULL;

    *blob_out = NULL;

    hr = D3DCompile(
        source,
        strlen(source),
        NULL,
        NULL,
        NULL,
        entry,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        blob_out,
        &errors
    );

    if (FAILED(hr)) {
        if (errors) {
            fprintf(stderr, "D3DCompile %s/%s failed:\n%.*s\n",
                    entry,
                    target,
                    (int)ID3D10Blob_GetBufferSize(errors),
                    (const char *)ID3D10Blob_GetBufferPointer(errors));
        }
    }

    SAFE_RELEASE(errors);
    return hr;
}

static HRESULT ensure_render_pipeline(void)
{
    HRESULT hr;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    D3D11_INPUT_ELEMENT_DESC layout[2];
    D3D11_BUFFER_DESC vb_desc;
    D3D11_SUBRESOURCE_DATA vb_data;
    D3D11_SAMPLER_DESC samp_desc;
    D3D11_BUFFER_DESC cb_desc;

    static const char hlsl[] =
        "Texture2D tex0 : register(t0);\n"
        "SamplerState samp_linear : register(s0);\n"
        "SamplerState samp_point  : register(s1);\n"
        "cbuffer FilterParams : register(b0) {\n"
        "    float2 tex_size;\n"
        "    float2 inv_tex_size;\n"
        "    int filter_mode;\n"
        "    float lanczos_a;\n"
        "    float2 pad0;\n"
        "};\n"
        "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut vs_main(VSIn input) {\n"
        "    VSOut output;\n"
        "    output.pos = float4(input.pos, 0.0, 1.0);\n"
        "    output.uv = input.uv;\n"
        "    return output;\n"
        "}\n"
        "float4 load_px(int2 p) {\n"
        "    p = clamp(p, int2(0, 0), int2(tex_size) - int2(1, 1));\n"
        "    return tex0.Load(int3(p, 0));\n"
        "}\n"
        "float cubic_weight(float x) {\n"
        "    x = abs(x);\n"
        "    if (x < 1.0) return 1.5*x*x*x - 2.5*x*x + 1.0;\n"
        "    if (x < 2.0) return -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0;\n"
        "    return 0.0;\n"
        "}\n"
        "float sinc_pi(float x) {\n"
        "    x = abs(x);\n"
        "    if (x < 1.0e-5) return 1.0;\n"
        "    return sin(3.14159265358979323846 * x) / (3.14159265358979323846 * x);\n"
        "}\n"
        "float lanczos_weight(float x) {\n"
        "    x = abs(x);\n"
        "    if (x >= lanczos_a) return 0.0;\n"
        "    return sinc_pi(x) * sinc_pi(x / lanczos_a);\n"
        "}\n"
        "float4 sample_nearest(float2 uv) {\n"
        "    int2 p = int2(floor(uv * tex_size));\n"
        "    return load_px(p);\n"
        "}\n"
        "float4 sample_bicubic(float2 uv) {\n"
        "    float2 coord = uv * tex_size - 0.5;\n"
        "    int2 base = int2(floor(coord));\n"
        "    float2 f = coord - float2(base);\n"
        "    float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
        "    float weight_sum = 0.0;\n"
        "    [unroll] for (int j = -1; j <= 2; ++j) {\n"
        "        float wy = cubic_weight(float(j) - f.y);\n"
        "        [unroll] for (int i = -1; i <= 2; ++i) {\n"
        "            float wx = cubic_weight(float(i) - f.x);\n"
        "            float w = wx * wy;\n"
        "            sum += load_px(base + int2(i, j)) * w;\n"
        "            weight_sum += w;\n"
        "        }\n"
        "    }\n"
        "    return saturate(sum / max(weight_sum, 1.0e-6));\n"
        "}\n"
        "float4 sample_lanczos3(float2 uv) {\n"
        "    float2 coord = uv * tex_size - 0.5;\n"
        "    int2 base = int2(floor(coord));\n"
        "    float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
        "    float weight_sum = 0.0;\n"
        "    [unroll] for (int j = -2; j <= 3; ++j) {\n"
        "        float wy = lanczos_weight(coord.y - float(base.y + j));\n"
        "        [unroll] for (int i = -2; i <= 3; ++i) {\n"
        "            float wx = lanczos_weight(coord.x - float(base.x + i));\n"
        "            float w = wx * wy;\n"
        "            sum += load_px(base + int2(i, j)) * w;\n"
        "            weight_sum += w;\n"
        "        }\n"
        "    }\n"
        "    return saturate(sum / max(weight_sum, 1.0e-6));\n"
        "}\n"
        "float4 ps_main(VSOut input) : SV_Target {\n"
        "    if (filter_mode == 0) return sample_nearest(input.uv);\n"
        "    if (filter_mode == 1) return tex0.Sample(samp_linear, input.uv);\n"
        "    if (filter_mode == 2) return sample_bicubic(input.uv);\n"
        "    return sample_lanczos3(input.uv);\n"
        "}\n";

    static const Vertex2D vertices[4] = {
        {{-1.0f, -1.0f}, {0.0f, 1.0f}},
        {{-1.0f,  1.0f}, {0.0f, 0.0f}},
        {{ 1.0f, -1.0f}, {1.0f, 1.0f}},
        {{ 1.0f,  1.0f}, {1.0f, 0.0f}}
    };

    if (g_vertex_shader && g_pixel_shader && g_input_layout && g_vertex_buffer &&
        g_linear_sampler && g_point_sampler && g_filter_cbuffer) {
        return S_OK;
    }

    hr = compile_shader(hlsl, "vs_main", "vs_4_0", &vs_blob);
    if (FAILED(hr)) goto cleanup;

    hr = compile_shader(hlsl, "ps_main", "ps_4_0", &ps_blob);
    if (FAILED(hr)) goto cleanup;

    hr = ID3D11Device_CreateVertexShader(
        g_device,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        NULL,
        &g_vertex_shader
    );
    if (FAILED(hr)) goto cleanup;

    hr = ID3D11Device_CreatePixelShader(
        g_device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob),
        NULL,
        &g_pixel_shader
    );
    if (FAILED(hr)) goto cleanup;

    ZeroMemory(layout, sizeof(layout));
    layout[0].SemanticName = "POSITION";
    layout[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    layout[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    layout[1].SemanticName = "TEXCOORD";
    layout[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    layout[1].InputSlot = 0;
    layout[1].AlignedByteOffset = 8;
    layout[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    hr = ID3D11Device_CreateInputLayout(
        g_device,
        layout,
        2,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &g_input_layout
    );
    if (FAILED(hr)) goto cleanup;

    ZeroMemory(&vb_desc, sizeof(vb_desc));
    vb_desc.ByteWidth = sizeof(vertices);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    ZeroMemory(&vb_data, sizeof(vb_data));
    vb_data.pSysMem = vertices;

    hr = ID3D11Device_CreateBuffer(g_device, &vb_desc, &vb_data, &g_vertex_buffer);
    if (FAILED(hr)) goto cleanup;

    ZeroMemory(&samp_desc, sizeof(samp_desc));
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp_desc.MinLOD = 0.0f;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(g_device, &samp_desc, &g_linear_sampler);
    if (FAILED(hr)) goto cleanup;

    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = ID3D11Device_CreateSamplerState(g_device, &samp_desc, &g_point_sampler);
    if (FAILED(hr)) goto cleanup;

    ZeroMemory(&cb_desc, sizeof(cb_desc));
    cb_desc.ByteWidth = sizeof(FilterConstants);
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    hr = ID3D11Device_CreateBuffer(g_device, &cb_desc, NULL, &g_filter_cbuffer);

cleanup:
    SAFE_RELEASE(vs_blob);
    SAFE_RELEASE(ps_blob);
    return hr;
}

static HRESULT ensure_backbuffer_rtv(void)
{
    HRESULT hr;
    ID3D11Texture2D *backbuffer = NULL;

    if (g_backbuffer_rtv) return S_OK;

    hr = IDXGISwapChain_GetBuffer(g_swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateRenderTargetView(
        g_device,
        (ID3D11Resource *)backbuffer,
        NULL,
        &g_backbuffer_rtv
    );

    SAFE_RELEASE(backbuffer);
    return hr;
}

static HRESULT ensure_swapchain_size(UINT width, UINT height)
{
    HRESULT hr;

    if (width == 0 || height == 0) return S_FALSE;
    if (width == g_backbuffer_width && height == g_backbuffer_height) return S_OK;

    SAFE_RELEASE(g_backbuffer_rtv);

    hr = IDXGISwapChain_ResizeBuffers(g_swapchain, 0, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) return hr;

    g_backbuffer_width = width;
    g_backbuffer_height = height;
    return S_OK;
}

static HRESULT ensure_frame_gpu_texture(UINT width, UINT height)
{
    HRESULT hr;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    D3D11_TEXTURE2D_DESC current_desc;

    if (width == 0 || height == 0) return S_FALSE;

    if (g_frame_gpu_tex) {
        ZeroMemory(&current_desc, sizeof(current_desc));
        ID3D11Texture2D_GetDesc(g_frame_gpu_tex, &current_desc);
        if (current_desc.Width == width && current_desc.Height == height) {
            return S_OK;
        }
    }

    SAFE_RELEASE(g_frame_srv);
    SAFE_RELEASE(g_frame_gpu_tex);

    ZeroMemory(&td, sizeof(td));
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(g_device, &td, NULL, &g_frame_gpu_tex);
    if (FAILED(hr)) return hr;

    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format = td.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;

    return ID3D11Device_CreateShaderResourceView(
        g_device,
        (ID3D11Resource *)g_frame_gpu_tex,
        &srv_desc,
        &g_frame_srv
    );
}

static void compute_letterbox_viewport(UINT target_w, UINT target_h, D3D11_VIEWPORT *vp)
{
    double src_aspect;
    double dst_aspect;
    FLOAT draw_w;
    FLOAT draw_h;
    FLOAT off_x;
    FLOAT off_y;

    ZeroMemory(vp, sizeof(*vp));

    src_aspect = (double)g_frame_width / (double)g_frame_height;
    dst_aspect = (double)target_w / (double)target_h;

    if (dst_aspect > src_aspect) {
        draw_h = (FLOAT)target_h;
        draw_w = (FLOAT)((double)draw_h * src_aspect);
    } else {
        draw_w = (FLOAT)target_w;
        draw_h = (FLOAT)((double)draw_w / src_aspect);
    }

    if (draw_w < 1.0f) draw_w = 1.0f;
    if (draw_h < 1.0f) draw_h = 1.0f;
    if (draw_w > (FLOAT)target_w) draw_w = (FLOAT)target_w;
    if (draw_h > (FLOAT)target_h) draw_h = (FLOAT)target_h;

    off_x = ((FLOAT)target_w - draw_w) * 0.5f;
    off_y = ((FLOAT)target_h - draw_h) * 0.5f;

    vp->TopLeftX = off_x;
    vp->TopLeftY = off_y;
    vp->Width = draw_w;
    vp->Height = draw_h;
    vp->MinDepth = 0.0f;
    vp->MaxDepth = 1.0f;
}

static HRESULT present_gpu_frame(void)
{
    HRESULT hr;
    UINT target_w = g_client_width ? g_client_width : g_capture_width;
    UINT target_h = g_client_height ? g_client_height : g_capture_height;
    UINT stride = sizeof(Vertex2D);
    UINT offset = 0;
    D3D11_VIEWPORT vp;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11ShaderResourceView *null_srv[1] = {NULL};
    ID3D11SamplerState *samplers[2] = {NULL, NULL};
    FilterConstants params;

    if (!g_frame_pixels || !g_frame_width || !g_frame_height) return S_OK;

    hr = ensure_swapchain_size(target_w, target_h);
    if (hr == S_FALSE) return S_OK;
    if (FAILED(hr)) return hr;

    hr = ensure_backbuffer_rtv();
    if (FAILED(hr)) return hr;

    hr = ensure_render_pipeline();
    if (FAILED(hr)) return hr;

    hr = ensure_frame_gpu_texture(g_frame_width, g_frame_height);
    if (hr == S_FALSE) return S_OK;
    if (FAILED(hr)) return hr;

    ID3D11DeviceContext_UpdateSubresource(
        g_context,
        (ID3D11Resource *)g_frame_gpu_tex,
        0,
        NULL,
        g_frame_pixels,
        g_frame_pitch,
        0
    );

    ZeroMemory(&params, sizeof(params));
    params.tex_size[0] = (float)g_frame_width;
    params.tex_size[1] = (float)g_frame_height;
    params.inv_tex_size[0] = 1.0f / (float)g_frame_width;
    params.inv_tex_size[1] = 1.0f / (float)g_frame_height;
    params.filter_mode = (int)g_resize_filter;
    params.lanczos_a = 3.0f;

    ID3D11DeviceContext_UpdateSubresource(
        g_context,
        (ID3D11Resource *)g_filter_cbuffer,
        0,
        NULL,
        &params,
        0,
        0
    );

    compute_letterbox_viewport(target_w, target_h, &vp);

    ID3D11DeviceContext_OMSetRenderTargets(g_context, 1, &g_backbuffer_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_context, g_backbuffer_rtv, clear_color);
    ID3D11DeviceContext_RSSetViewports(g_context, 1, &vp);

    ID3D11DeviceContext_IASetInputLayout(g_context, g_input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetVertexBuffers(g_context, 0, 1, &g_vertex_buffer, &stride, &offset);

    ID3D11DeviceContext_VSSetShader(g_context, g_vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_context, g_pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, &g_frame_srv);
    ID3D11DeviceContext_PSSetConstantBuffers(g_context, 0, 1, &g_filter_cbuffer);

    samplers[0] = g_linear_sampler;
    samplers[1] = g_point_sampler;
    ID3D11DeviceContext_PSSetSamplers(g_context, 0, 2, samplers);

    ID3D11DeviceContext_Draw(g_context, 4, 0);

    ID3D11DeviceContext_PSSetShaderResources(g_context, 0, 1, null_srv);

    hr = IDXGISwapChain_Present(g_swapchain, 1, 0);
    if (SUCCEEDED(hr)) {
        fps_log_on_present();
    }

    g_need_resize_present = 0;
    return hr;
}

static HRESULT render_one_frame(void)
{
    HRESULT hr;
    HRESULT final_hr;
    DXGI_OUTDUPL_FRAME_INFO frame_info;
    IDXGIResource *desktop_resource = NULL;
    ID3D11Texture2D *desktop_tex = NULL;
    int frame_acquired = 0;

    ZeroMemory(&frame_info, sizeof(frame_info));

    hr = IDXGIOutputDuplication_AcquireNextFrame(g_dup, 16, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (g_need_resize_present && g_frame_pixels) {
            return present_gpu_frame();
        }
        return S_OK;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) return recreate_duplication();
    if (FAILED(hr)) return hr;

    frame_acquired = 1;

    hr = update_pointer_state(&frame_info);
    if (FAILED(hr)) goto cleanup;

    hr = IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D, (void **)&desktop_tex);
    if (FAILED(hr)) goto cleanup;

    hr = copy_desktop_texture_to_cpu(desktop_tex);
    if (FAILED(hr)) goto cleanup;

    draw_pointer_on_frame();
    hr = present_gpu_frame();

cleanup:
    final_hr = hr;
    SAFE_RELEASE(desktop_tex);
    SAFE_RELEASE(desktop_resource);

    if (frame_acquired) {
        HRESULT release_hr = IDXGIOutputDuplication_ReleaseFrame(g_dup);
        if (SUCCEEDED(final_hr) && FAILED(release_hr)) final_hr = release_hr;
    }

    if (final_hr == DXGI_ERROR_ACCESS_LOST) return recreate_duplication();
    return final_hr;
}

static void cleanup(void)
{
    free(g_pointer_shape);
    g_pointer_shape = NULL;
    free(g_frame_pixels);
    g_frame_pixels = NULL;

    SAFE_RELEASE(g_filter_cbuffer);
    SAFE_RELEASE(g_point_sampler);
    SAFE_RELEASE(g_linear_sampler);
    SAFE_RELEASE(g_vertex_buffer);
    SAFE_RELEASE(g_input_layout);
    SAFE_RELEASE(g_pixel_shader);
    SAFE_RELEASE(g_vertex_shader);
    SAFE_RELEASE(g_backbuffer_rtv);
    SAFE_RELEASE(g_frame_srv);
    SAFE_RELEASE(g_frame_gpu_tex);
    SAFE_RELEASE(g_staging_tex);
    SAFE_RELEASE(g_dup);
    SAFE_RELEASE(g_swapchain);
    SAFE_RELEASE(g_context);
    SAFE_RELEASE(g_device);
    SAFE_RELEASE(g_output1);
    SAFE_RELEASE(g_adapter);
}

int main(int argc, char **argv)
{
    HRESULT hr;
    UINT output_index;
    HINSTANCE inst = GetModuleHandleA(NULL);

    enum {
        OPT_FPS_LOG = 1000,
        OPT_FPS_LOG_INTERVAL = 1001
    };

    static const struct option long_options[] = {
        {"filter",           required_argument, NULL, 'f'},
        {"fps-log",          no_argument,       NULL, OPT_FPS_LOG},
        {"fps-log-interval", required_argument, NULL, OPT_FPS_LOG_INTERVAL},
        {"help",             no_argument,       NULL, 'h'},
        {0,                  0,                 0,     0}
    };

    make_process_dpi_aware();

    for (;;) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "f:h", long_options, &option_index);
        (void)option_index;

        if (c == -1) break;

        switch (c) {
        case 'f':
            if (!parse_filter_name(optarg, &g_resize_filter)) {
                fprintf(stderr, "Unknown filter: %s\n\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;

        case OPT_FPS_LOG:
            g_fps_log_enabled = 1;
            g_fps_log_interval_sec = 1.0;
            break;

        case OPT_FPS_LOG_INTERVAL:
            if (!parse_positive_double_or_zero(optarg, &g_fps_log_interval_sec)) {
                fprintf(stderr, "Invalid --fps-log-interval value: %s\n\n", optarg);
                print_usage(argv[0]);
                return 1;
            }
            g_fps_log_enabled = (g_fps_log_interval_sec > 0.0);
            break;

        case 'h':
            print_usage(argv[0]);
            return 0;

        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv[0]);
        fprintf(stderr, "\nAvailable outputs:\n");
        print_outputs();
        return 1;
    }

    if (optind + 1 != argc) {
        fprintf(stderr, "Unexpected extra argument: %s\n\n", argv[optind + 1]);
        print_usage(argv[0]);
        return 1;
    }

    output_index = (UINT)strtoul(argv[optind], NULL, 10);

    hr = select_output(output_index);
    if (FAILED(hr)) {
        fprintf(stderr, "select_output(%u) failed: 0x%08lx\n", output_index, (unsigned long)hr);
        return 1;
    }

    printf("Selected display %u: %ux%u, filter=%s, fps_log=%s",
           output_index,
           g_capture_width,
           g_capture_height,
           filter_name(g_resize_filter),
           g_fps_log_enabled ? "on" : "off");
    if (g_fps_log_enabled) {
        printf(", fps_log_interval=%.3f sec", g_fps_log_interval_sec);
    }
    printf("\n");

    if (g_output_desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        g_output_desc.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
        fprintf(stderr,
                "Note: this sample displays the raw duplicated surface. "
                "Rotated display modes may appear rotated. Rotation=%u\n",
                (UINT)g_output_desc.Rotation);
    }

    create_window_for_capture(inst);
    initialize_cursor_inside_source_state();
    create_d3d_and_swapchain();

    hr = create_duplication();
    if (FAILED(hr)) die_hr("IDXGIOutput1::DuplicateOutput", hr);

    fps_log_initialize();

    while (g_running) {
        MSG msg;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        maybe_activate_when_cursor_enters_source_display();

        hr = render_one_frame();
        if (FAILED(hr)) {
            fprintf(stderr, "render_one_frame failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
    }

    cleanup();
    return 0;
}
