# test_luna_auto.py — 全拼（朙月拼音）整句模式·分段开发自动化测试
# 第①段：无组句配置（poet max_sentences 默认 1）+ 逐词选词。
# 用户定案三段路线（2026-11-22）：①无组句逐词 → ②ngram 组句+LLM 重排
# 句子（只显示一条）→ ③词串。本脚本每段复跑，按日志断言行为：
#
#   L1 逐词两连（jintian→空格, shijian→空格）：
#      W 行 ok（flags=async）+ 上文传播（w2 ctx 含 w1 上屏词）
#      + debug.txt D 行 CE 数值（挂账回填的逐候选 CE——2026-11-22 补）
#   L2 长句连打（daziceshijinxingzhong，不上屏）：
#      第①段预期 = 末键 skip|full_len_lt2（满覆盖组 = poet 句候选 1 个
#      < 2 整窗跳过）；第②段（ngram 组句多候选进窗）落地后此断言反转为
#      ok + 句候选评分
#   L3 单音节 2 码（ni）：满覆盖单字组 ≥2 → 评分 + CE
#
# 与金标 test_ctx_auto.py 的分工：金标 = pdsp 双拼全场景上下文信号回归；
# 本脚本 = luna 全拼整句模式分段开发的日志正确性验证。当前方案须为
# luna_pinyin（语料最新 H 行校验，不符则提示手动切换）。
import ctypes
import os
import sys
import time
import subprocess

APPDATA = os.environ.get("APPDATA", "")
CORPUS = os.path.join(APPDATA, r"Rime\llm_rerank\rime_llm_corpus.tsv")
DEBUGF = os.path.join(APPDATA, r"Rime\llm_rerank\rime_llm_debug.txt")
TAG = ""

PASS, FAIL, WARN = [], [], []


def check(cond, name, detail=""):
    print(("  PASS  [%s] %s %s" if cond else "  FAIL  [%s] %s %s")
          % (TAG, name, detail if isinstance(detail, str) else ""))
    (PASS if cond else FAIL).append(name)


def warn(name, detail=""):
    print("  WARN  [%s] %s %s" % (TAG, name, detail))
    WARN.append(name)


# ── SendInput（与金标同构：INPUT union 必须含 MOUSEINPUT）──────────
u = ctypes.windll.user32
VK_BACK, VK_HOME, VK_ESCAPE = 0x08, 0x24, 0x1B


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
    inp = INPUT()
    inp.type = 1
    inp.ki = KEYBDINPUT(vk, 0, 2 if up else 0, 0, None)
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def key_tap(vk):
    send_key(vk)
    time.sleep(0.02)
    send_key(vk, up=True)


def type_code(code, gap=0.5):
    """全拼逐键：单节奏 500ms/键（慢节奏，挂账/回填窗口充分）。"""
    for ch in code:
        vk = ord(ch.upper())
        key_tap(vk)
        time.sleep(gap)


# ── notepad 窗口 ─────────────────────────────────────────────

def key_tap_win(vk):
    """Win+vk 组合（TSF 重连用：Win+Space 切走再切回）。"""
    down = INPUT(); down.type = 1
    down.ki = KEYBDINPUT(0x5B, 0, 0, 0, None)
    k = INPUT(); k.type = 1
    k.ki = KEYBDINPUT(vk, 0, 0, 0, None)
    seq = (INPUT * 2)(down, k)
    u.SendInput(2, seq, ctypes.sizeof(INPUT))
    time.sleep(0.07)
    up = INPUT(); up.type = 1
    up.ki = KEYBDINPUT(0x5B, 0, 2, 0, None)
    k2 = INPUT(); k2.type = 1
    k2.ki = KEYBDINPUT(vk, 0, 2, 0, None)
    seq2 = (INPUT * 2)(up, k2)
    u.SendInput(2, seq2, ctypes.sizeof(INPUT))


def ensure_notepad():
    subprocess.Popen(["notepad.exe"])
    time.sleep(1.5)
    hwnd = u.FindWindowW("Notepad", None)
    if not hwnd:
        print("ABORT: notepad not found")
        sys.exit(2)
    u.SetForegroundWindow(hwnd)
    time.sleep(0.5)
    return hwnd


def clear_doc():
    key_tap(VK_ESCAPE)
    time.sleep(0.2)
    # Ctrl+A 全选（CTRL=0x11 down → A down/up → CTRL up）+ BS 删除
    ctrl = INPUT(); ctrl.type = 1
    ctrl.ki = KEYBDINPUT(0x11, 0, 0, 0, None)
    a = INPUT(); a.type = 1
    a.ki = KEYBDINPUT(0x41, 0, 0, 0, None)
    a_up = INPUT(); a_up.type = 1
    a_up.ki = KEYBDINPUT(0x41, 0, 2, 0, None)
    ctrl_up = INPUT(); ctrl_up.type = 1
    ctrl_up.ki = KEYBDINPUT(0x11, 0, 2, 0, None)
    seq = (INPUT * 4)(ctrl, a, a_up, ctrl_up)
    u.SendInput(4, seq, ctypes.sizeof(INPUT))
    time.sleep(0.05)
    key_tap(VK_BACK)
    time.sleep(0.3)


# ── 语料观察（wait_new 兼容整句模式异步挂账行）──────────────
class CorpusLog:
    def __init__(self):
        self.mark = self._nlines()

    def _lines(self):
        try:
            with open(CORPUS, "r", encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def _nlines(self):
        return len(self._lines())

    def wait_new(self, code, timeout=4.0):
        """该码的最新评分受理行：优先 ok，兼容 skip|async_pending。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in reversed(self._lines()[self.mark:]):
                p = ln.split("\t")
                if len(p) < 14 or p[0] != "W" or p[4] != code:
                    continue
                if p[10] == "ok":
                    return {"ctx": p[5], "cands": p[7], "result": p[8],
                            "flags": p[13], "id": p[1], "pending": False}
                if p[10] == "skip" and p[11] == "async_pending":
                    return {"ctx": p[5], "cands": p[7], "result": "",
                            "flags": p[13], "id": p[1], "pending": True}
            time.sleep(0.15)
        return None

    def wait_skip(self, code, reason, timeout=4.0):
        """该码的最新 skip 行且 skip=reason（full_len_lt2 等预期性跳过）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in reversed(self._lines()[self.mark:]):
                p = ln.split("\t")
                if len(p) < 14 or p[0] != "W" or p[4] != code:
                    continue
                if p[10] == "skip" and p[11] == reason:
                    return {"id": p[1]}
            time.sleep(0.15)
        return None

    def wait_commit(self, code=None, timeout=4.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in reversed(self._lines()[self.mark:]):
                p = ln.split("\t")
                if len(p) < 11 or p[0] != "C":
                    continue
                if code is not None and p[5] != code:
                    continue
                return {"word": p[4], "code": p[5], "ref": p[3]}
            time.sleep(0.15)
        return None


class DebugLog:
    """debug.txt 的 P/D 行（debug_fusion 开启才写）。"""

    def __init__(self):
        self.mark = self._nlines()

    def _lines(self):
        try:
            with open(DEBUGF, "r", encoding="utf-8", errors="replace") as f:
                return f.read().splitlines()
        except OSError:
            return []

    def _nlines(self):
        return len(self._lines())

    def wait_d(self, wid, timeout=4.0):
        """wid 的 D 行列表 [(rank, ce, ce1..ce3, finrank)]；ce 数值 =
        挂账回填/同步评分的逐候选加权分（负值域，哨兵 = -1e10）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            rows = []
            for ln in self._lines()[self.mark:]:
                if not ln.startswith(wid + "|D|"):
                    continue
                f = dict(kv.split("=", 1) for kv in ln.split("|")[3:]
                         if "=" in kv)
                try:
                    rows.append((int(f.get("finrank", -1)),
                                 float(f.get("ce", "nan")),
                                 float(f.get("ce1", "nan"))))
                except ValueError:
                    pass
            if rows:
                return rows
            time.sleep(0.15)
        return None

    def wait_p(self, wid, timeout=3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for ln in self._lines()[self.mark:]:
                if ln.startswith(wid + "|P|"):
                    return ln
            time.sleep(0.15)
        return None


def type_word_and_wait(code, log, settle=1.5):
    type_code(code)
    time.sleep(settle)
    return log.wait_new(code)


def clear_and_focus(hwnd, log):
    u.SetForegroundWindow(hwnd)
    time.sleep(0.4)
    clear_doc()
    log.mark = log._nlines()


# ── 场景 ─────────────────────────────────────────────────────
def l1_two_words(hwnd, cor, dbg):
    """第①段主场景：逐词两连。挂账轮 skip|async_pending → 上屏收割
    ok|flags=async + P/D 行（CE 数值）；w2 上文含 w1 上屏词。"""
    print("[L1] two words: jintian, shijian")
    rows = {}
    for code in ("jintian", "shijian"):
        type_code(code)
        time.sleep(1.0)
        key_tap(0x20)          # 空格上屏 → OnCommit 收割 → ok 行 + P/D
        time.sleep(1.2)
        rows[code] = cor.wait_new(code)
    for code in ("jintian", "shijian"):
        r = rows[code]
        if r is None:
            # 冷启动豁免（同金标）：首词打在模型加载窗（skip=model_loading）
            # 或文档刚清空上文为空（skip=ctx_empty）——均属正确行为
            reason = None
            for why in ("model_loading", "ctx_empty"):
                if cor.wait_skip(code, why, timeout=1.5):
                    reason = why
                    break
            if reason:
                warn("L1 %s %s（冷启动豁免：首词无上文/模型加载中，属正确行为）"
                     % (code, reason))
                rows[code] = {"ctx": "", "result": "", "flags": "",
                              "id": "", "pending": True}
            else:
                check(False, "L1 row %s" % code, "no row")
            continue
        check(True, "L1 %s row 形态" % code, "flags=%r pending=%s" % (r["flags"], r.get("pending")))
        check(r["result"] != "", "L1 %s final 序非空" % code)
    # 上屏词（跟随 C 行实际提交）与上文传播
    c1 = cor.wait_commit(code="jintian")
    check(c1 is not None, "L1 jintian commit row")
    w1 = c1["word"] if c1 else "今天"
    r2 = rows.get("shijian")
    check(r2 is not None and w1 in r2["ctx"],
          "L1 shijian ctx has w1", "w1=%s ctx=…%s"
          % (w1, (r2 or {}).get("ctx", "")[-12:]))
    # CE 日志（D 行）：有评分的词逐候选 CE 数值且非哨兵；豁免词跳过
    for code in ("jintian", "shijian"):
        r = rows.get(code)
        wid = r["id"] if r else ""
        if not wid:
            continue      # 豁免词（无评分无事件 ID），D/P 断言跳过
        d = dbg.wait_d(wid)
        ok = bool(d) and any(row[1] > -1e9 for row in d)
        check(ok, "L1 %s D 行 CE 数值" % code,
              "rows=%d" % (len(d) if d else 0))
        p = dbg.wait_p(wid)
        check(p is not None, "L1 %s P 行" % code)


def l2_long_input(hwnd, cor, dbg):
    """第①段预期：有上文时长句连打，末键 full_len_lt2（满覆盖组 =
    poet 句候选 1 个 < 2 → 整窗跳过）。第②段（ngram 组句多候选进窗）
    落地后反转此断言。先打一词建立上文（ctx 空会先 ctx_empty 短路）。"""
    print("[L2] long input: jintian(上屏) + daziceshijinxingzhong")
    type_code("jintian")
    time.sleep(1.0)
    key_tap(0x20)
    time.sleep(1.2)
    type_code("daziceshijinxingzhong", gap=0.35)
    time.sleep(1.0)
    r = cor.wait_skip("daziceshijinxingzhong", "full_len_lt2")
    check(r is not None, "L2 末键 full_len_lt2（第①段预期整窗跳过）",
          "" if r else "未观察到（第②段行为？）")
    key_tap(VK_ESCAPE)     # 清残留编码不上屏
    time.sleep(0.4)


def l3_two_key(hwnd, cor, dbg):
    """单音节 2 码：有上文时打 ming → 满覆盖单字组（明/名/鸣…span
    ==[0,4)）≥2 → 评分。先打一词建立上文（ctx 空会 ctx_empty 短路）。"""
    print("[L3] two-key: jintian(上屏) + ming")
    type_code("jintian")
    time.sleep(1.0)
    key_tap(0x20)
    time.sleep(1.2)
    type_code("ming")
    time.sleep(1.0)
    key_tap(0x20)
    time.sleep(1.2)
    r = cor.wait_new("ming")
    if r is None:
        check(False, "L3 row ming", "no row")
        return
    d = dbg.wait_d(r["id"])
    check(bool(d) and any(row[1] > -1e9 for row in d),
          "L3 D 行 CE 数值", "rows=%d" % (len(d) if d else 0))
    c = cor.wait_commit(code="ming")
    check(c is not None and c["word"] != "", "L3 commit row",
          "word=%s" % (c["word"] if c else "-"))


def check_schema(log):
    """最新 H 行须为 luna_pinyin（当前方案校验）。"""
    try:
        with open(CORPUS, "r", encoding="utf-8", errors="replace") as f:
            hlines = [l for l in f.read().splitlines() if l.startswith("H\t")]
    except OSError:
        hlines = []
    if not hlines:
        warn("no H row")
        return
    last = hlines[-1].split("\t")
    schema = last[4] if len(last) > 4 else "?"
    if schema != "luna_pinyin":
        print("ABORT: 当前方案 = %s（需 luna_pinyin，请手动切换后重跑）"
              % schema)
        sys.exit(3)
    print("[pre] 当前方案 = luna_pinyin ✓（model=%s sentence=%s）"
          % (last[6] if len(last) > 6 else "?",
             last[10] if len(last) > 10 else "?"))


def main():
    global TAG
    check_schema(CorpusLog())
    hwnd = ensure_notepad()
    key_tap_win(0x20)
    time.sleep(0.4)
    key_tap_win(0x20)
    time.sleep(0.4)
    u.SetForegroundWindow(ctypes.c_void_p(hwnd))
    time.sleep(0.4)
    cor, dbg = CorpusLog(), DebugLog()

    for name, fn in (("L1", l1_two_words), ("L2", l2_long_input),
                     ("L3", l3_two_key)):
        TAG = name
        print("== %s ==" % name)
        clear_and_focus(hwnd, cor)
        fn(hwnd, cor, dbg)
        time.sleep(0.6)

    print("\n== %d/%d passed, %d warn ==" % (len(PASS), len(PASS) + len(FAIL),
                                             len(WARN)))
    for f in FAIL:
        print("  FAILED: %s" % f)
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
