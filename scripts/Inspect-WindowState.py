"""Read-only visibility diagnostic for an existing Lake Controller process.

Uses only process-name inventory and window queries. Does not open process
memory, attach a debugger, send messages, show windows, or change the network.
No text from network-interface/device controls is collected.
"""

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import json
from pathlib import Path
import sys


def inspect(selected_pid):
    if sys.platform != "win32":
        raise RuntimeError("Windows is required")
    kernel = c.WinDLL("kernel32", use_last_error=True)
    user = c.WinDLL("user32", use_last_error=True)

    class ProcessEntry(c.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("pid", w.DWORD),
                    ("heap", c.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                    ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD),
                    ("name", w.WCHAR * 260)]

    kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = kernel.Process32FirstW.argtypes
    kernel.CloseHandle.argtypes = [w.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    targets = set()
    try:
        entry = ProcessEntry()
        entry.dwSize = c.sizeof(entry)
        exists = kernel.Process32FirstW(snapshot, c.byref(entry))
        while exists:
            if entry.name.lower() == "lake controller.exe" and (not selected_pid or selected_pid == entry.pid):
                targets.add(entry.pid)
            exists = kernel.Process32NextW(snapshot, c.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    if selected_pid and selected_pid not in targets:
        raise RuntimeError(f"PID {selected_pid} is not an existing Lake Controller.exe; no window queried")

    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.EnumChildWindows.argtypes = [w.HWND, callback_type, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.GetWindowThreadProcessId.restype = w.DWORD
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.GetWindowLongW.argtypes = [w.HWND, c.c_int]
    user.GetWindowLongW.restype = w.LONG
    user.GetParent.argtypes = [w.HWND]
    user.GetParent.restype = w.HWND
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.IsWindowEnabled.argtypes = [w.HWND]
    user.GetPropW.argtypes = [w.HWND, w.LPCWSTR]
    user.GetPropW.restype = w.HANDLE
    results = {pid: {"pid": pid, "main_windows": [], "other_top_level_classes": {}} for pid in targets}

    @callback_type
    def visit(hwnd, child):
        pid = w.DWORD()
        tid = user.GetWindowThreadProcessId(hwnd, c.byref(pid))
        if pid.value not in targets:
            return True
        name = c.create_unicode_buffer(256)
        user.GetClassNameW(hwnd, name, len(name))
        result = results[pid.value]
        if name.value in ("ContourParent", "Lake Controller"):
            title = c.create_unicode_buffer(256)
            user.GetWindowTextW(hwnd, title, len(title))
            rect = w.RECT()
            rectangle_available = bool(user.GetWindowRect(hwnd, c.byref(rect)))
            style = user.GetWindowLongW(hwnd, -16) & 0xFFFFFFFF
            result["main_windows"].append({
                "hwnd": hex(hwnd), "tid": tid, "class": name.value, "title": title.value,
                "visible": bool(user.IsWindowVisible(hwnd)),
                "visible_style_bit": bool(style & 0x10000000),
                "enabled": bool(user.IsWindowEnabled(hwnd)), "style": hex(style),
                "host_setup_completed": bool(user.GetPropW(hwnd, "LakeWindowedFix.HostSetupCompleted")),
                "parent": hex(user.GetParent(hwnd) or 0),
                "rect": [rect.left, rect.top, rect.right, rect.bottom] if rectangle_available else None,
            })
        elif not child:
            classes = result["other_top_level_classes"]
            classes[name.value] = classes.get(name.value, 0) + 1
        if not child:
            user.EnumChildWindows(hwnd, visit, 1)
        return True

    user.EnumWindows(visit, 0)
    return {"captured_at": datetime.now(timezone.utc).isoformat(),
            "source": "local-win32-window-query", "scope": "current-desktop-only",
            "processes": list(results.values()),
            "limitations": ["No process memory, call stacks or renderer state inspected.",
                            "Window visibility does not establish graphics or network health.",
                            "This is local Python/Win32 output, not REA Evidence."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--output", type=Path, default=Path("private/window-state.json"))
    args = parser.parse_args()
    result = inspect(args.pid)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    print("Private report:", args.output.resolve())


if __name__ == "__main__":
    main()
