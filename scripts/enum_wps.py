# -*- coding: utf-8 -*-
# enum_wps.py — 枚举 wps.exe 可见顶层窗口（类名 | 标题）
# 用途：验证 WPS 统一标签页框架类名是否随活动标签组件类型变化
#（2026-09-11 串档定位：演示标签激活时类名仍 OpusApp、标题变
#  "演示文稿1 - WPS Office"——COM 前台门控与读链标题校验的判据来源）
import ctypes
import ctypes.wintypes as wt
import json
import subprocess

user32 = ctypes.windll.user32
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)


def windows_for_pid(target):
    rows = []

    def cb(hwnd, lp):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == target and user32.IsWindowVisible(hwnd):
            cls = ctypes.create_unicode_buffer(256)
            ttl = ctypes.create_unicode_buffer(512)
            user32.GetClassNameW(hwnd, cls, 256)
            user32.GetWindowTextW(hwnd, ttl, 512)
            if ttl.value:
                rows.append((hwnd, cls.value, ttl.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return rows


out = subprocess.check_output(
    ["powershell", "-NoProfile", "-Command",
     "Get-Process wps -ErrorAction SilentlyContinue | Select-Object Id "
     "| ConvertTo-Json"],
    text=True).strip()
pids = json.loads(out) if out else []
if isinstance(pids, dict):
    pids = [pids]
for p in pids:
    wins = windows_for_pid(int(p["Id"]))
    if wins:
        print("PID", p["Id"])
        for h, c, t in wins:
            print("  0x%X | %s | %s" % (h, c, t))
