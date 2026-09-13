# -*- coding: utf-8 -*-
"""test_ctx_auto.py — 真机自动化验证：模拟键击全流程 + 事件日志断言

用 SendInput 合成真实键击（经输入法全流程：编码→候选→LLM 重排→上屏，
不绕过任何环节），配合插件版 rime_llm_events.txt 每行的
`时间|计数|码|候选|上文尾15字节|结果|延迟|来源` 做断言，验证
COM/UIA/历史 上文来源在 commit/退格/Ctrl+Z/点击/粘贴 等信号后的行为
（2026-09-12 信号层统一的回归工具）。

用法（普通权限 python，目标应用由脚本拉起并置前）:
  python scripts\\test_ctx_auto.py                # 记事本全场景 s1-s5
  python scripts\\test_ctx_auto.py -s s2,s4       # 指定场景
  python scripts\\test_ctx_auto.py --app wps      # WPS：先手动开好空文档置前，
                                                  # 脚本倒计时后开始（断言 src=com）

前提:
  - 输入法处于中文态、方案为四码定长（拼读双拼）、无 CapsLock
  - 运行期间不要动键盘鼠标（合成输入与真实输入混流）
  - 本工具断言插件版日志（lua filter 路线）；当前 weasel 壳是哪版不影响，
    但 rime_llm.dll / lua 须为待验证版本

词表编码取自 llm_training.txt 真实记录。顶屏语义（2026-09-13 真机实测）：
满 4 码**不**自动顶屏（auto_select:false）——上屏由**下一词首键（第 5 键，
连打自然触发）或空格**触发；本工具连打场景天然满足，编辑前场景（退格/
撤销/点击）必须先空格顶屏，否则编辑键作用在残留编码上而非文档（虚 PASS）。
INPUT 结构铁律：union 必须含 MOUSEINPUT（x64 上 sizeof=40）——简化成只
KEYBDINPUT(32B) 会被 SendInput 静默拒绝返回 0，全部键击无效（排查极难）。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

# ── Win32 ──────────────────────────────────────────────────────
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

INPUT_KEYBOARD = 1
INPUT_MOUSE = 0
KEYEVENTF_KEYUP = 0x0002
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
VK_CONTROL = 0x11
VK_BACK = 0x08


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.POINTER(wt.ULONG))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]
    _fields_ = [("type", wt.DWORD), ("union", _U)]


def send_key(vk, up=False):
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.union.ki = KEYBDINPUT(vk, 0, KEYEVENTF_KEYUP if up else 0, 0, None)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def key_tap(vk, ctrl=False, hold=0.03):
    """单击一键；ctrl=True 时以 Ctrl+key 组合发出（down→key→up 顺序真实）。"""
    if ctrl:
        send_key(VK_CONTROL)
        time.sleep(hold)
    send_key(vk)
    time.sleep(hold)
    send_key(vk, up=True)
    if ctrl:
        time.sleep(0.02)
        send_key(VK_CONTROL, up=True)
    time.sleep(0.06)  # 键间隔（模拟真实节奏，输入法/引擎有时间处理）


def type_code(code, gap=0.07):
    for ch in code:
        send_key(ord(ch.upper()))
        time.sleep(gap)
        send_key(ord(ch.upper()), up=True)
        time.sleep(gap)


def mouse_click(x, y):
    user32.SetCursorPos(x, y)
    time.sleep(0.15)
    inp = INPUT()
    inp.type = INPUT_MOUSE
    inp.union.mi = MOUSEINPUT(0, 0, 0, MOUSEEVENTF_LEFTDOWN, 0, None)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    time.sleep(0.06)
    inp.union.mi = MOUSEINPUT(0, 0, 0, MOUSEEVENTF_LEFTUP, 0, None)
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    time.sleep(0.1)


def set_clipboard_text(text):
    CF_UNICODETEXT = 13
    GMEM_MOVEABLE = 0x0002
    data = text.encode("utf-16-le") + b"\x00\x00"
    # 64 位下 GlobalAlloc/GlobalLock 必须显式 restype=c_void_p——默认
    # int 返回截断高 32 位地址 → access violation（首版真机踩坑）
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalLock.restype = ctypes.c_void_p
    h = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    p = kernel32.GlobalLock(ctypes.c_void_p(h))
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(ctypes.c_void_p(h))
    user32.OpenClipboard(0)
    user32.EmptyClipboard()
    user32.SetClipboardData(CF_UNICODETEXT, ctypes.c_void_p(h))
    user32.CloseClipboard()


def find_window_by_pid(pid, timeout=8.0):
    """枚举可见顶层窗口找属于 pid 的主窗口（WPS 通用）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        result = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(hwnd, _):
            if user32.IsWindowVisible(hwnd):
                wpid = wt.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
                if wpid.value == pid:
                    result.append(hwnd)
            return True

        user32.EnumWindows(cb, 0)
        if result:
            return result[0]
        time.sleep(0.3)
    return None


def find_window_by_class(cls, timeout=8.0):
    """按窗口类名找可见顶层窗口（Win11 记事本=UWP 重定向，notepad.exe
    的 Popen PID 立即退出，PID 法失效；主窗口类名仍为 Notepad）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(hwnd, _):
            buf = ctypes.create_unicode_buffer(64)
            if user32.IsWindowVisible(hwnd) and user32.GetClassNameW(
                    hwnd, buf, 64) and buf.value == cls:
                found.append(hwnd)
            return True

        user32.EnumWindows(cb, 0)
        if found:
            return found[-1]  # 多个时取最新
        time.sleep(0.3)
    return None


# ── 事件日志观察（插件版 rime_llm_events.txt）───────────────────
class EventsLog:
    """行格式: 时间|计数|码|候选|上文尾(…前缀=截断)|结果|延迟ms|来源"""

    def __init__(self, path):
        self.path = path
        self.mark = self._nlines()

    def _lines(self):
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def _nlines(self):
        return len(self._lines())

    def wait_new(self, code, timeout=4.0):
        """等待本观察期之后出现的、码=code 的最新评分行，返回字段 dict。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in reversed(self._lines()[self.mark:]):
                parts = ln.split("|")
                if len(parts) >= 8 and parts[2] == code:
                    return {"code": parts[2], "cands": parts[3],
                            "ctx": parts[4], "result": parts[5],
                            "ms": parts[6], "src": parts[7]}
            time.sleep(0.15)
        return None


# ── 词表（llm_training.txt 反查：四码 → 首选上屏词）─────────────
WORDS = {
    "fsxd": "发现",
    "wkjl": "立即",
    "krng": "可能",
    "aljk": "实际",
    "cofs": "出发",
    "inuz": "因为",
    "iiol": "延迟",
    "fnxo": "分析",
    "lujl": "逻辑",
}

# ── 断言框架 ───────────────────────────────────────────────────
PASS, FAIL, WARN = [], [], []


def check(cond, name, detail=""):
    if cond:
        PASS.append(name)
        print("  PASS  %s %s" % (name, detail))
    else:
        FAIL.append(name)
        print("  FAIL  %s %s" % (name, detail))


def warn(name, detail=""):
    WARN.append(name)
    print("  WARN  %s %s" % (name, detail))


def type_word_and_wait(code, log, settle=0.45):
    """打一个四码词 → 等它自己的评分行。settle 给 kick 读链留时间。"""
    sub = log.mark
    type_code(code)
    time.sleep(settle)
    row = log.wait_new(code, timeout=3.0)
    return row


def ctx_has(row, word):
    return row is not None and word in row["ctx"]


def clear_doc():
    """场景隔离：Esc 清残留编码（上场景末词可能满 4 码未顶屏）→
    Ctrl+A 全选 → BS 删除。清空后光标前无文本 → 首词评分行 ctx 空 +
    src=rime 是正确行为，不是错误（首版无 Esc：残留编码令 Ctrl+A 部分
    失效，前场景文本泄漏进本场景断言窗）。"""
    key_tap(0x1B)
    key_tap(ord("A"), ctrl=True)
    key_tap(VK_BACK)
    time.sleep(0.4)


# ── 场景 ───────────────────────────────────────────────────────
def s1_base(log):
    """S1 基线：连打 3 词。首词=冷启动（ctx 空 + src=rime 属正确行为），
    第 2/3 词应为真文来源（uia/com）且上文含前一上屏词。"""
    print("[S1] base: 3 words commit")
    rows = {}
    for code in ("fsxd", "wkjl", "krng"):
        rows[code] = type_word_and_wait(code, log)
    if rows["fsxd"] is None:
        warn("S1 w1 no row (source-engine cold start: empty ctx -> "
             "no inference, no event line)")
    else:
        check(True, "S1 w1 scored", "src=%s" % rows["fsxd"]["src"])
    for code, word in (("wkjl", "立即"), ("krng", "可能")):
        row = rows[code]
        if row is None:
            check(False, "S1 row %s" % code, "no event line")
            continue
        check(row["src"] in ("uia", "com", "tsf"), "S1 %s src" % code,
              "src=%s ctx=…%s" % (row["src"], row["ctx"][-12:]))
    # 第 2/3 词的上文应含前一上屏词
    check(ctx_has(rows["wkjl"], "发现"), "S1 w2 ctx has w1",
          "ctx=…%s" % (rows["wkjl"] or {}).get("ctx", "")[-12:])
    check(ctx_has(rows["krng"], "立即"), "S1 w3 ctx has w2",
          "ctx=…%s" % (rows["krng"] or {}).get("ctx", "")[-12:])


def s2_backspace(log):
    """S2 退格（核心回归）：W1 W2 W3 → 空格顶屏 W3 → BS×2（删文档两字）→ W4。
    W4 上文应含 W2 且不含 W3（旧版此处旧快照冒充，含已删词）。
    注意：满 4 码不自动顶屏（auto_select:false）——上屏由下一词首键或空格
    触发；编辑键前必须先空格顶屏，否则 BS 删的是编码不是文档字（首版
    真机实测教训：BS 删编码导致 W3 从未上屏，断言虚 PASS）。"""
    print("[S2] backspace: w1 w2 w3, SP, BSx2, w4")
    for code in ("fsxd", "wkjl"):
        type_word_and_wait(code, log)
    type_code("iiol")
    key_tap(0x20)          # 空格顶屏"延迟"
    time.sleep(0.45)
    key_tap(VK_BACK)
    key_tap(VK_BACK)       # 删两字词"延迟"
    time.sleep(0.5)        # edit_reset kick → Sleep(50) → 读链
    row = type_word_and_wait("fnxo", log)
    if row is None:
        check(False, "S2 w4 row", "no event line")
        return
    check("延迟" not in row["ctx"], "S2 w4 ctx excludes deleted w3",
          "ctx=…%s" % row["ctx"][-15:])
    check(ctx_has(row, "立即"), "S2 w4 ctx has w2",
          "ctx=…%s" % row["ctx"][-15:])
    check(row["src"] in ("uia", "com", "tsf"), "S2 w4 src fresh-snapshot",
          "src=%s" % row["src"])


def s3_ctrlz(log):
    """S3 撤销：W1 W2 → 空格顶屏 W2 → Ctrl+Z（撤销 W2）→ W3；上文含 W1
    不含 W2。顶屏语义同 S2（先空格再撤销）。"""
    print("[S3] ctrl+z undo")
    type_word_and_wait("fsxd", log)
    type_code("wkjl")
    key_tap(0x20)          # 顶屏"立即"
    time.sleep(0.45)
    key_tap(ord("Z"), ctrl=True)
    time.sleep(0.5)
    row = type_word_and_wait("krng", log)
    if row is None:
        check(False, "S3 w3 row", "no event line")
        return
    check("立即" not in row["ctx"], "S3 ctx excludes undone w2",
          "ctx=…%s" % row["ctx"][-15:])
    check(ctx_has(row, "发现"), "S3 ctx has w1",
          "ctx=…%s" % row["ctx"][-15:])


def s4_click(log):
    """S4 光标移动：3 词（末词空格顶屏）→ Home 键（光标跳行首，导航键
    = edit_reset 信号）→ W4；W4 上文不应含任何词（光标前文本≈空）。
    首版用鼠标点击移光标——Win11 记事本文本区坐标随布局/缩放漂移
    （标签栏高度、行高、DPI），点击落空白区光标不动，真机三轮踩坑后
    改用 Home 键（同一信号链 is_nav_ish_key → edit_reset，确定性高）。
    鼠标点击路径的检测（click_happened）由 ime_heal 的复位点击间接
    覆盖（点击后探针评分即点击→kick→新快照链路）。"""
    print("[S4] caret move (Home) ")
    for code in ("fsxd", "wkjl"):
        type_word_and_wait(code, log)
    type_code("krng")
    key_tap(0x20)          # 顶屏"可能"（否则编辑键作用在残留编码上）
    time.sleep(0.8)        # 顶屏偶发慢：0.45s 时 Home 曾落进残留编码
    key_tap(0x24)          # Home：光标跳行首
    time.sleep(0.5)
    row = type_word_and_wait("fnxo", log)
    if row is None:
        check(False, "S4 w4 row", "no event line")
        return
    ok = ("立即" not in row["ctx"]) and ("可能" not in row["ctx"])
    check(ok, "S4 ctx excludes pre-move words",
          "ctx=…%s" % row["ctx"][-15:])


def s5_paste(log):
    """S5 粘贴：剪贴板置英文短语 → Ctrl+V → Esc 关浮窗 → 1s 恢复 → 打词；
    上文含短语尾部（lua 侧上文去空白 → 断言词用无空格形态）。
    WPS 粘贴三轮真机踩坑：Ctrl+V 松 Ctrl 弹"粘贴选项"浮窗吃后续键；
    浮窗 2.5s 不自灭；Esc 后焦点/TSF 会话恢复有竞态（0.3s 时首键直出
    键序错乱 ofsw）；Shift+Insert 的 Shift 释放被 weasel 当中英切换
    （英文态直出）——定案 Ctrl+V + Esc + 1.0s 恢复。"""
    print("[S5] paste (Ctrl+V + Esc + settle)")
    set_clipboard_text("abcdefgh paste ok")
    time.sleep(0.2)
    key_tap(ord("V"), ctrl=True)
    time.sleep(0.3)
    key_tap(0x1B)          # 关"粘贴选项"浮窗
    time.sleep(1.0)        # 浮窗关闭后焦点/TSF 会话恢复窗口
    row = type_word_and_wait("cofs", log)
    if row is None:
        check(False, "S5 w row", "no event line")
        return
    check("abcdefghpasteok" in row["ctx"].replace("…", ""),
          "S5 ctx has pasted tail",
          "ctx=…%s" % row["ctx"][-18:])


def find_office_window(timeout=3.0):
    """按类名找 Office 系前台候选窗口（WPS 文字/演示/表格）。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for cls in ("OpusApp", "PP12FrameClass", "XLMAIN"):
            found = []

            @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
            def cb_cls(hwnd, _, _cls=cls):
                b = ctypes.create_unicode_buffer(64)
                if user32.IsWindowVisible(hwnd) and user32.GetClassNameW(
                        hwnd, b, 64) and b.value == _cls:
                    found.append(hwnd)
                return True

            user32.EnumWindows(cb_cls, 0)
            if found:
                return found[-1]
        time.sleep(0.3)
    return None


def bring_foreground(hwnd):
    """把 hwnd 切回前台。终端跑命令会抢前台（用户真机反馈）——脚本进程
    非前台时 SetForegroundWindow 被系统前台锁拒绝，先用一次 Alt 键按下
    释放解锁（经典 hack：合成 Alt 后本进程获得 SetForegroundWindow 权）。"""
    send_key(0x12)          # Alt down
    time.sleep(0.05)
    send_key(0x12, up=True)
    time.sleep(0.05)
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    return user32.GetForegroundWindow() == hwnd


def ensure_foreground(hwnd=None, office=False):
    """打字前的安全门：目标窗口必须前台，否则重试；仍失败则中止——
    绝不向非目标窗口发合成键击。office=True 用于 WPS（前台类名校验 +
    偏离时自动找 Office 窗口切回——2026-09-14 用户真机反馈：终端跑命令
    即抢前台，原版只校验不切换直接 ABORT）。"""
    for _ in range(10):
        fg = user32.GetForegroundWindow()
        if hwnd is not None and fg == hwnd:
            return True
        if office:
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(fg, cls, 64)
            if cls.value in ("OpusApp", "PP12FrameClass", "XLMAIN"):
                return True
            oh = find_office_window(timeout=1.0)
            if oh:
                bring_foreground(oh)
                continue
        if hwnd is not None:
            user32.SetForegroundWindow(hwnd)
        time.sleep(0.3)
    return False


def ime_heal(hwnd, ev_path):
    """环境自愈（2026-09-13 深夜真机教训全收录）：
    1. 点击客户区复位 caret（编辑焦点丢失时注入键全部无效）
    2. wkjl 探针：满码评分行出现 = weasel 活 + 中文态（一石二鸟——
       英文态/其他 IME/键路由坏都测得出）
    3. 不行则 Win+Space 切输入法再试（最多 8 轮；列表循环位置不定，
       小狼毫通常在 2-3 轮内回到）
    4. 中途若输入法选择器滞留抢前台（类名异常）→ Esc 关闭
    注意：绝不能 taskkill WeaselServer——会丢各应用的输入法绑定（Windows
    自动切到其他 TIP），恢复只能逐窗 Win+Space，且部分应用要重启。"""
    def nlines():
        return len(open(ev_path, encoding="utf-8", errors="replace")
                   .read().splitlines())
    def probe():
        n0 = nlines()
        for ch in "wkjl":
            send_key(ord(ch.upper())); time.sleep(0.09)
            send_key(ord(ch.upper()), up=True); time.sleep(0.09)
        time.sleep(0.7)
        key_tap(0x1B)  # Esc 清残留编码
        time.sleep(0.3)
        return nlines() > n0
    for i in range(8):
        if hwnd:
            pt = wt.POINT(200, 150)
            user32.ClientToScreen(hwnd, ctypes.byref(pt))
            mouse_click(pt.x, pt.y)
        if probe():
            print("ime ok (round %d)" % i)
            return True
        print("ime probe failed (round %d), Win+Space ..." % i)
        send_key(0x5B); time.sleep(0.12); send_key(0x20); time.sleep(0.12)
        send_key(0x20, up=True); time.sleep(0.12)
        send_key(0x5B, up=True); time.sleep(1.5)
        cls = ctypes.create_unicode_buffer(64)
        fg = user32.GetForegroundWindow()
        user32.GetClassNameW(fg, cls, 64)
        if hwnd and fg != hwnd and cls.value == "Notepad":
            # 多记事本窗口间漂移（类名同、句柄不同）——精确拉回目标句柄
            user32.SetForegroundWindow(hwnd)
            time.sleep(0.4)
        elif hwnd and cls.value != "Notepad":
            key_tap(0x1B)
            time.sleep(0.3)
            user32.SetForegroundWindow(hwnd)
            time.sleep(0.4)
    return False


# ── 主流程 ─────────────────────────────────────────────────────
SCENARIOS = {"s1": s1_base, "s2": s2_backspace, "s3": s3_ctrlz,
             "s4": s4_click, "s5": s5_paste}


def main():
    global hwnd_holder
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--scenarios", default="s1,s2,s3,s4,s5")
    ap.add_argument("--app", default="notepad", choices=["notepad", "wps"])
    ap.add_argument("--engine", default="plugin", choices=["plugin", "source"],
                    help="plugin=lua 版 events 日志（rime_llm_events.txt）；"
                         "source=原生组件（rime_llm_filter_log.txt，event_log"
                         " 行格式与插件版一致，2026-09-14 核对 llm_filter.cc"
                         ":1136）")
    args = ap.parse_args()

    logname = ("rime_llm_filter_log.txt" if args.engine == "source"
               else "rime_llm_events.txt")
    events = os.path.join(os.environ["APPDATA"], "Rime", logname)
    if not os.path.exists(events):
        print("events log not found: %s" % events)
        sys.exit(2)
    log = EventsLog(events)

    hwnd = None
    if args.app == "notepad":
        # 复用现有记事本（clear_doc 会清掉旧内容）；没有才开新的。
        # 首版 taskkill /F 强杀被 Win11 记事本当崩溃——下次 Popen 恢复
        # 全部旧会话标签（真机实测一次弹出 6 个窗口），多窗口间前台/
        # 输入法绑定漂移令探针 8 轮全灭——绝不能强杀。
        hwnd = find_window_by_class("Notepad", timeout=1.5)
        if not hwnd:
            print("launching notepad ...")
            subprocess.Popen(["notepad.exe"])
            hwnd = find_window_by_class("Notepad")
        if not hwnd:
            print("ERROR: notepad window not found")
            sys.exit(2)
    else:
        # wps：终端跑命令会抢前台（用户真机反馈）——ime_heal 探针打字前
        # 必须先把 Office 窗口切回，否则打码进终端假成功
        oh = find_office_window()
        if not oh:
            print("ERROR: no WPS/Office window found - open a document first")
            sys.exit(2)
        bring_foreground(oh)
    user32.SetForegroundWindow(hwnd) if hwnd else None
    time.sleep(0.6)

    if not ime_heal(hwnd, events):
        print("ERROR: IME not usable after heal attempts - "
              "check tray input method is 小狼毫 Chinese mode, then rerun")
        sys.exit(2)

    print("==> keep hands off keyboard/mouse! starting in 3s ...")
    time.sleep(3)

    # 全选清空，保证起点干净（内容是脚本自己打的测试文本）
    key_tap(ord("A"), ctrl=True)
    key_tap(VK_BACK)

    names = [s.strip() for s in args.scenarios.split(",") if s.strip()]
    for name in names:
        fn = SCENARIOS.get(name)
        if not fn:
            print("unknown scenario %s" % name)
            continue
        if not ensure_foreground(hwnd, office=(args.app == "wps")):
            print("ABORT: target window not foreground (%s)" % name)
            sys.exit(2)
        clear_doc()
        try:
            fn(log)
        except Exception as e:
            FAIL.append("%s exception" % name)
            print("  EXC   %s: %r" % (name, e))
        time.sleep(0.6)

    total = len(PASS) + len(FAIL)
    print("\n== %d/%d passed, %d warn ==" % (len(PASS), total, len(WARN)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
