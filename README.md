# Monitor Mirror

Monitor Mirror is a Windows desktop capture utility that mirrors a selected DXGI output in a Direct3D 11 window. It supports selectable resize filters and optional FPS logging.

## Repository layout

```text
.
├── Makefile.mingw  # GNU make build, run, and clean targets for MinGW
├── Makefile.msvc   # MSVC nmake build, run, and clean targets
├── README.md       # Project documentation
└── src/
    └── main.c      # Application source code
```

Build artifacts are written to `bin/`, which is created by the selected build tool when needed.

## Requirements

- Windows
- For MinGW builds: MSYS2 UCRT64 or another GCC environment with Windows SDK/Direct3D headers and libraries, plus `make`
- For MSVC builds: Visual Studio Build Tools or Visual Studio with the C++ workload, `nmake`, and `getopt-msvc-helper`

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

Open a Developer Command Prompt for Visual Studio, then build `getopt.h` and `getopt.lib` with [getopt-msvc-helper](https://github.com/aont/getopt-msvc-helper). After building it, add the directory that contains `getopt.h` to the `INCLUDE` environment variable and the directory that contains `getopt.lib` to the `LIB` environment variable. For example:

```bat
git clone https://github.com/aont/getopt-msvc-helper.git
cd getopt-msvc-helper
nmake
set "INCLUDE=%CD%;%INCLUDE%"
set "LIB=%CD%;%LIB%"
cd ..\monitor-mirror
nmake /f Makefile.msvc
```

`Makefile.msvc` links against `getopt.lib`, which provides the `getopt.h` dependency used by the command-line option parser. If `getopt.h` or `getopt.lib` are in separate directories, add each directory to `INCLUDE` or `LIB` respectively before running `nmake`.

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
