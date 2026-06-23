# Monitor Mirror

Monitor Mirror is a Windows desktop capture utility that mirrors a selected DXGI output in a Direct3D 11 window. It supports selectable resize filters and optional FPS logging.

## Repository layout

```text
.
├── Makefile        # Build, run, and clean targets
├── README.md       # Project documentation
└── src/
    └── main.c      # Application source code
```

Build artifacts are written to `bin/`, which is created by `make` when needed.

## Requirements

- Windows
- MSYS2 UCRT64 or another GCC environment with Windows SDK/Direct3D headers and libraries
- `make`

## Build

```sh
make
```

The build produces:

```text
bin/monitor_mirror.exe
```

You can override the compiler and flags if needed:

```sh
make CC=gcc CFLAGS="-std=c11 -Wall -Wextra -O2 -D_CRT_SECURE_NO_WARNINGS"
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
make clean
```

Build and run the executable:

```sh
make run
```
