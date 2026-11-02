"""Run a native Qt/UIA fixture on a separate, non-interactive Windows desktop."""
import ctypes
from ctypes import wintypes as W
import msvcrt
import os
from pathlib import Path
import subprocess
import uuid


def run_fixture(executable, cwd, timeout):
    class StartupInfo(ctypes.Structure):
        _fields_ = [("cb", W.DWORD), ("lpReserved", W.LPWSTR), ("lpDesktop", W.LPWSTR),
                    ("lpTitle", W.LPWSTR), ("dwX", W.DWORD), ("dwY", W.DWORD),
                    ("dwXSize", W.DWORD), ("dwYSize", W.DWORD), ("dwXCountChars", W.DWORD),
                    ("dwYCountChars", W.DWORD), ("dwFillAttribute", W.DWORD), ("dwFlags", W.DWORD),
                    ("wShowWindow", W.WORD), ("cbReserved2", W.WORD), ("lpReserved2", ctypes.c_void_p),
                    ("hStdInput", W.HANDLE), ("hStdOutput", W.HANDLE), ("hStdError", W.HANDLE)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", W.HANDLE), ("thread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]

    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    user.CreateDesktopW.argtypes = [W.LPCWSTR, W.LPCWSTR, ctypes.c_void_p, W.DWORD, W.DWORD, ctypes.c_void_p]
    user.CreateDesktopW.restype = W.HANDLE
    user.CloseDesktop.argtypes = [W.HANDLE]
    kernel.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                    W.BOOL, W.DWORD, ctypes.c_void_p, W.LPCWSTR,
                                    ctypes.POINTER(StartupInfo), ctypes.POINTER(ProcessInfo)]
    kernel.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
    kernel.WaitForSingleObject.restype = W.DWORD
    kernel.GetExitCodeProcess.argtypes = [W.HANDLE, ctypes.POINTER(W.DWORD)]
    kernel.TerminateProcess.argtypes = [W.HANDLE, W.UINT]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    name = "CodexQtTest_" + uuid.uuid4().hex
    desktop = user.CreateDesktopW(name, None, None, 0, 0x10000000, None)
    if not desktop:
        raise ctypes.WinError(ctypes.get_last_error())
    process = ProcessInfo()
    output = Path(cwd) / "fixture-output.log"
    try:
        with open(os.devnull, "rb") as stdin, output.open("wb") as stdout:
            input_handle = msvcrt.get_osfhandle(stdin.fileno())
            output_handle = msvcrt.get_osfhandle(stdout.fileno())
            os.set_handle_inheritable(input_handle, True)
            os.set_handle_inheritable(output_handle, True)
            startup = StartupInfo()
            startup.cb = ctypes.sizeof(startup)
            startup.lpDesktop = "WinSta0\\" + name
            startup.dwFlags = 0x100
            startup.hStdInput = input_handle
            startup.hStdOutput = startup.hStdError = output_handle
            command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(executable), "--test-desktop"]))
            if not kernel.CreateProcessW(str(executable), command, None, None, True, 0x08000000,
                                         None, str(cwd), ctypes.byref(startup), ctypes.byref(process)):
                raise ctypes.WinError(ctypes.get_last_error())
            if kernel.WaitForSingleObject(process.process, int(timeout * 1000)) != 0:
                kernel.TerminateProcess(process.process, 124)
                kernel.WaitForSingleObject(process.process, 5000)
                raise subprocess.TimeoutExpired(str(executable), timeout)
            code = W.DWORD()
            if not kernel.GetExitCodeProcess(process.process, ctypes.byref(code)):
                raise ctypes.WinError(ctypes.get_last_error())
        return subprocess.CompletedProcess([str(executable)], code.value, output.read_bytes(), b"")
    finally:
        if process.thread:
            kernel.CloseHandle(process.thread)
        if process.process:
            kernel.CloseHandle(process.process)
        user.CloseDesktop(desktop)
