# -*- coding: utf-8 -*-
"""test_ctx_auto.py — 真机自动化验证：模拟键击全流程 + 事件日志断言

用 SendInput 合成真实键击（经输入法全流程：编码→候选→LLM 重排→上屏，
不绕过任何环节），读统一语料 rime_llm_corpus.tsv 的 W 行
（码/上文/引擎原序/评分窗/最终序/来源）做断言，验证 COM/UIA/历史
上文来源在 commit/退格/Ctrl+Z/点击/粘贴 等信号后的行为
（2026-09-12 信号层统一的回归工具；2026-11-17 起两版同读 corpus）。

用法（普通权限 python，目标应用由脚本拉起并置前）:
  python scripts\\test_ctx_auto.py                # 记事本全场景 s1-s6，双节奏
                                                  # （先 fast 后 slow 各一遍）
  python scripts\\test_ctx_auto.py -s s2,s4       # 指定场景
  python scripts\\test_ctx_auto.py --rhythm slow  # 只跑慢档（每键500ms/词间2s）
  python scripts\\test_ctx_auto.py --app wps      # WPS：先手动开好空文档置前，
                                                  # 脚本倒计时后开始（断言 src=com）

打字节奏（2026-11-11 用户定案：模拟打字测试必须两档都跑）：
  fast  = 每键 ~50ms、词间 50ms（时序设计标准的按键间隔下限——密集突发，
          考验不丢键/不串行）；slow = 每键 ~500ms、词间 2s（词间 >1s 空闲
          触发 TSF/上文采集的超时重读路径——调试窗口 v2026.11.11-2 缺陷
          即只在 slow 档现形）。classic = 旧缺省（键周期 ~140ms、词间
          ~0.45s）仅作对照。场景内的引擎等待（顶屏/kick 读链/粘贴浮窗）
          是引擎行为时序，不随节奏缩放。

前提:
  - 输入法处于中文态、方案为四码定长（拼读双拼）、无 CapsLock
  - 运行期间不要动键盘鼠标（合成输入与真实输入混流）
  - 本工具断言插件版日志（lua filter 路线）；当前 weasel 壳是哪版不影响，
    但 rime_llm.dll / lua 须为待验证版本

词表编码取自真实语料（旧 llm_training.txt / 现 rime_llm_corpus.tsv）。顶屏语义（2026-09-13 真机实测）：
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

# ── 打字节奏（2026-11-11 用户定案：模拟打字测试两档都要跑）─────────
# hold = 键按下→抬起；gap = 抬起→下一键（key_tap 路径，编辑键/导航键）
# tgap = type_code 的编码键间隔（down/up 各一次 tgap，键周期 = 2×tgap）
# word = 词间（type_word_and_wait 的 settle）
RHYTHMS = {
    "classic": {"hold": 0.03, "gap": 0.06, "tgap": 0.07, "word": 0.45},
    "fast":    {"hold": 0.02, "gap": 0.03, "tgap": 0.025, "word": 0.05},
    "slow":    {"hold": 0.20, "gap": 0.30, "tgap": 0.25, "word": 2.0},
}
RHY = RHYTHMS["classic"]
TAG = ""  # 断言名前缀（区分双节奏轮次，如 "[fast] S1 w2 ctx has w1"）


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


def key_tap(vk, ctrl=False, hold=None):
    """单击一键；ctrl=True 时以 Ctrl+key 组合发出（down→key→up 顺序真实）。
    hold/键间隔缺省按当前打字节奏（RHY）取值。"""
    if hold is None:
        hold = RHY["hold"]
    if ctrl:
        send_key(VK_CONTROL)
        time.sleep(hold)
    send_key(vk)
    time.sleep(hold)
    send_key(vk, up=True)
    if ctrl:
        time.sleep(0.02)
        send_key(VK_CONTROL, up=True)
    time.sleep(RHY["gap"])  # 键间隔（模拟真实节奏，输入法/引擎有时间处理）


def type_code(code, gap=None):
    if gap is None:
        gap = RHY["tgap"]
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


# ── 语料观察（两版统一 rime_llm_corpus.tsv 的 W 行）─────────────
# 2026-11-22 按方案分文件夹：语料在 llm_rerank\<方案>\ 下，旧位置（llm_rerank
# 根 / Rime 用户目录根）的混合历史文件仍可能被读到——统一汇总全部候选文件，
# 新行按 ts 排序（跨文件合并，"最新行"语义不变）。
def corpus_paths():
    base = os.path.join(os.environ.get("APPDATA", ""), "Rime")
    paths = [os.path.join(base, "rime_llm_corpus.tsv")]
    root = os.path.join(base, "llm_rerank")
    paths.append(os.path.join(root, "rime_llm_corpus.tsv"))
    try:
        for name in sorted(os.listdir(root)):
            p = os.path.join(root, name, "rime_llm_corpus.tsv")
            if os.path.isfile(p):
                paths.append(p)
    except OSError:
        pass
    return [p for p in paths if os.path.isfile(p)]


class EventsLog:
    """W 行 TSV: W id ts src code ctx engine_top window final tick status skip prep flags"""

    def __init__(self, paths):
        self.paths = list(paths)
        self.mark = {p: self._nlines(p) for p in self.paths}

    def _lines(self, path):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def _nlines(self, path):
        return len(self._lines(path))

    def _refresh(self):
        """测试中途出现的新方案文件（首次建目录）：从现在起算，不吃旧行。"""
        for p in corpus_paths():
            if p not in self.mark:
                self.mark[p] = self._nlines(p)
                self.paths.append(p)

    def _new(self):
        """本观察期新增行，跨文件按 ts 升序（新→旧 = reversed）。"""
        self._refresh()
        out = []
        for p in self.paths:
            lines = self._lines(p)
            m = self.mark.get(p, 0)
            if len(lines) < m:      # 文件被重写/截断
                m = 0
            for ln in lines[m:]:
                if not ln or ln.startswith("#"):
                    continue
                parts = ln.split("\t")
                ts = parts[2] if len(parts) > 2 else ""
                out.append((ts, ln))
        out.sort(key=lambda x: x[0])
        return [x[1] for x in out]

    def reset_marks(self):
        self._refresh()
        for p in self.paths:
            self.mark[p] = self._nlines(p)

    def wait_new(self, code, timeout=4.0):
        """等待本观察期之后出现的、码=code 的最新评分行。优先 ok 行（final
        序可用）；无 ok 时接受 skip=async_pending 行（2026-11-22 整句模式
        异步基建：评分移出按键路径后，挂账轮第 4 键写 skip 行、ok 行要等
        上屏/下一 query 收割回填（跨场景可达 5-10s）——pending 行的
        ctx/src/engine/window 字段挂账轮已写齐，足以承载上下文传播断言；
        result 为空，依赖 final 的断言（S6 ranks）仍只认 ok 行）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            # 反向取第一个 ok 或 async_pending 行 = 最新评分受理行
            #（ok 优先会吃到旧场景的 ok 行——pending 行更新却一致）
            for ln in reversed(self._new()):
                parts = ln.split("\t")
                if len(parts) < 14 or parts[0] != "W" or parts[4] != code:
                    continue
                if parts[10] == "ok":
                    return {"code": parts[4], "cands": parts[7],
                            "ctx": parts[5], "result": parts[8],
                            "src": parts[3], "id": parts[1],
                            "pending": False}
                if parts[10] == "skip" and parts[11] == "async_pending":
                    return {"code": parts[4], "cands": parts[7],
                            "ctx": parts[5], "result": "",
                            "src": parts[3], "id": parts[1],
                            "pending": True}
            time.sleep(0.15)
        return None

    def wait_commit(self, code=None, word=None, timeout=4.0):
        """等待本观察期之后的 C 提交行（按码或按词匹配，取最新一条）。
        C id ts ref word code rank_engine rank_window rank_final tick flags
        —— 拆分提交断言用：词 C 行 code 应干净且 ref 回挂评分 W；
        拆分前的病态形态 = word 带标点（"家庭，"）code="jstg," rank 全 -1。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in reversed(self._new()):
                parts = ln.split("\t")
                if len(parts) < 11 or parts[0] != "C":
                    continue
                if code is not None and parts[5] != code:
                    continue
                if word is not None and parts[4] != word:
                    continue
                return {"word": parts[4], "code": parts[5], "ref": parts[3],
                        "rank_engine": int(parts[6]),
                        "rank_window": int(parts[7]),
                        "rank_final": int(parts[8]), "tick": parts[9],
                        "id": parts[1]}
            time.sleep(0.15)
        return None


# ── 词表（真实语料反查：四码 → 首选上屏词）─────────────────────
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
    name = TAG + name
    if cond:
        PASS.append(name)
        print("  PASS  %s %s" % (name, detail))
    else:
        FAIL.append(name)
        print("  FAIL  %s %s" % (name, detail))


def warn(name, detail=""):
    name = TAG + name
    WARN.append(name)
    print("  WARN  %s %s" % (name, detail))


def type_word_and_wait(code, log, settle=None):
    """打一个四码词 → 等它自己的评分行。settle = 词间（打字节奏定案：
    fast 50ms / slow 2s / classic 0.45s）——评分行在第 4 键同步写出，
    wait_new 自行轮询，settle 只承担词间节奏与 kick 读链余量。"""
    if settle is None:
        settle = RHY["word"]
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
    # 第 2/3 词的上文应含前一上屏词。上屏词 = 前一词评分行结果序的首选
    # （LLM 重排可能不选词表首选：v3 常数下"发现"后 wkjl 合法改选 痢疾，
    # 2026-11-11 本机实测——断言跟随实际提交而非词表，避免环境态漂移误报。
    # w1 冷启动无评分行，其上屏词是方案原始首选，w2 断言仍用词表的 发现）
    # 上屏词 = 前一词 C 提交行的实际词（2026-11-22 异步基建：评分行可能是
    # pending（result 空）；wkjl 由 krng 首键顶屏，断言时 C 行已写——跟随
    # 实际提交而非词表，避免环境态漂移误报；C 行无则回退 result 首选）
    w2_committed = "立即"
    c_w2 = log.wait_commit(code="wkjl", timeout=2.0)
    if c_w2 is not None:
        w2_committed = c_w2["word"]
    elif (rows["wkjl"] or {}).get("result"):
        w2_committed = rows["wkjl"]["result"].split(",")[0]
    check(ctx_has(rows["wkjl"], "发现"), "S1 w2 ctx has w1",
          "ctx=…%s" % (rows["wkjl"] or {}).get("ctx", "")[-12:])
    check(ctx_has(rows["krng"], w2_committed),
          "S1 w3 ctx has w2",
          "w2=%s ctx=…%s" % (w2_committed,
                             (rows["krng"] or {}).get("ctx", "")[-12:]))


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
    type_word_and_wait("fnxo", log)   # 编辑后首词：源码版可能无 AI 无行
    row = type_word_and_wait("lujl", log)  # 第二词断言（fnxo 已由本词
    if row is None:                      # 首键顶屏，ctx 含 fnxo）
        check(False, "S2 w5 row", "no event line")
        return
    check("延迟" not in row["ctx"], "S2 w5 ctx excludes deleted w3",
          "ctx=…%s" % row["ctx"][-15:])
    check(ctx_has(row, "立即") or ctx_has(row, "分析"), "S2 w5 ctx has w2/w4",
          "ctx=…%s" % row["ctx"][-15:])
    check(row["src"] in ("uia", "com", "tsf"), "S2 w5 src fresh-snapshot",
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
    type_word_and_wait("krng", log)
    row = type_word_and_wait("lujl", log)
    if row is None:
        check(False, "S3 w4 row", "no event line")
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
    type_word_and_wait("fnxo", log)
    row = type_word_and_wait("lujl", log)
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
    type_word_and_wait("cofs", log)
    row = type_word_and_wait("lujl", log)
    if row is None:
        check(False, "S5 w2 row", "no event line")
        return
    check("abcdefghpasteok" in row["ctx"].replace("…", "")
          or "abcdefghpasteok出发" in row["ctx"].replace("…", ""),
          "S5 ctx has pasted tail",
          "ctx=…%s" % row["ctx"][-24:])


def s6_punct_split(log):
    """S6 拆分提交（2026-11-20 punctuator patch 回归）：4 码词 + 标点键 →
    词与标点分两次提交。断言四件：
    1) 词 C 行码干净（"jstg" 非 "jstg,"——拆分前病态形态是 word 带标点
       的复合提交）；
    2) 词 C 行 ref 回挂评分 W 且 rank 三字段非 -1（ref 断链=拆分前病灶）；
    3) 标点独立一条 C 行（word="，"），ref 空（评分窗已被词提交消费）；
    4) 端到端文本与并提交时代一致：后续词的上文含「词+标点」。
    先导词 fsxd 建立上文——clear_doc 后首词是冷启动（空上文不重排，W 为
    skip=ctx_empty 空表，rank 全 -1 属正确行为，S1 同理）；jstg 带上文
    真正进评分窗才有 rank 可断言。上屏词跟随 C 行实际提交（LLM 重排可
    合法改选，同 S1 定案）。"""
    print("[S6] punct split: jstg + comma -> two commits")
    type_word_and_wait("fsxd", log)     # 先导词：建立上文（冷启动豁免）
    type_code("jstg")
    time.sleep(max(0.3, RHY["word"]))
    key_tap(0xBC)                       # VK_OEM_COMMA：词确认 + 标点独立提交
    c_word = log.wait_commit(code="jstg", timeout=4.0)
    if c_word is None:
        check(False, "S6 word commit row", "no C row (code=jstg)")
        return
    check(c_word["code"] == "jstg" and "，" not in c_word["word"],
          "S6 word commit clean", "word=%s code=%s"
          % (c_word["word"], c_word["code"]))
    check(c_word["ref"] != "", "S6 word ref linked to scoring W",
          "ref=%s" % c_word["ref"][-14:])
    check(c_word["rank_engine"] >= 1 and c_word["rank_window"] >= 1
          and c_word["rank_final"] >= 1, "S6 word ranks populated",
          "rE=%d rW=%d rF=%d" % (c_word["rank_engine"],
                                 c_word["rank_window"],
                                 c_word["rank_final"]))
    c_punct = log.wait_commit(word="，", timeout=3.0)
    if c_punct is None:
        check(False, "S6 punct commit row", "no C row (word=，)")
        return
    check(c_punct["ref"] == "", "S6 punct commit standalone",
          "ref=%s code=%s" % (c_punct["ref"], c_punct["code"]))
    row = type_word_and_wait("fsxd", log)   # 后续词：上文应含「词+标点」
    if row is None:
        check(False, "S6 follow-up row", "no event line")
        return
    check(ctx_has(row, c_word["word"] + "，"), "S6 doc text has word+punct",
          "ctx=…%s" % row["ctx"][-15:])


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
             "s4": s4_click, "s5": s5_paste, "s6": s6_punct_split}


def main():
    global RHY, TAG
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--scenarios", default="s1,s2,s3,s4,s5,s6")
    ap.add_argument("--app", default="notepad", choices=["notepad", "wps"])
    ap.add_argument("--rhythm", default="both",
                    choices=["classic", "fast", "slow", "both"],
                    help="打字节奏（2026-11-11 定案：模拟打字两档都要跑，"
                         "both=先 fast 后 slow 各一遍场景表，为缺省）："
                         "fast=每键50ms/词间50ms；slow=每键500ms/词间2s；"
                         "classic=旧缺省节奏仅作对照")
    ap.add_argument("--engine", default="plugin", choices=["plugin", "source"],
                    help="plugin/source 均读统一语料 rime_llm_corpus.tsv 的 W 行"
                         "（2026-11-17 记录重构）")
    args = ap.parse_args()

    logname = "rime_llm_corpus.tsv"
    paths = corpus_paths()
    if not paths:
        print("events log not found under %s\\Rime\\llm_rerank"
              % os.environ.get("APPDATA", ""))
        sys.exit(2)
    print("[pre] corpus files: %d（按方案分目录，汇总读取）" % len(paths))
    log = EventsLog(paths)

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
    rhythm_list = ["fast", "slow"] if args.rhythm == "both" else [args.rhythm]
    for rh in rhythm_list:
        RHY = RHYTHMS[rh]
        TAG = "[%s] " % rh if len(rhythm_list) > 1 else ""
        print("==> rhythm=%s (key %dms, word %dms)" % (
            rh, int((RHY["hold"] + RHY["gap"]) * 1000),
            int(RHY["word"] * 1000)))
        for name in names:
            fn = SCENARIOS.get(name)
            if not fn:
                print("unknown scenario %s" % name)
                continue
            if not ensure_foreground(hwnd, office=(args.app == "wps")):
                print("ABORT: target window not foreground (%s)" % name)
                sys.exit(2)
            clear_doc()
            # 场景起点推进观察位：同名码在上一场景/上一节奏轮的旧行不作数
            #（wait_new 反向找最新行，不推进的话陈旧行会假 PASS——双节奏
            # 同码跑两遍放大了这一既有隐患）
            log.reset_marks()
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
