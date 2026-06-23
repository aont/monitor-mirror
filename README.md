# Monitor Mirror

Monitor Mirror is a Windows desktop capture utility that mirrors a selected DXGI output in a Direct3D 11 window. It supports selectable resize filters and optional FPS logging.

## Repository layout

```text
.
├── Makefile.mingw  # GNU make build, run, and clean targets for MinGW
├── Makefile.msvc   # MSVC nmake build, run, and clean targets
├── README.md       # Project documentation
├── vcpkg.json      # vcpkg manifest for MSVC dependencies
└── src/
    └── main.c      # Application source code
```

Build artifacts are written to `bin/`, which is created by the selected build tool when needed.

## Requirements

- Windows
- For MinGW builds: MSYS2 UCRT64 or another GCC environment with Windows SDK/Direct3D headers and libraries, plus `make`
- For MSVC builds: Visual Studio Build Tools or Visual Studio with the C++ workload, `nmake`, and vcpkg

## Build

### MinGW / GNU make

```sh
make -f Makefile.mingw
```

The build produces:

```text
bin/monitor_mirror.exe
```

### MSVC / nmake

Open a Developer Command Prompt for Visual Studio, install the vcpkg manifest dependencies, then build with `nmake`:

```bat
vcpkg install
nmake /f Makefile.msvc
```

The vcpkg manifest installs `getopt-win32`, which provides the `getopt.h` dependency used by the command-line option parser. `Makefile.msvc` assumes the default vcpkg manifest output path and target triplet (`vcpkg_installed\x64-windows`). Override `VCPKG_INSTALLED_DIR` or `VCPKG_TARGET_TRIPLET` on the `nmake` command line if you use a different install directory or triplet:

```bat
nmake /f Makefile.msvc VCPKG_TARGET_TRIPLET=x86-windows
```


You can override the compiler and flags if needed:

```sh
make -f Makefile.mingw CC=gcc CFLAGS="-std=c11 -Wall -Wextra -O2 -D_CRT_SECURE_NO_WARNINGS"
```

## Usage

List available DXGI outputs by running the program without a display index:

```sh
./bin/monitor_mirror.exe
```

Mirror a display by passing its output index:

```sh
./bin/monitor_mirror.exe 0
```

Choose a resize filter:

```sh
./bin/monitor_mirror.exe --filter bilinear 0
./bin/monitor_mirror.exe -f lanczos 0
```

Choose how the mirror window is raised when the cursor enters the mirrored source display:

```sh
./bin/monitor_mirror.exe --raise-mode foreground 0
./bin/monitor_mirror.exe --raise-mode topmost-pulse 0
```

Raise modes are:

- `foreground` / `activate` - use the existing foreground activation path.
- `topmost-pulse` / `topmost` - temporarily set the mirror window to `HWND_TOPMOST`, then immediately return it to `HWND_NOTOPMOST` so it comes forward without staying always-on-top.

Supported filters are:

- `nearest` / `point`
- `bilinear` / `linear`
- `bicubic` / `cubic`
- `lanczos` / `lanczos3`

Enable FPS logging:

```sh
./bin/monitor_mirror.exe --fps-log 0
./bin/monitor_mirror.exe --fps-log-interval 2.5 0
```

## Maintenance targets

Remove generated build artifacts:

```sh
make -f Makefile.mingw clean
```

For MSVC / nmake builds:

```bat
nmake /f Makefile.msvc clean
```

Build and run the executable:

```sh
make -f Makefile.mingw run
```

For MSVC / nmake builds:

```bat
nmake /f Makefile.msvc run
```
