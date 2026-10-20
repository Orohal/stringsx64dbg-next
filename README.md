# StringsX64DbgNext

A clean C++ rewrite of the x64dbg Strings plugin. It produces the familiar
`strings.dp64` and `strings.dp32` plugin packages without depending on Qt.

## Features

- Scan the debuggee's main module for printable ANSI and UTF-16LE strings.
- Configure the minimum length and optionally require null termination.
- Filter results, copy a selected result, export results as UTF-8 CSV, and
  follow an address in x64dbg's dump.
- Run memory scanning on a worker thread and cancel it when the debug session
  stops or the window closes.
- Resolve the main module when a scan starts, including sessions already
  running when the plugin window is opened.

## Build

Requirements: Windows, CMake 3.21+, a Visual Studio C++ toolchain, and an
x64dbg checkout containing `pluginsdk/` and its x86/x64 import libraries.

```powershell
cmake -S . -B build-x64 -A x64 -DX64DBG_SDK_ROOT="C:\path\to\x64dbg"
cmake --build build-x64 --config Release
ctest --test-dir build-x64 -C Release --output-on-failure
```

For a 32-bit package, configure a separate build directory with `-A Win32`:

```powershell
cmake -S . -B build-x86 -A Win32 -DX64DBG_SDK_ROOT="C:\path\to\x64dbg"
cmake --build build-x86 --config Release
ctest --test-dir build-x86 -C Release --output-on-failure
```

The package is written to
`build-x64\package\x64\Release\plugins\strings.dp64` or
`build-x86\package\x86\Release\plugins\strings.dp32`.

## Use

Copy the matching package into x64dbg's `plugins` directory, then restart the
debugger. Open **Plugins > Strings** to scan the main module.
