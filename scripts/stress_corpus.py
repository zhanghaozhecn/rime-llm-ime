# stress_corpus.py — 用语料里的真实码序列回放打字（用户定案 2026-11-22：
# "直接用语料进行模拟打字，以便最快速度获取日志"）。
# 从 rime_llm_corpus.tsv 提取 tsf 来源的真实输入码（按首次出现序），SendInput
# 回放到前台 notepad；逐码 CPU 剖面（哪类码烧 CPU 一目了然）+ 卡死守候
#（>0.4 核/s 持续 → procdump 全 dump → 杀重启）。
import ctypes
import os
import sys
import time
import subprocess

APPDATA = os.environ.get("APPDATA", "")
CORPUS = os.path.join(APPDATA, r"Rime\llm_rerank\rime_llm_corpus.tsv")
NOTEPAD_HWND = 393644
KEY_GAP = 0.06          # 码内 60ms/键 = 快速连打（贴近卡死场景）
CODE_PAUSE = 0.35
HANG_RATE = 0.4         # 核/秒

MAX_PATH_LEN = 260


def rip_snapshot(target_pid):
    """瞬时抓取 target 进程全部线程的 RIP（挂起->GetThreadContext->恢复），
    100ms 级完成——脉冲式 CPU 尖峰的即时现场。"""
    import ctypes.wintypes as wt
    k32 = ctypes.windll.kernel32
    TH32CS_SNAPTHREAD = 0x4
    class THREADENTRY32(ctypes.Structure):
        _fields_ = [("cnt", wt.DWORD), ("owner", wt.DWORD), ("tid", wt.DWORD),
                    ("usage", wt.DWORD), ("sq", wt.DWORD), ("kpri", ctypes.c_long)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.cnt = ctypes.sizeof(THREADENTRY32)
    tids = []
    if k32.Thread32First(snap, ctypes.byref(e)):
        while True:
            if e.owner == target_pid:
                tids.append(e.tid)
            if not k32.Thread32Next(snap, ctypes.byref(e)):
                break
    k32.CloseHandle(snap)

    class CONTEXT(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in (
            "P1Home","P2Home","P3Home","P4Home","P5Home","P6Home",
            "ContextFlags","MxCsr")] +             [(n, ctypes.c_uint16) for n in (
            "SegCs","SegDs","SegEs","SegFs","SegGs","SegSs","EFlags")] +             [(n, ctypes.c_uint64) for n in (
            "Dr0","Dr1","Dr2","Dr3","Dr6","Dr7",
            "Rax","Rcx","Rdx","Rbx","Rsp","Rbp","Rsi","Rdi",
            "R8","R9","R10","R11","R12","R13","R14","R15","Rip")] +             [(n, ctypes.c_uint64 * 26) for n in ("XMM",)] +             [(n, ctypes.c_uint8 * 512) for n in ("Vector",)]

    # x64 CONTEXT: ContextFlags@0x30, Rip@0xF8（用显式 offset 兜底）
    results = []
    CONTEXT_CONTROL = 0x100001
    for tid in tids:
        h = k32.OpenThread(0x001FFFFF, False, tid)   # THREAD_ALL_ACCESS
        if not h:
            results.append((tid, -1, "open-fail"))
            continue
        if k32.SuspendThread(h) == -1:
            k32.CloseHandle(h)
            continue
        ctx = CONTEXT()
        ctx.ContextFlags = CONTEXT_CONTROL
        ok = k32.GetThreadContext(h, ctypes.byref(ctx))
        rip = ctypes.c_uint64()
        ctypes.memmove(ctypes.addressof(rip),
                       ctypes.addressof(ctx) + 0xF8, 8)
        k32.ResumeThread(h)
        k32.CloseHandle(h)
        if ok:
            results.append((tid, rip.value, "ok"))
        else:
            results.append((tid, -1, "ctx-fail"))
    return results

def mod_of_addr(addr, mods):
    for name, base, size in mods:
        if base <= addr < base + size:
            return "%s+0x%x" % (name.split(chr(92))[-1], addr - base)
    return "unknown=0x%x" % addr

def server_mods():
    import subprocess, re
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq WeaselServer.exe",
                        "/FO", "CSV"], capture_output=True)
    out = r.stdout.decode("gbk", "replace")
    tpid = 0
    for line in out.splitlines():
        if "WeaselServer" in line:
            tpid = int(line.split('","')[1])
    # 模块表用 psutil 不可用则退化为 python 遍历 Module32
    import ctypes.wintypes as wt
    k32 = ctypes.windll.kernel32
    TH32CS_SNAPMODULE = 0x8
    TH32CS_SNAPMODULE32 = 0x10
    class MODULEENTRY32(ctypes.Structure):
        _fields_ = [("cnt", wt.DWORD), ("pid", wt.DWORD),
                    ("base", ctypes.c_void_p), ("size", wt.DWORD),
                    ("hmod", ctypes.c_void_p),
                    ("mod", wt.WCHAR * 256), ("exe", wt.WCHAR * MAX_PATH_LEN)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                        tpid)
    e = MODULEENTRY32()
    e.cnt = ctypes.sizeof(MODULEENTRY32)
    mods = []
    if k32.Module32FirstW(snap, ctypes.byref(e)):
        while True:
            mods.append((e.exe, e.base or 0, e.size))
            if not k32.Module32NextW(snap, ctypes.byref(e)):
                break
    k32.CloseHandle(snap)
    return tpid, mods

MAX_PATH_LEN = 260

PROCDUMP = r"C:\Windows\Temp\procdump64.exe"

u = ctypes.windll.user32


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVK", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                ("dwFlags", ctypes.c_uint), ("time", ctypes.c_uint),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                ("mouseData", ctypes.c_uint), ("dwFlags", ctypes.c_uint),
                ("time", ctypes.c_uint),
                ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT(ctypes.Structure):
    class _I(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]
    _anonymous_ = ("i",)
    _fields_ = [("type", ctypes.c_uint), ("i", _I)]


def send_key(vk, up=False):
    i = INPUT()
    i.type = 1
    i.ki = KEYBDINPUT(vk, 0, 2 if up else 0, 0, None)
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def tap(vk, win=False):
    seq = []
    if win:
        w = INPUT(); w.type = 1; w.ki = KEYBDINPUT(0x5B, 0, 0, 0, None)
        seq.append(w)
    k = INPUT(); k.type = 1; k.ki = KEYBDINPUT(vk, 0, 0, 0, None)
    seq.append(k)
    u.SendInput(len(seq), (INPUT * len(seq))(*seq), ctypes.sizeof(INPUT))
    time.sleep(0.05)
    seq2 = []
    if win:
        w = INPUT(); w.type = 1; w.ki = KEYBDINPUT(0x5B, 0, 2, 0, None)
        seq2.append(w)
    k = INPUT(); k.type = 1; k.ki = KEYBDINPUT(vk, 0, 2, 0, None)
    seq2.append(k)
    u.SendInput(len(seq2), (INPUT * len(seq2))(*seq2), ctypes.sizeof(INPUT))


def cpu_seconds():
    import ctypes.wintypes as wt
    class PINFO(ctypes.Structure):
        _fields_ = [("ut", wt.FILETIME), ("kt", wt.FILETIME),
                    ("st", wt.FILETIME), ("et", wt.FILETIME)]
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid[0])
    if not h:
        return -1
    pi = PINFO()
    ok = ctypes.windll.kernel32.GetProcessTimes(
        h, ctypes.byref(pi.et), ctypes.byref(pi.et),
        ctypes.byref(pi.kt), ctypes.byref(pi.ut))
    ctypes.windll.kernel32.CloseHandle(h)
    if not ok:
        return -1
    return (pi.ut.dwLowDateTime + (pi.ut.dwHighDateTime << 32)) / 1e7


pid = [0]


def server_pid():
    import ctypes.wintypes as wt

    class PINFO(ctypes.Structure):
        _fields_ = [("ut", wt.FILETIME), ("kt", wt.FILETIME),
                    ("st", wt.FILETIME), ("et", wt.FILETIME)]
    # 找 pid：Toolhelp32 快照
    TH32CS_SNAPPROCESS = 0x2
    class PROCESSENTRY32(ctypes.Structure):
        _fields_ = [("cnt", wt.DWORD), ("pid", wt.DWORD),
                    ("ppid", wt.DWORD), ("threads", wt.DWORD)]
    k32 = ctypes.windll.kernel32
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32()
    e.cnt = ctypes.sizeof(PROCESSENTRY32)
    found = 0
    if k32.Process32FirstW(snap, ctypes.byref(e)):
        while True:
            name = e.pid and ""
            # PROCESSENTRY32W 多了 exeName 字段——改用 W 版结构
            break
    k32.CloseHandle(snap)
    return found


# 简化：pid 用 PowerShell 预先取好传入
def main():
    global pid
    if len(sys.argv) > 1:
        pid[0] = int(sys.argv[1])
    else:
        print("usage: stress_corpus.py <server_pid>")
        sys.exit(2)

    # ── 语料码序列提取（tsf 来源真实输入码，首次出现序）──
    codes, seen = [], set()
    with open(CORPUS, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split("\t")
            if p[0] == "W" and len(p) > 5 and p[3] == "tsf":
                c = p[4]
                if (c and c not in seen and 1 < len(c) <= 24
                        and all("a" <= x <= "z" for x in c)):
                    seen.add(c)
                    codes.append(c)
    print("codes=%d (unique, tsf)" % len(codes))
    codes = codes[:80]      # 控制时长 ~90s

    # ── 前台 + TSF 重连 ──
    u.SetForegroundWindow(ctypes.c_void_p(NOTEPAD_HWND))
    time.sleep(0.4)
    tap(0x20, win=True)
    time.sleep(0.4)
    tap(0x20, win=True)
    time.sleep(0.4)
    u.SetForegroundWindow(ctypes.c_void_p(NOTEPAD_HWND))
    time.sleep(0.3)

    rep = []
    hang = False
    t_all0 = time.time()
    c_prev = cpu_seconds()
    for i, code in enumerate(codes):
        c0 = cpu_seconds()
        for ch in code:
            tap(ord(ch.upper()))
            time.sleep(KEY_GAP)
        # 每 3 码一个空格上屏（建立/刷新上文），否则退格清编码
        if i % 3 == 0:
            tap(0x20)
            time.sleep(CODE_PAUSE * 0.6)
        else:
            for _ in code:
                tap(0x08)
                time.sleep(0.04)
        c1 = cpu_seconds()
        d = c1 - c0
        rep.append((code, round(d, 3)))
        if d > 1.0:
            print("EXPENSIVE: %s cpu=%.2fs" % (code, d))
        # 卡死检测（每码）
        c2 = cpu_seconds()
        time.sleep(0.15)
        c3 = cpu_seconds()
        rate = (c3 - c2) / 0.15
        if rate > HANG_RATE:
            print("HANG detected after %s rate=%.2f/s" % (code, rate))
            try:
                tpid, mods = server_mods()
                snaps = rip_snapshot(tpid)
                from collections import Counter
                cc = Counter()
                for tid, rip, st in snaps:
                    if rip > 0:
                        cc[mod_of_addr(rip, mods)] += 1
                print("RIP snapshot:", dict(cc.most_common(10)))
                for tid, rip, st in snaps:
                    if rip > 0 and "rime.dll" in mod_of_addr(rip, mods):
                        print("  HOT tid=%d %s" % (tid, mod_of_addr(rip, mods)))
            except Exception as ex:
                print("rip snapshot failed:", ex)
            dump = r"C:\Windows\Temp\weasel_corpus_stress.dmp"
            if os.path.exists(dump):
                os.remove(dump)          # 清旧 dump：存在性检查才有效
            r = subprocess.run([PROCDUMP, "-accepteula", "-ma", str(pid[0]), dump],
                               capture_output=True, text=True, errors="replace")
            print("procdump rc=%s" % r.returncode)
            tail = (r.stdout or "") + (r.stderr or "")
            print("procdump tail:", " | ".join(tail.strip().splitlines()[-3:]))
            print("dump=%s size=%s" % (os.path.exists(dump),
                                       os.path.getsize(dump) if os.path.exists(dump) else 0))
            subprocess.run(["taskkill", "/F", "/IM", "WeaselServer.exe"],
                           capture_output=True)
            time.sleep(2)
            subprocess.Popen(["explorer.exe",
                              r"C:\Program Files\Rime\weasel-0.17.4\WeaselServer.exe"])
            time.sleep(3)
            hang = True
            break

    total = time.time() - t_all0
    print("done %.0fs codes=%d hang=%s" % (total, len(rep), hang))
    rep.sort(key=lambda x: -x[1])
    print("== top-15 expensive codes ==")
    for code, d in rep[:15]:
        print("  %8.3fs  %s" % (d, code))


if __name__ == "__main__":
    main()
