# Copy_C for Windows 11

Copy_C is a small native C++ desktop app that saves the middle 20 seconds of a
selected MP4 as `original-name_20onec.mp4` in the same folder. The picker opens
in the Windows Downloads folder. Existing output files require confirmation.

## Requirements

- Windows 11
- FFmpeg's `ffmpeg.exe` and `ffprobe.exe` available on `PATH`

For example, install FFmpeg with `winget install Gyan.FFmpeg`, then reopen the
terminal or Visual Studio Developer Command Prompt so the updated `PATH` is
available.

## Build with Visual Studio

Install Visual Studio 2022 with the **Desktop development with C++** workload,
open a Developer PowerShell in this directory, and run:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable will be `build\Release\Copy_C.exe` (or `build\Copy_C.exe` with a
single-configuration CMake generator). Copy the executable to the Desktop or
pin it to Start to launch it.

The program uses the Windows native file picker and C++17; it has no third-party
GUI-library dependency. FFmpeg is a separate command-line dependency used for
duration detection and encoding.
