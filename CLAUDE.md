# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

DF_PlayClick is a Visual Studio C++ Win32 solution with two GUI executables:

- `DF_PlayClick_Server`: master controller. It listens for TCP clients, shows each connected client IP, ready state, and remark, supports right-click remark/kick actions, and sends trigger commands to all ready clients.
- `DF_PlayClick_Client`: controlled client. It connects to the server, remembers the last successful server IP in `DF_PlayClick_Client.ini` beside the EXE, lets the user bind a visible HWND target window, pick/edit a client-area click coordinate, toggle ready state, and execute a countdown click when triggered.

Shared protocol/socket helpers live in `Shared/` and are included by both projects.

## Build and verification commands

Use the installed Visual Studio MSBuild path from Git Bash. Prefer dash-form MSBuild switches because slash switches can be path-converted by Git Bash.

```bash
"d:/visual Studio/visual studio IDE/MSBuild/Current/Bin/MSBuild.exe" "d:/visual Studio/visual studio Project/DF_PlayClick/DF_PlayClick.sln" -p:Configuration=Debug -p:Platform=x64 -m
```

Other common configurations:

```bash
"d:/visual Studio/visual studio IDE/MSBuild/Current/Bin/MSBuild.exe" "d:/visual Studio/visual studio Project/DF_PlayClick/DF_PlayClick.sln" -p:Configuration=Release -p:Platform=x64 -m
"d:/visual Studio/visual studio IDE/MSBuild/Current/Bin/MSBuild.exe" "d:/visual Studio/visual studio Project/DF_PlayClick/DF_PlayClick.sln" -p:Configuration=Debug -p:Platform=Win32 -m
```

Run the protocol test:

```bash
g++ -std=c++17 -I"d:/visual Studio/visual studio Project/DF_PlayClick" "d:/visual Studio/visual studio Project/DF_PlayClick/Tests/ProtocolTests.cpp" -o "d:/visual Studio/visual studio Project/DF_PlayClick/Tests/ProtocolTests.exe" && "d:/visual Studio/visual studio Project/DF_PlayClick/Tests/ProtocolTests.exe"
```

Check static CRT dependency for Debug x64 output when needed:

```bash
"d:/visual Studio/visual studio IDE/VC/Tools/MSVC/14.29.30133/bin/Hostx86/x64/dumpbin.exe" -dependents "d:/visual Studio/visual studio Project/DF_PlayClick/x64/Debug/DF_PlayClick_Server.exe"
"d:/visual Studio/visual studio IDE/VC/Tools/MSVC/14.29.30133/bin/Hostx86/x64/dumpbin.exe" -dependents "d:/visual Studio/visual studio Project/DF_PlayClick/x64/Debug/DF_PlayClick_Client.exe"
```

Expected build outputs are under `x64/Debug/` or `x64/Release/` at the solution root.

## Architecture notes

- The wire protocol is line-delimited text in `Shared/Protocol.h`: `HELLO`, `READY 0/1`, and `TRIGGER <seconds>`. Keep protocol changes backward-consistent between server and client and update `Tests/ProtocolTests.cpp` for parser/builder behavior.
- `Shared/SocketUtil.h` owns process-wide Winsock startup through `SocketUtil::WsaSession` and provides `SendLine()`/`SendAll()` helpers.
- The server owns a listening socket plus an accept thread. Each accepted client gets a `ClientInfo` stored in `g_clients` and a detached receive thread. Worker threads must post UI refreshes back to the main window with `PostMessageW`; do not update Win32 controls directly from worker threads.
- Server ListView rows store a `ClientInfo*` in `LVITEMW::lParam`; right-click remark/kick actions resolve that pointer back through `FindClient()` under `g_clientsMutex`. Preserve this mapping if the list display or sorting changes.
- Client connection state is held in `g_socket`, `g_connected`, and `g_ready`. The receive thread parses `TRIGGER` messages and posts `WM_TRIGGER_RECEIVED`; the UI thread starts the countdown timer and sends the actual mouse input.
- Client HWND binding enumerates visible top-level windows, stores them in `g_windows`, and treats click coordinates as bound-window client-area coordinates. `ClickBoundPoint()` converts client coordinates to screen coordinates before `SendInput()`.
- Both Win32 windows are implemented directly in single `.cpp` files with module-level state, control IDs, `CreateControls()`, and a `WindowProc()`. Keep edits localized unless a cross-cutting protocol or shared socket change is required.

## Project conventions and pitfalls

- Source files contain Chinese string literals. Preserve UTF-8 with BOM for `.cpp` files to avoid Visual Studio C4819 and broken Chinese literals.
- The Visual Studio projects intentionally use static runtime libraries: Debug uses `MultiThreadedDebug` (`/MTd`) and Release uses `MultiThreaded` (`/MT`) for both Win32 and x64. Do not change back to dynamic CRT unless explicitly requested.
- For Win32 control IDs cast through `static_cast<INT_PTR>(IDC_...)` before `reinterpret_cast<HMENU>(...)` to avoid x64 cast warnings.
- Keep `ServerApp.cpp` including both `commctrl.h` and `windowsx.h`; ListView declarations come from `commctrl.h`, and context-menu coordinate macros come from `windowsx.h`.
- `WM_CTLCOLORSTATIC` and `WM_CTLCOLOREDIT` are used to keep labels/status/edit controls white; preserve this behavior when changing layouts.
