// test_llm_context.cpp — 上文来源判定纯逻辑回归测试 (gold 断言)
//
// 覆盖 librime/src/rime/gear/llm_filter.cc 中 ctx_logic 命名空间的判定：
//   滞后检测 (WPS)、残留检测 (UTF-8 边界对齐)、空文本分类 (transient vs 真空)
// 这些判定是源码版最独有、改动最频繁的逻辑 (2026-08-13/14 多次真机修复)，
// 抽取为不依赖引擎/模型的纯函数独立测试，防止未来改动破坏行为。
//
// 注意：本文件复制 ctx_logic（源码版无 librime 测试基础设施可挂载），
// 修改 llm_filter.cc 的判定必须同步本文件并重跑。
//
// 编译:
//   cl /O2 /std:c++17 /EHsc /DNDEBUG /utf-8 test_llm_context.cpp /Fe:test_llm_context.exe
// 运行: test_llm_context.exe   (退出码 0=全部通过)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ctx_logic {

// 与 llm_filter.cc 的 ctx_logic 完全一致（改判定必须同步）
inline bool lagging(const std::string &tsf, const std::string &hist) {
  if (tsf.empty() || hist.empty())
    return false;  // 空文本不在滞后检测范围内 (调用侧已排除非空 TSF)
  size_t tlen = tsf.size();
  return tlen * 2 < hist.size() && hist.size() > tlen &&
         hist.compare(hist.size() - tlen, tlen, tsf) == 0;
}

inline std::string hist_tail(const std::string &hist, size_t max_bytes) {
  // std::min<size_t>: 显式模板参数, 与 llm_filter.cc 保持一致
  size_t n = std::min<size_t>(max_bytes, hist.size());
  size_t start = hist.size() - n;
  while (start < hist.size() &&
         (static_cast<unsigned char>(hist[start]) & 0xC0) == 0x80)
    ++start;  // 跳过 continuation bytes, 取完整字符
  return hist.substr(start);
}

inline bool stale(const std::string &tsf, const std::string &hist) {
  std::string tail = hist_tail(hist, 8);
  return !tail.empty() && tsf.find(tail) == std::string::npos;
}

inline bool transient_empty(const std::string &hist,
                            unsigned long long age_ms) {
  return age_ms < 1500 && !hist.empty();
}

}  // namespace ctx_logic

using ctx_logic::hist_tail;
using ctx_logic::lagging;
using ctx_logic::stale;
using ctx_logic::transient_empty;

static int g_fail = 0;

static void check(bool cond, const char *name) {
  if (cond) {
    std::printf("PASS: %s\n", name);
  } else {
    std::printf("FAIL: %s\n", name);
    ++g_fail;
  }
}

int main() {
  std::printf("== llm context-logic tests ==\n");

  // ── 滞后检测 (WPS) ──
  // WPS: 连续打 N 个"测试" TSF 只采到 2 字符 → TSF 是历史尾部且明显更短
  check(lagging("测试", "化学试剂测试"), "L1 WPS: 2字tsf是6字hist尾部且明显更短");
  check(!lagging("试剂测试", "化学试剂测试"), "L2 长度接近(4字vs6字)不判滞后");
  check(!lagging("英雄事迹", "化学试剂测试"), "L3 非尾部子串不判滞后");
  check(!lagging("测试", "测试"), "L4 hist==tsf 不判滞后 (长度相等)");
  check(!lagging("", "化学试剂测试"), "L5 空tsf不判滞后");

  // ── hist_tail UTF-8 边界对齐 ──
  // "化学试剂测试" = 6 个汉字 × 3 字节; 取 8 字节窗: 窗起点落在字符中间则
  // 前跳到完整字符起点, 返回 ≤8 字节的完整字符尾部
  check(hist_tail("化学试剂测试", 8) == "测试", "H1 8字节窗前跳到完整汉字起点(6字节)");
  check(hist_tail("化学试剂测试", 6) == "测试", "H2 6字节窗恰为2个汉字");
  check(hist_tail("abc测试", 8) == "bc测试", "H3 窗起点落在'c'('bc'非汉字) → 直接取8字节");
  check(hist_tail("", 8) == "", "H4 空历史返回空");
  check(hist_tail("测", 8) == "测", "H5 短历史取完整");

  // ── 残留检测 (陈旧 TSF 文本) ──
  // 正常: 光标前文本必含最近上屏词 (hist 尾部); 不含 → 过期 → 用历史
  check(!stale("化学试剂测试", "化学试剂测试"), "S1 TSF含hist尾部 → 不陈旧");
  check(stale("英雄事迹", "化学试剂测试"), "S2 TSF不含hist尾部 → 陈旧");
  check(!stale("前面内容剂测试", "化学试剂测试"), "S3 TSF包含tail子串 → 不陈旧");
  check(!stale("测试", "测试"), "S4 hist==tsf → 不陈旧");
  check(stale("", "化学试剂测试"), "S5 空TSF文本 → 陈旧 (无上屏词)");
  check(!stale("测试化学试剂", "测"), "S6 hist单字'测'且TSF含之 → 不陈旧");

  // ── 空文本分类 (transient vs 真空) ──
  check(transient_empty("测试", 100), "T1 commit后100ms历史非空 → transient, 用历史");
  check(!transient_empty("测试", 1500), "T2 边界age=1500 → 真空 (>=1500不用历史)");
  check(!transient_empty("测试", 5000), "T3 5s → 真空 (光标在文档开头/全删)");
  check(!transient_empty("", 100), "T4 历史空 → 不用历史");

  std::printf("\n%s (%d failures)\n", g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
              g_fail);
  return g_fail ? 1 : 0;
}
