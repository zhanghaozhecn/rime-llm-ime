//
// llm_filter.cc - LLM candidate rerank filter
//
// Context text (rime_api get_context_text, collected by TSF frontend)
// + llama.cpp inference to rerank candidates.
// Core algorithm ported from llm_rerank project (rime_llm.cpp):
//   ctx decode once -> KV copy -> parallel candidate decode -> CE score
//
// 挂载语义（2026-08-29 定案，显式组件）：方案须在 engine/filters 显式
// 列出 llm_filter 才参与重排——位置由方案与其他 filter 的先后关系决定
//（如需在 simplifier/uniquifier 之后，由方案作者自行安排）。未列出 =
// 该方案无 LLM 重排。enabled 仍为运行时开关（默认 false 纯透传），
// 参数只来自**方案 schema.yaml 的 llm_rerank: 配置节**（2026-09-30 定案：
// 与插件版统一，取消全局 %APPDATA%\Rime\llm_rerank.yaml；改配置 = 改方案，
// 由 GUI 触发重新部署，部署期快照，无热重载）。
//
#include <rime/gear/llm_filter.h>
#include <rime/gear/prefix_gate_model.h>   // 2026-11-25 前缀确认门模型（生成）
#include <rime/gear/llm_ngram.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>
#include <boost/regex.hpp>  // code_pattern：与 speller/auto_select_pattern 同引擎同语义

#include "llama.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>  // COM 光标上文旁路: GetActiveObject/VARIANT
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace rime {

// ============================================================
// model state (module-level, one model per process)
// ============================================================
static llama_model *g_model = nullptr;
static llama_context *g_ctx = nullptr;
static const llama_vocab *g_vocab = nullptr;
static std::mutex g_mutex;
static std::atomic<bool> g_loaded{false};
static std::atomic<bool> g_loading{false};

// schema llm_rerank/ section (defaults match the old project):
//   enabled: true|false — false disables rerank entirely (pass-through)
//   code_pattern: 编码串**全串**正则匹配（2026-09-30 取代 min/max_code_len）；
//                 语义与引擎同 speller/auto_select_pattern（boost::regex_match）。
//                 例：'.{4}' 恰 4 码 / '.{4,}' 4 码以上 / '.{3,4}' / '[abcde]{4}'；
//                 空串 = 不限制；非法模式回退默认 '.{4}' 并写日志。
//   min_code_len / max_code_len: **旧键，已不生效**（解析保留仅为兼容历史
//                 方案；等价换算 min=4,max=0 ⇒ code_pattern '.{4}'）
//   two_code_sentence: true = 满长组优先（词长==⌊码长/2⌋ 的候选一组直接优先,
//                 组内融合分降序、同分稳定; 2026-11-07 取代 expected_length_weight）
//   freq_beta: **已删**（2026-11-05 标定产物退出用户配置面——用户没有标定能力；
//                 β=8 为内置常数与 τ 同待遇，方案残留旧行读取端忽略、保存时剥除）。
//                 用户词频对数融合 fused = score + β·log(1+eff)（score=原始
//                 LLM 分 −CE；对数域加法=词频无上限可翻盘），eff=Rime
//                 formula_d 时间衰减计数（tick 每词提交+1，与引擎调频同源；
//                 user_eff.tsv 持久化（旧名 user_freq.tsv）。β=8 连续文章语料重标定 (2026-11-02,
//                 平台 8~30 取左段; 旧 1.5 出自独立窗口径)
// 排序管线（顺序固定，与插件版一致）: CE 评分序 → 词频融合（融合应用于
//   评分序之上）→ 两码整句满长组优先（开关开启时）→ 稳定排序（失败哨兵
//   分排尾部）。
// CE 位置权重 (与插件版同源, 首项归一规范形; 2026-11-14 默认模型改
//   Qwen2.5-0.5B 后重标定 (1,1.10,0.60)+β14, 用户域 34k + wiki 池化 180k
//   网格, 偶半选→奇半验四半区一致, 见研究库 calibrate_model_05b; 旧
//   (1,1.05,0.45) 为 0.8B v3 值): ce2 判别
//   力最强(上下文契合+词内连贯), ce3 深入词内部降权; 4+ token 词只算前三
//   项——尾部外推证伪删除(λ 0→1 端到端命中率单调递减, λ=0 最优)。
// 参数来源（2026-09-30 用户定案，两版统一）：**只读方案的 llm_rerank 配置节**
//（%APPDATA%\Rime\*.schema.yaml；GUI/接入助手写入，部署期快照）——全局
// %APPDATA%\Rime\llm_rerank.yaml 已取消，与插件版（rime-lua 读同一节）一致：
// 改参数 = 改方案 + 重新部署，不再有 mtime 热重载。2026-08-29 起为显式组件：
// 方案 engine/filters 列出 llm_filter 才参与重排（enabled 默认 false 纯透传）。
// 默认模型路径 = RIME 用户目录根\Qwen...gguf（2026-08-31 用户澄清定案：
// "用户文件夹"= 小狼毫右键的用户文件夹——方案配置所在处，模型直接放根
// 目录、不套子文件夹，与 simplifier 取 user_data_dir 同源；8-27 曾误用
// %USERPROFILE%\gguf_models\）。
// 自定义位置用 llm_rerank model_path 显式指向。
// 注意 Service.deployer().user_data_dir 在引擎启动后才就绪，故不能在
// 静态初始化求值——g_model_path 留空，加载时懒取默认。
// 默认模型 = Qwen3-0.6B-Base-Q4_K_M（2026-11-19 用户定案 0.6B base；
// 2026-11-22 补齐此处内置默认——定案当时只改了真机方案节的显式
// model_path，C++ 默认仍指 qwen2.5-0.5b-instruct，无 model_path 的方案
// （如 GUI 接入的朙月拼音，模板注释该行）会加载不存在的文件 → failed
// to load model 死循环，每键 skip=model_loading 无推理，真机排查发现）。
// 前身 2026-11-16 的 qwen2.5-0.5b-instruct；更旧 2026-11-14 Qwen2.5-0.5B
// （base）、Qwen3.5-0.8B）
static std::string default_model_path() {
  try {
    return (Service::instance().deployer().user_data_dir /
            "Qwen3-0.6B-Base-Q4_K_M.gguf")
        .string();
  } catch (...) {
    return "Qwen3-0.6B-Base-Q4_K_M.gguf";
  }
}
static std::string g_model_path;  // 空 = 未配置，load_model 时按默认兜底
static std::string g_loaded_from; // 当前已加载模型来自的路径（变更检测）
static bool g_enabled = false;  // CPU only; GPU build retired (not published)
// 触发条件（2026-09-30 起）：编码串是否匹配 code_pattern，**全串匹配**，
// 语义与引擎同 Rime 自身的 speller/auto_select_pattern（boost::regex_match）。
// 取代旧的两个整数键 min_code_len / max_code_len（用户配置面已移除；解析端
// 保留 has_min/max 字段仅为兼容历史方案，本过滤层不再读它们）。
// 默认 ".{4}" = 恰 4 码，与旧默认（min=4, max=0 不限）生产主路径一致。
static const char *kDefaultCodePattern = ".{4}";
static std::string g_code_pattern = kDefaultCodePattern;
static boost::regex g_code_re;          // 编译缓存（llm_apply_params 时重建）
static bool g_code_re_ok = true;        // 模式非法 → false（回退默认成功后仍为 true）
static int g_min_code_len = 4;          // 保留：仅日志/兼容，不再作为用户配置项
static int g_max_code_len = 0;          // 保留：同上（0 = 无上限）
static bool g_sentence_mode = false;  // 整句模式：满覆盖候选进评分窗 (2026-11-22
// 用户定案，取代 two_code_sentence 两码整句满长规则——词长==⌊码长/2⌋ 退役；
// span 判定候选覆盖整个活动段，引擎 lattice 保证简拼/单韵母短码的合法解析
// 共存，两码方案下与旧规则逐位等价)

// 前缀软确认（2026-11-24 定案：合并单一路线，取代窗口 A + 句级 B 两路）：
// 唯一 pending 生产者 = 组句（ngram）给文本 + 窗口融合序投票：
//   ① 组句侧：top-20 组句按前导词(lead) 分组做 softmax，P(lead) ≥ 0.95
//      （研究库「组句自信前缀的更本质度量」：同精度下召回比"5 条 lead 全
//      一致"高 51%；阈值 0.90 落进双峰低谷，0.95–0.97 才稳）；
//   ② 窗口侧：该 lead 文本必须是窗口融合序第 1 且 span 对齐（文本一致铁律）；
//   ③ 守卫（2026-11-24 标定，**音节单位**）：确认后剩余 ≥2 音节；单音节
//      lead 例外（剩余 ≥1）。组句句子字数 = 音节数（中文一字一音节）；
//      不用码数——全拼 1 音节 2–6 码、简拼 1 码、双拼 2 码/音节，混拼后
//      码数与音节数无固定关系（旧「≥6 码 / 余 ≥4 码」是拍的，已退役）。
// 挂起后在下一次刷新路径执行 ctx->Select → Segment::Close 截段 → Forward +
// Compose 只重译剩余码（组合区内预确认，不上屏）。开关 = 方案节
// `llm_rerank/ngram_group`（GUI「N-gram 组句候选（含前缀自动确认）」，
// 2026-11-24 用户定案：确认信号只有组句 lead，独立键 prefix_confirm 退役）；
// 环境变量 RIME_LLM_PREFIX_CONFIRM=1 保留为真机实验的强制覆盖。
// θ 与单字倍数已退役（位次门不需要 θ）；组句返回空的窗不再自动确认
//（研究库：该档窗口路径 A 精度仅 56.8%）。
static bool g_prefix_confirm = false;      // 生效值（llm_apply_params 计算）
static bool g_prefix_confirm_env = []() {
  const char *e = getenv("RIME_LLM_PREFIX_CONFIRM");
  return e && std::string(e) == "1";
}();
// 测试钩子（2026-11-23，仅排障）：RIME_LLM_TEST_PREFIX_CONFIRM=1 → 每键在
// 活动段选第一个"只覆盖前缀"的候选（end < 段末），验证 Select → Close 截段
// → Forward + Compose 在 filter 阶段的安全性（不依赖 LLM 评分）
static bool g_test_prefix_confirm = []() {
  const char *e = getenv("RIME_LLM_TEST_PREFIX_CONFIRM");
  return e && std::string(e) == "1";
}();
// 组句 lead 概率阈值（2026-11-24）：P(lead) ≥ 0.95（top-20 softmax）。
// 冒烟/排障可用 RIME_LLM_PREFIX_P 覆盖。
static double g_prefix_confirm_p = []() {
  const char *e = getenv("RIME_LLM_PREFIX_P");
  double v = e ? atof(e) : 0.95;
  return (v >= 0.0 && v <= 1.0) ? v : 0.95;
}();
// 前缀确认绝对置信度门（2026-11-25 研究定案）：窗口融合序第 1 候选的
// **首 token CE**（原始正 CE，小 = 模型认为该词很可能）≤ 阈值；空上文
// （无 CE）用 ngram 句分差 d≥阈值；有效上文 ≤1 字不确认（1 字前缀精度
// 仅 82%）。0 = 关闭（回到 2026-11-24 行为）。环境变量供真机 A/B。
static double g_prefix_ce1 = []() {
  const char *e = getenv("RIME_LLM_PREFIX_CE1");
  if (!e || !*e) return 11.0;
  double v = atof(e);
  return (v >= 0.0 && v <= 1e6) ? v : 11.0;
}();
static double g_prefix_d = []() {
  const char *e = getenv("RIME_LLM_PREFIX_D");
  if (!e || !*e) return 10.0;
  double v = atof(e);
  return (v >= 0.0 && v <= 1e6) ? v : 10.0;
}();

// ═══ 前缀确认门：LightGBM 模型（2026-11-25 用户定案落地）═══
// 41 个可部署特征（顺序见研究库 eval/calib/export_prefix_gate.py）→
// LightGBM 300 树 / 15 叶 → p̂ ≥ τ 才确认；取代 `P≥0.95 ∧ ce1≤11` 两道
// 手调门（P 仍是特征之一；训练域下界 p≥0.5 保留）。模型表随源码编译内嵌
// （prefix_gate_model.h，研究库 export 子命令生成）。
// A/B：RIME_LLM_PREFIX_MODEL=0 禁用模型（回退 ce1 门）、
// RIME_LLM_PREFIX_TAU 覆盖 τ、RIME_LLM_PREFIX_TABLOCK=0 关 ta 族黑名单。
static bool g_prefix_model_on = []() {
  const char *e = getenv("RIME_LLM_PREFIX_MODEL");
  if (e && *e && atof(e) == 0.0) return false;
  return true;
}();
static double g_prefix_tau = []() {
  const char *e = getenv("RIME_LLM_PREFIX_TAU");
  if (!e || !*e) return kPrefixTau;
  double v = atof(e);
  return (v >= 0.0 && v <= 1.0) ? v : kPrefixTau;
}();
static bool g_prefix_tablock_on = []() {
  const char *e = getenv("RIME_LLM_PREFIX_TABLOCK");
  return !(e && *e && atof(e) == 0.0);
}();

// ta 族（先验可论证的结构性同音歧义；研究库 2026-11-25：40 触发 13 错，
// 真实用户域 8/10 = 80%；禁用后整体 +0.07~0.08pp、user 域 +0.55pp）
static bool PrefixTaBlocked(const std::string &lead) {
  static const char *kTa[] = {"他", "它", "她", "他们", "它们", "她们"};
  for (const char *t : kTa)
    if (lead == t) return true;
  return false;
}

// 字频表（模型文件自带；8421 字 → 首次访问建 unordered_map，一次 ~1ms）
static float PrefixCharFreq(const std::string &ch) {
  static const std::unordered_map<std::string, float> m = []() {
    std::unordered_map<std::string, float> t;
    t.reserve((size_t)kPrefixFreqN * 2);
    for (int i = 0; i < kPrefixFreqN; i++)
      t.emplace(kPrefixFreqChar[i], kPrefixFreqLog[i]);
    return t;
  }();
  auto it = m.find(ch);
  return it == m.end() ? -13.0f : it->second;
}

// 树推理（平坦数组，节点下标 1-based/树内；全局 = off + local−1）
static double PrefixModelScore(const double *x) {
  double s = 0.0;
  for (int t = 0; t < kPrefixTreeN; t++) {
    const int off = kPrefixTreeOff[t];
    int node = off;                        // 0-based 全局下标（local 1）
    while (kPrefixF[node] != 0) {
      const int f = kPrefixF[node] - 1;
      node = off +
             (x[f] <= kPrefixT[node] ? kPrefixL[node] : kPrefixR[node]) - 1;
    }
    s += kPrefixV[node];
  }
  if (s > 35.0) return 1.0;
  if (s < -35.0) return 0.0;
  return 1.0 / (1.0 + std::exp(-s));
}

// ── N-gram 组句候选（2026-11-23 整句模式第②段，与插件版同构）─────────
// 组句器 = llm_ngram.{h,cc}（万象 LTS `.gram` + 万象词库词级 beam，算法与
// 研究库 eval/tools/sentence_gen_gram.py 同构）。开关 = 方案节
// `llm_rerank/ngram_group`，**只在整句模式 + 方案输出简体**时生效——
// 组句词表是简体，纯繁体方案不启用。
// 资产在用户目录 llm_ngram\（安装器可选下载）；缺失时 60 秒重试一次。
static bool g_ngram_group = false;        // 生效值（llm_apply_params 计算）
// 输出字形门（2026-11-23 晚修）：filters 里确有 simplifier（且非 s2t 反向）
// = 方案输出简体。旧判据只认 simplifier/option_name 非空，把朙月这类
// **无 option_name 的恒转换**方案误判成繁体——真机报障根因（组句静默关闭
// → 单字前缀确认结构上永不触发）。
static bool g_simplified_scheme = false;
static std::string g_simplifier_option;  // simplifier/option_name（空 = 恒转换）
static std::mutex g_ngram_mu;
static rime_llm_ngram::Engine *g_ngram_engine = nullptr;
static std::string g_ngram_sig;
static std::time_t g_ngram_retry_at = 0;
struct NgramGenInfo {   // 组句候选的附加信息（对齐 text）
  std::string lead;     // 前导词（beam 分词的第一词）
  int lead_end = 0;     // 前导词覆盖到的 code 偏移
  double score = 0.0;   // ngram 分（调试用）
};
struct PendingConfirm {
  bool valid = false;
  std::string input;    // 活动段切片（判定时的 code）
  size_t seg_start = 0; // 活动段在原始 input 中的起点
  std::string text;     // 待确认候选文本
  size_t end_pos = 0;   // 候选覆盖到的码位置（< 段末）
  int tries = 0;        // 重试计数（可重试失败上限，2026-11-24）
};
static PendingConfirm g_pending_confirm;
static std::mutex g_pending_confirm_mu;
static bool g_in_confirm = false;  // 防重入（Select → Compose → Apply 递归）
// 用户词频对数融合 (2026-09-02): fused = score + β·log(1+eff), β **内置常数**
// (2026-11-05 标定产物退出用户配置面——用户没有标定能力, 与 kFreqTau 同待遇;
//  **2026-11-14 默认模型改 Qwen2.5-0.5B 后重标定** β=14 + kCeW (1.0,1.10,
//  0.60), 用户域 34k + wiki 池化 180k 网格, 偶半选→奇半验四半区一致, 见
//  研究库 calibrate_model_05b; 旧 β=12/(1.05,0.45) 为 0.8B v3 六维值)
static double g_freq_beta = 14.0;
// CE 位置权重（唯一真源；2026-11-14 随默认模型 0.5B 重标定, 旧 1.05/0.45 为
// 0.8B v3 值）。2026-11-25 从前缀门函数内提到文件作用域——模型门的 41 特征里
// 的 CE 加权和也要用它，两处必须同值。
static constexpr double kCeW[3] = {1.0, 1.10, 0.60};

// debug_fusion 诊断 (2026-09-03 插件版先行、本文件移植): true = 逐块记录
// 评分→词频融合→词长加成全过程写 rime_llm_debug.txt (与插件版同名同格式;
// 差异: commit 行无码字段 — OnCommit 只收到上屏文本; reset 行按代次触发,
// 无法区分具体编辑键)。默认 false, 关闭时仅一次 bool 读零开销。
static bool g_debug_fusion = false;
// COM 文档模型光标上文旁路开关 (2026-09-10 WPS 实验移植, 默认开; 定义与
// 模块见下方 comctx 节 / 消费点 GetContextTextPair)。2026-09-29 定案删
// com_context 配置项（两版同步）：旁路无常关配置，排障直接改此值重编。
static bool g_com_ctx_enabled = true;
static int g_min_tokens = 1;
static int g_max_ctx_tokens = 20;  // 2026-11-19: 0.6B ctx*=20（旧 10 为 0.5B 时代值）
static int g_n_threads = 4;        // default = GGML_DEFAULT_N_THREADS; override via cpu_cores

static void log_msg(const char *fmt, ...);    // defined below (fwd decl)
static void log_error(const char *fmt, ...);  // defined below (fwd decl)
static void write_m(const char *reason);      // defined below (fwd decl)
static void commit_stack_refund_or_clear(const char *reason);  // fwd decl
                                                              // (回撤退款)
static std::vector<llama_token> tokenize(const char *text);  // fwd decl
static double cross_entropy(const float *logits, int vs, int target_id);  // fwd decl
static std::string ConfirmedPrefixText(Context *ctx);  // fwd decl（前缀确认）

// KV 容量（2026-11-23 第②段：128 → 512，与插件版同值）。3-token 路径只需
// 11 seq × (ctx 20 + cand 2) = 242；整句 CE 路径需 1 × ctx + n × (ctx + 48)
// （n ≤ 6 候选、ctx ≤ 20 → ≈ 310）。512 两者都够，且整句 CE 与 3-token
// 共用同一 context/seq0/prep（合并收益：省掉每次一条重复的 ctx forward +
// O(vocab) 归一化）。历史 128 为 3-token 单独口径（64 时 S2 恒 ~100ms）。
static int g_n_ctx = 512;
static int g_n_seq_max = 12;       // template seq 0 + up to 11 worker seqs
static int g_max_candidates = 6;   // candidates participating in scoring
                                   // (2026-11-19 随生产模型切 Qwen3-0.6B base 定案 k*=6)

// ==== 参数来源（2026-09-30 用户定案：两版统一回归 Rime 原生做法）====
// **只读方案 schema.yaml 的 llm_rerank: 配置节**（部署期快照 Apply/Initialize）。
// 全局 %APPDATA%\Rime\llm_rerank.yaml 层**已取消**：与插件版一样，配置写在
// 方案里、改完由 GUI 触发重新部署（enabled / model_path 变更随之生效）。
// 旧键 min_code_len / max_code_len / min_tokens 仍解析（兼容历史方案），
// 但不再参与判定（触发条件 = code_pattern；最少上文 token 恒 1）。
struct LlmParamSet {
  bool has_enabled = false;      bool enabled = false;
  bool has_min_code_len = false; int min_code_len = 4;   // 旧键：解析保留、不再生效
  bool has_max_code_len = false; int max_code_len = 0;   // 旧键：解析保留、不再生效
  bool has_code_pattern = false; std::string code_pattern;  // 2026-09-30 新键
  bool has_sentence = false;     bool sentence_mode = false;
  bool has_ngram_group = false;   bool ngram_group = false;     // 2026-11-23 第②段
  // freq_beta 字段已删（2026-11-05 退出用户配置面）→ g_freq_beta 内置常数
  bool has_min_tokens = false;   int min_tokens = 1;
  bool has_max_tokens = false;   int max_tokens = 20;  // 2026-11-19: 0.6B ctx*=20
  bool has_max_cand = false;     int max_cand = 6;     // 2026-11-19: 0.6B k*=6
  bool has_cpu_cores = false;    int cpu_cores = 4;
  bool has_debug_fusion = false; bool debug_fusion = false;
  bool has_model_path = false;   std::string model_path;
};
static LlmParamSet g_schema_params;  // 方案 llm_rerank 节快照（部署期固定）
static void load_model_async();      // fwd decl（定义在下方）

static void llm_apply_params() {
  const LlmParamSet &s = g_schema_params;
  g_enabled = s.has_enabled ? s.enabled : false;
  // 整句模式先于 code_pattern 读取（2026-11-22 锁定编码匹配要用）
  g_sentence_mode = s.has_sentence ? s.sentence_mode : false;
  // N-gram 组句候选（2026-11-23 第②段）：只在整句模式 + 方案输出简体时生效
  //（组句词表 = 万象简体；繁体方案不启用，避免吐简体候选）。
  g_ngram_group = g_sentence_mode && g_simplified_scheme &&
                  (s.has_ngram_group ? s.ngram_group : false);
  // 前缀自动确认（2026-11-24 用户定案：并入 ngram_group 单开关）：确认信号
  // 只有组句 lead 一条来源，要前缀确认就必须有万象资产，故不再有独立配置键
  //（旧键 `llm_rerank/prefix_confirm` 读取端忽略、GUI 保存/重建节时剥除）。
  // 环境变量保留为真机实验强制覆盖。
  g_prefix_confirm = g_prefix_confirm_env || g_ngram_group;
  // 触发条件：code_pattern。空串 = 不限制（总是匹配）。
  // 整句模式锁定编码匹配（2026-11-22 用户定案）：sentence_mode on 时
  // 恒 '.{2,}'（2 码起参与重排）——方案节该行读取端忽略（手改 yaml 也
  // 不生效）；off 时方案节值生效
  g_code_pattern = s.has_code_pattern ? s.code_pattern : kDefaultCodePattern;
  if (g_sentence_mode)
    g_code_pattern = ".{2,}";
  g_code_re_ok = true;
  if (!g_code_pattern.empty()) {
    try {
      g_code_re = boost::regex(g_code_pattern);
    } catch (const std::exception &e) {
      log_error("code_pattern invalid ('%s': %s) -> fallback '%s'",
              g_code_pattern.c_str(), e.what(), kDefaultCodePattern);
      g_code_pattern = kDefaultCodePattern;
      try {
        g_code_re = boost::regex(g_code_pattern);
      } catch (...) {
        g_code_re_ok = false;  // 默认模式都编不过（理论上不会）→ 全部透传
      }
    }
  }
  // 旧键 min/max_code_len 仍解析（兼容历史方案），但**不再参与触发判定**；
  // 仅保留数值以免日志/兼容字段为空。
  g_min_code_len = s.has_min_code_len ? s.min_code_len : 4;
  g_max_code_len = s.has_max_code_len ? s.max_code_len : 0;
  // g_freq_beta 恒为内置常数（2026-11-05 退出用户配置面，不再从方案读取；
  // 方案残留 freq_beta 旧行忽略，保存时由 GUI 剥除）
  g_debug_fusion = s.has_debug_fusion ? s.debug_fusion : false;
  // g_com_ctx_enabled 不再读配置（2026-09-29 删 com_context 配置项）——
  // 保持定义处 true，排障改代码重编
  g_min_tokens = s.has_min_tokens ? s.min_tokens : 1;
  g_max_ctx_tokens = s.has_max_tokens ? s.max_tokens : 20;
  g_max_candidates = s.has_max_cand ? s.max_cand : 6;
  g_n_threads = s.has_cpu_cores ? s.cpu_cores : 4;
  // cap threads at hardware cores: 低核机器不应超订（变慢 + 每线程额外内存）
  unsigned hw = std::thread::hardware_concurrency();
  if (hw > 0 && (unsigned)g_n_threads > hw)
    g_n_threads = (int)hw;
  g_model_path = s.has_model_path ? s.model_path : default_model_path();
}

static void unload_model();  // 定义在后（enabled 关闭/路径变更时卸载，前置声明）

// ------------------------------------------------------------
// N-gram 组句器：资产路径解析 + 进程内一次加载（engine thread）
// ------------------------------------------------------------
static bool ngram_ensure(const std::string &schema_id) {
  if (g_ngram_engine && g_ngram_engine->ready())
    return true;
  const std::time_t now = std::time(nullptr);
  if (now < g_ngram_retry_at) {
    log_msg("ngram: skip (retry cooldown)");  // 诊断（2026-11-23 晚）
    return false;
  }
  const std::string user =
      Service::instance().deployer().user_data_dir.u8string();
  if (user.empty()) {
    log_msg("ngram: skip (user_data_dir empty)");  // 诊断（2026-11-23 晚）
    return false;
  }
  const char *ge = getenv("RIME_LLM_NGRAM_PATH");
  const char *de = getenv("RIME_LLM_NGRAM_DICT");
  const std::string gram = (ge && *ge)
                               ? std::string(ge)
                               : user + "/llm_ngram/wanxiang-lts-zh-hans.gram";
  const std::string dict =
      (de && *de) ? std::string(de) : user + "/llm_ngram/dicts";
  const std::string cache_dir = user + "/llm_rerank/ngram_lex";
  CreateDirectoryA(cache_dir.c_str(), nullptr);
  const std::string cache =
      cache_dir + "/" + (schema_id.empty() ? "unknown" : schema_id) + ".bin";
  // 万象词库六件（zi/jichu/duoyin/diming/mingren/yiren，自带词频列）
  const std::vector<std::string> files = {"zi",   "jichu",  "duoyin",
                                          "diming", "mingren", "yiren"};
  const std::string sig = gram + "\x1f" + dict + "\x1f" + cache;
  {
    std::lock_guard<std::mutex> lk(g_ngram_mu);
    if (g_ngram_engine && sig == g_ngram_sig && g_ngram_engine->ready())
      return true;
    if (!g_ngram_engine)
      g_ngram_engine = new rime_llm_ngram::Engine();
    if (!g_ngram_engine->Load(gram, dict, files, "", cache)) {
      g_ngram_sig.clear();
      g_ngram_retry_at = now + 60;
      log_error("ngram assets unavailable (%s) -> group candidates off",
                g_ngram_engine->error().c_str());
      return false;
    }
    g_ngram_sig = sig;
  }
  log_msg("ngram: %s", g_ngram_engine->status().c_str());
  return true;
}

static std::vector<rime_llm_ngram::Sentence> ngram_generate(
    const std::string &code,
    const std::string &ctx,
    const std::string &schema_id) {
  std::vector<rime_llm_ngram::Sentence> out;
  // 2026-11-24：码串必须是**纯小写字母**——组合区 input 可能含分隔符空格
  //（rime 音节分隔）或标点时，生成器的码格无意义，且真机怀疑与卡死相关。
  if (code.empty())
    return out;
  for (unsigned char c : code)
    if (c < 'a' || c > 'z')
      return out;
  if (!ngram_ensure(schema_id))
    return out;
  rime_llm_ngram::Params p;
  p.topk = 20;         // 2026-11-24 定案：top-20 供 lead 概率质量（softmax）
                       // 与显示首选（ngram 序第 1）；beam 本就探索 32，
                       // 实测 4.0 → 4.1ms/窗（零额外成本）
  p.beam = 32;         // 生产参数：研究库实测 15ms/句，hit@5 73%/61%
  p.min_weight = 10.0;
  p.max_edges = 8;
  p.ctx_tail = 10;
  rime_llm_ngram::Engine *eng = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_ngram_mu);
    eng = g_ngram_engine;
  }
  if (eng)
    out = eng->Generate(code, ctx, p);
  if (out.empty())  // 诊断（2026-11-23 晚）：空结果也要可见
    log_msg("ngram: generate empty code='%s' ctxlen=%zu eng=%d",
            code.c_str(), ctx.size(), eng ? 1 : 0);
  return out;
}

// ============================================================
// commit-history fallback state (engine thread only: OnCommit sink
// callback and Apply both run on the engine thread, no lock needed)
// ============================================================
static std::string g_fallback_buffer;  // session committed texts
static int g_fallback_gen_seen = 0;    // consumed reset generation

// 受限窗口粘性降级 (2026-08-18 八轮, 用户决策; 2026-09-11 简化定案):
// lagging 命中一次即标记本窗口"受限" (WPS 类应用 TSF 只暴露最近
// composition), 之后整窗只用历史上文——受限 store 快照不跟光标走
//（格内鼠标移位后首词拿到旧光标尾巴, 真机定局实验）, TSF 永不直接
// 消费。标记留存: reset 后仍处 Office 前台（WPS 内部 DocumentMgr 切换
// ——格/标签移动、编辑键）仅清历史、保留受限; 离开 Office 前台（真切
// 窗）才清除重评。鼠标移动 = reset 语义: focus:switch（WPS 格/标签）+
// 新鲜送达非历史尾部（同窗内无 DocumentMgr 切换的移动, 仅作事件信号）。
// engine thread only (同上)。
static bool g_ctx_limited = false;
static int g_limited_gen_seen = 0;

// ============================================================
// COM 文档模型光标上文旁路 (2026-09-10 WPS 实验移植; 探针源码 =
// scripts/probe_wps_ctx.cpp; 定案与真机数据 memory/wps-context-
// investigation.md 十一节)。WPS 文档正文只在自绘画布 (TSF store 只有
// composition), 但 COM 文档模型可读: WPS 12.1 实测文档打开即注册 ROT
// (打字前台态可用, 无微软 KB238610 失焦坑), ActiveWindow.Selection 前
// 64 字读链稳态 ~5.5ms (6 跳 IDispatch), 24/24 拍逐词跟随。
//
// 消费语义 (见 GetContextTextPair): 仅粘性降级 (g_ctx_limited, WPS 类
// 应用) 场景 — COM 缓存新鲜 → {com_text, "com"} (徽章仍 AI·TSF);
// 失败/过期/总开关关（代码级，见 g_com_ctx_enabled）→ 原 {hist, "rime"}
// (八轮方案零改动)。
// 好应用零开销: 线程懒启动 (首次降级命中才起), 前台 Office 才读 +
// 未附着 500ms 快速重试 / 附着后 2s 周期 (与插件版同款; 2026-09-11
// 审查发现历史 backoff 指数退避为死变量——附着成功即复位, 稳态实际
// 500ms 轮询, 已删并归位 2s)。COM 只在旁路线程调用 (STA 出站调用无需
// 消息泵, probe 实测背书); 引擎线程仅经 mutex 读缓存快照。
// ============================================================
#ifdef _WIN32
namespace comctx {

static std::mutex g_mu;             // guards g_text/g_stamp/g_title
static std::string g_text;          // UTF-8 caret-preceding text
static unsigned long long g_stamp = 0;   // GetTickCount64() of last good read
static std::wstring g_title;        // 发布时的前台标题（消费端指纹）

static std::mutex g_cv_mu;
static std::condition_variable g_cv;
static std::atomic<bool> g_kick{false};
static std::atomic<bool> g_started{false};

// 前台 Office 判定（2026-09-10 定案取代 lagging 后果判定做 COM 消费门控;
// 2026-09-11 扩展演示）: OpusApp 同属 MS Word（开发代号 Opus）与 WPS 文字
//（复刻 Word 窗口体系）; PP12FrameClass 同属 MS PowerPoint（2010+）与
// WPS 演示（实测 12.1.0.28505）→ 两个类名圈定"文字 + 演示"。输入法跟前台
// 焦点走, GetForegroundWindow 即打字处（候选窗不抢前台）。引擎线程每键
// 调用, 微秒级。表格 XLMAIN（复刻 Excel）刻意不含——单元格编辑态 COM
// 盲区（Office 系通病: 编辑态对象模型挂起、无 Selection.Start 等价物）,
// 输入法打字恰在盲区, 落 TSF/历史兜底。
enum class OfficeKind { NONE, WRITER, PPT };
static OfficeKind foreground_office() {
  HWND fg = GetForegroundWindow();
  if (!fg)
    return OfficeKind::NONE;
  wchar_t cls[64];
  if (GetClassNameW(fg, cls, 64) <= 0)
    return OfficeKind::NONE;
  if (wcscmp(cls, L"OpusApp") == 0)
    return OfficeKind::WRITER;
  if (wcscmp(cls, L"PP12FrameClass") == 0)
    return OfficeKind::PPT;
  return OfficeKind::NONE;
}

// ---- IDispatch 精简封装 (与探针同款; 错误只返回 hr, 调用方决策) ----
static HRESULT disp_invoke(IDispatch *obj, const wchar_t *name, WORD flags,
                           DISPPARAMS *pdp, VARIANT *ret) {
  DISPID dispid = 0;
  HRESULT hr = obj->GetIDsOfNames(IID_NULL, (LPOLESTR *)&name, 1,
                                   LOCALE_USER_DEFAULT, &dispid);
  if (FAILED(hr))
    return hr;
  VariantInit(ret);
  return obj->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, pdp, ret,
                     nullptr, nullptr);
}
static HRESULT disp_get(IDispatch *obj, const wchar_t *name, VARIANT *ret) {
  DISPPARAMS dp;
  ZeroMemory(&dp, sizeof(dp));
  return disp_invoke(obj, name, DISPATCH_PROPERTYGET, &dp, ret);
}

// BSTR → UTF-8（文字/演示两条读链共用；失败时 out 不变返回 false）
static bool bstr_to_utf8(BSTR s, std::string &out) {
  if (!s)
    return false;
  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return false;
  out.resize(n - 1);
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return true;
}

static void wlower(wchar_t *s) {
  for (; *s; ++s)
    if (*s >= L'A' && *s <= L'Z')
      *s = *s + 32;
}

// 文档名 ⊂ 前台标题校验（WPS 统一标签页防串档，2026-09-11 真机两轮定案）：
// WPS 多组件标签共用单一框架窗口，前台类名恒为 OpusApp（实测演示标签激活
// 仍 OpusApp、标题变"演示文稿1 - WPS Office"）——类名门控区分不了活动
// 组件。帧标题随活动标签文档名变，组件模型的 ActiveDocument.Name /
// ActivePresentation.Name 固定指本组件自己的活动文档：名字（全名与去
// 扩展名两种形态）都不在前台标题中 = 活动标签是另一组件（或前台是微软
// Office 而后台 WPS 同开），本组件读出的文本不属于当前光标处，判
// TAB_IDLE 落 TSF/历史。锚点不可用 Window.Caption——实测带修改标记尾缀
// " *" 且无扩展名剥不掉（"文字文稿1 *" ⊄ "文字文稿1 - WPS Office"）恒
// 误杀。名字取不到/去扩展名后过短（单字符易误配）→ 保守放行。
static bool name_matches_fg_title(const VARIANT &vname) {
  if (vname.vt != VT_BSTR || !vname.bstrVal || !*vname.bstrVal)
    return true;
  wchar_t full[128] = {0}, stem[128] = {0};
  const wchar_t *base = wcsrchr(vname.bstrVal, L'\\');
  base = base ? base + 1 : vname.bstrVal;
  wcsncpy_s(full, base, _TRUNCATE);
  wcsncpy_s(stem, base, _TRUNCATE);
  wchar_t *dot = wcsrchr(stem, L'.');
  if (dot && dot != stem)
    *dot = 0;
  if (wcslen(stem) < 2)
    return true;
  HWND fg = GetForegroundWindow();
  wchar_t title[256] = {0};
  if (!fg || GetWindowTextW(fg, title, 256) <= 0)
    return true;  // 标题取不到（罕见），保守放行
  wlower(full);
  wlower(stem);
  wlower(title);
  return wcsstr(title, full) != nullptr || wcsstr(title, stem) != nullptr;
}

enum class ReadResult {
  OK,
  NO_DOC,
  DEAD,
  TAB_IDLE
};  // NO_DOC: 无文档(保留引用) / DEAD: RPC 断(WPS 退出) / TAB_IDLE:
    // 附着正常但活动标签非本组件（WPS 统一标签页串档拦截）

// 活动标签判定（不依赖文件名，2026-09-11 三轮真机定案）：文字组件视图
// 窗口（_WwB 类，ActiveWindow.Hwnd）的根窗口 == 前台窗口 && 视图可见
// = 文字标签/文字窗口活动。统一标签页：视图随活动标签显隐（实测即时
// 翻转）；多独立窗口：视图都可见，但只有活动窗口的根 == 前台（他窗/
// 他应用前台时根不等——微软 Word 前台场景由此天然排除，无需名字）。
// 句柄取不到 = UNKNOWN（回落名字校验兜底）。
enum class TabActive { YES, NO, UNKNOWN };
static TabActive writer_tab_active(IDispatch *app_w) {
  if (!app_w)
    return TabActive::UNKNOWN;
  VARIANT vwin, vh;
  VariantInit(&vwin); VariantInit(&vh);
  long long h = 0;
  if (SUCCEEDED(disp_get(app_w, L"ActiveWindow", &vwin)) &&
      vwin.vt == VT_DISPATCH && vwin.pdispVal &&
      SUCCEEDED(disp_get(vwin.pdispVal, L"Hwnd", &vh))) {
    if (vh.vt == VT_I4)
      h = vh.lVal;
    else if (vh.vt == VT_I8)
      h = vh.llVal;
    else if (vh.vt == VT_R8)
      h = (long long)vh.dblVal;
  }
  VariantClear(&vwin); VariantClear(&vh);
  if (!h)
    return TabActive::UNKNOWN;
  HWND root = GetAncestor((HWND)(LONG_PTR)h, GA_ROOT);
  HWND fg = GetForegroundWindow();
  if (!root || !fg)
    return TabActive::UNKNOWN;
  if (root != fg)
    return TabActive::NO;
  return IsWindowVisible((HWND)(LONG_PTR)h) ? TabActive::YES : TabActive::NO;
}

// ---- 演示读链（WPP/PowerPoint，2026-09-11 本机实测验证）----
// app.ActiveWindow.Selection.Type==3(文本编辑) 时：start = TextRange.Start
//（框内 1-based 偏移）；前文 = ShapeRange.TextFrame.TextRange.Characters(
// start-len, len).Text（len = min(64, start-1)）。Type!=3 → NO_DOC
//（形状/幻灯片选择态）。表格 XLMAIN 不在此链（编辑态盲区）。
static ReadResult ppt_read_chain(IDispatch *app, std::string &out,
                                 TabActive wta) {
  VARIANT vwin, vsel, vtype, vstart, vshape, vframe, vrng, vtxt;
  VARIANT vpres, vname;
  VariantInit(&vwin); VariantInit(&vsel); VariantInit(&vtype);
  VariantInit(&vstart); VariantInit(&vshape); VariantInit(&vframe);
  VariantInit(&vrng); VariantInit(&vtxt);
  VariantInit(&vpres); VariantInit(&vname);
  auto vt_ok = [](const VARIANT &v) {
    return v.vt == VT_DISPATCH && v.pdispVal;
  };
  HRESULT hr;
  auto cleanup = [&]() {
    VariantClear(&vwin); VariantClear(&vsel); VariantClear(&vtype);
    VariantClear(&vstart); VariantClear(&vshape); VariantClear(&vframe);
    VariantClear(&vrng); VariantClear(&vtxt);
    VariantClear(&vpres); VariantClear(&vname);
  };
  // 串档拦截①：文字视图判定活动 = 活动标签必是文字（演示/表格都不是）
  if (wta == TabActive::YES) {
    cleanup();
    return ReadResult::TAB_IDLE;
  }
  if (FAILED(hr = disp_get(app, L"ActiveWindow", &vwin)) || !vt_ok(vwin)) {
    cleanup();
    if (hr == DISP_E_EXCEPTION)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  // 串档拦截②：演示无窗口句柄（WPP Window.Hwnd/HWND 均空，实测），
  // 名字校验是演示链主判据——挡表格标签/微软 Office 前台；同基名文档
  // 场景由拦截①的 wta 覆盖
  if (FAILED(hr = disp_get(app, L"ActivePresentation", &vpres)) ||
      !vt_ok(vpres)) {
    cleanup();
    if (hr == DISP_E_EXCEPTION)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  disp_get(vpres.pdispVal, L"Name", &vname);
  if (!name_matches_fg_title(vname)) {
    cleanup();
    return ReadResult::TAB_IDLE;
  }
  if (FAILED(hr = disp_get(vwin.pdispVal, L"Selection", &vsel)) ||
      !vt_ok(vsel) ||
      FAILED(hr = disp_get(vsel.pdispVal, L"Type", &vtype)) ||
      vtype.vt != VT_I4) {
    cleanup();
    if (hr == DISP_E_EXCEPTION)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  if (vtype.lVal != 3) {  // ppSelectionText = 3：非文本编辑态
    cleanup();
    return ReadResult::NO_DOC;
  }
  ReadResult res = ReadResult::OK;
  do {
    if (FAILED(hr = disp_get(vsel.pdispVal, L"TextRange", &vrng)) ||
        !vt_ok(vrng) ||
        FAILED(hr = disp_get(vrng.pdispVal, L"Start", &vstart)) ||
        vstart.vt != VT_I4) {
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    long start = vstart.lVal;                     // 1-based
    long len = start - 1 > 64 ? 64 : start - 1;   // 前文字数（截 64）
    if (len <= 0) { out.clear(); break; }  // 光标在框首：前文真空
    if (FAILED(hr = disp_get(vsel.pdispVal, L"ShapeRange", &vshape)) ||
        !vt_ok(vshape) ||
        FAILED(hr = disp_get(vshape.pdispVal, L"TextFrame", &vframe)) ||
        !vt_ok(vframe)) {
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    VARIANT vfull;
    VariantInit(&vfull);
    if (FAILED(hr = disp_get(vframe.pdispVal, L"TextRange", &vfull)) ||
        !vt_ok(vfull)) {
      VariantClear(&vfull);
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    // Characters(start_index, length)：rgvark 反序 [length, start_index]
    VARIANT args[2];
    VariantInit(&args[0]); args[0].vt = VT_I4; args[0].lVal = len;
    VariantInit(&args[1]); args[1].vt = VT_I4;
    args[1].lVal = start - len;                   // 起始字符 = start-len
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    dp.rgvarg = args; dp.cArgs = 2;
    VariantClear(&vrng);  // 复用作出参：先释放首个 TextRange 引用
    if (FAILED(hr = disp_invoke(vfull.pdispVal, L"Characters",
                                DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                &dp, &vrng)) || !vt_ok(vrng)) {
      VariantClear(&vfull);
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    VariantClear(&vfull);
    if (FAILED(hr = disp_get(vrng.pdispVal, L"Text", &vtxt)) ||
        vtxt.vt != VT_BSTR) {
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    bstr_to_utf8(vtxt.bstrVal, out);
  } while (false);
  cleanup();
  return res;
}

static ReadResult read_chain(IDispatch *app, std::string &out,
                             TabActive wta) {
  // app.ActiveWindow → win.Selection → sel.Start → app.ActiveDocument →
  // doc.Range(pos-64, pos).Text   (rgvark 反序: 末参数在前)
  VARIANT vwin, vsel, vstart, vdoc, vrng, vtxt, vname;
  VariantInit(&vwin); VariantInit(&vsel); VariantInit(&vstart);
  VariantInit(&vdoc); VariantInit(&vrng); VariantInit(&vtxt);
  VariantInit(&vname);
  auto vt_ok = [](const VARIANT &v) { return v.vt == VT_DISPATCH && v.pdispVal; };
  HRESULT hr;
  auto cleanup = [&]() {
    VariantClear(&vwin); VariantClear(&vsel); VariantClear(&vstart);
    VariantClear(&vdoc); VariantClear(&vrng); VariantClear(&vtxt);
    VariantClear(&vname);
  };
  // 串档拦截①：视图窗口判定非活动（统一标签页切走/他窗前台）——
  // 不依赖文件名，同基名文档也能正确区分
  if (wta == TabActive::NO) {
    cleanup();
    return ReadResult::TAB_IDLE;
  }
  if (FAILED(hr = disp_get(app, L"ActiveWindow", &vwin)) || !vt_ok(vwin)) {
    cleanup();
    // RPC 层失败 (hr 为 RPC_*/CO_*) → WPS 进程不在; DISP 异常 → 无文档
    if (hr == DISP_E_EXCEPTION || hr == DISP_E_MEMBERNOTFOUND)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  // ActiveDocument 前移：名字校验（串档拦截②兜底）与下方 Range 共用
  if (FAILED(hr = disp_get(app, L"ActiveDocument", &vdoc)) || !vt_ok(vdoc)) {
    cleanup();
    if (hr == DISP_E_EXCEPTION || hr == DISP_E_MEMBERNOTFOUND)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  if (wta == TabActive::UNKNOWN) {
    // 视图句柄取不到时回落名字校验（YES 时有 root==fg 硬判据，跳过
    // 免受标题格式差异影响——Caption 轮误杀的教训）
    disp_get(vdoc.pdispVal, L"Name", &vname);
    if (!name_matches_fg_title(vname)) {
      cleanup();
      return ReadResult::TAB_IDLE;
    }
  }
  if (FAILED(hr = disp_get(vwin.pdispVal, L"Selection", &vsel)) ||
      !vt_ok(vsel) ||
      FAILED(hr = disp_get(vsel.pdispVal, L"Start", &vstart)) ||
      vstart.vt != VT_I4) {
    cleanup();
    if (hr == DISP_E_EXCEPTION || hr == DISP_E_MEMBERNOTFOUND)
      return ReadResult::NO_DOC;
    return ReadResult::DEAD;
  }
  long pos = vstart.lVal, from = pos - 64;
  if (from < 0) from = 0;
  ReadResult res = ReadResult::OK;
  do {
    VARIANT args[2];  // rgvark 反序: 末参数 (pos) 在前
    VariantInit(&args[0]); args[0].vt = VT_I4; args[0].lVal = pos;
    VariantInit(&args[1]); args[1].vt = VT_I4; args[1].lVal = from;
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    dp.rgvarg = args; dp.cArgs = 2;
    if (FAILED(hr = disp_invoke(vdoc.pdispVal, L"Range",
                                DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                &dp, &vrng)) || !vt_ok(vrng)) {
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    if (FAILED(hr = disp_get(vrng.pdispVal, L"Text", &vtxt)) ||
        vtxt.vt != VT_BSTR) {
      res = (hr == DISP_E_EXCEPTION) ? ReadResult::NO_DOC : ReadResult::DEAD;
      break;
    }
    bstr_to_utf8(vtxt.bstrVal, out);
  } while (false);
  cleanup();
  return res;
}

static void thread_proc() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  // 仅 WPS 系 ProgID（2026-09-11 定案）：COM 旁路只补 TSF/UIA 墙上唯一
  // 缺口=WPS——微软 Word/PPT 的 TSF store 完整（事件驱动时序优于 2s 轮询），
  // 无需 COM。Word/PPT 的 ProgID 回落已撤（共存双注册误附风险归零）。
  // 注意"附着成功 = 对的组件"已被 2026-09-11 串档真机证伪：WPS 统一标签
  // 页前台类名恒 OpusApp（演示标签激活也不变），Kwps 附着成功但活动标签
  // 是演示 → 读出文字文档冒充上文——组件消解改由读链标题校验（见
  // caption_matches_fg_title）+ 双附着探读（见下方循环）完成；前台是
  // 微软 Word 而后台 WPS 同开的场景同理由标题校验拦截。
  CLSID clsid_kwps, clsid_kwpp;
  bool has_kwps = SUCCEEDED(CLSIDFromProgID(L"Kwps.Application", &clsid_kwps));
  bool has_kwpp = SUCCEEDED(CLSIDFromProgID(L"KWPP.Application", &clsid_kwpp));
  if (!has_kwps && !has_kwpp) {
    CoUninitialize();  // 与 CoInitializeEx 配对（未装 WPS，旁路停用）
    return;
  }
  // 双组件持久附着 + 探读（2026-09-11 串档修复）：WPS 统一标签页的框架
  // 类名不随活动标签组件变（真机实测演示标签激活仍 OpusApp），旧"前台
  // 类名选单附着"设计失效——文字附着会一直读出文字文档全文冒充演示上
  // 文。改为 Kwps/KWPP 双附着，每轮按"前台类型对位优先、另一组件兜底"
  // 顺序读，由读链标题校验（TAB_IDLE）裁决哪个组件的活动文档才是当前
  // 光标处。DEAD（宿主退出）单独释放该组件下轮懒重附；双双无产出 →
  // 清缓存（消费端落 TSF/历史），未附着时 500ms 快速重试不变。
  IDispatch *app_w = nullptr;  // Kwps.Application（文字）
  IDispatch *app_p = nullptr;  // KWPP.Application（演示）
  bool logged_w = false, logged_p = false;
  bool com_pending = true;  // 前台 Office 且未附着 → 500ms 快速重试（文档
                            // 打开瞬间 WPS 才注册 ROT，缩短发现间隙；附着
                            // 成功回 2s 周期，开销仅微秒级 ROT 查询）
  for (;;) {
    // 周期 2s 兜底 (任何时刻缓存 age ≤ ~2s) + kick 即时唤醒 (OnCommit)
    {
      std::unique_lock<std::mutex> lk(g_cv_mu);
      g_cv.wait_for(lk, std::chrono::milliseconds(
                            g_kick.load() ? 1 : (com_pending ? 500 : 2000)));
    }
    if (!g_com_ctx_enabled) {
      std::unique_lock<std::mutex> lk(g_cv_mu);
      g_cv.wait_for(lk, std::chrono::seconds(5));
      continue;
    }
    bool woken_by_kick = g_kick.exchange(false);
    com_pending = false;
    // kick 统一延迟 ~50ms 再读（2026-09-12 信号层统一，与插件版同源）：
    // 编辑键/点击的 kick 若立即读会赶在应用处理完退格/点击之前拿到旧
    // 文本；commit kick 的文档更新同量级。下一键远晚于 100ms（人手速度），
    // 统一延迟无感；连续 kick 在 sleep 期间再次置位，下一轮 1ms 即醒合并。
    if (woken_by_kick) Sleep(50);
    // 读端前台判定（2026-09-10 与插件版对齐）：前台非 Office 跳过 COM 读
    //（消费端已有前台门控，旧缓存不会被误用——不清也安全；跳过读省
    // Office 后台挂机时的 0.3% CPU）
    OfficeKind fg = foreground_office();
    if (fg == OfficeKind::NONE) {
      continue;  // 回到 wait（正常周期），本轮不做 COM 读
    }
    // 文字附着先行：ActiveWindow.Hwnd 的视图窗口判定是活动标签主信号
    //（无文字文档时 UNKNOWN 回落名字校验）
    if (has_kwps && !app_w) {
      IUnknown *punk = nullptr;
      if (SUCCEEDED(GetActiveObject(clsid_kwps, nullptr, &punk)) && punk) {
        punk->QueryInterface(IID_IDispatch, (void **)&app_w);
        punk->Release();
        if (app_w && !logged_w) {
          log_msg("com ctx: attached via ROT (Kwps.Application)");
          logged_w = true;
        }
      }
    }
    TabActive wta = writer_tab_active(app_w);
    if (wta == TabActive::NO && app_w) {
      // 跨进程标签切换重附（2026-09-11 四轮真机定案）：新建文档/演示
      // 标签各起新 wps.exe、各持帧窗口，仅活动标签所属进程的帧可见——
      // 旧附着实例的视图 root≠前台恒 NO。实测 GetActiveObject 返回最新
      // 激活实例（sees 跟随活动标签切换），重附一次即取到活动实例，仍
      // NO 才真非活动（表格标签/他应用前台）。GetActiveObject 微秒级，
      // 每轮至多重附一次。
      app_w->Release();
      app_w = nullptr;
      if (has_kwps) {
        IUnknown *punk = nullptr;
        if (SUCCEEDED(GetActiveObject(clsid_kwps, nullptr, &punk)) && punk) {
          punk->QueryInterface(IID_IDispatch, (void **)&app_w);
          punk->Release();
        }
      }
      wta = writer_tab_active(app_w);
    }
    // 前台类型对位优先、另一组件兜底（统一标签页两类都可能藏在其后）
    OfficeKind order[2] = {fg, (fg == OfficeKind::WRITER) ? OfficeKind::PPT
                                                          : OfficeKind::WRITER};
    bool published = false;
    for (int i = 0; i < 2 && !published; i++) {
      bool is_p = (order[i] == OfficeKind::PPT);
      if (is_p ? !has_kwpp : !has_kwps)
        continue;
      IDispatch *&app = is_p ? app_p : app_w;
      bool &logged = is_p ? logged_p : logged_w;
      if (!app) {
        IUnknown *punk = nullptr;
        if (SUCCEEDED(GetActiveObject(is_p ? clsid_kwpp : clsid_kwps, nullptr,
                                      &punk)) &&
            punk) {
          punk->QueryInterface(IID_IDispatch, (void **)&app);
          punk->Release();
        }
        if (app && !logged) {
          log_msg("com ctx: attached via ROT (%s)",
                  is_p ? "KWPP.Application" : "Kwps.Application");
          logged = true;
        }
      }
      if (!app)
        continue;
      std::string txt;
      ReadResult r =
          is_p ? ppt_read_chain(app, txt, wta) : read_chain(app, txt, wta);
      if (r == ReadResult::OK) {
        wchar_t wt[256] = {0};
        HWND fh = GetForegroundWindow();
        if (fh)
          GetWindowTextW(fh, wt, 256);
        std::lock_guard<std::mutex> lk(g_mu);
        g_text = txt;
        g_stamp = GetTickCount64();
        g_title = wt;  // 消费端指纹: 标题变 = 切标签/切窗
        published = true;
      } else if (r == ReadResult::DEAD) {  // 宿主退出 (WPS/Word/WPP): 丢弃重附
        if (logged) {
          log_msg("com ctx: office app gone, will re-attach");
          logged = false;
        }
        app->Release();
        app = nullptr;
      }
      // NO_DOC (文档全关/演示非文本编辑态) 与 TAB_IDLE (活动标签非本
      // 组件): 保留 app 引用, 试另一组件
    }
    if (!published) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_text.clear();
      g_stamp = 0;
      g_title.clear();
    }
    com_pending = (app_w == nullptr && app_p == nullptr);  // 全未附着才快试
  }
}

#ifdef _WIN32
static void llm_context_reset_hook(void*, const char*);  // 定义在 invalidate/kick 之后
#endif
static void ensure_started() {  // 首次粘性降级命中时调用 (引擎线程)
  bool expect = false;
  if (g_started.compare_exchange_strong(expect, true)) {
    // 编辑键 reset 钩子 (2026-09-14 信号时机对齐插件版): server IPC 收到
    // ResetContext 递增代次的瞬间 (RimeResetContextText) 即 invalidate+kick
    // ——只挂 GetContextTextPair (满码评分时) 比插件版 processor 按键回调
    // 慢一拍, 编辑后首词 ctx 空不推理。回调跑在 server IPC 线程锁外,
    // invalidate (mutex) + kick (atomic+cv) 无阻塞微秒级。注册放这里而
    // 非静态构造: 跨 TU 静态初始化顺序不定 (api.cc 的 g_context_text_
    // mutex 可能未构造), 首次使用时一切静态早已就绪; 代价是首个评分前
    // 的编辑键 miss 一次 (窗口极小)。
#ifdef _WIN32
    RimeSetContextResetHook(&llm_context_reset_hook, nullptr);
#endif
    std::thread(thread_proc).detach();
  }
}
static void kick() { g_kick.store(true); g_cv.notify_all(); }  // OnCommit 后
// 任何"光标前文本可能变了"的已知信号（编辑键/点击，2026-09-12 信号层
// 统一）：成对失效快照 + kick 延迟重读（~50ms）。失效保证读回窗口内
// 旧快照不冒充（消费端落 TSF/历史），kick 保证应用处理完编辑后读到新
// 真文。TSF 推送制本身事件自愈无需此信号——受益者是 COM 快照（原本
// 对非 commit 编辑事件是盲区，旧快照 2.5s 新鲜窗内冒充真文）。
static void invalidate() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_text.clear();
  g_stamp = 0;
  g_title.clear();
}
#ifdef _WIN32
static void llm_context_reset_hook(void*, const char* reason) {
  comctx::invalidate();
  comctx::kick();
  write_m(reason);  // 常开语料 M 事件 + 按需 debug R 行
  commit_stack_refund_or_clear(reason);  // 回撤退款/边界清栈
}
#endif

// 引擎线程消费: 新鲜窗口内的缓存文本 (空串 = 不可用 → 调用方回落 hist)。
// 标题指纹：发布后前台标题变了（WPS 切标签/切窗即换标题）→ 旧快照属于
// 旧标签，拒用——挡住 ≤2s 轮询间隙的串档（OnCommit kick 会立刻换上新
// 标签文本）。任一侧标题取不到 → 保守放行。
static std::string snapshot(unsigned max_age_ms) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_stamp && GetTickCount64() - g_stamp <= max_age_ms) {
    wchar_t t[256] = {0};
    HWND fh = GetForegroundWindow();
    bool title_ok = (!fh || GetWindowTextW(fh, t, 256) <= 0);
    if (title_ok || g_title.empty() || g_title == t)
      return g_text;
  }
  return std::string();
}

}  // namespace comctx
#endif  // _WIN32

// ============================================================
// prepare pre-decode state: after commit, asynchronously run
// Step 1 (ctx decode -> save logits) so the next score call can
// skip Step 1 when the ctx matches.
// ============================================================
static std::vector<llama_token> g_prep_ctx;     // pre-decoded ctx tokens
static std::vector<float>       g_prep_logits;  // ctx_last logits
static float                    g_prep_m = 0;   // ctx_last logits 的 softmax max（随 prep 缓存，命中路径免 O(vocab) 重扫）
static double                   g_prep_lse = 0; // ctx_last logits 的 logsumexp（同上）
static bool                     g_prep_ready = false;
static std::atomic<int>         g_prep_seq{0};  // request seq, stale requests skip
static long                     g_seq0_gen = 0; // seq0 KV generation: bumped by any decode covering seq0
static long                     g_prep_gen = 0; // generation at which prep was produced

// 2026-11-18: prepare() used to silently drop its request when the inference
// lock was held (try_to_lock). Nothing retried it, so the next full-code score
// paid the whole S1 (measured 58-136ms on real logs). Remember the dropped
// request and re-run it right after score_batch releases the lock.
static std::mutex                g_prep_pending_mu;
static std::vector<llama_token>  g_prep_pending_ctx;
static int                       g_prep_pending_seq = -1;
static void request_prepare_retry(const std::vector<llama_token> &ids,
                                  int seq) {
  std::lock_guard<std::mutex> lk(g_prep_pending_mu);
  g_prep_pending_ctx = ids;
  g_prep_pending_seq = seq;
}
static bool take_prepare_retry(std::vector<llama_token> &out, int &seq) {
  std::lock_guard<std::mutex> lk(g_prep_pending_mu);
  if (g_prep_pending_seq < 0)
    return false;
  out = std::move(g_prep_pending_ctx);
  seq = g_prep_pending_seq;
  g_prep_pending_ctx.clear();
  g_prep_pending_seq = -1;
  return true;
}

// ============================================================
// score result cache: same (ctx, input) reuses the previous rerank
// (翻页/候选窗重建不重复推理 — 对齐插件版 _G.llm_filter_cache)。
// 只存评分顺序（候选文本），词频融合/两码整句排序与 AI 徽章每次
// 按当前配置重放，改参数后重新部署缓存仍正确。reset 代次变
// （编辑键/窗口切换）→ 失效；ctx/input 变 → key 不匹配自然失效。
// engine 线程专用（Apply/Collect 均在引擎线程），无锁。
// ============================================================
// 评分结果缓存已删除（2026-11-11 用户定案）：同 (上文+编码) 的复用在生产里
// 命中率极低（每个新词的上文都变），却要维护一整套失效逻辑（编辑键/切窗代次/
// 候选集变化重放），删掉以简化。仍保留 reset 代次，仅用于调试窗口的"上文重置"
// 事件（编辑键/切窗信号本身对排障有用）。
static int                       s_seen_gen = -1;  // 上次见到的 reset 代次

// ============================================================
// 用户词频账本（2026-11-17 用户定案：衰减账 + 累计账合并为单文件单账本）:
// 两本账原本键相同（词\t码）、计数时机相同（OnCommit）、落盘节奏相同
// （每 20 词全量重写）——合并后一个 map、一次重写、单一格式，消除双文件
// 跨文件漂移与"同形异义格式"误读（2026-11-16 累计账读取事故）。
// engine thread only, 无锁。衰减 = librime algo::formula_d (引擎调频同源):
//   提交: dee = 1 + dee·exp((t_old - t_now)/τ)
//   查询: eff = dee·exp((t_word - t_now)/τ)   未提交期间持续衰减
//   τ=400; tick 每词提交 +1 (同 userdb UpdateTickCount)；半衰 ≈277 次上屏
// （2026-11-06 v3 六维联合标定；旧 800 出自 2026-11-02 连续文章重标定）。
// 累计计数 cum 不衰减（log(cum+1) 融合项）。
// 文件: RIME 用户目录 user_ledger.tsv（两版共用）
//   首行 "#tick=N"; 随后 # 注释行（列含义, 落盘自带）; 数据行
//   "词\t码\tdee\ttick\tcum"（2026-11-20 去掉行类型列 s——每行省 2 字节,
//   词必非 ASCII 不会与注释/旧格式混淆; 码可空）。
//   读取端兼容旧 6 列 s 行（s\t词\t码\tdee\ttick\tcum, 2026-11-20 前）。
//   修剪: eff<1e-3 且 cum=0 才丢——含中文词条终身保留（旧累计账语义），
//   全角标点等非中文行随衰减淘汰。
// 旧两账本读取端一次性迁移（只读, 旧文件保留不删；落盘写新名）:
//   user_eff.tsv（回退 user_freq.tsv）5 字段 → dee/tick, cum=commits(含中文时)
//   user_unigram.tsv（回退 user_bigram.tsv）u 行 → cum=max(cum, 次数)
//   eff 的 commits 列对含中文词条 = 真实累计次数（同一提交两处计数），
//   取 max 可补回 2026-11-16 读取事故丢失的累计次数。
// n_seen 不再记录（2026-11-17 用户定案：不参与打分, 无消费方）。
// ============================================================
struct UserStatEntry {
  double dee = 0;        // 衰减计数 (formula_d 的 dee)
  long long tick = 0;    // 最后提交 tick
  long long cum = 0;     // 累计提交次数 (原始计数不衰减; 含中文词条)
  // [回撤退款]（2026-11-20）bump 前快照浅栈（cap 4）——formula_d 递推
  // 确定性，恢复前像即精确退款；栈只在内存，跨重启为空则不退（回退现
  // 行为）。
  struct Snap {
    long long tick = 0;  // 该次 bump 的 tick（与 C 行 tick 对账）
    double dee = 0;      // bump 前 dee
    long long t = 0;     // bump 前 tick
    long long cum = 0;   // bump 前 cum
  };
  std::vector<Snap> hist;
};
static std::map<std::string, UserStatEntry> g_user_stats;
static long long g_user_tick = 0;
static bool g_user_stats_loaded = false;
// 内存账本所属方案（2026-11-22 按方案分目录）：切方案时旧账本先落盘到
// **旧方案**目录，再清空重载新方案账本——否则新方案保存会把旧方案条目
// 写进新文件（两套方案混写）
static std::string g_stats_schema;
static void user_stats_save_to(const std::string &sid);  // 定义在后
static int  g_user_stats_dirty = 0;
// [回撤退款] 引擎线程 bump/查询 × server IPC 线程退款（reset hook）→
// 账本统一互斥（recursive：bump→ensure_loaded/save 嵌套加锁）。
static std::recursive_mutex g_ledger_mu;
static constexpr double kFreqTau = 400.0;  // rime formula_d 时间常数 (v3 2026-11-06 六维联合标定; 旧 800 出自 2026-11-02)

// 日志轮转（2026-09-04）：超过 limit 改名 .old（单文件轮转）防无限增长；
// 改名失败（被占等罕见）则继续追加，不丢日志。
static void rotate_if_large(const char *path, unsigned long long limit) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) &&
      ((((unsigned long long)fa.nFileSizeHigh << 32) |
        (unsigned long long)fa.nFileSizeLow) > limit)) {
    std::string oldp = std::string(path) + ".old";
    DeleteFileA(oldp.c_str());
    MoveFileA(path, oldp.c_str());
  }
}

// 旧 open_log_file 已由下方 log_file_path/corpus_write_raw/debug_write_raw
// 取代（2026-11-17 记录重构）。保留此实现仅供旧调用兼容，新代码不再使用。
static FILE *open_log_file(const char *filename) {
  char path[MAX_PATH];
  const RimeApi *api = rime_get_api();
  if (api && api->get_user_data_dir) {
    const char *ud = api->get_user_data_dir();
    if (ud && *ud && strlen(ud) < MAX_PATH - 64) {
      snprintf(path, sizeof(path), "%s\\%s", ud, filename);
      rotate_if_large(path, 5ull * 1024 * 1024);
      return fopen(path, "a");
    }
  }
#ifdef _WIN32
  GetTempPathA(sizeof(path), path);
  strncat(path, filename, sizeof(path) - strlen(path) - 1);
  rotate_if_large(path, 5ull * 1024 * 1024);
  return fopen(path, "a");
#else
  (void)path;
  return nullptr;
#endif
}

// ── 方案子目录（2026-11-22 用户定案：四件全分）───────────────────────
// 目录 = <用户文件夹>\llm_rerank\<方案 ID>\：corpus/debug/账本/词码对
// 四个文件全部按方案隔离（不同方案混写同一语料/日志会让分析无法按方案
// 切分）。迁移规则："有状态资产"（账本/词码对）在该方案首次读写时从旧
// 位置**复制**一份（旧文件保留不删）——每个方案各得一份快照，此后独立
// 演进（键 = 词+码，码与方案绑定、天然不相交，历史条目跨方案共存无害）；
// 日志（corpus/debug）不复制，该方案从空文件开始写（否则每方案各背一份
// 全量历史，违背分目录初衷；旧混合日志留作历史）。
static std::mutex g_schema_mu;
static std::string g_schema_dir;

static std::string schema_dir_sanitized(const std::string &sid) {
  if (sid.empty())
    return "";
  std::string out;
  for (size_t i = 0; i < sid.size() && i < 64; i++) {
    unsigned char c = (unsigned char)sid[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    out.push_back(ok ? (char)c : '_');
  }
  return out;
}

// LoadSchema / Apply 刷新（方案切换 = 新 session → 新 schema_id）
static void llm_set_schema_id(const std::string &sid) {
  std::string s = schema_dir_sanitized(sid);
  std::lock_guard<std::mutex> lk(g_schema_mu);
  if (g_schema_dir != s)
    g_schema_dir = s;
}

static std::string llm_schema_dir_name() {
  std::lock_guard<std::mutex> lk(g_schema_mu);
  return g_schema_dir;  // 空 = 方案不可得 → 调用方退回 llm_rerank 根
}

static bool llm_dir_ensure(const char *dir) {
  if (!CreateDirectoryA(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    return false;
  return true;
}

// 解析 filename 的最终路径：<llm_rerank>\<方案>\filename。
// migrate=true（账本/词码对）→ 该方案首用时从旧位置（llm_rerank 根 /
// 用户文件夹根）复制一份，旧文件保留；复制失败暂用旧位置，下次再试。
// 返回 false = 拿不到用户目录（调用方自行回退）。
static bool llm_file_path_ex(const char *filename, char *path, size_t n,
                             bool migrate, const std::string &sid) {
  const RimeApi *api = rime_get_api();
  if (!api || !api->get_user_data_dir) return false;
  const char *ud = api->get_user_data_dir();
  if (!ud || !*ud || strlen(ud) >= n - 80) return false;
  char root[MAX_PATH], dir[MAX_PATH];
  snprintf(root, sizeof(root), "%s\\llm_rerank", ud);
  // 方案不可得 → 直接用 llm_rerank 根（部署期/崩溃恢复等边缘场景，旧行为）
  if (sid.empty())
    snprintf(dir, sizeof(dir), "%s", root);
  else
    snprintf(dir, sizeof(dir), "%s\\%s", root, sid.c_str());
  snprintf(path, n, "%s\\%s", dir, filename);
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
    return true;  // 该方案文件已有 → 直接用
  if (migrate) {
    // 旧位置 1 = llm_rerank 根（方案层还不存在时）；
    // 旧位置 2 = 用户文件夹根
    for (int i = 0; i < 2; i++) {
      const char *src = (i == 0) ? root : ud;
      char oldp[MAX_PATH];
      snprintf(oldp, sizeof(oldp), "%s\\%s", src, filename);
      if (strcmp(oldp, path) == 0)   // 目标即旧位置（方案不可得）
        continue;
      if (GetFileAttributesA(oldp) == INVALID_FILE_ATTRIBUTES)
        continue;
      if (llm_dir_ensure(root) && llm_dir_ensure(dir) &&
          CopyFileA(oldp, path, TRUE)) {
        // 顺带复制 .old 轮转副本（有则复制，无则无操作）
        char oldo[MAX_PATH], newo[MAX_PATH];
        snprintf(oldo, sizeof(oldo), "%s.old", oldp);
        snprintf(newo, sizeof(newo), "%s.old", path);
        if (GetFileAttributesA(oldo) != INVALID_FILE_ATTRIBUTES)
          CopyFileA(oldo, newo, TRUE);
        return true;
      }
      snprintf(path, n, "%s", oldp);  // 复制失败：暂用旧位置，下次再试
      return true;
    }
  }
  if (!llm_dir_ensure(root) || !llm_dir_ensure(dir)) {
    // 目录建不出（权限异常等）：退回用户文件夹根原位置，绝不丢数据
    snprintf(path, n, "%s\\%s", ud, filename);
    return true;
  }
  return true;
}

// 日志（corpus/debug）：不迁移，该方案独享新文件
static bool llm_file_path(const char *filename, char *path, size_t n) {
  return llm_file_path_ex(filename, path, n, false, llm_schema_dir_name());
}

// 有状态资产（账本/词码对）：首用复制快照
static bool llm_state_file_path(const char *filename, char *path, size_t n) {
  return llm_file_path_ex(filename, path, n, true, llm_schema_dir_name());
}

// 指定方案的账本/词码对路径（切方案时把**旧方案**内存账本落盘到旧目录）
static bool llm_state_file_path_for(const std::string &sid,
                                    const char *filename, char *path,
                                    size_t n) {
  return llm_file_path_ex(filename, path, n, true, sid);
}

// user_ledger.tsv 路径 (RIME 用户目录 llm_rerank 子目录; 与日志同目录解析)。
// 文件名（2026-11-17 用户定案）：user_ledger.tsv = 合并账本（衰减 dee +
// 累计 cum 同一行）；旧 user_eff.tsv / user_unigram.tsv 由读取端一次性
// 迁移（只读, 旧文件保留不删），落盘一律写新名。
static bool user_ledger_file(char *path, size_t n) {
  // 有状态资产：该方案首次使用时从旧位置复制快照（2026-11-22 分目录）
  return llm_state_file_path("user_ledger.tsv", path, n);
}

// 指定方案的账本路径（切方案落盘旧账本用，2026-11-22）
static bool user_ledger_file_for(const std::string &sid, char *path,
                                 size_t n) {
  return llm_state_file_path_for(sid, "user_ledger.tsv", path, n);
}

// 含中文字符判定（U+4E00–U+9FFF；UTF-8 三字节首字节 E4–E9）。
static bool text_has_cjk_utf8(const std::string &w) {
  size_t i = 0;
  while (i < w.size()) {
    unsigned char c = (unsigned char)w[i];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2
            : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len == 3 && c >= 0xE4 && c <= 0xE9 && i + 2 < w.size() &&
        (unsigned char)w[i + 1] >= 0x80 && (unsigned char)w[i + 2] >= 0x80)
      return true;
    i += len;
  }
  return false;
}

// 按名字依次回退打开用户目录文件（新名优先、旧名兜底）。
static FILE *open_user_file(const char *const *names, int count) {
  const RimeApi *api = rime_get_api();
  if (!api || !api->get_user_data_dir)
    return nullptr;
  const char *ud = api->get_user_data_dir();
  if (!ud || !*ud)
    return nullptr;
  char path[MAX_PATH];
  for (int i = 0; i < count; i++) {
    snprintf(path, sizeof(path), "%s\\%s", ud, names[i]);
    if (FILE *f = fopen(path, "r"))
      return f;
  }
  return nullptr;
}

// 旧衰减账（user_eff.tsv / user_freq.tsv）: 5 字段 词\t码\tcommits\tdee\ttick。
// commits 对含中文词条 = 真实累计提交次数（两处计数同源同一提交），迁移时
// 直接作为 cum——可补回 2026-11-16 累计账读取事故丢失的次数。旧词级行
// （4/2 字段）不再迁移：分键语义下无法拆分到词码对。
static void user_stats_load_legacy_eff() {
  static const char *names[] = {"user_eff.tsv", "user_freq.tsv"};
  FILE *f = open_user_file(names, 2);
  if (!f)
    return;
  char line[1024];
  long long max_tick = 0, tick_hdr = 0;
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (strncmp(line, "#tick=", 6) == 0) {
      tick_hdr = atoll(line + 6);
      continue;
    }
    char *p[6];
    int nf = 0;
    p[nf++] = line;
    while (nf < 6) {
      char *t = strchr(p[nf - 1], '\t');
      if (!t)
        break;
      *t = 0;
      p[nf++] = t + 1;
    }
    if (nf < 5)
      continue;
    UserStatEntry e;
    e.dee = atof(p[3]);
    e.tick = atoll(p[4]);
    e.cum = text_has_cjk_utf8(p[0]) ? atoll(p[2]) : 0;
    if (e.dee > 0 || e.cum > 0) {
      g_user_stats[std::string(p[0]) + "\t" + p[1]] = e;
      if (e.tick > max_tick)
        max_tick = e.tick;
    }
  }
  fclose(f);
  if (tick_hdr > 0)
    g_user_tick = tick_hdr;
  else if (g_user_tick <= 0)
    g_user_tick = max_tick;
}

// 旧累计账（user_unigram.tsv / user_bigram.tsv）: u\t词\t码\t次数。
// 旧行照原值接管（含非中文行, 保持迁移前评分口径）；新提交只对含中文
// 词条递增 cum。3 字段旧词级 u 行与旧 b 行忽略（合并口径语义已废弃）。
static void user_stats_load_legacy_uni() {
  static const char *names[] = {"user_unigram.tsv", "user_bigram.tsv"};
  FILE *f = open_user_file(names, 2);
  if (!f)
    return;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (strncmp(line, "#n=", 3) == 0)
      continue;  // n_seen 不再记录/消费（2026-11-17 用户定案）
    char *p[4];
    int nf = 0;
    p[nf++] = line;
    while (nf < 4) {
      char *t = strchr(p[nf - 1], '\t');
      if (!t)
        break;
      *t = 0;
      p[nf++] = t + 1;
    }
    if (nf < 4 || strcmp(p[0], "u") != 0)
      continue;
    long long n = atoll(p[3]);
    auto key = std::string(p[1]) + "\t" + p[2];
    auto it = g_user_stats.find(key);
    if (it != g_user_stats.end()) {
      if (n > it->second.cum)
        it->second.cum = n;
    } else {
      UserStatEntry e;
      e.cum = n;
      g_user_stats[key] = e;
    }
  }
  fclose(f);
}

static void user_stats_ensure_loaded() {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  const std::string sid = llm_schema_dir_name();
  if (g_user_stats_loaded && g_stats_schema == sid)
    return;
  if (g_user_stats_loaded) {
    // 方案切换（2026-11-22 分目录）：旧方案账本落盘到旧目录后重载新方案
    user_stats_save_to(g_stats_schema);
    g_user_stats.clear();
  }
  g_user_stats_loaded = true;
  g_stats_schema = sid;
  char path[MAX_PATH];
  if (!user_ledger_file(path, sizeof(path)))
    return;
  FILE *f = fopen(path, "r");
  if (!f) {
    // 首次升级：旧两账本一次性合并（只读, 旧文件保留；落盘写新账本）
    user_stats_load_legacy_eff();
    user_stats_load_legacy_uni();
    return;
  }
  char line[1024];
  long long max_tick = 0, tick_hdr = 0;
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (line[0] == '#') {
      // 首行 #tick=N; 其余 # 行 = 列含义注释（2026-11-20 起落盘自带）
      if (strncmp(line, "#tick=", 6) == 0)
        tick_hdr = atoll(line + 6);
      continue;
    }
    // 新: 词\t码\tdee\ttick\tcum; 旧(2026-11-20 前): s\t词\t码\tdee\ttick\tcum
    char *p[6];
    int nf = 0;
    p[nf++] = line;
    while (nf < 6) {
      char *t = strchr(p[nf - 1], '\t');
      if (!t)
        break;
      *t = 0;
      p[nf++] = t + 1;
    }
    const char *word, *code, *dee, *tk, *cu;
    if (nf == 6 && strcmp(p[0], "s") == 0) {  // 旧 s 行
      word = p[1]; code = p[2]; dee = p[3]; tk = p[4]; cu = p[5];
    } else if (nf == 5) {  // 2026-11-20 起新格式（词必非 ASCII, 不会撞 s 行）
      word = p[0]; code = p[1]; dee = p[2]; tk = p[3]; cu = p[4];
    } else {
      continue;
    }
    UserStatEntry e;
    e.dee = atof(dee);
    e.tick = atoll(tk);
    e.cum = atoll(cu);
    g_user_stats[std::string(word) + "\t" + code] = e;
    if (e.tick > max_tick)
      max_tick = e.tick;
  }
  fclose(f);
  if (tick_hdr > 0)
    g_user_tick = tick_hdr;
  else if (g_user_tick <= 0)
    g_user_tick = max_tick;
}

// 衰减有效计数 (融合用): rime formula_d 查询式（词码对）
static double user_freq_eff(const std::string &w, const std::string &code) {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  user_stats_ensure_loaded();
  auto it = g_user_stats.find(w + "\t" + code);
  if (it == g_user_stats.end() || it->second.dee <= 0)
    return 0;
  return it->second.dee *
         exp((double)(it->second.tick - g_user_tick) / kFreqTau);
}

// ── 用户累计 Unigram 计数 (v3 2026-11-06; 累计 Bigram 退役 2026-11-08) ──
// 融合项 = log(uni+1)（2026-11-11 简化：原 log p1 的分母与 δ 缩放均为窗内
// 常数 = 排序不变量，位级等价去除）。累计 Bigram 项已于 2026-11-08 退役。
// 累计计数与衰减计数同存 user_ledger.tsv 的 cum 列（2026-11-17 合并）；
// n_seen 连记录一并取消（不参与打分, 无消费方）。
static double user_unigram_term(const std::string &x, const std::string &code) {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  user_stats_ensure_loaded();
  auto it = g_user_stats.find(x + "\t" + code);
  long long cum = it == g_user_stats.end() ? 0 : it->second.cum;
  return log((double)(cum + 1));
}

// 累计 unigram 原始计数（调试窗口"累计频"列 / pairfix fade 门用）
static long long user_uni_count(const std::string &x, const std::string &code) {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  user_stats_ensure_loaded();
  auto it = g_user_stats.find(x + "\t" + code);
  return it == g_user_stats.end() ? 0 : it->second.cum;
}

// ── 词码对扣分修正（2026-11-21 用户定案解耦：只承载负分 CE 偏差降噪）──
// 表放用户目录 llm_pairfix.tsv，行 `词<TAB>码<TAB>-分数`（无第三列 =
// 旧默认 -5）；`#` 注释；无文件 = 机制休眠；融合读一次（每窗一次，
// 相对推理成本可忽略）——编辑文件即时生效。负分保留 fade：用户该词码对
// 累计 >= kPairFixFadeUni 次 -> 预置让位。正分行读取端跳过（2026-11-17
// 的 +200 固定首选/+100 固定次选语义废除——固定候选归拼读双拼
// pin_candidates.tsv + pin_filter.lua 全局重排，与 LLM 重排解耦）。
// 旧 2026-11-13 pairfix 推导（wiki 拆半/用户域回放）保留在文件头历史注释：
// 与@vkh 等污染对按字频抬分；负分只破 CE 险胜的近平局。
static const double kPairFixPenaltyDefault = 5.0;
static const long long kPairFixFadeUni = 3;

static bool pairfix_file(char *path, size_t n) {
  // 有状态资产：该方案首次使用时从旧位置复制快照（2026-11-22 分目录）
  return llm_state_file_path("llm_pairfix.tsv", path, n);
}

// 读参考表 → out[key 词\t码] = 有符号分数；返回 false = 文件缺失（休眠）
static bool pairfix_table(std::map<std::string, double> *out) {
  char path[MAX_PATH];
  if (!pairfix_file(path, sizeof(path)))
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (!len || line[0] == '#')
      continue;
    char *t1 = strchr(line, '\t');
    if (!t1)
      continue;
    *t1 = '\0';
    char *t2 = strchr(t1 + 1, '\t');
    // 无第三列 = 旧默认扣 5；第三列负数 = 扣分。2026-11-21 解耦：正分
    // （固定候选）行跳过——归 pin_candidates.tsv / pin_filter.lua。
    double delta = -kPairFixPenaltyDefault;
    if (t2) {
      *t2 = '\0';
      char *end = nullptr;
      double v = strtod(t2 + 1, &end);
      if (end == t2 + 1)
        continue;  // 第三列非法：与插件版一致，跳过
      if (v > 0)
        continue;
      delta = v;
    }
    if (*line && *(t1 + 1))
      (*out)[std::string(line) + "\t" + (t1 + 1)] = delta;
  }
  fclose(f);
  return true;
}

// 命中返回负分扣分修正（fade 门）；正分行读取端已跳过（解耦后无正分）
static double pairfix_delta(const std::map<std::string, double> &tbl,
                            const std::string &w,
                            const std::string &code) {
  auto it = tbl.find(w + "\t" + code);
  if (it == tbl.end())
    return 0.0;
  if (it->second > 0.0)
    return it->second;
  if (user_uni_count(w, code) >= kPairFixFadeUni)
    return 0.0;
  return it->second;
}

static void user_stats_save_to(const std::string &sid) {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  char path[MAX_PATH];
  const std::string target = sid.empty() ? llm_schema_dir_name() : sid;
  bool got = target == llm_schema_dir_name()
                 ? user_ledger_file(path, sizeof(path))
                 : user_ledger_file_for(target, path, sizeof(path));
  if (!got)
    return;
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "#tick=%lld\n", g_user_tick);
  // 列含义注释（2026-11-20 起每帧自带；读取端跳过一切 # 行）
  fprintf(f, "#词\t码\t衰减计数dee\t最近提交tick\t累计次数cum\n");
  fprintf(f, "#码可空（2026-11-21 起简码/顶屏提交也记录完整输入码，空码仅剩"
             "无 composition 的边缘提交）；dee 每词提交 = 1 + dee·exp(-Δ/400)；"
             "tick 每词提交 +1；cum 仅中文词递增\n");
  for (auto &kv : g_user_stats) {
    // 落盘修剪：衰减有效计数 <1e-3 且无累计提交的冷词条不写（对融合分
    // 影响 <0.002 nats）——含中文词条 cum>0 终身保留（旧累计账语义），
    // 非中文行（全角标点等）随衰减淘汰，文件有界于中文词表规模。
    double eff = kv.second.dee *
                 exp((double)(kv.second.tick - g_user_tick) / kFreqTau);
    if (eff < 1e-3 && kv.second.cum <= 0)
      continue;
    fprintf(f, "%s\t%.3f\t%lld\t%lld\n", kv.first.c_str(), kv.second.dee,
            kv.second.tick, kv.second.cum);
  }
  fclose(f);
}

static void user_stats_save() {
  user_stats_save_to(g_stats_schema);
}

static void user_stats_bump(const std::string &w, const std::string &code) {
  // 仅计含中文的上屏（2026-11-20 用户定案：纯标点不入账——标点永不进
  // 重排窗口，dee 纯浪费且常用标点（，。）持续 bump 永不达修剪线）；连
  // tick 也不推，更同源 rime userdb "词条提交才推 tick"（旧口径 = 非
  // ASCII 全入账，2026-09-03 一字词定案）。退款弹栈照常，无账目 →
  // RF refund=0。cum 只对含中文词条递增（标定语义）。
  bool has_non_ascii = false;
  for (unsigned char ch : w)
    if (ch >= 0x80) {
      has_non_ascii = true;
      break;
    }
  if (!has_non_ascii)
    return;
  if (!text_has_cjk_utf8(w))
    return;
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);  // 回撤退款跨线程
  user_stats_ensure_loaded();
  ++g_user_tick;
  UserStatEntry &e = g_user_stats[w + "\t" + code];
  // [回撤退款] bump 前快照进浅撤销栈（cap 4，回撤环总在近处）
  e.hist.push_back({g_user_tick, e.dee, e.tick, e.cum});
  if (e.hist.size() > 4)
    e.hist.erase(e.hist.begin());
  e.dee = 1 + e.dee * exp((double)(e.tick - g_user_tick) / kFreqTau);
  e.tick = g_user_tick;
  if (text_has_cjk_utf8(w))
    ++e.cum;
  if (++g_user_stats_dirty >= 20) {
    user_stats_save();
    g_user_stats_dirty = 0;
  }
}

// [回撤退款]（2026-11-20）: 提交被回撤（M backspace/delete/undo）→ 撤销
// 该次 bump。按条目撤销栈顶 tick 匹配提交 tick → 恢复 bump 前像（d/t/c
// 精确还原）；栈空（跨重启）/tick 不配 → 不退（回退现行为）。退款立即
// 落盘（悬在内存被杀进程吞掉就白退）；cum=0 的错词由既有修剪规则自然
// 淘汰。server IPC 线程调用（reset hook），与引擎线程 bump/查询互斥。
static bool user_stats_refund(const std::string &w, const std::string &code,
                              long long tick) {
  std::lock_guard<std::recursive_mutex> lk(g_ledger_mu);
  user_stats_ensure_loaded();
  auto it = g_user_stats.find(w + "\t" + code);
  if (it == g_user_stats.end() || it->second.hist.empty())
    return false;
  UserStatEntry::Snap s = it->second.hist.back();
  if (s.tick != tick)
    return false;
  it->second.hist.pop_back();
  it->second.dee = s.dee;
  it->second.tick = s.t;
  it->second.cum = s.cum;
  user_stats_save();
  g_user_stats_dirty = 0;
  return true;
}

// 最近一次候选窗的输入码（词码对记账 OnCommit 用; engine thread 写读）
static std::string g_last_input;

// ============================================================
// 记录文件（2026-11-17 用户定案：只保留两个文件）
//   常开语料 rime_llm_corpus.tsv（TSV，不轮转，独立可分析）
//   按需 debug rime_llm_debug.txt（| 分隔，10MB 单轮转）
// 事件 ID 串联两文件；debug 只写语料没有的字段（CE/eff/耗时/错误），
// 不重复词/码/上下文/候选序/配置。
// ============================================================
static const char *kCorpusFile = "rime_llm_corpus.tsv";
static const char *kDebugFile = "rime_llm_debug.txt";
static const char *kCorpusMagic = "#RIME_CORPUS\tv2\t";
static const char *kDebugMagic = "#RIME_DEBUG|v2|";
static const unsigned long long kDebugRotateBytes = 10ull * 1024 * 1024;
static std::mutex g_log_mu;
// 头部检查按**路径**缓存（2026-11-22 分方案）：同一进程可能写多个方案的
// 语料/debug 文件，一次性布尔会让第二个方案跳过头部校验直接追加
static std::string g_corpus_checked_path;
static bool g_corpus_ok = true;
static std::string g_debug_checked_path;
static bool g_debug_ok = true;
static std::atomic<long long> g_event_seq{0};
static std::mutex g_event_mu;
static std::string g_last_event_id;

struct LastWindow {
  std::string id;
  std::string code;
  std::vector<std::string> engine;
  std::vector<std::string> window;
  std::vector<std::string> final_list;
  long long tick = 0;
};
static LastWindow g_last_window;  // engine thread only (Apply/Collect/OnCommit)

// [回撤退款]（2026-11-20）提交栈（cap 8）: OnCommit（引擎线程）压入，
// reset hook（server IPC 线程）在 backspace/delete/undo 时弹出退款；
// navigation/enter/focus 边界清栈——与语料 strict 弹栈口径逐字对齐。
struct CommitRec {
  std::string word;
  std::string code;
  long long tick = 0;
  std::string cid;
};
static std::mutex g_commit_mu;
static std::vector<CommitRec> g_commit_stack;
static void commit_stack_push(const std::string &word,
                              const std::string &code, long long tick,
                              const std::string &cid) {
  std::lock_guard<std::mutex> lk(g_commit_mu);
  CommitRec rec;
  rec.word = word;
  rec.code = code;
  rec.tick = tick;
  rec.cid = cid;
  g_commit_stack.push_back(rec);
  if (g_commit_stack.size() > 8)
    g_commit_stack.erase(g_commit_stack.begin());
}
static void commit_stack_clear() {
  std::lock_guard<std::mutex> lk(g_commit_mu);
  g_commit_stack.clear();
}

static bool log_file_path(const char *filename, char *path, size_t n) {
  // 2026-11-21 起 corpus/debug 收进用户文件夹 llm_rerank 子目录
  // （llm_file_path 内含根目录旧文件一次性迁移）；用户目录不可得才回退 %TEMP%
  if (llm_file_path(filename, path, n))
    return true;
#ifdef _WIN32
  char tmp[MAX_PATH];
  GetTempPathA(sizeof(tmp), tmp);
  snprintf(path, n, "%s%s", tmp, filename);
  return true;
#else
  (void)path;
  (void)n;
  return false;
#endif
}

static std::string now_stamp() {
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[40];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear,
           st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds);
  return b;
#else
  std::time_t t = std::time(nullptr);
  std::tm tm_buf;
  localtime_r(&t, &tm_buf);
  char b[32];
  std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm_buf);
  return b;
#endif
}

static std::string now_hms() {
  std::time_t t = std::time(nullptr);
  std::tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char ts[16];
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
  return ts;
}

static std::string new_event_id(const char *kind, long long tick) {
  (void)kind;
  long long seq = ++g_event_seq;
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[96];
  snprintf(b, sizeof(b), "%04d%02d%02dT%02d%02d%02d%03d-%lld-%04lld",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds, tick, seq);
  return b;
#else
  char b[64];
  snprintf(b, sizeof(b), "0-%lld-%04lld", tick, seq);
  return b;
#endif
}

static void remember_event(const std::string &id) {
  std::lock_guard<std::mutex> lk(g_event_mu);
  g_last_event_id = id;
}

static std::string last_event_id() {
  std::lock_guard<std::mutex> lk(g_event_mu);
  return g_last_event_id;
}

static std::string tsv_escape(const std::string &s) {
  std::string t;
  t.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': t += "\\\\"; break;
      case '\t': t += "\\t"; break;
      case '\n': t += "\\n"; break;
      case '\r': t += "\\r"; break;
      default: t += c;
    }
  }
  return t;
}

static std::string pipe_escape(const std::string &s) {
  std::string t = s;
  for (auto &c : t) {
    if (c == '|')
      c = '/';
    else if (c == '\n' || c == '\r')
      c = ' ';
  }
  return t;
}

static std::string cand_escape(const std::string &s) {
  std::string t;
  t.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': t += "\\\\"; break;
      case ',': t += "\\,"; break;
      case '\t': t += "\\t"; break;
      case '\n': t += "\\n"; break;
      case '\r': t += "\\r"; break;
      default: t += c;
    }
  }
  return t;
}

static std::string join_strs(const std::vector<std::string> &v, size_t maxn) {
  std::string t;
  size_t n = v.size() < maxn ? v.size() : maxn;
  for (size_t i = 0; i < n; i++) {
    if (i)
      t += ',';
    t += cand_escape(v[i]);
  }
  return t;
}

static std::vector<std::string> texts_of(const std::vector<an<Candidate>> &v,
                                         size_t maxn) {
  std::vector<std::string> out;
  size_t n = v.size() < maxn ? v.size() : maxn;
  out.reserve(n);
  for (size_t i = 0; i < n; i++)
    out.push_back(v[i]->text());
  return out;
}

static std::string ctx_tail(const std::string &s, size_t n) {
  if (s.size() <= n)
    return s;
  size_t cut = s.size() - n;
  while (cut < s.size() && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    ++cut;
  return s.substr(cut);
}

static std::string path_base(const std::string &p) {
  size_t x = p.find_last_of("\\/");
  return x == std::string::npos ? p : p.substr(x + 1);
}

static bool check_header_locked(const char *path, const char *magic,
                                const char *header_line) {
  FILE *f = fopen(path, "rb");
  if (f) {
    char line[512] = {0};
    bool ok = fgets(line, sizeof(line), f) &&
              strncmp(line, magic, strlen(magic)) == 0;
    fclose(f);
    return ok;
  }
  f = fopen(path, "ab");
  if (!f)
    return false;
  fputs(header_line, f);
  fputc('\n', f);
  fclose(f);
  return true;
}

static void corpus_write_raw(const std::string &line) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  char path[MAX_PATH];
  if (!log_file_path(kCorpusFile, path, sizeof(path)))
    return;
  if (g_corpus_checked_path != path) {
    g_corpus_checked_path = path;
    g_corpus_ok = check_header_locked(
        path, kCorpusMagic,
        (std::string(kCorpusMagic) + now_stamp() + "\tsource").c_str());
  }
  if (!g_corpus_ok)
    return;
  FILE *f = fopen(path, "ab");
  if (!f)
    return;
  fwrite(line.data(), 1, line.size(), f);
  fputc('\n', f);
  fclose(f);
}

static void debug_write_raw(const std::string &line, bool force) {
  if (!force && !g_debug_fusion)
    return;
  std::lock_guard<std::mutex> lk(g_log_mu);
  char path[MAX_PATH];
  if (!log_file_path(kDebugFile, path, sizeof(path)))
    return;
  if (g_debug_checked_path != path) {
    g_debug_checked_path = path;
    g_debug_ok = check_header_locked(
        path, kDebugMagic,
        (std::string(kDebugMagic) + now_stamp() + "|source").c_str());
  }
  if (!g_debug_ok)
    return;
  rotate_if_large(path, kDebugRotateBytes);
  FILE *f = fopen(path, "ab");
  if (!f)
    return;
  fwrite(line.data(), 1, line.size(), f);
  fputc('\n', f);
  fclose(f);
}

static void log_msg(const char *fmt, ...) {
  if (!g_debug_fusion)
    return;
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debug_write_raw(std::string("-|X|scope=ops|msg=") + pipe_escape(buf), false);
}

static void log_error(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debug_write_raw(std::string("-|X|scope=error|msg=") + pipe_escape(buf), true);
}

static std::string write_w(const std::string &id, const std::string &src,
                           const std::string &code, const std::string &ctx,
                           const std::vector<std::string> &engine,
                           const std::vector<std::string> &window,
                           const std::vector<std::string> &final_list,
                           long long tick, const std::string &status,
                           const std::string &skip, bool prep,
                           const std::string &flags) {
  std::string line = "W\t" + id + "\t" + now_stamp() + "\t" +
                     tsv_escape(src) + "\t" + tsv_escape(code) + "\t" +
                     tsv_escape(ctx_tail(ctx, 120)) + "\t" +
                     join_strs(engine, 10) + "\t" + join_strs(window, 20) +
                     "\t" + join_strs(final_list, 20) + "\t" +
                     std::to_string(tick) + "\t" + status + "\t" +
                     tsv_escape(skip) + "\t" + (prep ? "1" : "0") + "\t" +
                     tsv_escape(flags);
  corpus_write_raw(line);
  remember_event(id);
  g_last_window.id = id;
  g_last_window.code = code;
  g_last_window.engine = engine;
  g_last_window.window = window;
  g_last_window.final_list = final_list;
  g_last_window.tick = tick;
  return id;
}

static int rank_of(const std::vector<std::string> &v, const std::string &w) {
  for (size_t i = 0; i < v.size(); i++)
    if (v[i] == w)
      return (int)i + 1;
  return -1;
}

static std::string write_c(const std::string &word, const std::string &code,
                           long long tick, bool cjk) {
  std::string ref;
  int re = -1, rw = -1, rf = -1;
  if (!code.empty() && g_last_window.code == code) {
    ref = g_last_window.id;
    re = rank_of(g_last_window.engine, word);
    rw = rank_of(g_last_window.window, word);
    rf = rank_of(g_last_window.final_list, word);
  }
  // 窗口只对应一次输入尝试：任何提交后消费，避免空码提交后旧窗被
  // 后续同码简码提交误引用。
  g_last_window = LastWindow();
  std::string id = new_event_id("C", tick);
  std::string line = "C\t" + id + "\t" + now_stamp() + "\t" + ref + "\t" +
                     tsv_escape(word) + "\t" + tsv_escape(code) + "\t" +
                     std::to_string(re) + "\t" + std::to_string(rw) + "\t" +
                     std::to_string(rf) + "\t" + std::to_string(tick) + "\t" +
                     (cjk ? "cjk=1" : "");
  corpus_write_raw(line);
  remember_event(id);
  return id;
}

static const char *reset_kind(const char *reason) {
  if (!reason)
    return "unknown";
  if (strcmp(reason, "focus:switch") == 0)
    return "focus";
  if (strcmp(reason, "editkey:backspace") == 0)
    return "backspace";
  if (strcmp(reason, "editkey:delete") == 0)
    return "delete";
  if (strcmp(reason, "editkey:navigation") == 0)
    return "navigation";
  if (strcmp(reason, "editkey:enter") == 0)
    return "enter";
  if (strcmp(reason, "editkey:undo-clipboard") == 0)
    return "undo-clipboard";
  return "unknown";
}

static void write_m(const char *reason) {
  std::string id = new_event_id("M", 0);
  std::string ref = last_event_id();
  std::string line = "M\t" + id + "\t" + now_stamp() + "\t" + ref + "\t" +
                     reset_kind(reason) + "\t\t";
  corpus_write_raw(line);
  int gen = 0;
  const RimeApi *api = rime_get_api();
  if (api && api->context_reset_generation)
    gen = api->context_reset_generation();
  debug_write_raw(id + "|R|gen=" + std::to_string(gen), false);
}

// [回撤退款] reset hook 调用（server IPC 线程）: backspace/delete/undo 弹
// 最近提交退款（RF debug 行只记 id+tick，词/码在 cid 关联的 C 行里）；
// navigation/enter/focus 边界清栈；未映射原因保守不动栈。
static void commit_stack_refund_or_clear(const char *reason) {
  const char *kind = reset_kind(reason);
  bool retract = !strcmp(kind, "backspace") || !strcmp(kind, "delete") ||
                 !strcmp(kind, "undo-clipboard");
  if (!retract) {
    if (!strcmp(kind, "unknown"))
      return;
    commit_stack_clear();
    return;
  }
  CommitRec rec;
  {
    std::lock_guard<std::mutex> lk(g_commit_mu);
    if (g_commit_stack.empty())
      return;
    rec = g_commit_stack.back();
    g_commit_stack.pop_back();
  }
  bool ok = user_stats_refund(rec.word, rec.code, rec.tick);
  debug_write_raw(rec.cid + "|RF|refund=" + (ok ? "1" : "0") +
                      "|tick=" + std::to_string(rec.tick),
                  false);
}

static void write_h(const std::string &schema, const std::string &model) {
  char beta[16];
  snprintf(beta, sizeof(beta), "%.2f", g_freq_beta);
  std::string id = new_event_id("H", g_user_tick);
  std::string line = "H\t" + id + "\t" + now_stamp() + "\tsource\t" +
                     tsv_escape(schema) + "\t" + tsv_escape(path_base(model)) +
                     "\t" + tsv_escape(g_code_pattern) + "\t" +
                     std::to_string(g_max_candidates) + "\t" +
                     std::to_string(g_max_ctx_tokens) + "\t" +
                     (g_sentence_mode ? "1" : "0") + "\t" + beta + "\t400";
  corpus_write_raw(line);
}

// 诊断数值: 哨兵/非有限 → 短标记, 免 %f 展开 -1e308 撑爆行宽
static std::string dbg_num(double v) {
  if (!(v > -1e307))
    return "-inf";
  char b[32];
  snprintf(b, sizeof(b), "%.3f", v);
  return b;
}

// 右对齐 8 列版（CE 列哨兵 -1e10 若用 %8.3f 会展开成 16 字符撑破行宽）
static std::string dbg_num8(double v) {
  std::string s = dbg_num(v);
  if (s.size() < 8)
    s.insert(0, 8 - s.size(), ' ');
  return s;
}

// debug 行（只写语料没有的字段；首字段 = 语料事件 ID）
static void debug_write_p(const std::string &id, double total, bool prep) {
  char b[160];
  snprintf(b, sizeof(b), "%s|P|total=%.1f|prep=%d", id.c_str(), total,
           prep ? 1 : 0);
  debug_write_raw(b, false);
}

static void debug_write_d(const std::string &id, int final_rank, double ce,
                          double ce1, double ce2, double ce3, double eff,
                          double fb, double u1, int full, double key,
                          double sdelta, int cerank, int finrank) {
  char b[512];
  snprintf(b, sizeof(b),
           "%s|D|%d|ce=%s|ce1=%s|ce2=%s|ce3=%s|eff=%.3f|fb=%.3f|u1=%.3f"
           "|full=%d|key=%s|sdelta=%g|cerank=%d|finrank=%d",
           id.c_str(), final_rank, dbg_num(ce).c_str(), dbg_num(ce1).c_str(),
           dbg_num(ce2).c_str(), dbg_num(ce3).c_str(), eff, fb, u1, full,
           dbg_num(key).c_str(), sdelta, cerank, finrank);
  debug_write_raw(b, false);
}

static void debug_write_e(const std::string &id, double before, double after,
                          int dirty) {
  char b[192];
  snprintf(b, sizeof(b), "%s|E|eff_before=%.3f|eff_after=%.3f|dirty=%d",
           id.c_str(), before, after, dirty);
  debug_write_raw(b, false);
}

// ============================================================
// 实时调试管道 rime_llm_live（2026-11-10，与插件版 rime_llm.cpp 同协议）：
// debug_fusion 开时把 V/L/C/S/E/R 行协议广播给外部查看器
// （WeaselLLMDebug.exe）。行语义见查看器源头部注释。设计约束：
//   - 懒启动：首次 live_send 才起接纳线程（debug_fusion 关 = 零开销）
//   - 广播带 50ms 超时（overlapped WriteFile）——查看器卡死/断开即踢，
//     绝不阻塞打字路径；无连接 = 一次 mutex + 空列表遍历
//   - rime.dll 随 WeaselServer 进程生命周期加载，接纳线程不设停机
// 仅 Windows（其余平台 live_send 为空实现）。
// ============================================================
#ifdef _WIN32
static const char *kLivePipeName = "\\\\.\\pipe\\rime_llm_live";
static const char *kLiveProtoDate = "2026-11-10";

static std::mutex g_live_mu;
static std::vector<HANDLE> g_live_clients;
static std::atomic<bool> g_live_started{false};

// 单句柄单次写：overlapped + 50ms 超时（超时/失败由调用方踢客户端）
static bool live_write_raw(HANDLE h, const std::string &s) {
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!ov.hEvent)
    return false;
  bool ok = false;
  BOOL pending = WriteFile(h, s.data(), (DWORD)s.size(), NULL, &ov);
  if (pending || GetLastError() == ERROR_IO_PENDING) {
    DWORD written = 0;
    if (GetOverlappedResultEx(h, &ov, &written, 50, FALSE))
      ok = true;
    else
      CancelIo(h);
  }
  CloseHandle(ov.hEvent);
  return ok;
}

static void live_drop_client(HANDLE h) {
  DisconnectNamedPipe(h);
  CloseHandle(h);
}

// 接纳线程：每次建一个管道实例阻塞等连接，连上即推 V 行并加入客户端
// 列表，随后建下一实例。上限 8 个并发查看器。
static void live_thread_proc() {
  for (;;) {
    HANDLE h = CreateNamedPipeA(kLivePipeName,
                                PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                8, 64 * 1024, 64 * 1024, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      Sleep(2000);
      continue;
    }
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
      CloseHandle(h);
      Sleep(2000);
      continue;
    }
    BOOL conn = ConnectNamedPipe(h, &ov);
    if (!conn && GetLastError() == ERROR_IO_PENDING)
      conn = WaitForSingleObject(ov.hEvent, INFINITE) == WAIT_OBJECT_0;
    BOOL connected = conn || GetLastError() == ERROR_PIPE_CONNECTED;
    CloseHandle(ov.hEvent);
    if (!connected) {
      CloseHandle(h);
      Sleep(500);
      continue;
    }
    std::string hello =
        std::string("V|rime-llm-live|1|source|") + kLiveProtoDate + "\n";
    if (live_write_raw(h, hello)) {
      std::lock_guard<std::mutex> lk(g_live_mu);
      g_live_clients.push_back(h);
    } else {
      live_drop_client(h);
    }
  }
}

static void live_broadcast(const std::string &line) {
  bool f = false;
  if (g_live_started.compare_exchange_strong(f, true))
    std::thread(live_thread_proc).detach();
  std::string s = line + "\n";
  std::lock_guard<std::mutex> lk(g_live_mu);
  for (auto it = g_live_clients.begin(); it != g_live_clients.end();) {
    if (live_write_raw(*it, s))
      ++it;
    else {
      live_drop_client(*it);
      it = g_live_clients.erase(it);
    }
  }
}

// 行发送入口：debug_fusion 门控（与 rime_llm_debug.txt 同开关）
static void live_send(const std::string &line) {
  if (!g_debug_fusion || line.empty())
    return;
  live_broadcast(line);
}
#else
static void live_send(const std::string &) {}  // non-Windows: no-op
#endif  // _WIN32

// 自由文本字段转义：| → 全角｜、换行 → 空格（行协议字段分隔符保护，
// 与插件版 lua live_esc 一致）
static std::string live_esc(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '|') {
      r += "\xEF\xBD\x9C";  // ｜ (U+FF5C)
    } else if (c == '\n' || c == '\r') {
      r += ' ';
    } else {
      r += c;
    }
  }
  return r;
}

// 数值字段：有限 → %.3f；否则空串（"无分"标记，与协议约定一致）
static std::string live_num(double v) {
  if (!(v == v) || v <= -1e307)
    return "";
  char b[32];
  snprintf(b, sizeof(b), "%.3f", v);
  return b;
}

// ============================================================
// async model loading (non-blocking for the IME)
// ============================================================
static void load_model_async() {
  // 路径变更自动重载（2026-09-01）：GUI/schema 改 model_path 而模型已
  // 加载时，旧模型驻留内存、新路径被无视——此处检测变更即卸载，随后
  // 走正常加载。检测收敛在本函数，部署期 Apply 调用点自动受益。
  if (g_loaded.load() && g_loaded_from != g_model_path) {
    log_msg("model path changed: %s -> %s (reload)",
            g_loaded_from.c_str(), g_model_path.c_str());
    unload_model();
  }
  if (g_loaded.load() || g_loading.load())
    return;
  g_loading.store(true);

  std::thread([]() {
#ifdef _WIN32
    // 加载期 (2GB 模型读取 + 反量化 + warmup 全线程 decode) 会吃满
    // CPU/IO, 期间新进程启动 (QQ 音乐等 CEF 应用) 会被拖到秒级挂起;
    // 降低本线程 (及 warmup 创建的 llama worker 线程) 优先级让位系统,
    // 加载完成后恢复
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    if (g_model_path.empty())
      g_model_path = default_model_path();  // 此刻 Service 目录已就绪
    log_msg("loading model: %s", g_model_path.c_str());

    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    mparams.load_mode = LLAMA_LOAD_MODE_MMAP;  // 2026-11-15 升级: use_mmap 字段已删, load_mode 显式 mmap 等价

    g_model = llama_model_load_from_file(g_model_path.c_str(), mparams);
    if (!g_model) {
      log_error("failed to load model");
      g_loading.store(false);
      return;
    }
    g_vocab = llama_model_get_vocab(g_model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = g_n_ctx;
    cparams.n_threads = g_n_threads;
    cparams.n_threads_batch = g_n_threads;
    cparams.n_seq_max = g_n_seq_max;

    g_ctx = llama_init_from_model(g_model, cparams);
    if (!g_ctx) {
      log_error("failed to create context");
      llama_model_free(g_model);
      g_model = nullptr;
      g_loading.store(false);
      return;
    }

    // warmup 前恢复 NORMAL: llama 的 decode 是多线程协同 (7 个 worker
    // 条件变量同步), 低优先级下 worker 会被 NORMAL 线程饿死 → 加载
    // 永远卡在 warmup (实测 13 分钟无 model ready)
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#endif
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      const char *warmup = "\n";
      llama_token tokens[4];
      int n_tokens = llama_tokenize(g_vocab, warmup, (int)strlen(warmup),
                                    tokens, 4, true, true);
      if (n_tokens > 0) {
        llama_batch batch = llama_batch_get_one(tokens, n_tokens);
        llama_decode(g_ctx, batch);
      }
    }

    g_loaded_from = g_model_path;  // 供路径变更检测（load_model_async 头部）
    g_loaded.store(true);
    g_loading.store(false);
    log_msg("model ready (n_ctx=%d threads=%d)",
            g_n_ctx, g_n_threads);
  }).detach();
}

// release the loaded model (2GB) when rerank is disabled via schema
// re-deploy. The filter is rebuilt on every deploy; the constructor's
// enabled=false（部署期 Apply）及路径变更重载（load_model_async）
// 都会调用——关开关真正释放内存而不是驻留到进程退出。
static void unload_model() {
  // filter rebuild can race an in-flight load (async thread); wait for it
  // to finish before freeing under the lock
  while (g_loading.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_loaded.load() || g_model || g_ctx) {
    if (g_ctx) {
      llama_free(g_ctx);
      g_ctx = nullptr;
    }
    if (g_model) {
      llama_model_free(g_model);
      g_model = nullptr;
    }
    g_vocab = nullptr;
    g_loaded.store(false);
    g_prep_ready = false;
    g_prep_ctx.clear();
    g_prep_logits.clear();
    log_msg("model unloaded");
  }
}

// ============================================================
// normalize: a newline (CRLF / LF / CR, all three forms) is a
// paragraph boundary - only the last paragraph counts as context,
// so multi-line documents stay separated ("进行打字测试\n进行
// 打字测试" -> "进行打字测试"). Within the paragraph, strip
// whitespace so prepare() and score_batch() always compare
// identical token sequences (prep hit requires exact ctx match).
// ============================================================
static std::string normalize_ctx(const std::string& s) {
  std::string t = s;
  // last paragraph: cut at the last newline character
  size_t p = t.find_last_of("\r\n");
  if (p != std::string::npos)
    t = t.substr(p + 1);
  // strip remaining whitespace inside the paragraph
  t.erase(std::remove_if(t.begin(), t.end(),
                         [](unsigned char c) { return std::isspace(c) != 0; }),
          t.end());
  return t;
}

// ============================================================
// 上文来源判定纯逻辑 (滞后/残留/新鲜度/空文本) — 不依赖引擎与模型,
// 可独立测试。scripts/test_llm_context.cpp 复制本命名空间做 gold
// 断言 — 修改判定必须同步测试文件并重跑 (改错会误用 TSF/历史通道)。
// ============================================================
namespace ctx_logic {

// 滞后检测 (2026-08-14, WPS): WPS 的 TSF 文本访问只暴露最近 composition
// 相关文本 (实测连续打 N 个"测试"只采到 2 字符), 此时 TSF 文本是上屏历史
// 的尾部子串且明显更短 → 粘性降级到历史 (2026-08-18 用户最终决策, 标
// AI·历史; 判据取舍见 GetContextTextPair 头注释)。
inline bool lagging(const std::string &tsf, const std::string &hist) {
  if (tsf.empty() || hist.empty())
    return false;  // 空文本不在滞后检测范围内 (调用侧已排除非空 TSF)
  size_t tlen = tsf.size();
  return tlen * 2 < hist.size() && hist.size() > tlen &&
         hist.compare(hist.size() - tlen, tlen, tsf) == 0;
}

// 取 hist 尾部 ≤max_bytes 的完整字符 (UTF-8 边界对齐): 按字节切会在汉字
// 中间切开 → 子串匹配恒失败 → 中文尾部恒误判 (2026-08-13 实测)。
inline std::string hist_tail(const std::string &hist, size_t max_bytes) {
  // std::min<size_t>: 显式模板参数使 min 宏 (windows.h) 不展开 (min 后跟
  // `<` 非 `(`), 与 GetContextTextPair 原实现一致
  size_t n = std::min<size_t>(max_bytes, hist.size());
  size_t start = hist.size() - n;
  while (start < hist.size() &&
         (static_cast<unsigned char>(hist[start]) & 0xC0) == 0x80)
    ++start;  // 跳过 continuation bytes, 取完整字符
  return hist.substr(start);
}

// 残留检测: 陈旧 TSF 文本可能属于其他应用 (32 位应用加载官方 TSF 无采集
// 代码, context_text 残留上次应用旧文本)。commit history 是当前会话同步
// 累积的, 正常场景光标前文本必然包含最近上屏词 (hist 尾部); 不含 → 过期。
inline bool stale(const std::string &tsf, const std::string &hist) {
  std::string tail = hist_tail(hist, 8);
  return !tail.empty() && tsf.find(tail) == std::string::npos;
}

// 尾部连贯判定（鼠标移动 reset 信号, 2026-09-11 深夜全应用推广）:
// h 与 t 任一为另一的尾部 = 光标仍在连续打字位置——好应用 tsf 是全文
//（历史为其尾部）、受限 store 的 tsf=最后上屏词（为历史尾部）。互不为
// 尾部 = 光标被移走（同窗内移动不触发 reset 代次，但 selection-change
// 会送达新位置文本）
inline bool ends_with(const std::string &h, const std::string &t) {
  return t.size() <= h.size() &&
         h.compare(h.size() - t.size(), t.size(), t) == 0;
}
inline bool tail_consistent(const std::string &h, const std::string &t) {
  return ends_with(h, t) || ends_with(t, h);
}

// 空文本分类: age<1.5s 且历史非空 → transient empty (commit 后 TSF 异步
// 刷新未落地 / selection-change 采到不稳定选择) → 用历史兜底让候选窗仍
// 重排; 否则真空 (光标在文档开头/全删, 历史上屏词在光标后不是上文) → 跳过。
inline bool transient_empty(const std::string &hist,
                            unsigned long long age_ms) {
  return age_ms < 1500 && !hist.empty();
}

}  // namespace ctx_logic

// ============================================================
// UTF-8 字符数（数非续字节 0x10xxxxxx）— 用于 long-word-first 词长排序
// ============================================================
static size_t utf8_len(const std::string &s) {
  size_t n = 0;
  for (unsigned char ch : s)
    if ((ch & 0xC0) != 0x80)
      n++;
  return n;
}

// ============================================================
// tokenize
// ============================================================
static std::vector<llama_token> tokenize(const char *text) {
  std::vector<llama_token> toks(128);
  int n = llama_tokenize(g_vocab, text, (int)strlen(text), toks.data(),
                         (int)toks.size(), true, true);
  if (n < 0) {
    toks.resize(-n);
    n = llama_tokenize(g_vocab, text, (int)strlen(text), toks.data(),
                       (int)toks.size(), true, true);
  }
  toks.resize(n > 0 ? n : 0);
  return toks;
}

// 推理用上文尾部文本（2026-11-12 调试窗口协议 v4，灰显用）：截尾保留的
// ids 逐 token 拼 piece —— 字节级 BPE 的各 token piece 恰好等于原文中
// 连续字节段，拼接 = 精确尾部（真实 token 边界，非字符数估算）。
static std::string tokens_piece(const std::vector<llama_token> &ids) {
  std::string s;
  char buf[256];
  for (auto id : ids) {
    int n = llama_token_to_piece(g_vocab, id, buf, sizeof(buf), 0, true);
    if (n > 0) s.append(buf, n);
  }
  return s;
}

// 候选词 token 边界段显（2026-11-21 调试窗口协议 v7，"词"列）：逐 token 拼
// piece，每个 token 结束时若已拼出完整 UTF-8 字符则封一段——分隔符只落在
// token 边界 ∩ 字符边界：单 token 含多字不劈（标志/测试 在 Qwen3-0.6B 为
// 单 token → 原样显示），一个字由 2 token 拼成 → 合并成一段（不劈字），
// 多 token 词按真实边界分段（错事=[错][事] → 错/事，cmak → cm/ak）。
// 段间 '/'。拼接结果 = 原词文本（字节级 BPE 的 piece 拼接恰好还原原文，
// 同 tokens_piece）。
// ⚠ 字符完整性判定（2026-11-21 踩坑修正，插件 rime_llm.cpp 同款）：UTF-8
// 多字节字符**以续字节结尾**（错 = E9 94 99，末字节是续字节）——"末字节
// 非续字节"判定是反的，会让 CJK 永不封段、整词并一段（曾据此误报错事/
// 标志等全为单 token，评分路径 ce2 有值才暴露）。正确做法 = 用 lead 字节
// 推断期望长度、数续字节凑齐才算完整。
static std::string token_seg_display(const std::vector<llama_token> &ids) {
  std::string out, cur;
  char buf[256];
  int remain = 0;  // 当前字符还差的续字节数（0 = 处于字符边界）
  for (auto id : ids) {
    int n = llama_token_to_piece(g_vocab, id, buf, sizeof(buf), 0, true);
    for (int k = 0; k < n; k++) {
      unsigned char b = (unsigned char)buf[k];
      cur += (char)b;
      if (remain > 0) {
        remain--;
      } else if (b < 0x80) {
        // ASCII：自身即完整字符，remain 保持 0
      } else if ((b & 0xE0) == 0xC0) {
        remain = 1;   // 2 字节字符（还差 1 续字节）
      } else if ((b & 0xF0) == 0xE0) {
        remain = 2;   // 3 字节字符（CJK 常态）
      } else {
        remain = 3;   // 4 字节字符（扩展区）
      }
    }
    if (remain == 0 && !cur.empty()) {  // token 结束且字符完整 → 封段
      if (!out.empty()) out += '/';
      out += cur;
      cur.clear();
    }
  }
  if (!cur.empty()) {  // 末段残字节（正常不可达）原样收尾，不丢字
    if (!out.empty()) out += '/';
    out += cur;
  }
  return out;
}

// ============================================================
// CE helper: -log(softmax(x)[target])
// 通用优化（2026-11-06，两版同步；不依赖特定指令集、不改评分语义）：
// ① 尾部跳过：与 max 相差 > kLseSkipNat 的项不做 exp——单项贡献
//    < e^-20 ≈ 2.1e-9，全词表（26 万封顶）总贡献 < 5.5e-4，即 lse 偏差
//    < 0.0006 nats，比候选间典型 CE 差（≥0.01）小一个数量级以上；
//    真实分布高度集中，跳过量通常 >90%（近乎均匀的病态分布退化为原速）。
// ② exp 用 float 版 expf（标量运算，无 SIMD 依赖），求和仍 double 累加。
// 实测（13900H 锁频）：单次全词表归一化 ~2ms → ~0.1ms；prep 命中路径
// 配合归一化量缓存（见 score_batch）CE1 段 ~0ms。
// ============================================================
static const float kLseSkipNat = 20.0f;  // 尾部跳过阈值（nats，误差界见上）

static double cross_entropy(const float *logits, int vs, int target_id) {
  float m = -1e30f;
  for (int k = 0; k < vs; k++)
    if (logits[k] > m)
      m = logits[k];
  const float cut = m - kLseSkipNat;
  double se = 0;
  for (int k = 0; k < vs; k++)
    if (logits[k] >= cut)
      se += expf(logits[k] - m);
  return -((double)(logits[target_id] - m) - log(se));
}

// Normalizer (max, logsumexp) for a logits vector, computed once and reused
// across multiple CE targets. Step 1 scores all candidates against the same
// ctx logits, so the O(vocab) scan was repeated per candidate (5x waste,
// ~5ms of the CE1 stage); with a shared normalizer it becomes O(vocab) once
// + O(1) per target. 2026-11-06: tail skip + expf on top, and the normalizer
// result is cached with the prep state — the prep-hit path skips the scan
// entirely (CE1 stage ~0ms).
static void logits_normalizer(const float *logits, int vs, float &m, double &lse) {
  m = -1e30f;
  for (int k = 0; k < vs; k++)
    if (logits[k] > m)
      m = logits[k];
  const float cut = m - kLseSkipNat;
  double se = 0;
  for (int k = 0; k < vs; k++)
    if (logits[k] >= cut)
      se += expf(logits[k] - m);
  lse = log(se);
}
static double ce_target(const float *logits, int target_id, float m, double lse) {
  return -((double)(logits[target_id] - m) - lse);
}

// ctx KV 就绪（调用方持 g_mutex，2026-11-23 与插件版同构）：返回 ctx 末位
// logits（指向 g_prep_logits）与归一化量；失败返回 nullptr。三条路径：
//   ① prep 完全命中（ctx 相同）→ 零成本
//   ② 前缀延伸（新 ctx = 旧 prep ctx + 新增 token）→ 只 decode 增量：
//      前缀软确认后 ctx = 上文 + P，上文部分的 KV 原样保留（§11.8 协同）
//   ③ 否则全量重建（memory_clear + decode）
static const float *ensure_ctx_kv(const std::vector<llama_token> &ctx_ids,
                                  int vs, float &m, double &lse) {
  if (g_prep_ready && ctx_ids == g_prep_ctx && g_seq0_gen == g_prep_gen) {
    m = g_prep_m;
    lse = g_prep_lse;
    return g_prep_logits.data();
  }
  int from = 0;
  bool extend = g_prep_ready && g_seq0_gen == g_prep_gen &&
                ctx_ids.size() > g_prep_ctx.size() &&
                std::equal(g_prep_ctx.begin(), g_prep_ctx.end(),
                           ctx_ids.begin());
  if (extend) {
    from = (int)g_prep_ctx.size();
  } else {
    llama_memory_clear(llama_get_memory(g_ctx), false);
  }
  const int ctx_len = (int)ctx_ids.size();
  // 增量是稀有路径（确认前缀后 / 上文短），只在此时留痕
  if (extend)
    log_error("ctx_kv: extend from=%d ctx=%d prep=%d gen=%ld", from, ctx_len,
              (int)g_prep_ctx.size(), g_seq0_gen);
  llama_batch b = llama_batch_init(ctx_len - from, 0, 1);
  int row = 0;
  for (int j = from; j < ctx_len; j++, row++) {
    b.token[row] = ctx_ids[j];
    b.pos[row] = j;
    b.n_seq_id[row] = 1;
    b.seq_id[row][0] = 0;
    b.logits[row] = (j + 1 == ctx_len) ? 1 : 0;
  }
  b.n_tokens = row;
  bool ok = llama_decode(g_ctx, b) == 0;
  llama_batch_free(b);
  if (!ok) {
    if (extend) {
      // 增量失败（KV 耗尽等）：置无效并回退全量重建一次（递归深度 1）
      g_prep_ready = false;
      return ensure_ctx_kv(ctx_ids, vs, m, lse);
    }
    log_error("ctx decode failed");
    return nullptr;
  }
  g_seq0_gen++;
  const float *cl = llama_get_logits_ith(g_ctx, ctx_len - from - 1);
  if (!cl) return nullptr;
  g_prep_logits.assign(cl, cl + vs);
  g_prep_gen = g_seq0_gen;
  g_prep_ctx = ctx_ids;
  logits_normalizer(g_prep_logits.data(), vs, g_prep_m, g_prep_lse);
  g_prep_ready = true;
  m = g_prep_m;
  lse = g_prep_lse;
  return g_prep_logits.data();
}

// ============================================================
// core scoring:
//   Step 1: decode ctx -> save logits -> CE of cand[0] from it
//   Step 2: KV copy ctx -> worker seqs, parallel decode cand[0] first
//           tokens (rows deduped by first token, 2026-11-07) -> CE of cand[1]
//   Step 3: decode second tokens (rows deduped by (c0,c1) pair; extra
//           pairs seq_cp their own seq) -> CE of cand[2]
// ============================================================
// 返回 true = 评分完成（结果可发布）；false = 中途取消（异步评分器代次
// 变化——新 submit 覆盖 / cancel，结果整体丢弃）。中断检查点 = 拿锁后 /
// CE1 后(Step2 前) / Step2 后(Step3 前)：批 decode 单步 ~10-25ms，Step
// 粒度中断足够（2026-11-22 异步基建，两版同构）。同步调用（非异步路径/
// 研究工具）传缺省 nullptr/0，恒走完。
static bool score_batch(const std::vector<llama_token> &ctx_ids,
                        const std::vector<std::vector<llama_token>> &cands,
                        std::vector<double> &scores_out,
                        std::vector<std::array<double, 3>> *ce3_out = nullptr,
                        bool *prep_hit_out = nullptr,
                        const std::atomic<long> *cancel_gen = nullptr,
                        long my_gen = 0) {
  scores_out.assign(cands.size(), -1e10);
  if (ce3_out)  // 三分量初值 NaN（缺 token / 失败位 → 调试端显示空）
    ce3_out->assign(cands.size(), {NAN, NAN, NAN});
  if (prep_hit_out)
    *prep_hit_out = false;
  int n_cands = (int)cands.size();
  if (n_cands == 0)
    return true;

  auto cancelled = [&]() {
    return cancel_gen &&
           cancel_gen->load(std::memory_order_relaxed) != my_gen;
  };

  auto t0 = std::chrono::high_resolution_clock::now();
  std::unique_lock<std::mutex> lock(g_mutex);
  // 拿锁后先查一次：等待锁期间任务可能已被覆盖（白跑整段 Step1）
  if (cancelled())
    return false;
  auto t1 = std::chrono::high_resolution_clock::now();
  double wait_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();


  int ctx_len = (int)ctx_ids.size();
  int vs = llama_vocab_n_tokens(g_vocab);

  // group candidates by token count
  std::vector<int> idx2, idx3;
  for (int i = 0; i < n_cands; i++) {
    if (cands[i].size() >= 2)
      idx2.push_back(i);
    if (cands[i].size() >= 3)
      idx3.push_back(i);
  }
  int M = (int)idx2.size();
  int K = (int)idx3.size();

  // Step 1: ctx KV —— prep 完全命中零成本 / 前缀延伸只 decode 增量 / 否则
  // 全量（2026-11-23 与插件版同构，见 ensure_ctx_kv）。
  bool use_prep = g_prep_ready && ctx_ids == g_prep_ctx &&
                  g_seq0_gen == g_prep_gen;
  if (prep_hit_out)
    *prep_hit_out = use_prep;
  float m0 = 0;
  double lse0 = 0;
  double ms1 = 0;
  auto ts1_0 = std::chrono::high_resolution_clock::now();
  const float *ctx_logits = ensure_ctx_kv(ctx_ids, vs, m0, lse0);
  auto ts1_1 = std::chrono::high_resolution_clock::now();
  ms1 = std::chrono::duration<double, std::milli>(ts1_1 - ts1_0).count();
  if (!ctx_logits) return true;  // ctx decode failed（结果全哨兵照常发布）
  // Step 1 CE: P(cand[0] | ctx) for all candidates
  // all candidates share the same ctx logits -> one normalizer scan
  // (2026-11-06: normalizer cached with prep state, hit path skips the scan)
  auto ts_ce1_0 = std::chrono::high_resolution_clock::now();
  std::vector<double> ce_sum(n_cands, 0.0);
  // CE 位置权重 (kCeW[0/1/2]): 见文件头注释; 4+ token 只算前三项, 无外推
  for (int i = 0; i < n_cands; i++) {
    double c1 = ce_target(ctx_logits, cands[i][0], m0, lse0);
    ce_sum[i] = kCeW[0] * c1;
    if (ce3_out)
      (*ce3_out)[i][0] = c1;  // 原始分量（未乘权重，调试展开用）
  }
  auto ts_ce1_1 = std::chrono::high_resolution_clock::now();
  double ms_ce1 =
      std::chrono::duration<double, std::milli>(ts_ce1_1 - ts_ce1_0).count();

  // 中断检查（Step2 前）：此时未分配 worker seq，无 KV 泄漏
  if (cancelled())
    return false;

  // Step 2/3: 前缀去重批量 decode（2026-11-07，两版同步）
  // 行按"已解码前缀"分组：step2 行 = 候选首 token（去重）；step3 行 =
  // (首token, 次token) 对（去重）。同一行上的多个候选从同一 logits 行
  // 读各自的 CE 目标——CE 数学与逐候选行完全一致，只省掉重复行的
  // decode 与 head 计算（标定语料实测 20.3% 行共享首 token、34.1% 窗
  // 省 ≥1 行；行边际成本实测 ~8-9ms，典型窗口省 ~6-7ms）。
  // seq 分配：每个 step2 行一条 worker seq（seq_cp 0→s）；step3 对里
  // 该首 token 的第一个对复用 step2 seq，同首 token 的后续对
  // seq_cp(step2 seq→新 seq) 携带 [ctx, c0] KV——worker seq 总数 =
  // 不同前缀路径数 ≤ 候选数 ≤ n_seq_max-1，天然不越界。
  double ms2a = 0, ms2b = 0, ms3 = 0;
  int seq_top = 0;  // 使用的最大 worker seq（尾部清理用）
  // step2 行：按首 token 去重（线性扫描，候选数 ≤ max_candidates）
  std::vector<llama_token> r2_tok;
  std::vector<std::vector<int>> r2_cands;
  for (int ci : idx2) {
    size_t u = 0;
    while (u < r2_tok.size() && r2_tok[u] != cands[ci][0]) u++;
    if (u == r2_tok.size()) {
      r2_tok.push_back(cands[ci][0]);
      r2_cands.emplace_back();
    }
    r2_cands[u].push_back(ci);
  }
  const int U = (int)r2_tok.size();
  // step3 行：按 (首token, 次token) 对去重
  struct Row3 { int base; llama_token tok1; std::vector<int> cands; int seq; };
  std::vector<Row3> rows3;
  for (int ci : idx3) {
    int base = -1;
    for (int u = 0; u < U; u++)
      if (r2_tok[u] == cands[ci][0]) { base = u; break; }
    int v = 0;
    while (v < (int)rows3.size() &&
           !(rows3[v].base == base && rows3[v].tok1 == cands[ci][1])) v++;
    if (v == (int)rows3.size()) rows3.push_back({base, cands[ci][1], {}, 0});
    rows3[v].cands.push_back(ci);
  }

  bool step2_ok = false;
  if (U > 0) {
    auto ts2_0 = std::chrono::high_resolution_clock::now();
    // KV copy ctx (seq 0) -> 每个 step2 行一条 worker seq
    for (int u = 0; u < U; u++)
      llama_memory_seq_cp(llama_get_memory(g_ctx), 0, u + 1, 0, -1);
    seq_top = U;
    auto ts2_kv = std::chrono::high_resolution_clock::now();

    llama_batch b2 = llama_batch_init(U, 0, U);
    for (int u = 0; u < U; u++) {
      b2.token[u] = r2_tok[u];
      b2.pos[u] = ctx_len;
      b2.n_seq_id[u] = 1;
      b2.seq_id[u][0] = u + 1;
      b2.logits[u] = 1;
    }
    b2.n_tokens = U;
    step2_ok = llama_decode(g_ctx, b2) == 0;
    if (step2_ok) {
      for (int u = 0; u < U; u++) {
        float *l = llama_get_logits_ith(g_ctx, u);
        if (!l) {
          for (int ci : r2_cands[u]) ce_sum[ci] = -1e10;
          if (ce3_out)
            for (int ci : r2_cands[u]) (*ce3_out)[ci] = {NAN, NAN, NAN};
          continue;
        }
        for (int ci : r2_cands[u]) {
          double c2 = cross_entropy(l, vs, cands[ci][1]);
          ce_sum[ci] += kCeW[1] * c2;
          if (ce3_out)
            (*ce3_out)[ci][1] = c2;
        }
      }
    } else {
      for (int ci : idx2)
        ce_sum[ci] = -1e10;
      if (ce3_out)
        for (int ci : idx2) (*ce3_out)[ci] = {NAN, NAN, NAN};
      // B7（2026-11-21）：改 log_error（force 写 X 行）——原 log_msg 被
      // debug_fusion 门控，debug 关时推理失败零痕迹，用户无从排查
      log_error("step2 decode failed");
      // 失败自愈：prep 命中路径无 memory_clear，decode 失败多为 KV 耗尽
      // （见下方 worker 清理说明）——置无效强制下次 score 走全流程重建
      g_prep_ready = false;
    }
    llama_batch_free(b2);
    auto ts2_1 = std::chrono::high_resolution_clock::now();
    ms2a = std::chrono::duration<double, std::milli>(ts2_kv - ts2_0).count();
    ms2b = std::chrono::duration<double, std::milli>(ts2_1 - ts2_kv).count();
  }

  // Step 3: 前缀 (c0,c1) 去重 decode
  const int V = (int)rows3.size();
  if (V > 0 && step2_ok) {
    // 中断检查（Step3 前）：step2 worker seq 已分配，先释放再退出
    if (cancelled()) {
      auto *mem0 = llama_get_memory(g_ctx);
      for (int s = 1; s <= seq_top; s++)
        llama_memory_seq_rm(mem0, s, 0, -1);
      return false;
    }
    auto ts3_0 = std::chrono::high_resolution_clock::now();
    // seq 分配：每个首 token 的第一个对复用 step2 seq；后续对
    // seq_cp(step2 seq → 新 seq)——拷贝发生在 step3 decode 的 update
    // 阶段，取的是 step2 后（ctx+c0）的状态，c1 尚未 decode
    {
      std::vector<int> base_cnt(U, 0);
      int fresh = U;
      for (auto &r : rows3) {
        if (r.base < 0) continue;  // 理论不可达（idx3 ⊆ idx2），防御
        if (++base_cnt[r.base] == 1) {
          r.seq = r.base + 1;      // 复用 step2 seq
        } else {
          r.seq = ++fresh;
          llama_memory_seq_cp(llama_get_memory(g_ctx),
                              r.base + 1, r.seq, 0, -1);
        }
      }
      seq_top = fresh;
    }
    llama_batch b3 = llama_batch_init(V, 0, V);
    for (int v = 0; v < V; v++) {
      b3.token[v] = rows3[v].tok1;
      b3.pos[v] = ctx_len + 1;
      b3.n_seq_id[v] = 1;
      b3.seq_id[v][0] = rows3[v].seq;
      b3.logits[v] = 1;
    }
    b3.n_tokens = V;
    if (llama_decode(g_ctx, b3) == 0) {
      for (int v = 0; v < V; v++) {
        float *l = llama_get_logits_ith(g_ctx, v);
        if (!l) {
          for (int ci : rows3[v].cands) ce_sum[ci] = -1e10;
          if (ce3_out)
            for (int ci : rows3[v].cands) (*ce3_out)[ci] = {NAN, NAN, NAN};
          continue;
        }
        for (int ci : rows3[v].cands) {
          double c3 = cross_entropy(l, vs, cands[ci][2]);
          ce_sum[ci] += kCeW[2] * c3;
          if (ce3_out)
            (*ce3_out)[ci][2] = c3;
        }
      }
    } else {
      for (auto &r : rows3)
        for (int ci : r.cands) ce_sum[ci] = -1e10;
      if (ce3_out)
        for (auto &r : rows3)
          for (int ci : r.cands) (*ce3_out)[ci] = {NAN, NAN, NAN};
      // B7（2026-11-21）：同 step2——失败日志改 force（X|scope=error）
      log_error("step3 decode failed");
      g_prep_ready = false;  // 同 Step2：失败自愈
    }
    llama_batch_free(b3);
    auto ts3_1 = std::chrono::high_resolution_clock::now();
    ms3 = std::chrono::duration<double, std::milli>(ts3_1 - ts3_0).count();
  } else if (V > 0) {
    // step2 失败：idx3 候选直接置哨兵（旧实现跟随一次必败 decode，结果相同）
    for (auto &r : rows3)
      for (int ci : r.cands) ce_sum[ci] = -1e10;
  }

  // worker 序列 KV 清理（2026-09-04 修"推理自停"根因，与插件版同步）：
  // seq_cp 是共享标记非复制，Step2/3 的 decode cell 评分后仍归属 worker
  // seq；prep 命中路径无 memory_clear，同 ctx 连续评分（退格重打/改码
  // 不换 ctx/失败重试）每轮净耗 cell——n_ctx=128 约 14 轮耗尽 → decode
  // 静默失败 → 全哨兵分且 prep_ready 仍真 → 卡死失败循环，仅 commit
  // 触发 prepare 或重部署可解（非本机实测）。评分尾部立即释放 worker
  // 归属：共享 cell 只去掉一个归属方，seq0 与 prep 状态不受影响，prep
  // 命中照常，每次评分净耗归零。2026-11-07 起清理 1..seq_top
  // （= 不同前缀路径数，含 step3 分叉的新 seq）。
  if (seq_top > 0) {
    auto *mem = llama_get_memory(g_ctx);
    for (int s = 1; s <= seq_top; s++)
      llama_memory_seq_rm(mem, s, 0, -1);
  }

  // final scoring: weighted CE sum (first 3 tokens only, no extrapolation
  // - removed 2026-09-16, lambda=0 was optimal across the whole 0..1 sweep)
  auto ts_sc_0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < n_cands; i++) {
    scores_out[i] = ce_sum[i] > -1e9 ? -ce_sum[i] : -1e10;
  }

  auto t2 = std::chrono::high_resolution_clock::now();
  double total_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
  double ms_score =
      std::chrono::duration<double, std::milli>(t2 - ts_sc_0).count();
  // one score line per inference (prep hit check)
  // timing: wait=S1(lock) S1=ctx decode(0 on prep hit) CE1=P(cand0|ctx)
  //         KV=KV copy S2=decode cand0 S3=decode cand1 score=weighted sum
  //         r2/r3 = 前缀去重后的 step2/step3 行数（2026-11-07）
  log_msg("score: wait=%.0fms S1=%.0fms CE1=%.0fms KV=%.0fms S2=%.0fms "
          "S3=%.0fms score=%.0fms total=%.0fms prep=%d ctx_tok=%d cand=%d "
          "r2=%d r3=%d",
          wait_ms, ms1, ms_ce1, ms2a, ms2b, ms3, ms_score, total_ms,
          use_prep ? 1 : 0, ctx_len, n_cands, U, V);
  return true;
}

// ============================================================
// 整句 CE 打分（2026-11-23 第②段；**2026-11-24 起不再被调用**）
//
// 2026-11-24 定案改为 phrase-only：组句只提供文本 + "自信前缀"概率，句子
// 本身不参与 CE（前缀确认的窗口证据由 3-token 融合序给出）。本函数保留为
// 回滚参照（若将来重启整句重排可接回），当前无调用点。
//
// 「LLM 从 5 个组句候选中选 1 个」的真正依据：3-token 路径只看句首三个
// token，对整句重排几乎无效（研究 §11.7：A 区 +6pp、B 区 +1pp，词表内
// 甚至 −1.7pp）；整句 CE 重排 top-1 万象 64%/57%。
//
// 与 3-token 路径**共用 g_ctx / seq0 / prep**（2026-11-23 合并）：
//   Step 1: ctx 就绪——prep 命中（同 ctx，seq0 的 KV+logits 已在）则零成本；
//           否则 decode 到 seq0 并刷新 prep（判据与 score_batch 一致）
//   Step 2: seq_cp(0→i+1) 后把 n 个候选的全部 token 放进**一个 batch**
//           一次 forward，CE 只做查表/归一化
// 整句跑完只清 worker seq（1..n），seq0 与 prep 保持有效——下一次
// score_batch / score_full_batch 仍命中 prep。
//
// sum_out = 完整候选的条件 CE 和（正数，越小越可能）；ce3_out = 前三个
// token 的**原始 CE**（未加权，供子词块零 decode 复用——PM 的前 k 个 token
// 与某个 FM 候选逐位相同时，条件概率与 FM 同位置分量完全相同）。
// 返回 false = 前置条件不成立（候选数/长度/KV 预算）或 ctx decode 失败。
// ============================================================
static bool score_full_batch(const std::vector<llama_token> &ctx_ids,
                             const std::vector<std::vector<llama_token>> &cands,
                             std::vector<double> &sum_out,
                             std::vector<int> &ntok_out,
                             std::vector<std::array<double, 3>> *ce3_out,
                             int max_tokens = 48) {
  int n = (int)cands.size();
  sum_out.assign(n, 0.0);
  ntok_out.assign(n, 0);
  if (ce3_out)
    ce3_out->assign(n, {NAN, NAN, NAN});
  if (n == 0 || n > g_n_seq_max - 1 || ctx_ids.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_loaded.load() || !g_ctx)
    return false;
  const int ctx_len = (int)ctx_ids.size();
  const int vs = llama_vocab_n_tokens(g_vocab);
  int max_len = 0;
  for (const auto &c : cands)
    max_len = (std::max)(max_len, (int)c.size());
  // 每 seq 预算：总 KV 扣掉 ctx 后 n 条 worker 均分（每条需 ctx_len+候选长度）
  int budget = (g_n_ctx - ctx_len) / n - ctx_len - 1;
  max_len = (std::min)(max_len, (std::min)(max_tokens, budget));
  if (max_len <= 0)
    return false;

  // ---- ctx KV：prep 命中零成本 / 前缀延伸只 decode 增量 / 否则全量 ----
  float m0 = 0;
  double lse0 = 0;
  const float *cl = ensure_ctx_kv(ctx_ids, vs, m0, lse0);
  if (!cl)
    return false;  // ctx decode 失败：本轮回 nil，下次全流程兜底

  // 首 token CE（共享 ctx 末位 logits，须在候选 decode 之前算完）
  for (int i = 0; i < n; i++) {
    if (cands[i].empty())
      continue;
    double c = ce_target(cl, cands[i][0], m0, lse0);
    sum_out[i] = c;
    ntok_out[i] = 1;
    if (ce3_out)
      (*ce3_out)[i][0] = c;
  }
  // ctx KV → 每个候选 seq（先清目的 seq：prep 命中路径不清整个 KV，
  // 上一轮残留会让 seq_cp 叠加出脏 KV）
  auto *mem0 = llama_get_memory(g_ctx);
  for (int i = 0; i < n; i++) {
    llama_memory_seq_rm(mem0, i + 1, 0, -1);
    llama_memory_seq_cp(mem0, 0, i + 1, 0, -1);
  }

  // ---- 一次性 decode 全部候选 token（每候选一条 seq，ctx KV 已 copy）----
  // 逐步 decode（每 token 一次 forward）在 0.6B 上每步约 20ms、15 步 ≈ 300ms；
  // 这里把所有候选 token 放进**同一个 batch**、一次 forward 取全位置 logits，
  // CE 只做查表/归一化——同一数学，forward 次数从 ΣN 降到 1。
  std::vector<int> base(n, -1);
  std::vector<int> lens(n, 0);
  int total = 0;
  for (int i = 0; i < n; i++) {
    lens[i] = (std::min)((int)cands[i].size(), max_len);
    total += lens[i];
  }
  if (total <= 0) {
    for (int i = 0; i < n; i++)
      llama_memory_seq_rm(mem0, i + 1, 0, -1);
    return true;
  }
  llama_batch b = llama_batch_init(total, 0, n);
  int row = 0;
  for (int i = 0; i < n; i++) {
    base[i] = row;
    int len = lens[i];
    for (int j = 0; j < len; j++, row++) {
      b.token[row] = cands[i][j];
      b.pos[row] = ctx_len + j;
      b.n_seq_id[row] = 1;
      b.seq_id[row][0] = i + 1;
      // 该 token 的 logits 预测下一个 token；最后一个不用
      b.logits[row] = (j + 1 < len) ? 1 : 0;
    }
  }
  b.n_tokens = row;
  bool ok = llama_decode(g_ctx, b) == 0;
  if (ok) {
    for (int i = 0; i < n; i++) {
      int len = lens[i];
      for (int j = 1; j < len; j++) {
        float *l = llama_get_logits_ith(g_ctx, base[i] + j - 1);
        if (!l) {
          sum_out[i] = 1e10;
          break;
        }
        float m;
        double lse;
        logits_normalizer(l, vs, m, lse);
        double c = ce_target(l, cands[i][j], m, lse);
        sum_out[i] += c;
        ntok_out[i] = j + 1;
        if (ce3_out && j < 3)
          (*ce3_out)[i][j] = c;
      }
    }
  } else {
    // 失败自愈：prep 命中路径不清 KV，decode 失败可能是 KV 耗尽——
    // 置无效强制下一次走全流程重建（同 score_batch Step2/3）
    log_error("full-sentence decode failed (%d cands, %d tokens)", n, total);
    g_prep_ready = false;
    for (int i = 0; i < n; i++)
      sum_out[i] = 1e10;
  }
  llama_batch_free(b);
  // worker seq 清理（seq0 与 prep 保留，3-token 路径仍可命中）
  for (int i = 0; i < n; i++)
    llama_memory_seq_rm(mem0, i + 1, 0, -1);
  return true;
}

// ============================================================
// prepare: pre-decode Step 1 (ctx decode -> save logits) so the
// next score_batch can skip Step 1 when the ctx matches.
// Runs in the g_mutex (same as score_batch); called from a detached
// thread after commit. seq is a request sequence: a newer prepare
// supersedes an older one, which skips itself when it acquires the lock.
// ============================================================
static void prepare(const std::vector<llama_token> &ctx_ids, int seq) {
  // 推理进行中 (score_batch 持锁) 则放弃本轮: prep 只是预解码优化,
  // 下次 score 的全流程 (self-refresh) 兜底, 不排队等待 —— 避免
  // prepare 风暴占用 g_mutex 阻塞 IPC 消息线程 (QQ 音乐等应用打不开)
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    // 2026-11-18: do not drop silently - remember it and retry after the
    // in-flight score releases the lock (see retry_pending_prepare()).
    request_prepare_retry(ctx_ids, seq);
    log_msg("prepare: DEFER score in progress (seq=%d ctx_tok=%d)", seq,
            (int)ctx_ids.size());
    return;
  }

  int ctx_len = (int)ctx_ids.size();
  if (ctx_len == 0)
    return;

  // dedup: same ctx already pre-decoded (by a previous prepare or by a
  // score full pass self-refresh) -> nothing to do
  if (g_prep_ready && ctx_ids == g_prep_ctx)
    return;

  // stale request: a newer prepare arrived while this one waited for the lock
  if (seq != g_prep_seq.load()) {
    log_msg("prepare: SKIP stale seq=%d current=%d ctx_tok=%d", seq,
            g_prep_seq.load(), ctx_len);
    return;
  }
  auto t0 = std::chrono::high_resolution_clock::now();
  int vs = llama_vocab_n_tokens(g_vocab);
  auto *mem = llama_get_memory(g_ctx);

  g_prep_ready = false;
  g_prep_ctx.clear();
  g_prep_logits.clear();

  llama_memory_clear(mem, false);
  llama_batch ctx_batch = llama_batch_init(ctx_len, 0, 1);
  for (int j = 0; j < ctx_len; j++) {
    ctx_batch.token[j] = ctx_ids[j];
    ctx_batch.pos[j] = j;
    ctx_batch.n_seq_id[j] = 1;
    ctx_batch.seq_id[j][0] = 0;
  }
  ctx_batch.logits[ctx_len - 1] = 1;
  ctx_batch.n_tokens = ctx_len;
  if (llama_decode(g_ctx, ctx_batch) != 0) {
    llama_batch_free(ctx_batch);
    log_error("prepare: ctx decode failed");
    return;  // score falls back to the full pass
  }
  float *cl = llama_get_logits_ith(g_ctx, ctx_len - 1);
  if (!cl) {
    llama_batch_free(ctx_batch);
    log_error("prepare: no logits");
    return;
  }
  g_prep_logits.assign(cl, cl + vs);
  llama_batch_free(ctx_batch);

  g_seq0_gen++;  // seq0 KV updated (this pass)
  g_prep_gen = g_seq0_gen;
  g_prep_ctx = ctx_ids;
  // 归一化量随 prep 一并算好（2026-11-06）：score 的 prep 命中路径直接取用，
  // 省一次 O(vocab) 扫描。放在 g_prep_ready 置位前、锁内完成，无 torn state。
  logits_normalizer(g_prep_logits.data(), vs, g_prep_m, g_prep_lse);
  g_prep_ready = true;
  auto t1 = std::chrono::high_resolution_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  log_msg("prepare: seq=%d ctx_tok=%d %.0fms", seq, ctx_len, ms);
}

// 2026-11-18: run a prepare() that was deferred because the inference lock
// was busy. Called right after score_batch() returns (lock released).
static void retry_pending_prepare() {
  std::vector<llama_token> ids;
  int seq = -1;
  if (!take_prepare_retry(ids, seq))
    return;
  if (!g_loaded.load() || ids.empty())
    return;
  if (seq != g_prep_seq.load())
    return;  // a newer request superseded it
  std::thread([ids, seq]() { prepare(ids, seq); }).detach();
  log_msg("prepare: retry deferred ctx_tok=%d seq=%d", (int)ids.size(), seq);
}

// ============================================================
// 异步评分器（2026-11-22 异步基建，两版同构）
// 同步路径零 LLM：Collect 每键 async_submit 投递，评分在持久工作线程执行；
// 字词模式 submit+wait（与既有同步评分等价的延迟面——当前词重排零回归），
// 整句模式 submit 不等待（0 LLM 打字流畅），结果经后续 Collect 的收割点
// 回填（同指纹重复 query 即缓存命中；整句挂账补写语料 W ok）。
// 任务槽覆盖式 + 代次中断（submit/cancel 递增 g_async_gen，score_batch
// 每 Step 边界检查）+ 40ms 去抖（非 immediate 任务，等编码稳定才开跑）。
// 指纹 = 归一化上文 \x1f 候选 \x1e 拼接原串（上文/候选变化自然失效，无需
// 失效链）。结果槽只存最近完成一份。
// ============================================================
static std::atomic<long> g_async_gen{1};
static std::mutex g_async_mu;
static std::condition_variable g_async_cv;
static std::thread g_async_thread;
static std::atomic<bool> g_async_started{false};

static constexpr int kAsyncDebounceMs = 40;

// 整句模式同步等待预算（2026-11-23 修）：原先整句模式"投递即走"，当前键
// 只能显示层 1 序，LLM 评分只在同指纹重查时才用上（真机 100ms/键实测
// 连续整句打字从未上屏）。改为每键最多等这么久——真机实测 6 个长词候选
// 的 3-token 打分约 65ms，150ms 留余量；超时仍回退挂账 pending（层 1 序）。
// 环境变量 RIME_LLM_SENTENCE_WAIT_MS 可覆盖（0 = 旧的"投递即走"语义，
// 测试/回退用）。
static int g_sentence_wait_ms = []() {
  const char *e = getenv("RIME_LLM_SENTENCE_WAIT_MS");
  if (!e)
    return 150;
  int v = atoi(e);
  return v < 0 ? 150 : v;
}();

struct AsyncTask {
  bool valid = false;
  long gen = 0;                // 提交时的代次（中断判定基准）
  bool immediate = false;      // wait 模式：跳过去抖立即开跑
  std::string fingerprint;
  std::string ctx_norm;        // normalize_ctx 后的上文（token 化源）
  std::vector<std::string> cand_texts;
  // 子词（PM，部分覆盖候选）清单：保留字段（统一准入后恒空）
  std::vector<std::string> pm_texts;
  std::chrono::steady_clock::time_point enq_at;
};
static AsyncTask g_async_task;

struct AsyncRes {
  bool valid = false;
  std::string fingerprint;
  // 提交序文本（2026-11-24）：指纹改成顺序无关后，候选序会随融合变化，
  // 应用快照必须**按文本**映射而不是按下标。
  std::vector<std::string> cand_texts;
  std::vector<std::string> pm_texts;
  std::vector<double> scores;   // 提交序（= 提交时 candidates_ 的序）
  std::vector<std::array<double, 3>> ce3;
  std::vector<std::string> segs;  // token 边界段显（协议 v7，对齐提交序）
  // 子词复用分：保留字段（统一准入后恒空）
  std::vector<double> pm_scores;
  std::string ctx_used;           // 推理用上文尾部（协议 v4，调试灰显）
  bool prep_hit = false;
  double ev_ms = 0;
};
static AsyncRes g_async_result;

// 无按键刷新门控（2026-11-23 第②段）：整句 CE 落地时自增。整句 CE 已于
// 2026-11-24 退役（phrase-only），代次现在恒不前进——刷新链只由
// RimeLlmPendingConfirm()（有挂起确认）驱动。槽保留：rime_api.h 的
// RimeLlmAsyncRevision() 仍需一个稳定指针。
static std::atomic<int> g_async_revision{0};
std::atomic<int> *g_llm_async_revision_slot() {
  return &g_async_revision;
}

// 刷新重入深度（2026-11-23 晚修）：RimeRefreshComposition 执行期间 >0。
// 前缀软确认的 ctx->Select 只能在刷新路径里做——在 PushInput 的
// update_notifier 里改组合区会让 process_key 后续流程异常（真机表现为
// 该键无响应、TSF 侧卡死，需杀服务）。见 OnContextUpdate。
static std::atomic<int> g_refresh_depth{0};
std::atomic<int> *g_llm_refresh_depth_slot() {
  return &g_refresh_depth;
}

// 待执行的前缀确认（2026-11-23 晚修）：weasel 侧 Refresh 据此在代次未变时
// 也允许一次重译（挂起确认必须有机会被刷新执行，否则要等到下一个 LLM
// 结果落地才生效）。经 g_llm_pending_confirm_query 暴露给 rime_api_impl.h。
bool g_llm_pending_confirm_query() {
  std::lock_guard<std::mutex> lk(g_pending_confirm_mu);
  return g_pending_confirm.valid;
}

// 指纹（2026-11-22）：归一化上文 \x1f 候选 \x1e 拼接；2026-11-23 追加子词
// 清单（\x1d 分隔，整句模式才有）——子词复用分与候选集属同一快照，指纹
// 必须绑定，否则同 FM 不同 PM 的 query 会错命中缓存（与插件版
// async_fingerprint 同构；字词模式 pm 为空 → 指纹格式保持不变）。
static std::string async_fingerprint(const std::string &ctx_norm,
                                     const std::vector<std::string> &cands,
                                     const std::vector<std::string> &pm) {
  // 2026-11-24：候选/子词按**文本排序**后再拼指纹（顺序无关）——此前顺序
  // 参与指纹，刷新重排一次指纹即变 → 重新投递 → 新结果再换序 → 自持循环
  //（真机表现为服务满 CPU 空转、所有 TSF 应用在管线上等死）。
  std::string fp = ctx_norm;
  fp.push_back('\x1f');
  {
    std::vector<std::string> cs = cands;
    std::sort(cs.begin(), cs.end());
    for (size_t i = 0; i < cs.size(); i++) {
      if (i)
        fp.push_back('\x1e');
      fp += cs[i];
    }
  }
  if (!pm.empty()) {
    fp.push_back('\x1d');
    std::vector<std::string> ps = pm;
    std::sort(ps.begin(), ps.end());
    for (size_t i = 0; i < ps.size(); i++) {
      if (i)
        fp.push_back('\x1e');
      fp += ps[i];
    }
  }
  // 评分口径标记（保留一个尾标记：历史快照/插件版同构；整句 CE 已于
  // 2026-11-24 退役，现在恒为 3-token 口径）
  fp.push_back('\x1b');
  return fp;
}

// 挂账：整句模式异步投递时登记（fp → W 行所需信息），评分结果收割时补写
// W ok（flags=async）+ P 行。任务被覆盖/取消则条目随容量清理丢弃（快速
// 连打的中间键不落语料 = "只有完成的评分才落 W"的异步语义）。
struct AsyncPending {
  std::string wid;
  std::string code;            // 活动段切片码
  std::string wctx;
  std::vector<std::string> engine_vec;
  std::vector<std::string> window_vec;   // 层1窗（提交序）
  long long tick = 0;
  std::string src;
  bool sent_on = false;        // 挂账来源（整句模式恒 true）
};
static std::map<std::string, AsyncPending> g_async_pending;

// 评分快照缓存（fp → AsyncRes）：同指纹重复 query（翻页/候选窗重建/收割
// 回填）命中即融合，0 LLM。融合分每次现算（eff/pairfix 微秒级），缓存只存
// 原始评分快照——语义与同步版逐位一致。
static std::map<std::string, AsyncRes> g_async_cache;

// 挂账回填：补写 W ok + P 行。final = 窗内候选融合重排（文本域现算，与
// 同步版融合公式逐位一致；eff/uni/pairfix 为微秒级内存查询；账本 tick 与
// 提交时刻的漂移 ≤1 词提交，v1 接受——D/L/C 明细行省略，W final 序已够
// 评测）。
static void async_write_pending(const AsyncPending &pend,
                                const AsyncRes &snap) {
  std::map<std::string, double> pf_tbl;
  bool pf_on = pairfix_table(&pf_tbl);
  double min_valid = 0.0;
  bool have_min = false;
  for (size_t i = 0; i < pend.window_vec.size(); i++) {
    double s = i < snap.scores.size() ? snap.scores[i] : -1e10;
    if (s > -1e9 && (!have_min || s < min_valid)) {
      min_valid = s;
      have_min = true;
    }
  }
  std::vector<size_t> ord(pend.window_vec.size());
  std::vector<double> fused(pend.window_vec.size(), -1e308);
  for (size_t i = 0; i < pend.window_vec.size(); i++) {
    ord[i] = i;
    double s = i < snap.scores.size() ? snap.scores[i] : -1e10;
    const std::string &t = pend.window_vec[i];
    double s1 = g_freq_beta * std::log(1.0 + user_freq_eff(t, pend.code)) +
                user_unigram_term(t, pend.code);
    double pf = pf_on ? pairfix_delta(pf_tbl, t, pend.code) : 0.0;
    if (s > -1e9)
      fused[i] = s + s1 + pf;
    else  // 无 CE 分（哨兵/缺失）：层1 分 + 窗内最低 LLM 分（压制窗底）
          // ——不叠词码对修正（2026-11-24：pairfix 只修 CE 系统性偏差）
      fused[i] = s1 + (have_min ? min_valid : 0.0);
  }
  std::stable_sort(ord.begin(), ord.end(), [&fused](size_t a, size_t b) {
    return fused[a] > fused[b];  // stable = 同分保提交序（层1 序）
  });
  std::vector<std::string> final_list;
  for (size_t k : ord) final_list.push_back(pend.window_vec[k]);
  write_w(pend.wid, pend.src, pend.code, pend.wctx, pend.engine_vec,
          pend.window_vec, final_list, pend.tick, "ok", "",
          snap.prep_hit, "async");
  debug_write_p(pend.wid, snap.ev_ms, snap.prep_hit);
  // D 行（2026-11-22 整句分段开发补）：挂账回填此前只有 W ok + P 行，
  // CE 数值无日志——评测与调试都要逐候选 CE。字段语义与主路径一致：
  // ce=加权分（scores）、ce1-3=原始分量、eff/u1 回填时刻现算、full=1
  //（挂账恒整句模式）、key=融合分、sdelta=词码对修正、cerank=提交序
  //（层1）、finrank=回填后名次。
  for (size_t r = 0; r < ord.size(); r++) {
    size_t i = ord[r];
    double ce = i < snap.scores.size() ? snap.scores[i] : -1e10;
    auto c3 = (i < snap.ce3.size())
                  ? snap.ce3[i]
                  : std::array<double, 3>{NAN, NAN, NAN};
    const std::string &t = pend.window_vec[i];
    double eff = user_freq_eff(t, pend.code);
    double u1 = user_unigram_term(t, pend.code);
    double pf = (pf_on && ce > -1e9)
                    ? pairfix_delta(pf_tbl, t, pend.code) : 0.0;
    debug_write_d(pend.wid, (int)r + 1, ce, c3[0], c3[1], c3[2], eff,
                  g_freq_beta * std::log(1.0 + eff), u1, 1, fused[i], pf,
                  (int)i + 1, (int)r + 1);
  }
}

// 子词复用分（reuse_pm_scores）已于 2026-11-24 退役：统一准入后子词
// 直接进评分窗拿真分，不再需要 FM 前缀复用。

static void async_worker_loop() {
  for (;;) {
    AsyncTask task;
    {
      std::unique_lock<std::mutex> lk(g_async_mu);
      g_async_cv.wait(lk, [] { return g_async_task.valid; });
      task = g_async_task;
      g_async_task.valid = false;   // 领走
      // 去抖：非 immediate 任务等 40ms 无更新才开跑；期间新任务到达
      //（notify 唤醒）则换新任务重新等。immediate（字词 wait 路径）跳过。
      if (!task.immediate) {
        for (;;) {
          auto deadline =
              task.enq_at + std::chrono::milliseconds(kAsyncDebounceMs);
          bool updated = g_async_cv.wait_until(lk, deadline, [&] {
            return g_async_task.valid && g_async_task.enq_at > task.enq_at;
          });
          if (!updated)
            break;
          task = g_async_task;
          g_async_task.valid = false;
        }
      }
    }
    // 领取后已被覆盖/取消（gen 前进）：丢弃，回到等待
    if (task.gen != g_async_gen.load(std::memory_order_relaxed))
      continue;

    // 模型懒加载触发（与同步路径同语义：不等待加载——加载中任务发布
    // 失败结果，wait 路径上层 skip not_ready；就绪后后续任务正常评分）
    if (!g_loaded.load()) {
      if (!g_loading.load())
        load_model_async();
      {
        std::lock_guard<std::mutex> lk(g_async_mu);
        g_async_result = AsyncRes{};
        g_async_cv.notify_all();
      }
      continue;
    }

    AsyncRes res;
    res.fingerprint = task.fingerprint;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<llama_token> ctx_ids = tokenize(task.ctx_norm.c_str());
    bool ctx_ok = (int)ctx_ids.size() >= g_min_tokens;
    std::vector<std::vector<llama_token>> cand_ids;
    if (ctx_ok) {
      // token 域截尾（与同步路径同款：保留尾部 g_max_ctx_tokens 个 token；
      // ctx_used = 截尾时保留段的 piece 拼接，未截尾 = 全文）
      if ((int)ctx_ids.size() > g_max_ctx_tokens) {
        ctx_ids.erase(ctx_ids.begin(),
                      ctx_ids.end() - g_max_ctx_tokens);
        res.ctx_used = tokens_piece(ctx_ids);
      } else {
        res.ctx_used = task.ctx_norm;
      }
      for (auto &s : task.cand_texts) {
        auto ids = tokenize(s.c_str());
        if (ids.empty())
          ids.push_back(0);
        res.segs.push_back(token_seg_display(ids));
        cand_ids.push_back(std::move(ids));
      }
    }
    if (!ctx_ok || cand_ids.size() < 2) {
      std::lock_guard<std::mutex> lk(g_async_mu);
      g_async_result = AsyncRes{};   // 失败发布（valid=false）
      g_async_cv.notify_all();
      continue;
    }
    bool done = false;
    try {
      done = score_batch(ctx_ids, cand_ids, res.scores, &res.ce3,
                         &res.prep_hit, &g_async_gen, task.gen);
    } catch (...) {
      log_error("exception in async score_batch");
    }
    // 评分线程锁已释放：补跑被延迟的 prepare（同步路径收尾动作同款）
    retry_pending_prepare();
    auto t1 = std::chrono::high_resolution_clock::now();
    res.ev_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (done) {
      // 子词复用分通道已于 2026-11-24 退役（子词进窗拿真分，不再需要
      // FM 前缀复用）；task.pm_texts 现恒空。
      res.valid = true;
      res.cand_texts = task.cand_texts;  // 快照按文本映射（2026-11-24）
      std::lock_guard<std::mutex> lk(g_async_mu);
      g_async_result = std::move(res);
      g_async_cv.notify_all();
    }
    // 中断（done=false）：不发布，结果槽留给新任务
  }
}

static void async_ensure_thread() {
  if (g_async_started.load())
    return;
  std::lock_guard<std::mutex> lk(g_async_mu);
  if (!g_async_started.load()) {
    g_async_thread = std::thread(async_worker_loop);
    g_async_thread.detach();    // 进程内常驻，随进程退出
    g_async_started.store(true);
  }
}

static std::string async_submit(const std::string &ctx_norm,
                                const std::vector<std::string> &cand_texts,
                                const std::vector<std::string> &pm_texts,
                                bool immediate) {
  if (cand_texts.size() < 2 || ctx_norm.empty())
    return "";
  async_ensure_thread();
  std::string fp = async_fingerprint(ctx_norm, cand_texts, pm_texts);
  {
    std::lock_guard<std::mutex> lk(g_async_mu);
    long gen = g_async_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    g_async_task = AsyncTask{};
    g_async_task.valid = true;
    g_async_task.gen = gen;
    g_async_task.immediate = immediate;
    g_async_task.fingerprint = fp;
    g_async_task.ctx_norm = ctx_norm;
    g_async_task.cand_texts = cand_texts;
    g_async_task.pm_texts = pm_texts;
    g_async_task.enq_at = std::chrono::steady_clock::now();
  }
  g_async_cv.notify_all();
  return fp;
}

// 字词模式：阻塞等待指纹匹配的评分结果（与既有同步评分等价的延迟面）。
static bool async_wait(const std::string &fp, int timeout_ms, AsyncRes &out) {
  std::unique_lock<std::mutex> lk(g_async_mu);
  bool got = g_async_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [&] {
                                   return g_async_result.valid &&
                                          g_async_result.fingerprint == fp;
                                 });
  if (!got)
    return false;
  out = g_async_result;
  g_async_result.valid = false;   // 取走即清（调用方有缓存）
  return true;
}

// 整句模式：非阻塞收割最近完成的评分结果。
static bool async_poll(AsyncRes &out) {
  std::lock_guard<std::mutex> lk(g_async_mu);
  if (!g_async_result.valid)
    return false;
  out = g_async_result;
  g_async_result.valid = false;
  return true;
}

// 代次递增 + 清任务槽：提交后中断在跑的评分（省 CPU 与锁占用）。
// 结果槽保留——完成的评分成果仍可被后续收割点回收。
static void async_cancel() {
  g_async_gen.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(g_async_mu);
  g_async_task = AsyncTask{};
}

// 收割点（2026-11-22）：回收最近完成的评分成果——回填缓存 + 补写挂账
//（W ok / flags=async + P 行）。两个调用点：① Collect 入口（每次 query，
// 翻页/候选窗重建命中缓存即 0 LLM）；② OnCommit（上屏时本词评分多半已
// 完成——立即落账，场景内最后一词也有 ok 行，语料断言与词码对 ref 关联
// 都不再依赖"下一次 query"）。调用方须与 Collect 同线程（commit 通知与
// process_key 同在 server 消息线程——语料写入无跨线程竞争）。
static void async_harvest() {
  // 单槽结果（3-token 路径；整句 CE 已于 2026-11-24 退役）
  {
    AsyncRes old;
    if (async_poll(old)) {
      g_async_cache[old.fingerprint] = old;
      auto pit = g_async_pending.find(old.fingerprint);
      if (pit != g_async_pending.end()) {
        async_write_pending(pit->second, old);
        g_async_pending.erase(pit);
      }
    }
  }
  if (g_async_cache.size() > 16)
    g_async_cache.clear();
}

// ============================================================
// LlmRerankTranslation
// ============================================================

LlmRerankTranslation::LlmRerankTranslation(an<Translation> translation,
                                           const std::string &input,
                                           size_t seg_start, size_t seg_end,
                                           const std::string &ctx,
                                           const std::string &src,
                                           Engine *engine)
    : translation_(translation), input_(input), seg_start_(seg_start),
      seg_end_(seg_end), ctx_(ctx), src_(src), engine_(engine) {
  // 词码对记账（2026-11-13）: 记最近窗码供 OnCommit 用（commit 时 input
  // 已清空; 同插件版 pending_code 捕获语义。engine thread, 无竞争）
  g_last_input = input;
  Collect();
}

bool LlmRerankTranslation::Next() {
  // 2026-11-24 关键修复（真机整机卡死根因）：必须与基类 Exhaustible 配合。
  // `Menu::Prepare` 的循环是
  //   while (candidates_.size() < requested && !result_->exhausted())
  //     { Peek(); Next(); }
  // 本类此前从不 set_exhausted(true)：当请求条数 > 我们提供的条数时，
  // Peek() 返回空、Next() 原地返回 false → 循环永不退出（满 CPU 空转，
  // 服务 IPC 全堵，所有 TSF 应用一起冻住）。走到末尾即置 exhausted。
  if (index_ >= candidates_.size()) {
    set_exhausted(true);
    return false;
  }
  ++index_;
  if (index_ >= candidates_.size())
    set_exhausted(true);
  return !exhausted();
}

an<Candidate> LlmRerankTranslation::Peek() {
  if (index_ >= candidates_.size())
    return nullptr;
  return candidates_[index_];
}

void LlmRerankTranslation::Collect() {
  // v3 两层重排 (2026-11-07 用户定案): 层1 = 全体候选 eff+累计Unigram 打分稳定
  // 降序（同分保引擎序）→ 前 max_candidates 进 LLM 评分（层2 三项融合重排,
  // 在下方融合块）, 其余按层1 序透传。目标在层1 前 k 外 → 必然不在首位
  // （评测 sweep_2layer 同语义, 全窗参评）。文件序无关性: 引擎调频与否仅
  // 影响冷词同分打破（目标卷入 ≤0.05pp 窗, 研究库 §三十三·附 6）。
  const size_t kMaxRerank = (size_t)(g_max_candidates > 0 ? g_max_candidates : 6);
  std::vector<an<Candidate>> tail;      // 窗外候选（层1 序透传，FM+子词混排）
  std::vector<double> s1_head;   // 层1 分（对齐 candidates_; 无 CE 分回退用）
  std::vector<char> win_fm;      // 窗内候选 FM 标志（对齐 candidates_，整句模式）
  // 词码对表：融合前读一次（2026-11-21 解耦后只含负分），本窗融合复用。
  std::map<std::string, double> pf_tbl;
  bool pf_on = false;

  std::vector<an<Candidate>> allc;
  // 限量收集（2026-11-22 卡死修复，与插件版同构）：旧全量收集强制引擎
  // 一次性完整翻译所有候选——朙月大词库的候选爆炸主要来自连续声母简拼
  // （全拼串的 completion 有限，简拼串海量），单码秒级 CPU
  // （enabled=false 对照实测 40 倍差）→ 每键卡死。
  // 双重限制：16 上限 + **30ms 时间预算**（个数闸管收多少、预算闸管贵不
  // 贵；深枚举的单次 Peek 内部秒级、预算无法打断——必须在深枚举发起前
  // 拦住）。上限取 16（2026-11-23 wiki 全拼实测：真实引擎候选序中正确候选
  // 位置 p95=2 / p99=7 / p99.9=20，16 覆盖 99.82% 词型、99.92% 词频加权；
  // 32 的边际覆盖仅 +0.16pp，性价比不足）。
  constexpr size_t kMaxCollect = 16;
  const auto collect_budget = std::chrono::milliseconds(30);
  const auto collect_start = std::chrono::steady_clock::now();
  for (size_t n = 0; n < kMaxCollect; n++) {
    if (std::chrono::steady_clock::now() - collect_start > collect_budget)
      break;
    auto cand = translation_->Peek();
    if (!cand)
      break;
    allc.push_back(cand);
    translation_->Next();
  }
  // engine_top 用引擎原序（组句注入前捕获；语料 W 的 engine_top 字段口径不变）
  std::vector<std::string> engine_vec = texts_of(allc, 10);

  // ═══ N-gram 组句候选前置注入（2026-11-23 第②段，与插件版同构）═══
  // 生成 top-20 整句候选放在候选流最前（同文本去重），后续层1/准入/3-token
  // 评分/前缀判定沿用既有逻辑：span == 活动段 → FM。组句**不参与 CE**
  // （2026-11-24 phrase-only：显示只置顶 ngram 序第 1，top-20 分数供 lead
  // 概率）。只在整句模式 + 方案输出简体 + ≥4 码（≈2 音节）时启用。
  // 注入点的开关态 gate（2026-11-23 晚修）：option_name 存在时（显式开关
  // 方案）按当前状态判定；空 = simplifier 恒转换，恒可注入。
  const bool simp_on =
      g_simplifier_option.empty() ||
      (engine_ && engine_->context() &&
       engine_->context()->get_option(g_simplifier_option));
  std::map<std::string, NgramGenInfo> gen_info;
  std::vector<std::string> gen_texts;   // ngram 序（显示置顶/全句 CE 投递用）
  if (g_sentence_mode && g_ngram_group && simp_on &&
      (seg_end_ - seg_start_) >= 4) {
    // 2026-11-24 修（真机报障"确认第一个词后候选变乱/不再确认"）：
    // Collect 的 `input_` **本身就是活动段切片**（Apply 已按段边界 substr 过，
    // 见 LlmRerankTranslation 构造调用），而 seg_start_/seg_end_ 是**原串
    // 坐标**——此前再 substr 一次 = 双重偏移，Select 后切片变 'i'、生成器
    // 按错误码出"哦正常"这类垃圾候选。直接用 input_。
    const std::string &code_g = input_;
    log_msg("ngram: inject code='%s' seg=[%d,%d) simp_on=%d", code_g.c_str(),
            (int)seg_start_, (int)seg_end_, (int)simp_on);
    std::string sid;
    if (engine_ && engine_->schema())
      sid = engine_->schema()->schema_id();
    std::string gctx = ctx_;
    if (g_prefix_confirm) {
      std::string conf = ConfirmedPrefixText(engine_->context());
      if (!conf.empty())
        gctx += conf;
    }
    std::vector<rime_llm_ngram::Sentence> gen =
        ngram_generate(code_g, gctx, sid);
    if (!gen.empty()) {
      std::vector<an<Candidate>> merged;
      std::map<std::string, char> seen_t;   // 只装"注入/去重"的显示那 1 条
      std::map<std::string, char> gen_seen; // 组句内部去重（概率表用）
      for (const auto &s : gen) {
        if (s.text.empty() || gen_seen.count(s.text))
          continue;
        gen_seen[s.text] = 1;
        // 2026-11-24 修（真机报障"部分候选不正常"）：组句必须**从码位 0 起
        // 连续覆盖到码末**才允许注入——多段组合/切片异常时，生成器可能给出
        // 只覆盖尾部的句子（如 code='ozhengchang' → 哦正常），这些句子被
        // 强行标成全段满覆盖后会冒充首选。覆盖校验取 words[].code_end 序列。
        bool covers = !s.words.empty();
        int prev_end = 0;
        for (const auto &w : s.words) {
          if (w.code_end <= prev_end) {
            covers = false;
            break;
          }
          prev_end = w.code_end;
        }
        if (!covers || prev_end != (int)code_g.size()) {
          log_msg("ngram: drop partial '%s' cover=%d/%d", s.text.c_str(),
                  prev_end, (int)code_g.size());
          continue;
        }
        // 2026-11-24 用户报障「象/向/想 等整字从候选里消失」：只把**显示
        // 置顶的那 1 条**（ngram 序第 1，phrase-only 显示）注入并登记去重；
        // 其余 19 条只进 gen_info 供前缀概率。旧实现把 top-20 全部文本都塞进
        // seen_t/去重表，与组句同名的引擎候选被整条丢弃且无替代。
        if (gen_texts.empty()) {
          seen_t[s.text] = 1;
          merged.push_back(an<Candidate>(new SimpleCandidate(
              "ngram_group", seg_start_, seg_end_, s.text)));
        }
        NgramGenInfo gi;
        if (!s.words.empty()) {
          gi.lead = s.words[0].word;
          gi.lead_end = s.words[0].code_end;
        }
        gi.score = s.score;
        gen_info[s.text] = gi;
        gen_texts.push_back(s.text);
      }
      for (const auto &c : allc)
        if (!seen_t.count(c->text()))
          merged.push_back(c);
      allc.swap(merged);
    }
  } else if ((seg_end_ - seg_start_) >= 4) {
    // 诊断（2026-11-23 晚）：组句未注入时给出具体门控值
    log_msg("ngram: no-inject sent=%d ng=%d simp_on=%d len=%d",
            (int)g_sentence_mode, (int)g_ngram_group, (int)simp_on,
            (int)(seg_end_ - seg_start_));
  }

  // 整句模式准入（2026-11-24 用户定案，取代 2026-11-22「满覆盖优先
  // 占满评分窗」）：层1 词频在**等长词 + 子词**里统一取前 max_candidates，
  // 窗内全部真算 CE——与字词模式完全一致（等长 ≠ 概率高）。组句不占窗席、
  // 不参与评分（2026-11-24 phrase-only），显示端只置顶 ngram 序第 1。
  // FM（span 恰为活动段）判定保留给组句覆盖校验与调试 full 标志。
  bool sent_on = g_sentence_mode;
  std::vector<char> is_fm;   // 对齐 allc（sent_on 时有效）
  if (sent_on) {
    is_fm.resize(allc.size(), 0);
    for (size_t i = 0; i < allc.size(); i++)
      is_fm[i] = (allc[i]->start() == seg_start_ &&
                  allc[i]->end() == seg_end_) ? 1 : 0;
  }

  // 语料 W 事件（2026-11-17）：一次满码尝试一条，含 skip 原因。
  // 只在 enabled + code_pattern 匹配时进入 Collect（Apply 已筛），
  // 部分输入码不会到这里，避免每键落一行。
  std::string wctx = normalize_ctx(ctx_);
  std::string wid = new_event_id("W", g_user_tick);
  std::vector<std::string> window_vec;
  bool w_written = false;
  std::string wstatus = "ok";
  std::string wskip;
  bool prep_hit = false;  // 上文 KV 预解码命中（调试 L 行"预解"列）
  // 无上文（2026-11-24 用户定案）：CE 不可算，但整句模式下前缀确认照常
  // ——融合分按剩余项（β·log1p(eff) + log(uni+1) + 词码对修正）计算。
  // Apply 已保证：只有整句模式 + 前缀确认 + 组句都开且 code≥4 码时才放行
  // 到这里的空 ctx；ctx 里已含组合区已确认前缀（上一条 phrase 就是"临时
  // 上文"，确认后 ctx 非空、自动回到完整 CE 融合）。函数级作用域（末尾
  // W flags 写 nofreq 要用）。
  bool freq_only = false;

  // 异步基建收割点（2026-11-22）：每次 Collect 先收割最近完成的异步评分
  //（回填缓存 + 挂账补写）——同指纹重复 query（翻页/候选窗重建）即命中
  // 0 LLM。收割与本次 query 自身的指纹无关（收割的是历史任务成果）。
  async_harvest();

  if (allc.size() >= 2 && g_loaded.load()) {
    // 词码对分数表：融合段共用一次读取（2026-11-21 解耦后只含负分，不参与
    // 窗口准入）；无文件 = 休眠。
    pf_on = pairfix_table(&pf_tbl);
    std::vector<size_t> idx(allc.size());
    std::vector<double> s1(allc.size());
    for (size_t i = 0; i < allc.size(); i++) {
      idx[i] = i;
      // 词码对记账（2026-11-13）: eff/uni 查询键 = (候选, 当前窗码 input_)
      s1[i] = g_freq_beta * std::log(1.0 + user_freq_eff(allc[i]->text(), input_)) +
              user_unigram_term(allc[i]->text(), input_);
    }
    std::stable_sort(idx.begin(), idx.end(),
                     [&s1](size_t a, size_t b) { return s1[a] > s1[b]; });
    size_t nk = (std::min)(kMaxRerank, allc.size());
    // 窗口准入（2026-11-24 统一口径）：层1 词频在等长词+子词里统一取前
    // nk 进窗（与字词模式一致）。组句不占窗席、不评分，也不进 tail——
    // 它们由显示端单独置顶。**只排除显示置顶的那 1 条**（2026-11-24 真机
    // 报障：旧实现按 top-20 全部文本排除，与组句同名的引擎候选整条消失）。
    const std::string gen_show =
        gen_texts.empty() ? std::string() : gen_texts[0];
    {
      std::vector<bool> in_win(allc.size(), false);
      size_t taken = 0;
      for (size_t k = 0; k < idx.size() && taken < nk; k++) {
        size_t c = idx[k];
        if (!gen_show.empty() && allc[c]->text() == gen_show)
          continue;
        in_win[c] = true;
        ++taken;
      }
      for (size_t k = 0; k < idx.size(); k++) {
        size_t c = idx[k];
        if (in_win[c]) {
          candidates_.push_back(allc[c]);
          s1_head.push_back(s1[c]);
          win_fm.push_back(sent_on ? is_fm[c] : (char)0);
        } else if (gen_show.empty() || allc[c]->text() != gen_show) {
          tail.push_back(allc[c]);   // 窗外候选：层1 序透传
        }
      }
    }
    // 语料 W 的 window 字段 = 层1/TCS 准入后、重排前的评分窗（原序）。
    window_vec = texts_of(candidates_, 20);
  } else {
    // 未加载/单候选: 引擎原序（行为与两层之前一致）
    live_send(std::string("S|") + now_hms() + "|" +
              (allc.size() < 2 ? "few_candidates" : "model_loading") + "|" +
              live_esc(input_) + "|" + src_);
    for (size_t i = 0; i < allc.size(); i++) {
      if (i < kMaxRerank)
        candidates_.push_back(allc[i]);
      else
        tail.push_back(allc[i]);
    }
    window_vec = texts_of(candidates_, 20);
    write_w(wid, src_, input_, wctx, engine_vec, window_vec, {}, g_user_tick,
            "skip", allc.size() < 2 ? "few_candidates" : "model_loading",
            false, "");
    w_written = true;
  }

  // 满覆盖准入后窗不足 2 个（不足即空着的极端形态）：无重排意义、整窗跳过
  // 评分——显式发 S 事件（此前该路径静默，调试窗口对此窗零输出）。
  // skip 名沿用 full_len_lt2（语料解析兼容）。
  if (sent_on && candidates_.size() < 2 && allc.size() >= 2) {
    live_send(std::string("S|") + now_hms() + "|full_len_lt2|" +
              live_esc(input_) + "|" + src_);
    write_w(wid, src_, input_, wctx, engine_vec, window_vec, {}, g_user_tick,
            "skip", "full_len_lt2", false, "");
    w_written = true;
  }

  if (candidates_.size() >= 2 && g_loaded.load()) {
    // rerank with LLM (ctx_ computed by LlmFilter::Apply: TSF context
    // text, or commit history fallback when TSF is unavailable)
    std::string ctx = normalize_ctx(ctx_);
    freq_only = ctx.empty();

    // reset 代次（编辑键/切窗）：只用于调试事件（插件版由 processor 在编辑键
    // 回调记 reset 行；源码版无 lua 层，在下次 Collect 见到代次变化时补记，
    // 首见不记免启动噪音）。评分缓存已删，此处不再失效任何状态。
    int gen = 0;
    if (const RimeApi *api = rime_get_api())
      if (api->context_reset_generation)
        gen = api->context_reset_generation();
    if (gen != s_seen_gen) {
      if (g_debug_fusion && s_seen_gen != -1)
        live_send(std::string("R|") + now_hms() + "|代次 " +
                  std::to_string(s_seen_gen) + "→" + std::to_string(gen) +
                  " (编辑键/切窗)");
      s_seen_gen = gen;
    }

    double ev_ms = 0;
    std::vector<int> order;                     // candidate indices, score desc
    std::vector<double> score_of(candidates_.size(), 0.0);  // 词频融合重放用
    std::vector<char> has_score(candidates_.size(), 0);
    std::vector<std::array<double, 3>> ce3_of(  // CE 三分量原始值（调试展开）
        candidates_.size(), {NAN, NAN, NAN});

    std::vector<char> dbg_full(candidates_.size(), 0);

    // 候选 token 边界段显（协议 v7，"词"列 标志→单token原样 / cmak→cm/ak）：
    // 与 candidates_ 对齐；只在下方 token 化处填（ctx 不足/评分跳过时留空
    // → 查看器显示原词）
    std::vector<std::string> seg_of(candidates_.size());

    // 推理用上文尾部文本（协议 v4，调试窗口灰显；快照路径从 snap 读）
    std::string ctx_used;
    {
      // 上文 token 数检查（与同步版同语义，skip 原因语料兼容）：不足即
      // ctx_short 跳过，不投递异步任务
      std::vector<llama_token> ctx_probe = tokenize(ctx.c_str());
      if (freq_only) {
        // 无上文：CE 不可算 → 融合只剩词频剩余项（β·log1p(eff) +
        // log(uni+1) + 词码对修正）。order 先置层1 序（identity），下方
        // 融合块按 fused 重排；W 记 flags=nofreq（status=ok）。
        order.resize(candidates_.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
        live_send(std::string("S|") + now_hms() + "|freq_only|" +
                  live_esc(input_) + "|" + src_);
      } else if ((int)ctx_probe.size() >= g_min_tokens) {
        // ═══ 评分三路径（2026-11-22 异步基建）═══
        // 指纹 = 归一化上文 \x1f 候选 \x1e 拼接（2026-11-23 再追加子词
        // 清单 \x1d——复用分与候选集同快照；与插件版同构；上下文/候选变化
        // 自然失效，无需失效链）。①缓存命中（同指纹重复 query，0 LLM）
        // ②字词模式 submit+wait（immediate 跳去抖 + 250ms 上限）
        // ③整句模式 submit+wait（immediate + 短预算 g_sentence_wait_ms，
        //   2026-11-23 修：原"投递即走"使 LLM 结果上不了屏）；超时回落
        //   挂账 pending，由后续 Collect 收割回填。
        std::vector<std::string> cand_texts;
        cand_texts.reserve(candidates_.size());
        for (size_t ci = 0; ci < candidates_.size(); ci++)
          cand_texts.push_back(candidates_[ci]->text());
        // 窗口评分口径 = 3-token（2026-11-24 统一窗口后恒为 3-token：
        // 等长词与子词同池比较，必须同口径；组句不再参与评分——phrase-only
        // 定案，组句只提供文本与"自信前缀"概率）。
        std::string fp = async_fingerprint(ctx, cand_texts, {});

        AsyncRes snap;
        bool have_snap = false;
        auto cit = g_async_cache.find(fp);
        if (cit != g_async_cache.end()) {
          snap = cit->second;
          have_snap = true;
        }
        // 同指纹任务已在飞（挂账中）：不重复投递——连打时每键覆盖任务槽
        // 会让评分永远算不完。继续显示层 1 序，等结果落地后由刷新链/
        // 下一次 Collect 收割上屏。
        const bool inflight = !have_snap && g_async_pending.count(fp) > 0;
        if (!have_snap && !inflight) {
          // immediate：窗口 3-token 在键预算内（150ms）完成即上屏
          std::string sfp = async_submit(ctx, cand_texts, {}, true);
          if (sfp.empty()) {
            wstatus = "skip";
            wskip = "async_failed";  // 任务不成立（理论上文/窗已过守卫）
            live_send(std::string("S|") + now_hms() + "|async_failed|" +
                      live_esc(input_) + "|" + src_);
          } else {
            AsyncRes res;
            const int wait_ms = sent_on ? g_sentence_wait_ms : 250;
            if (async_wait(sfp, wait_ms, res)) {
              g_async_cache[sfp] = res;
              if (g_async_cache.size() > 16)
                g_async_cache.clear();
              snap = res;
              have_snap = true;
            } else if (sent_on) {
              // 整句模式预算内未完成：回退挂账 pending（本轮层 1 序），
              // 评分结果由后续 Collect 的收割点补写语料（W ok /
              // flags=async + P 行），同指纹重查可命中缓存。
              AsyncPending pend;
              pend.wid = wid;
              pend.code = input_;
              pend.wctx = wctx;
              pend.engine_vec = engine_vec;
              pend.window_vec = window_vec;
              pend.tick = g_user_tick;
              pend.src = src_;
              pend.sent_on = true;
              g_async_pending[sfp] = pend;
              if (g_async_pending.size() > 32)
                g_async_pending.clear();
              wstatus = "skip";
              wskip = "async_pending";
              live_send(std::string("S|") + now_hms() + "|async_pending|" +
                        live_esc(input_) + "|" + src_);
            } else {
              // 字词模式：同步等待评分完成（等价既有同步评分——当前词
              // 重排零回归；wait 超时 = skip，评分线程继续跑完发布，
              // 后续 Collect 可经收割点回收进缓存）
              wstatus = "skip";
              wskip = "async_timeout";
              live_send(std::string("S|") + now_hms() + "|async_timeout|" +
                        live_esc(input_) + "|" + src_);
            }
          }
        } else if (inflight) {
          wstatus = "skip";
          wskip = "async_pending";
          live_send(std::string("S|") + now_hms() + "|async_pending|" +
                    live_esc(input_) + "|" + src_);
        }

        if (have_snap && snap.scores.size() == candidates_.size() &&
            snap.cand_texts.size() == candidates_.size()) {
          ev_ms = snap.ev_ms;
          prep_hit = snap.prep_hit;
          ctx_used = snap.ctx_used;
          // 2026-11-24：按**文本**映射（指纹顺序无关，候选序会随融合变化）
          std::map<std::string, size_t> sidx;
          for (size_t k = 0; k < snap.cand_texts.size(); k++)
            sidx.emplace(snap.cand_texts[k], k);
          for (size_t i = 0; i < candidates_.size(); i++) {
            auto it = sidx.find(candidates_[i]->text());
            if (it == sidx.end()) {
              score_of[i] = -1e10;
              has_score[i] = false;
              ce3_of[i] = std::array<double, 3>{NAN, NAN, NAN};
              continue;
            }
            const size_t k = it->second;
            score_of[i] = snap.scores[k];
            has_score[i] = snap.scores[k] > -1e9;
            ce3_of[i] = snap.ce3.size() == snap.scores.size()
                            ? snap.ce3[k]
                            : std::array<double, 3>{NAN, NAN, NAN};
            if (k < snap.segs.size())
              seg_of[i] = snap.segs[k];
          }
          std::vector<int> ord(candidates_.size());
          for (size_t i = 0; i < ord.size(); i++)
            ord[i] = (int)i;
          std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
            return score_of[(size_t)a] > score_of[(size_t)b];
          });
          order = std::move(ord);
        } else if (have_snap) {
          // 快照长度失配（防御，正常不可达——fp 绑定同候选集）
          wstatus = "skip";
          wskip = "score_failed";
          live_send(std::string("S|") + now_hms() + "|score_failed|" +
                    live_esc(input_) + "|" + src_);
          debug_write_raw(wid + "|X|scope=score|msg=scores length mismatch",
                          false);
        }
      } else {
        wstatus = "skip";
        wskip = "ctx_short";
        live_send(std::string("S|") + now_hms() + "|ctx_short|" +
                  live_esc(input_) + "|" + src_);
        debug_write_raw(wid + "|X|scope=ctx|msg=ctx_tokens=" +
                            std::to_string((int)ctx_probe.size()) +
                            " min=" + std::to_string(g_min_tokens),
                        false);
      }
    }

    // CE 序名次 (诊断): 此刻 order = 评分序 (缓存命中 = 缓存的评分序)
    std::vector<int> ce_rank(candidates_.size(), 0);
    for (size_t k = 0; k < order.size(); k++)
      ce_rank[(size_t)order[k]] = (int)k + 1;

    // 对数词频融合 (freq_beta, 2026-09-02; 词频 Rime 时间衰减 2026-08-21):
    // fused = score + β·log(1+eff)。score = 原始 LLM 分 (−CE, 对数概率域),
    // eff = librime algo::formula_d 指数衰减计数 (τ=800 tick, tick=每词提交
    // +1, 引擎调频同源) — 近期常打的词权重高, 久未使用的自动消退。
    // 对数域加法 = 词频无上限: 强个人高频词可翻盘 CE 分差 (工业输入法通行
    // 结构, 替代旧凸组合 (1-w)·minmax+w·eff/(eff+k) 的结构性封顶)。
    // β=1.5 标定 (本机打字真实候选窗回放, 事前 eff 口径): 真实窗子集
    // β∈[1,2] 平台 +0.38pp, 大 β 为标签偏好假象; 改错数随 β 单调下降
    // (高频首选获自保护)。失败哨兵/缺分不参与融合, 保序排尾。
    // 应用于评分/缓存顺序之上、两码整句段之前 (与插件版一致);
    // 缓存只存分数序, 融合每次按当前衰减重放。稳定排序保同分原序 (CE 序)。
    // 融合分叠加词码对扣分修正（负分=CE 系统性偏差降噪，保留 fade；
    // 2026-11-21 解耦后无正分——固定候选归 pin_filter.lua 全局重排）。
    // debug.txt 行尾 修正%+g，C 行尾追加 sdelta（与插件版一致）。
    std::vector<double> fused_of(candidates_.size(), -1e308);
    if (order.size() > 1) {
      // 无 CE 分候选的融合（2026-11-13 用户定案，取代"回退后两项和"）:
      // CE 取窗内已算 CE 的**最低分**（score = −CE 负值域，最低分 = 最差
      // 证据 → 该候选排到窗底但保留与层1 的正常耦合）。旧语义（融合=层1
      // 分）会让无 CE 候选以 S1=0 压过全部负分候选跳首选——aki 实况窗
      // 两例实证。触发器随时代更替：当年 = tokenizer 打不出「𬤊」（2026-
      // 11-15 llama 升级后已可表示——2026-11-21 全词典 8133 单字核查，
      // 99.7% 单 token、余 24 字 2 token，无一不可表示）；现在 = 评分批
      // 次 decode 失败的逐候选哨兵分（score_batch step2/3 失败路径 +
      // 失败自愈）。全窗无有效 CE 时回退层1 分（加 0）。
      double min_valid = 1e308;
      for (int i : order)
        if (has_score[i] && score_of[i] < min_valid)
          min_valid = score_of[i];
      std::vector<std::pair<double, int>> fused;  // (total, idx)
      fused.reserve(order.size());
      for (int i : order) {
        double t;
        if (has_score[i]) {
          t = score_of[i] +
              g_freq_beta * std::log(1.0 + user_freq_eff(
                                  candidates_[i]->text(), input_)) +
              user_unigram_term(candidates_[i]->text(), input_);
        } else {
          double s1v = i < s1_head.size() ? s1_head[i] : -1e308;
          t = s1v + (min_valid < 1e308 ? min_valid : 0.0);
        }
        // 词码对分数修正（有符号；正分不受 fade，负分保留 fade）。
        // **只对已算 CE 的候选生效**（2026-11-24 用户定案）：pairfix 修的是
        // CE 的系统性偏差，无 CE 分（评分失败/无上文 freq_only）时无从修，
        // 加了反而凭空改词频序。
        if (pf_on && has_score[i]) {
          double d = pairfix_delta(pf_tbl, candidates_[i]->text(), input_);
          if (d != 0.0)
            t += d;
        }
        fused_of[i] = t;
        fused.emplace_back(t, i);
      }
      std::stable_sort(fused.begin(), fused.end(),
                       [](const std::pair<double, int> &a,
                          const std::pair<double, int> &b) {
                         return a.first > b.first;
                       });
      order.clear();
      for (auto &x : fused)
        order.push_back(x.second);
    }

    // ═══ 前缀软确认：合并单一路线（2026-11-24 定案）═══
    // 组句给文本（top-20 按 lead 分组 softmax，P(lead) ≥ 0.95）∧ 该文本是
    // 窗口融合序第 1（span 对齐、文本一致）→ 挂起。窗口路径 A（margin）与
    // 句级 CE 序分支退役：融合序位次本身就是 CE(+词频) 的全序比较，再叠
    // margin 净收益反降（研究库）；θ 与单字倍数不需要。组句返回空（码切在
    // 音节中间，占 39.5%）的窗不再自动确认。**守卫 = 音节单位**（2026-11-24
    // 标定 + 真机定案）：确认后**剩余 ≥2 音节**（单字 lead 的"余 1 例外"
    // 已退役——`xiazai` 在 xia 确认「下」会把 2 字词拆成单字+单字）——组句
    // 句子字数 = 音节数，码数在简拼/双拼/混拼下不是通用单位。链式确认逐次
    // 重新判门（下一次刷新/Collect 在更短的码上重算，不沿用旧 pending）。
    // **绝对置信度门（2026-11-25 研究定案）**：有效上文 0 字（新文档句首）
    // → ngram 句分差 d≥g_prefix_d；有效上文 ≤1 字 → 不确认；≥2 字 →
    // 窗口第 1 候选的首 token CE ≤ g_prefix_ce1；CE 未就绪 → 不确认。
    // g_prefix_ce1=0 关闭新门（回到 2026-11-24 行为；环境变量 A/B）。
    if (g_debug_fusion && !gen_texts.empty())
      log_error("pfx: enter gen=%zu order=%zu", gen_texts.size(),
                order.size());
    if (g_prefix_confirm && g_sentence_mode && !gen_texts.empty() &&
        !order.empty()) {
      // ① lead 概率质量（top-20 softmax，按 lead 分组）
      double mx = -1e308;
      for (const auto &t : gen_texts) {
        auto it = gen_info.find(t);
        if (it != gen_info.end() && it->second.score > mx)
          mx = it->second.score;
      }
      if (mx > -1e307) {
        std::map<std::string, double> mass;
        std::map<std::string, int> le_of;
        std::map<std::string, std::string> txt_of;   // 该 lead 的最高分句
        double tot = 0.0;
        for (const auto &t : gen_texts) {   // gen_texts = ngram 序（分数降序）
          auto it = gen_info.find(t);
          if (it == gen_info.end() || it->second.lead.empty())
            continue;
          double w = std::exp(it->second.score - mx);
          tot += w;
          mass[it->second.lead] += w;
          le_of.emplace(it->second.lead, it->second.lead_end);
          txt_of.emplace(it->second.lead, t);   // 首见 = 最高分句
        }
        const std::string *lead = nullptr;
        double best = -1.0;
        for (const auto &kv : mass) {   // std::map 有序 → 同质量取文本序
          if (kv.second > best) {
            best = kv.second;
            lead = &kv.first;
          }
        }
        // 模型门取代 P≥0.95 硬门（2026-11-25 用户定案）：P 只作特征之一，
        // 这里只留训练域下界 p≥0.5（cohort 只在该区间标定）。模型关闭时
        // 仍用旧 P 阈值。ta 族黑名单同在此处拦掉（结构性同音歧义）。
        const double p_gate = g_prefix_model_on ? 0.5 : g_prefix_confirm_p;
        if (g_debug_fusion && lead && tot > 0.0) {
          if (best / tot < p_gate)
            log_error("pfx: p below gate lead='%.16s' p=%.3f pgate=%.2f",
                      lead->c_str(), best / tot, p_gate);
          else if (g_prefix_tablock_on && PrefixTaBlocked(*lead))
            log_error("pfx: ta blocked lead='%.16s' p=%.3f",
                      lead->c_str(), best / tot);
        }
        if (lead && tot > 0.0 && best / tot >= p_gate &&
            !(g_prefix_tablock_on && PrefixTaBlocked(*lead))) {
          int le = le_of[*lead];
          size_t want_end = seg_start_ + (size_t)le;
          // ② 音节守卫：句子字数 = 音节数；**剩余必须 ≥2 音节**
          //（2026-11-24 真机定案：单字 lead 的"余 1 例外"退役——`xiazai`
          // 想在 xia 就确认「下」，剩 zai 只能出单字，「下载」这个 2 字词
          // 被拆开；标定：该例外仅 +112 触发，去掉后精度 95.48%/召回
          // 40.23%/K=5 1935，几乎无损）
          size_t sl = utf8_len(*lead);
          size_t st = utf8_len(txt_of[*lead]);
          size_t sr = st >= sl ? st - sl : 0;
          if (le >= 0 && want_end < seg_end_ && st >= sl && sr >= 2) {
            // ②b 绝对置信度门（2026-11-25 研究定案）：窗口融合序第 1 的
            // 首 token CE（原始正 CE）≤ g_prefix_ce1；空上文无 CE → ngram
            // 句分差 d ≥ g_prefix_d；有效上文 ≤1 字不确认。
            // 0 = 关闭（回到 2026-11-24 行为）。
            bool conf_ok = true;
            double ce1_v = NAN, d_v = NAN, model_p = NAN;
            size_t eff_chars = 0;
            if (g_prefix_ce1 > 0.0 || g_prefix_model_on) {
              std::string eff_ctx = ctx_;
              if (engine_ && engine_->context()) {
                std::string conf = ConfirmedPrefixText(engine_->context());
                if (!conf.empty()) eff_ctx += conf;
              }
              eff_chars = utf8_len(eff_ctx);
              size_t i1 = (size_t)order[0];
              if (eff_chars == 0) {
                // 空上文：该 lead 的最高分句 vs 最佳不同 lead 句
                double top_s = -1e308;
                auto ti = txt_of.find(*lead);
                if (ti != txt_of.end()) {
                  auto gi = gen_info.find(ti->second);
                  if (gi != gen_info.end()) top_s = gi->second.score;
                }
                double best_other = -1e308;
                for (const auto &t : gen_texts) {
                  auto it = gen_info.find(t);
                  if (it != gen_info.end() &&
                      it->second.lead != *lead &&
                      it->second.score > best_other)
                    best_other = it->second.score;
                }
                d_v = (best_other > -1e307) ? top_s - best_other : 1e9;
                conf_ok = top_s > -1e307 && d_v >= g_prefix_d;
              } else if (eff_chars < 2) {
                conf_ok = false;
              } else if (g_prefix_model_on) {
                // ── 模型门（2026-11-25）：41 特征 → 分数 ≥ τ ──
                if (!(has_score[i1] && i1 < ce3_of.size())) {
                  conf_ok = false;    // CE 未就绪 → 不判
                } else {
                  auto ce_sum = [&](size_t ci, int *ntok) {
                    double s = 0.0;
                    int nt = 0;
                    for (int k = 0; k < 3; k++) {
                      double c = ce3_of[ci][(size_t)k];
                      if (c == c && c > -1e8) { s += kCeW[k] * c; nt++; }
                    }
                    *ntok = nt;
                    return s;
                  };
                  // 评分窗 CE 序（升 ce；同分保持 order 序 = 研究侧稳定排序）
                  std::vector<std::pair<double, size_t>> ceord;
                  for (size_t oi = 0; oi < order.size(); oi++) {
                    size_t ci = (size_t)order[oi];
                    if (!has_score[ci] || ci >= ce3_of.size()) continue;
                    int nt = 0;
                    double cs = ce_sum(ci, &nt);
                    if (nt > 0) ceord.emplace_back(cs, ci);
                  }
                  std::stable_sort(ceord.begin(), ceord.end(),
                                   [](const std::pair<double, size_t> &a,
                                      const std::pair<double, size_t> &b) {
                                     return a.first < b.first;
                                   });
                  int lead_nt = 0;
                  double lead_ce = ce_sum(i1, &lead_nt);
                  if (lead_nt == 0) {
                    conf_ok = false;
                  } else {
                    const size_t kNone = (size_t)-1;
                    size_t r2i = kNone;
                    for (const auto &e : ceord)
                      if (candidates_[e.second]->text() != *lead) {
                        r2i = e.second;
                        break;
                      }
                    double r2_ce = 0.0, r2_ce1 = -1.0, wm = 0.0;
                    int r2_nt = 0;
                    size_t r2_chars = 0;
                    if (r2i != kNone) {
                      int nt2 = 0;
                      r2_ce = ce_sum(r2i, &nt2);
                      r2_nt = nt2;
                      double c1r2 = ce3_of[r2i][0];
                      r2_ce1 = (c1r2 == c1r2) ? c1r2 : -1.0;
                      r2_chars = utf8_len(candidates_[r2i]->text());
                      wm = r2_ce - lead_ce;
                    }
                    // 组句侧
                    double top_s = -1e308;
                    std::string top_lead;
                    if (!gen_texts.empty()) {
                      auto gi = gen_info.find(gen_texts[0]);
                      if (gi != gen_info.end()) {
                        top_s = gi->second.score;
                        top_lead = gi->second.lead;
                      }
                    }
                    double best_other = -1e308;
                    int cnt_lead = 0;
                    for (const auto &t : gen_texts) {
                      auto it = gen_info.find(t);
                      if (it == gen_info.end()) continue;
                      if (it->second.lead == *lead) cnt_lead++;
                      if (!top_lead.empty() && it->second.lead != top_lead &&
                          it->second.score > best_other)
                        best_other = it->second.score;
                    }
                    double dv = (top_s > -1e307 && best_other > -1e307)
                                    ? top_s - best_other : 30.0;
                    if (dv > 40.0) dv = 40.0;
                    double top_score = (top_s > -1e307) ? -top_s : 60.0;
                    if (top_score > 60.0) top_score = 60.0;
                    const double p = best / tot;
                    double c1 = ce3_of[i1][0], c2 = ce3_of[i1][1],
                           c3v = ce3_of[i1][2];
                    const double ce1f = (c1 == c1) ? c1 : -1.0;
                    const double ce2f = (c2 == c2) ? c2 : -1.0;
                    const double ce3f = (c3v == c3v) ? c3v : -1.0;
                    int n_single = 0, n_same_span = 0;
                    const size_t lead_end_abs = seg_start_ + (size_t)le;
                    for (const auto &cp : candidates_) {
                      if (utf8_len(cp->text()) == 1) n_single++;
                      if (cp->end() == lead_end_abs) n_same_span++;
                    }
                    double cf_min = 1e9, cf_sum = 0.0;
                    int cf_n = 0;
                    for (size_t b = 0; b < lead->size();) {
                      unsigned char ch = (unsigned char)(*lead)[b];
                      size_t cl = (ch < 0x80) ? 1
                                 : (ch >= 0xF0) ? 4
                                 : (ch >= 0xE0) ? 3 : 2;
                      std::string one = lead->substr(b, cl);
                      b += cl;
                      double fv = PrefixCharFreq(one);
                      cf_sum += fv;
                      cf_n++;
                      if (fv < cf_min) cf_min = fv;
                    }
                    const size_t lead_len = sl;
                    const double ntok = (double)lead_nt;
                    double x[kPrefixFeatN];
                    int xi = 0;
                    x[xi++] = p;
                    x[xi++] = std::log((std::max)(p, 1e-6) /
                                       (std::max)(1.0 - p, 1e-6));
                    x[xi++] = dv;
                    x[xi++] = top_score;
                    x[xi++] = (double)mass.size();
                    x[xi++] = (double)cnt_lead;
                    x[xi++] = (double)gen_texts.size();
                    x[xi++] = (double)st;
                    x[xi++] = (double)lead_len;
                    x[xi++] = (double)sr;
                    x[xi++] = (double)le;
                    x[xi++] = (double)lead_len / (std::max)((double)le, 1.0);
                    x[xi++] = wm;
                    x[xi++] = (double)ceord.size();
                    x[xi++] = (double)candidates_.size();
                    x[xi++] = (double)(seg_end_ - seg_start_);
                    x[xi++] = (double)eff_chars;
                    x[xi++] = ce1f;
                    x[xi++] = ce2f;
                    x[xi++] = ce3f;
                    x[xi++] = lead_ce;
                    x[xi++] = ntok;
                    x[xi++] = (double)lead_len / (std::max)(ntok, 1.0);
                    x[xi++] = lead_ce / (std::max)(ntok, 1.0);
                    x[xi++] = lead_ce / (std::max)((double)lead_len, 1.0);
                    x[xi++] = ntok > 1.0 ? 1.0 : 0.0;
                    x[xi++] = r2_ce1;
                    x[xi++] = r2_ce;
                    x[xi++] = (double)r2_nt;
                    x[xi++] = r2_ce / (std::max)((double)r2_nt, 1.0);
                    x[xi++] = (double)(r2_nt - lead_nt);
                    x[xi++] = ce1f / (std::max)((double)lead_len, 1.0);
                    x[xi++] = (double)r2_chars;
                    x[xi++] = r2_chars == 1 ? 1.0 : 0.0;
                    x[xi++] = r2_ce1 - ce1f;
                    x[xi++] = (double)n_single;
                    x[xi++] = (double)n_same_span;
                    x[xi++] = lead_len == 1 ? 1.0 : 0.0;
                    x[xi++] = (double)n_single /
                              (std::max)((double)candidates_.size(), 1.0);
                    x[xi++] = cf_n ? cf_min : -13.0;
                    x[xi++] = cf_n ? cf_sum / cf_n : -13.0;
                    model_p = PrefixModelScore(x);
                    conf_ok = model_p >= g_prefix_tau;
                    if (getenv("RIME_LLM_PREFIX_DUMP")) {   // 特征对拍用
                      std::string dump;
                      char nb[64];
                      for (int k = 0; k < kPrefixFeatN; k++) {
                        snprintf(nb, sizeof nb, k ? "	%.9g" : "%.9g", x[k]);
                        dump += nb;
                      }
                      snprintf(nb, sizeof nb, "	%.9g", model_p);
                      dump += nb;
                      log_error("pfxdump: %s", dump.c_str());
                    }
                  }
                }
              } else if (has_score[i1] && i1 < ce3_of.size()) {
                ce1_v = ce3_of[i1][0];
                conf_ok = (ce1_v == ce1_v) && ce1_v <= g_prefix_ce1;
              } else {
                conf_ok = false;   // CE 未就绪 → 不确认（比按层1 序确认安全）
              }
            }
            // ③ 窗口融合序第 1 ∧ span/文本一致（文本一致铁律）
            size_t i1 = (size_t)order[0];
            const auto &c1 = candidates_[i1];
            if (c1->text() == *lead && c1->start() == seg_start_ &&
                c1->end() == want_end && conf_ok) {
              std::lock_guard<std::mutex> lk(g_pending_confirm_mu);
              g_pending_confirm.valid = true;
              g_pending_confirm.tries = 0;
              g_pending_confirm.input = input_;
              g_pending_confirm.seg_start = seg_start_;
              g_pending_confirm.text = c1->text();
              g_pending_confirm.end_pos = c1->end();
              if (g_debug_fusion)
                log_error("pfx: pending='%s' end=%d p=%.3f syll=%zu+%zu "
                          "ce1=%.2f d=%.1f ctx=%zu mp=%.4f tau=%.4f",
                          c1->text().c_str(), (int)c1->end(), best / tot, sl,
                          sr, ce1_v, d_v, eff_chars, model_p,
                          g_prefix_model_on ? g_prefix_tau : 0.0);
            } else if (g_debug_fusion) {
              if (c1->text() == *lead && c1->start() == seg_start_ &&
                  c1->end() == want_end)
                log_error("pfx: conf fail lead='%.16s' ce1=%.2f d=%.1f "
                          "ctx=%zu mp=%.4f ce1_thr=%.1f tau=%.4f",
                          lead->c_str(), ce1_v, d_v, eff_chars, model_p,
                          g_prefix_ce1, g_prefix_tau);
              else
                log_error("pfx: rank1 '%s' != lead '%.16s' p=%.3f",
                          c1->text().c_str(), lead->c_str(), best / tot);
            }
          } else if (g_debug_fusion) {
            log_error("pfx: guard lead='%.16s' syll=%zu+%zu need=2",
                      lead->c_str(), sl, sr);
          }
        }
      }
    }

    // 两码整句最终置顶块已删（2026-11-22 整句模式定案）：满覆盖候选已在
    // 层1 准入时结构性占满评分窗（窗内全 FM，见上方准入块），融合序即
    // 满覆盖组内序——旧保险块对最终序恒 no-op，连同词长==⌊码长/2⌋ 判据
    // 一并退役。调试 full 标志（协议字段：win=1 行 FM 与否）由窗准入的
    // win_fm 直接标记。
    if (g_sentence_mode) {
      for (size_t i = 0; i < dbg_full.size() && i < win_fm.size(); i++)
        dbg_full[i] = win_fm[i] ? true : false;
    }

    // debug P + 逐候选 D（2026-11-17）：只写语料没有的特征/耗时；
    // 词、候选序、上下文都在语料 W 行，靠 W-id 关联，不重复记录。
    if (g_debug_fusion) {
      debug_write_p(wid, ev_ms, prep_hit);
    }
    if (g_debug_fusion && !order.empty()) {
      for (size_t k = 0; k < order.size(); k++) {
        int i = order[k];
        // 无上文（freq_only）时融合不含 CE、只含词频剩余项 → 也要显示真实
        // eff/fb/u1（2026-11-24：旧代码 !has_score 一律写 0，验算对不上）
        const bool has_terms = has_score[i] || freq_only;
        double eff = has_terms
                         ? user_freq_eff(candidates_[i]->text(), input_)
                         : 0;
        double fb = has_terms ? g_freq_beta * std::log1p(eff) : 0;
        double u1 = has_terms
                        ? user_unigram_term(candidates_[i]->text(), input_)
                        : 0;
        // 词码对修正只随 CE 分生效（无 CE 时融合里没有它，行尾也写 0）
        double sdelta = (pf_on && has_score[i])
                            ? pairfix_delta(pf_tbl,
                                            candidates_[i]->text(), input_)
                            : 0.0;
        double ce = has_score[i] ? score_of[i] : -1e308;
        double ce1 = -1e308, ce2 = -1e308, ce3v = -1e308;
        if (has_score[i] && i < (int)ce3_of.size()) {
          ce1 = ce3_of[(size_t)i][0];
          ce2 = ce3_of[(size_t)i][1];
          ce3v = ce3_of[(size_t)i][2];
        }
        int full = (g_sentence_mode && i < (int)win_fm.size() &&
                    win_fm[(size_t)i]) ? 1 : 0;
        debug_write_d(wid, (int)k + 1, ce, ce1, ce2, ce3v, eff, fb, u1, full,
                      fused_of[i], sdelta, ce_rank[i], (int)k + 1);
      }
    }
    // 实时调试 C 行（2026-11-10；2026-11-11 协议 v3：同时发"原始计数"与
    // "得分项"两套——衰减计数(eff)/累计次数(uni) 是证据，词频得分(β·log1p(eff))
    // /累计得分(log p1) 是真正进融合的项；尾接 CE 三分量 ce1/ce2/ce3（未乘
    // kCeW 权重，查看器常显）：窗口候选按最终序（ce/fused/cerank 取自重排
    // 状态）+ 层1 尾窗透传（win=0，无 CE 分），上限 20 行。order 空时（评分
    // 整体失败）窗口按引擎序全 win=1 无分。v5（2026-11-14）行尾再追加修正
    // 分 pf = 词码对预置降分修正的 λ（0 = 未命中）——总分已含 −λ, 查看器
    // 总分格据此补 −λ 标注恢复逐项验算（append-only, 旧查看器忽略）。
    // v7（2026-11-21）行尾再追加 seg = token 边界段显（"词"列：单 token 词
    // 原样、真实多 token 词段间 '/'、2 token 拼成的字合并整字——token 化
    // 在下方评分窗候选循环统一做，尾窗透传无 token 化 → 空串，查看器显示
    // 原词）。同日查看器显示口径（协议零改动部分）：修正分专属列删除
    // （命中时折进 CE 格数值 + 公式尾项，本端 ce/sdelta 照发不合并）。
    // L 头行也在此发（评分/融合后 ev_ms 才是终值；尾部字段 prep = 上文 KV
    // 预解码命中、v4 追加 ctx_used = 推理实际使用的上文尾部文本（调试窗口
    // 灰显前缀）。
    // 第 3 字段原为评分缓存 HIT/MISS——缓存已删（2026-11-11），保留字段位置
    // 发 "-"：旧查看器显示"评缓 -"，新查看器不显示该列。
    // n_seen 字段已删（2026-11-17：不参与打分, 连记录一并取消）；查看器按
    // 字段数兼容旧行（>12 取第 13 字段，否则第 12 字段 = ctx_used）。
    if (g_debug_fusion) {
      char msb[16];
      snprintf(msb, sizeof(msb), "%.0f", ev_ms);
      live_send(std::string("L|") + now_hms() + "|" + live_esc(input_) + "|-|" +
                src_ + "|" +
                live_esc(ctx) + "|" + std::to_string((long long)g_user_tick) +
                "|" + msb + "|1|" +
                std::to_string((int)candidates_.size()) + "|" +
                (prep_hit ? "1" : "0") + "|" +
                live_esc(ctx_used));
      std::map<std::string, int> eng_rank;  // 引擎原序（首现位置）
      for (size_t i = 0; i < allc.size(); i++)
        if (!eng_rank.count(allc[i]->text()))
          eng_rank[allc[i]->text()] = (int)i + 1;
      int emitted = 0;
      // src_idx = ce3_of 下标（层2 窗内候选）；-1 = 尾窗/无评分分量
      auto live_c = [&](const an<Candidate> &c, int src_idx,
                        const std::string &ce, const std::string &fused,
                        int cerank, int fin, int full, int win) {
        if (emitted >= 20)
          return;
        emitted++;
        double eff = user_freq_eff(c->text(), input_);
        double fb = g_freq_beta * std::log1p(eff);
        double u1 = user_unigram_term(c->text(), input_);
        // v5: 旧 pf = 负分幅度（仅扣分，0=未命中），保留给旧查看器；
        // v6 (2026-11-17): 行尾追加有符号 sdelta（+200/+100/-5/0）。
        double sdelta = pf_on ? pairfix_delta(pf_tbl, c->text(), input_) : 0.0;
        double pf = sdelta < 0.0 ? -sdelta : 0.0;
        std::string ce1, ce2, ce3s;
        if (src_idx >= 0 &&
            src_idx < (int)ce3_of.size()) {
          ce1 = live_num(ce3_of[(size_t)src_idx][0]);
          ce2 = live_num(ce3_of[(size_t)src_idx][1]);
          ce3s = live_num(ce3_of[(size_t)src_idx][2]);
        }
        auto er = eng_rank.find(c->text());
        // v7: seg = token 边界段显——尾窗透传（src_idx=-1）无 token 化 → 空串
        std::string seg = (src_idx >= 0 && src_idx < (int)seg_of.size())
                              ? seg_of[(size_t)src_idx] : std::string();
        live_send("C|" +
                  std::to_string(er != eng_rank.end() ? er->second : 0) +
                  "|" + live_esc(c->text()) + "|" + live_num(fb + u1) + "|" +
                  ce + "|" + live_num(eff) + "|" +
                  std::to_string(user_uni_count(c->text(), input_)) + "|" + fused +
                  "|" + std::to_string(full) + "|" + std::to_string(cerank) +
                  "|" + std::to_string(fin) + "|" + std::to_string(win) +
                  "|" + ce1 + "|" + ce2 + "|" + ce3s + "|" +
                  live_num(fb) + "|" + live_num(u1) + "|" +
                  live_num(pf) + "|" + live_num(sdelta) + "|" +
                  live_esc(seg));
                  // v3: 词频得分/累计得分; v5: 修正分 pf; v6: 有符号 sdelta;
                  // v7: token 边界段显 seg（"词"列按真实 token 边界分段）
      };
      if (!order.empty()) {
        std::vector<int> fin_rank(candidates_.size(), 0);
        for (size_t k = 0; k < order.size(); k++)
          fin_rank[(size_t)order[k]] = (int)k + 1;
        for (int i : order)
          live_c(candidates_[(size_t)i], i,
                 has_score[(size_t)i] ? live_num(score_of[(size_t)i]) : "",
                 live_num(fused_of[(size_t)i]), ce_rank[(size_t)i],
                 fin_rank[(size_t)i], dbg_full[(size_t)i] ? 1 : 0, 1);
        int base = (int)candidates_.size();
        for (size_t j = 0; j < tail.size(); j++)
          live_c(tail[j], -1, "", "", 0, base + (int)j + 1, 0, 0);
      } else {
        for (size_t i = 0; i < candidates_.size(); i++)
          live_c(candidates_[i], (int)i, "", "", 0, (int)i + 1, 0, 1);
        int base = (int)candidates_.size();
        for (size_t j = 0; j < tail.size(); j++)
          live_c(tail[j], -1, "", "", 0, base + (int)j + 1, 0, 0);
      }
    }
    if (!order.empty()) {
      // 应用顺序 (缓存命中与真实评分共用): 未匹配候选(新候选)落尾部
      std::vector<an<Candidate>> reranked;
      std::vector<bool> used(candidates_.size(), false);
      for (int i : order)
        if (i >= 0 && (size_t)i < candidates_.size() && !used[i]) {
          reranked.push_back(candidates_[i]);
          used[i] = true;
        }
      for (size_t i = 0; i < candidates_.size(); i++)
        if (!used[i])
          reranked.push_back(candidates_[i]);
      candidates_ = std::move(reranked);
    }
  }

  // ═══ 最终序（2026-11-24 phrase-only，评分/未评分/加载中等所有路径共用）═══
  // 组句 ngram 序首选 1 条置顶（句子不参与 CE 重排）→ 窗口融合序（等长词与
  // 子词统一概率序）→ 窗外层1 序。三块输出（FM 窗 / FM 窗外 / PM 块）与
  // "匹配在前、部分在后"不变量随之退役（用户 2026-11-24 定案）。
  {
    an<Candidate> best_gen;
    if (g_sentence_mode && !gen_texts.empty()) {
      // phrase-only（2026-11-24 定案）：显示首选 = ngram 序第 1，句子不再
      // 参与 CE 重排（全句 CE/full 槽已退役）。
      for (const auto &c : allc)
        if (c->text() == gen_texts[0]) {
          best_gen = c;
          break;
        }
    }
    std::vector<an<Candidate>> out;
    out.reserve(candidates_.size() + tail.size() + 1);
    if (best_gen)
      out.push_back(best_gen);
    for (auto &c : candidates_)
      out.push_back(c);
    for (auto &c : tail)
      out.push_back(c);
    candidates_.swap(out);
    if (g_sentence_mode && !gen_texts.empty())
      log_msg("ngram: display gen_kept=%d total=%d", best_gen ? 1 : 0,
              (int)candidates_.size());
  }
  if (!w_written) {
    write_w(wid, src_, input_, wctx, engine_vec, window_vec,
            texts_of(candidates_, 20), g_user_tick, wstatus, wskip, prep_hit,
            freq_only ? "nofreq" : "");
    w_written = true;
  }
}

// ============================================================
// LlmFilter
// ============================================================

static void on_context_changed(const char *text);  // defined below

LlmFilter::LlmFilter(const Ticket &ticket) : Filter(ticket) {
  // 方案子目录（2026-11-22 四件全分）：先固定当前方案 ID——H 行/账本/
  // 词码对/语料的路径解析都依赖它，必须早于本构造内的任何写入
  if (engine_ && engine_->schema())
    llm_set_schema_id(engine_->schema()->schema_id());
  // read LLM config from the scheme's llm_rerank section.
  // The filter's position in the filter pipeline is scheme-defined, so the
  // config lives in the scheme: the LLM-enabled scheme is maintained in the
  // rime-llm-ime project, while the published generic scheme stays clean.
  if (Config *config = engine_->schema()->config()) {
    // schema llm_rerank 节快照（部署期固定，改配置 = 改方案 + 重新部署）
    LlmParamSet &p = g_schema_params;
    p = LlmParamSet();
    string s;
    if (config->GetString("llm_rerank/model_path", &s) && !s.empty()) {
      p.has_model_path = true;
      p.model_path = s;
    }
    bool b = false;
    if (config->GetBool("llm_rerank/enabled", &b)) {
      p.has_enabled = true;
      p.enabled = b;
    }
    if (config->GetBool("llm_rerank/debug_fusion", &b)) {
      p.has_debug_fusion = true;
      p.debug_fusion = b;
    }
    int v = 0;
    std::string sv;
    if (config->GetString("llm_rerank/code_pattern", &sv)) {
      p.has_code_pattern = true;
      p.code_pattern = sv;
    }
    if (config->GetInt("llm_rerank/min_code_len", &v)) { p.has_min_code_len = true; p.min_code_len = v; }
    if (config->GetInt("llm_rerank/max_code_len", &v)) { p.has_max_code_len = true; p.max_code_len = v; }
    // expected_length_weight 不再读取（2026-11-07 由 two_code_sentence 取代；
    // 方案残留旧行忽略，保存时由 GUI 剥除）。freq_beta 同（2026-11-05）。
    if (config->GetInt("llm_rerank/min_tokens", &v)) { p.has_min_tokens = true; p.min_tokens = v; }
    if (config->GetInt("llm_rerank/max_tokens", &v)) { p.has_max_tokens = true; p.max_tokens = v; }
    if (config->GetInt("llm_rerank/max_candidates", &v)) { p.has_max_cand = true; p.max_cand = v; }
    if (config->GetInt("llm_rerank/cpu_cores", &v)) { p.has_cpu_cores = true; p.cpu_cores = v; }
    // bool b 复用上方 enabled/debug_fusion 的声明（同作用域）
    // 整句模式（2026-11-22 取代 two_code_sentence）：新键优先；旧键只在新键
    // 缺席时兜底一次——真机旧方案 two_code_sentence: true 直升整句模式，
    // 保存后自然换新键（硬忽略会让已启用整句的机器静默退化）
    if (config->GetBool("llm_rerank/sentence_mode", &b)) {
      p.has_sentence = true; p.sentence_mode = b;
    } else if (config->GetBool("llm_rerank/two_code_sentence", &b)) {
      p.has_sentence = true; p.sentence_mode = b;
    }
    // 前缀自动确认已并入 ngram_group（2026-11-24 用户定案）：旧键
    // `llm_rerank/prefix_confirm` 读取端忽略（真机方案里的残留行在 GUI
    // 保存/重建节时剥除）；生效判定见 llm_apply_params。
    // N-gram 组句候选（2026-11-23 第②段）：开关 + 输出字形门
    if (config->GetBool("llm_rerank/ngram_group", &b)) {
      p.has_ngram_group = true; p.ngram_group = b;
    }
    // 输出字形门（2026-11-23 晚修）：组句词表 = 万象简体，只在方案输出
    // 简体时注入。判据 = `engine/filters` 里确有 simplifier（排除 s2t 反向
    // 转换）。**不能只看 simplifier/option_name**：朙月 luna_pinyin 的
    // simplifier 无 option_name = opencc 默认 t2s 恒转换（实测候选恒简体），
    // option_name 为空却被旧判据当成繁体方案 → 组句静默关闭。option_name
    // 存在时（luna_pinyin_simp 的 zh_simp 这类显式开关）在注入点再按开关
    // 当前状态 gate——开关可运行时切换，不能在部署期固定。
    g_simplified_scheme = false;
    g_simplifier_option.clear();
    if (an<ConfigList> filters = config->GetList("engine/filters")) {
      for (size_t i = 0; i < filters->size(); ++i) {
        an<ConfigValue> fv = As<ConfigValue>(filters->GetAt(i));
        if (!fv) continue;
        const string &fname = fv->str();
        if (fname == "simplifier" ||
            (fname.size() > 11 && fname.compare(0, 11, "simplifier@") == 0)) {
          g_simplified_scheme = true;
          break;
        }
      }
      if (g_simplified_scheme &&
          config->GetString("simplifier/opencc_config", &sv) &&
          sv.find("s2t") != string::npos)
        g_simplified_scheme = false;  // 简→繁转换不算"输出简体"
      if (g_simplified_scheme &&
          config->GetString("simplifier/option_name", &sv))
        g_simplifier_option = sv;
    }
    llm_apply_params();
    {
      // 门控诊断（2026-11-23 晚，真机排查）：每个 session 构造一行，确认
      // 服务进程里读到的配置与 filters 列表。debug_fusion 关时不写。
      // rawpfx 已删（2026-11-24：prefix_confirm 并入 ngram_group，pfx 恒随 ng）
      bool raw_sent = false, raw_ng = false;
      config->GetBool("llm_rerank/sentence_mode", &raw_sent);
      config->GetBool("llm_rerank/ngram_group", &raw_ng);
      an<ConfigList> fl = config->GetList("engine/filters");
      an<ConfigValue> fv0 = fl ? As<ConfigValue>(fl->GetAt(0)) : nullptr;
      log_msg("gate: sent=%d pfx=%d ngram=%d simp=%d rawsent=%d rawng=%d "
              "nfilters=%d f0='%s' opt='%s'",
              (int)g_sentence_mode, (int)g_prefix_confirm, (int)g_ngram_group,
              (int)g_simplified_scheme, (int)raw_sent, (int)raw_ng,
              fl ? (int)fl->size() : -1,
              fv0 ? fv0->str().c_str() : "", g_simplifier_option.c_str());
    }
    std::string sid;
    if (engine_->schema())
      sid = engine_->schema()->schema_id();
    write_h(sid.empty() ? "unknown" : sid, g_model_path);
  }
  // hook engine commit sink: pre-decode the upcoming context after commit
  commit_conn_ = engine_->sink().connect(
      [this](const std::string &text) { OnCommit(text); });
  // 前缀软确认执行（2026-11-23）：挂到 context update_notifier——PushInput
  // 时 composition 仍是上一轮（menu 已填充），filter 阶段 seg.menu 恒 null
  if (engine_ && engine_->context())
    update_conn_ = engine_->context()->update_notifier().connect(
        [this](Context *ctx) { OnContextUpdate(ctx); });
  // register context-change callback: pre-decode on every TSF caret text
  // delivery (covers window switch / model-load cases with no commit)
  const RimeApi *api2 = rime_get_api();
  if (api2 && api2->set_context_changed_callback)
    api2->set_context_changed_callback(&on_context_changed);
  if (g_enabled)
    load_model_async();
  else {
    log_msg("config: enabled=false, LLM rerank disabled");
    // release a previously loaded model: enabled was toggled off via re-deploy
    unload_model();
  }
}

LlmFilter::~LlmFilter() {
  commit_conn_.disconnect();
  update_conn_.disconnect();
}

// ============================================================
// request async pre-decode of a raw context text (normalize +
// tokenize + prepare on a detached thread; dedup inside prepare)
// ============================================================
static void request_prepare(const std::string &raw_ctx) {
  if (raw_ctx.empty())
    return;
  std::thread([raw_ctx]() {
    std::string ctx = normalize_ctx(raw_ctx);
    std::vector<llama_token> ctx_ids = tokenize(ctx.c_str());
    if ((int)ctx_ids.size() < g_min_tokens)
      return;
    if ((int)ctx_ids.size() > g_max_ctx_tokens)
      ctx_ids.erase(ctx_ids.begin(), ctx_ids.end() - g_max_ctx_tokens);

    // bump request seq so any older in-flight prepare skips itself
    int seq = ++g_prep_seq;
    std::thread([ctx_ids, seq]() { prepare(ctx_ids, seq); }).detach();
  }).detach();
}

// ============================================================
// context-change callback: fired by rime_api whenever the TSF
// frontend delivers a new caret text (set_context_text). This
// covers pre-decode for the FIRST word after a window switch or
// after the model finished loading - cases where the context
// changes but no commit happens, so OnCommit never fires.
// prepare() dedups identical ctx, so per-key invocations are free.
// ============================================================
static void on_context_changed(const char *text) {
  if (!g_loaded.load() || g_loading.load())
    return;
  // Throttle: continuous doc updates (CEF scroll/lyrics/animation) can
  // fire this callback dozens of times per second; only the first within
  // the window runs (later updates are covered by the next callback or
  // the OnCommit poll). 100ms vs 300ms: a prep miss costs a full S1
  // (30-50ms) on the next word - measured 15/1395 score lines had
  // prep=0 under fast typing with the 300ms window. request_prepare
  // dedups identical ctx anyway, so the window only guards thread
  // spawn overhead; 100ms suffices.
  static std::atomic<long long> s_last_prep_ms{0};
  long long now =
#ifdef _WIN32
      (long long)GetTickCount64();
#else
      0;  // non-Windows: no throttling (unused path)
#endif
  if (now > 0 && now - s_last_prep_ms.load() < 100)
    return;
  s_last_prep_ms.store(now);
  request_prepare(text ? text : "");
}

void LlmFilter::OnCommit(const std::string &commit_text) {
  // 异步基建（2026-11-22）：先收割（上屏时本词评分多半已完成——立即落账，
  // 场景内最后一词也有 ok 行，语料断言/词码对 ref 不依赖"下一次 query"），
  // 再取消（提交后上文变化，中断在跑的评分——代次递增 + 清任务槽，
  // score_batch Step 边界生效；挂账条目随代次废弃）。结果槽内未收割的
  // 旧成果不受 cancel 影响。
  async_harvest();
  async_cancel();
  // Commit-history fallback accumulation (engine thread):
  // reset-generation sync first (edit keys / window switch invalidate the
  // old accumulation), then append this commit.
  const RimeApi *api = rime_get_api();
  if (api && api->context_reset_generation) {
    int gen = api->context_reset_generation();
    if (gen != g_fallback_gen_seen) {
      g_fallback_buffer.clear();
      g_fallback_gen_seen = gen;
    }
  }
  g_fallback_buffer += commit_text;
  // 用户词频累计 (freq_beta 对数融合; 仅含中文的词, 每 20 词落盘)。
  // 词码对记账（2026-11-13）: bump 键 = (词, 最近窗码 g_last_input——
  // commit 时 input 已清空, Collect 构造时快照; 消费即清, 非输入路径
  // 的 commit 不误记旧码)。debug commit 行: eff/tick/脏计数 bump 前后。
  std::string code = g_last_input;
  g_last_input.clear();
  bool has_cjk = false;
  for (unsigned char ch : commit_text)
    if (ch >= 0x80) {
      has_cjk = true;
      break;
    }
  // [简码/顶屏码补录]（2026-11-21）: 这类提交不触发重排、无 Collect 快照
  // （g_last_input 空）。本 sink 从 ConcreteEngine::OnCommit 发出、仍在
  // Context::Commit 的 Clear() 之前——此刻 context()->input() 即本次提交
  // 的完整输入码（简码 1-2 码也完整）。纯小写字母才采（标点/ASCII 不采）。
  if (code.empty() && has_cjk) {
    const std::string &live = engine_->context()->input();
    if (!live.empty() && live.size() <= 64 &&
        live.find_first_not_of("abcdefghijklmnopqrstuvwxyz") ==
            std::string::npos)
      code = live;
  }
  // 账本查询自带懒加载（2026-11-17 合并单账本），无需显式 ensure
  double eff_before = has_cjk ? user_freq_eff(commit_text, code) : 0;
  int dirty_before = g_user_stats_dirty;
  user_stats_bump(commit_text, code);
  double eff_after = has_cjk ? user_freq_eff(commit_text, code) : 0;
  long long tick_after = g_user_tick;
  // 常开语料 C 事件（提交词/码/名次；ref 指向同码 W）
  std::string cid = write_c(commit_text, code, tick_after, has_cjk);
  commit_stack_push(commit_text, code, tick_after, cid);  // 回撤退款
  if (g_debug_fusion && has_cjk) {
    debug_write_e(cid, eff_before, eff_after, dirty_before);
    // 实时调试 E 行（2026-11-10）：commit 落账（eff/tick 前后值）
    live_send(std::string("E|") + now_hms() + "|" + live_esc(commit_text) +
              "|" + live_num(eff_before) + "|" + live_num(eff_after) + "|" +
              std::to_string(g_user_tick - 1) + "|" +
              std::to_string((long long)g_user_tick));
  }
  // bound the fallback buffer: LLM context is at most ~20 tokens
  // (1-2 chars/token = 40 chars), keep only the most recent 64 UTF-8
  // bytes, aligned to character boundaries
  if (g_fallback_buffer.size() > 64) {
    size_t cut = g_fallback_buffer.size() - 64;
    while (cut < g_fallback_buffer.size() &&
           (static_cast<unsigned char>(g_fallback_buffer[cut]) & 0xC0) == 0x80)
      ++cut;  // skip UTF-8 continuation bytes
    g_fallback_buffer.erase(0, cut);
  }

  // The TSF frontend refreshes the context text asynchronously: right after
  // the commit key, the cached context still lacks the committed text. Poll
  // until it changes (usually <150ms), then pre-decode it in a background
  // thread so the next candidate window hits the prepared KV/logits.
  // (Note: the set_context_text callback also triggers request_prepare,
  // so this poll is a fallback for apps that deliver the update late.)
  if (!g_loaded.load() || g_loading.load())
    return;

  bool tsf_valid = api && api->context_text_valid && api->context_text_valid();
  // commit-history fallback string, computed on the engine thread (append
  // 已完成, 该值即含本词); 总是填充 — WPS 受限窗口的 prepare 分支也要用
  std::string fallback = g_fallback_buffer;

  std::thread([this, tsf_valid, fallback]() {
#ifdef _WIN32
    // 前台 writer（WPS/Word）：COM 是该场景的上文真源 —— kick 后短等
    // (~50ms 覆盖 5.5ms 读链) 取快照做 prepare，与 score 的 COM 同源命中。
    // COM 不可用 → fallback (hist)：WPS 场景 TSF 文本 lagging，poll 它做
    // prepare 只会与 score 不同源 → prep 恒 miss、每词全量 S1（既有缺陷，
    // 2026-09-10 修复）；Word 场景 COM 失败（失焦注册坑）时 hist 亦同源。
    if (g_com_ctx_enabled && comctx::foreground_office() != comctx::OfficeKind::NONE) {
      comctx::kick();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::string com = comctx::snapshot(800);
      if (!com.empty()) {
        request_prepare(com);
        return;
      }
      if (!fallback.empty())
        request_prepare(fallback);
      return;
    }
#endif
    std::string cur_ctx;
    if (tsf_valid) {
      std::string old_ctx = GetContextTextGlobal();
      cur_ctx = old_ctx;
      for (int i = 0; i < 30; i++) {  // up to ~600ms
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cur_ctx = GetContextTextGlobal();
        if (cur_ctx != old_ctx)
          break;
      }
      if (cur_ctx.empty() || cur_ctx == old_ctx)
        return;  // no change -> score full pass covers it
    } else {
      // TSF unavailable: commit history is already up to date
      cur_ctx = fallback;
      if (cur_ctx.empty())
        return;
    }
    request_prepare(cur_ctx);
  }).detach();
}

std::pair<std::string, std::string> LlmFilter::GetContextTextPair() const {
  const RimeApi *api = rime_get_api();

#ifdef _WIN32
  // 鼠标点击检测（2026-09-11 深夜用户定案：鼠标移动主要靠点击检测——
  // 任何点击=光标可能移动→清历史兜底，不做例外排除——候选窗/工具栏
  // 点击也清，上下文宁短不错。GetAsyncKeyState LSB=自本进程上次查询
  // 以来按下过，专为轮询设计；首次调用建立基线防进程历史点击误报。
  // 真文通道 tsf/COM 不受影响，只影响兜底。与插件版 lua click_happened
  // 同源同语义）
  {
    static bool s_click_inited = false;
    short b = GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
              GetAsyncKeyState(VK_MBUTTON);
    if (s_click_inited && (b & 1)) {
      g_fallback_buffer.clear();
      // 2026-09-12 信号层统一：点击也成对失效 COM 快照 + kick 重读——
      // 快照是旧时刻真文，点击移光标后同样冒充（标题不变挡不住同文档内
      // 移位）。点击候选窗选词场景：commit kick 刚刷新的正确快照被废，
      // kick ~50ms 后读回同样正确的文本（点击已处理完、含新上屏词），
      // 自愈无净损。
      comctx::invalidate();
      comctx::kick();
    }
    s_click_inited = true;
  }
#endif

  // 鼠标移动光标 = reset（补充通道）：新鲜 TSF 送达与历史上屏互不为
  // 尾部 = 光标被移走（无点击的位移/送达层信号）→ 清历史基座。与上方
  // 点击检测互补，真文通道不受影响。
  if (api && api->get_context_text) {
    const char *t = api->get_context_text();
    unsigned long long tage =
        api->context_text_age_ms ? api->context_text_age_ms() : ~0ULL;
    if (t && *t && tage < 5000 &&
        !ctx_logic::tail_consistent(g_fallback_buffer, t))
      g_fallback_buffer.clear();
  }

  // 受限窗口粘性降级 (2026-08-18 八轮, 用户决策): 见 g_ctx_limited 声明处
  // 注释。判据只有 lagging 一条, 其余信号刻意不粘:
  //  - 空文本/transient 空: 好应用提交后也会瞬时采空, 粘了会误降级;
  //  - stale (新鲜度过期): 好应用鼠标移动光标后必然出现 (设计上已知
  //    误判, 靠 fresh 窗口绕过), 粘了会在好应用误降级;
  //  - 中途出现的"全会话文本"不解除降级: 其内容 ≈ 本会话上屏历史 (七轮
  //    日志实证), 解除无质量收益, 只会重新引入徽章闪烁。
  if (api && api->context_reset_generation) {
    int gen = api->context_reset_generation();
    if (gen != g_limited_gen_seen) {
      // 受限标记留存（2026-09-11 简化定案）：reset 后仍处 Office 前台
      //（WPS 内部 DocumentMgr 切换：格/标签移动、编辑键）→ 保留受限
      // 判定，历史由 g_fallback_gen_seen 同步清空；离开 Office 前台
      //（真切窗）→ 清除重新评估。残余：WPS→微软 Word 同类名切换时
      // 受限会带过去，Word 落历史直到下次真切换（可接受）。
#ifdef _WIN32
      if (g_ctx_limited &&
          comctx::foreground_office() == comctx::OfficeKind::NONE)
        g_ctx_limited = false;
#else
      g_ctx_limited = false;
#endif
      // 编辑键/切窗（reset 代次）：与历史清空成对失效 COM 快照 + kick
      // 延迟重读（2026-09-12 信号层统一）——此块在下方 COM 消费之前，
      // 保证代次变化的首次调用就不会采信旧快照；kick ~50ms 后读到新
      // 真文，第二词起恢复。TSF 推送制事件自愈无需此信号。
#ifdef _WIN32
      comctx::invalidate();
      comctx::kick();
#endif
      g_limited_gen_seen = gen;
    }
  }

  // COM 文档模型旁路（前台门控，2026-09-10 定案——取代此前 lagging 触发）:
  // lagging 是后果判定（TSF 表现出受限），不能作为 COM 消费门控——无 COM
  // 的 lagging 应用会误用后台 WPS/Word 的文档文本；前台类名是身份判定，
  // 与 COM 读的 ActiveDocument（该应用的活动文档）语义精确对齐。前台
  // writer 命中时 COM 优先（文档真文含既有内容；WPS 的 TSF 在全会话/
  // composition 间闪烁，COM 是其超集，弃 TSF 无损）；COM 空（文档关闭/
  // 附着中/Word 冷启动失焦注册坑 KB238610）→ 落下面既有链。
  // 下方 lagging 粘性机制保留但与 COM 解耦：无 COM 的 lagging 应用仍由
  // 它兜底历史。
#ifdef _WIN32
  if (g_com_ctx_enabled && comctx::foreground_office() != comctx::OfficeKind::NONE) {
    comctx::ensure_started();
    std::string com = comctx::snapshot(2500);
    if (!com.empty())
      return {com, "com"};
  }
#endif

  if (g_ctx_limited) {
    // 受限窗口：上文只来自历史——TSF 永不直接消费（受限 store 快照不
    // 跟光标走，2026-09-11 定局实验：格内鼠标移位后首词拿到旧光标尾
    // 巴）。鼠标移动清历史由顶部全应用检测负责（含同窗内移动的送达
    // 感知）；WPS 格/标签移动由 focus:switch reset 清历史（受限标记
    // 留存，见上方代次同步）。历史空（reset/移位后首词）→ 不推理。
    std::string hist = CommitHistoryText();
    if (!hist.empty())
      return {hist, "rime"};
    return {"", "rime"};
  }

  if (api && api->get_context_text) {
    const char *text = api->get_context_text();
    if (text && *text) {
      std::string hist = CommitHistoryText();
      if (!hist.empty()) {
        // 滞后检测 (WPS): TSF 文本是历史尾部子串且明显更短 → 应用只暴露
        // 最近 composition (实测连续打 N 个"测试"只采到 2 字符) → 粘性降
        // 级: 本窗口后续全部直接用历史上文, 标 AI·历史 (2026-08-18 用户
        // 最终决策, 取代此前"标 tsf"的过渡方案)
        if (ctx_logic::lagging(text, hist)) {
          g_ctx_limited = true;
          return {hist, "rime"};
        }
        // 新鲜度判定: 5s 内送达的 TSF 文本直接可信——"fallback 尾部须在
        // TSF 文本中"的重合判据在光标移动后打词时必然误判 (新词在光标后,
        // 不在打词前采集的文本中)。新鲜送达 = 采集链路工作正常。
        bool fresh = api->context_text_age_ms &&
                     api->context_text_age_ms() < 5000;
        // 残留检测: 陈旧 TSF 文本可能属于其他应用; 光标前文本必然包含最近
        // 上屏词 (hist 尾部), 不含 → 过期 → 用 fallback
        if (!fresh && ctx_logic::stale(text, hist))
          return {hist, "rime"};
      }
      return {text, "tsf"};  // TSF caret text available
    }
    if (api->context_text_valid && api->context_text_valid()) {
      // TSF collection works but the current text is empty. Two cases:
      //  - transient empty: right after a commit the async TSF refresh
      //    (debounce + IPC) has not landed yet, or a selection-change
      //    collection caught an unstable selection → use the commit
      //    history fallback (synchronous sink) so the candidate window
      //    still re-ranks (2nd word never re-ranked, 3rd word onwards did);
      //  - genuinely empty for >1.5s (caret at document start / all
      //    deleted): the fallback words sit AFTER the caret, they are not
      //    context → skip rerank (2026-08-13: moving to document start
      //    still showed AI·历史 badge before this fix).
      unsigned long long age = api->context_text_age_ms
                                   ? api->context_text_age_ms()
                                   : ~0ULL;
      std::string hist = CommitHistoryText();
      if (ctx_logic::transient_empty(hist, age))
        return {hist, "rime"};
      return {"", "tsf"};
    }
  }
  // TSF never collected (app does not support TSF text access):
  // fall back to this session's commit history.
  return {CommitHistoryText(), "rime"};
}

std::string LlmFilter::CommitHistoryText() const {
  // Reset generation sync: after edit keys (BackSpace/Delete/navigation/
  // Enter) or a window switch the frontend bumps the generation via
  // reset_context_text(); the old accumulation is discarded and the
  // fallback restarts from the next commit.
  const RimeApi *api = rime_get_api();
  if (api && api->context_reset_generation) {
    int gen = api->context_reset_generation();
    if (gen != g_fallback_gen_seen) {
      g_fallback_buffer.clear();
      g_fallback_gen_seen = gen;
    }
  }
  return g_fallback_buffer;
}

std::string LlmFilter::GetContextTextGlobal() {
  const RimeApi *api = rime_get_api();
  if (!api || !api->get_context_text)
    return "";
  const char *text = api->get_context_text();
  return text ? text : "";
}

// 组合区已确认段文本（status >= kSelected）按顺序拼接（2026-11-23，§11.8）：
// 前缀软确认后这些词是确定 token，LLM 的 ctx 必须包含它们
//（CE(整句) = CE(P|C) + CE(R|C+P)，P 是常数项）。
static std::string ConfirmedPrefixText(Context *ctx) {
  std::string out;
  if (!ctx) return out;
  for (const Segment &seg : ctx->composition()) {
    if (seg.status < Segment::kSelected) continue;
    if (auto cand = seg.GetSelectedCandidate()) out += cand->text();
  }
  return out;
}

// 执行上一轮挂起的前缀软确认（下一次 Apply 入口调用）：输入前缀/段起点
// 未变才执行；menu 按文本找候选 → ctx->Select（Segment::Close 截段 →
// Forward → Compose 只重译剩余）。返回 true = 执行了确认（本键 translation
// 是确认前旧段的，调用方应跳过重排直接透传）。
static bool RunPendingConfirm(Context *ctx) {
  if (!ctx || g_in_confirm) return false;
  PendingConfirm pc;
  {
    std::lock_guard<std::mutex> lk(g_pending_confirm_mu);
    if (!g_pending_confirm.valid) return false;
    pc = g_pending_confirm;
  }
  auto clear_pending = []() {
    std::lock_guard<std::mutex> lk(g_pending_confirm_mu);
    g_pending_confirm.valid = false;
  };
  // 可重试失败（组合区暂空/菜单未就绪）：保留挂起等下一次刷新，但**限次**
  // ——上限 8 次后放弃，避免"刷新→重译→再刷新"的持续循环（2026-11-24）。
  auto bump_retry = []() {
    std::lock_guard<std::mutex> lk(g_pending_confirm_mu);
    if (!g_pending_confirm.valid) return;
    if (++g_pending_confirm.tries >= 8) {
      g_pending_confirm.valid = false;
      if (g_debug_fusion) log_error("pfx: exec abort retry limit");
    }
  };
  const std::string &inp = ctx->input();
  if (pc.input.empty() || inp.size() < pc.seg_start + pc.input.size()) {
    if (g_debug_fusion) log_error("pfx: exec abort input mismatch");
    clear_pending();
    return false;
  }
  if (inp.compare(pc.seg_start, pc.input.size(), pc.input) != 0) {
    if (g_debug_fusion) log_error("pfx: exec abort input differs");
    clear_pending();
    return false;
  }
  // 2026-11-24：组合区暂时为空 / 菜单未就绪 = **可重试**（刷新路径每 120ms
  // 会再来一次），保留挂起——此前在入口就清掉挂起，真机表现为"只有第一个
  // 词自动确认，之后永远 pending 不执行"。
  if (ctx->composition().empty()) {
    bump_retry();
    return false;
  }
  Segment &seg = ctx->composition().back();
  if (seg.start != pc.seg_start) {
    if (g_debug_fusion)
      log_error("pfx: exec abort seg_start=%d/%d", (int)seg.start,
                (int)pc.seg_start);
    clear_pending();
    return false;
  }
  if (!seg.menu) {
    bump_retry();
    return false;
  }
  size_t n = seg.menu->Prepare(16);
  for (size_t i = 0; i < n; i++) {
    auto c = seg.menu->GetCandidateAt(i);
    if (c && c->text() == pc.text) {
      g_in_confirm = true;
      ctx->Select(i);
      g_in_confirm = false;
      if (g_debug_fusion)
        log_error("pfx: exec select '%s' idx=%d", pc.text.c_str(), (int)i);
      clear_pending();
      return true;
    }
  }
  if (g_debug_fusion)
    log_error("pfx: exec abort '%s' not in top %d", pc.text.c_str(),
              (int)n);
  clear_pending();
  return false;
}

// 前缀软确认执行（2026-11-23）：在 PushInput 触发的 update_notifier 里做。
// 此时 composition 是上一轮的、seg.menu 已由 TranslateSegments 填充，Select
// 安全（Compose 只 set_composition，不再触发 update_notifier，无递归）。
void LlmFilter::OnContextUpdate(Context *ctx) {
  if (!ctx || g_in_confirm) return;
  // 测试钩子（仅排障）：选第一个只覆盖前缀的候选，验证 Select 链路
  if (g_test_prefix_confirm) {
    if (ctx->composition().empty()) return;
    Segment &seg = ctx->composition().back();
    log_error("tc: in=%s seg=[%d,%d) st=%d menu=%d", ctx->input().c_str(),
              (int)seg.start, (int)seg.end, (int)seg.status,
              seg.menu ? 1 : 0);
    if (seg.status >= Segment::kSelected || !seg.menu) return;
    size_t n = seg.menu->Prepare(16);
    for (size_t i = 0; i < n; i++) {
      auto cand = seg.menu->GetCandidateAt(i);
      if (i < 4)
        log_error("tc: c%zu end=%d text=%.24s", i,
                  cand ? (int)cand->end() : -1,
                  cand ? cand->text().c_str() : "");
      if (cand && cand->end() > seg.start && cand->end() < seg.end) {
        g_in_confirm = true;
        ctx->Select(i);
        g_in_confirm = false;
        return;
      }
    }
    return;
  }
  // 只在刷新路径执行（2026-11-23 晚修）：PushInput 触发的 notifier 里
  // ctx->Select 会让 process_key 中途改组合区——真机 = 该键无响应、TSF 卡死。
  if (g_prefix_confirm &&
      g_refresh_depth.load(std::memory_order_relaxed) > 0)
    RunPendingConfirm(ctx);
}

an<Translation> LlmFilter::Apply(an<Translation> translation,
                                 CandidateList *candidates) {
  if (!translation)
    return translation;

  // llm_rerank 子目录确保（2026-11-21 记录文件收进子目录）：进程内一次。
  // 与 enabled 无关——固定候选 lua（pin_candidates.tsv 仍在根目录）的
  // 一次性迁移假定该子目录可被尽早建出；记录文件 resolve 内也会兜底建。
  // 2026-11-22 分方案后：只确保根目录，方案层由 llm_file_path_ex 建。
  static bool llm_subdir_ensured = false;
  if (!llm_subdir_ensured) {
    llm_subdir_ensured = true;
    const RimeApi *api0 = rime_get_api();
    if (api0 && api0->get_user_data_dir) {
      const char *ud0 = api0->get_user_data_dir();
      if (ud0 && *ud0) {
        char root0[MAX_PATH];
        snprintf(root0, sizeof(root0), "%s\\llm_rerank", ud0);
        llm_dir_ensure(root0);
      }
    }
  }

  // 方案 ID 跟随（2026-11-22 分目录）：session 内固定，变更检查微秒级
  if (engine_ && engine_->schema())
    llm_set_schema_id(engine_->schema()->schema_id());

  // 词码对记账/语料 C 的码快照：必须在本函数**所有**早退路径之前更新。
  // 此前只在 LlmRerankTranslation 构造时赋值——ctx_empty/model_loading/
  // pattern 早退会留下上一个词的码，导致下一个提交的 C 行码错配。
  // 2026-11-22 整句模式取码：活动段切片。librime 只对未确认的最后一段
  // 做翻译（TranslateSegments），故活动段 = composition.back()；多段续打
  // （前段已选定）时 code_pattern/词频键/语料 code 随切片，单段组合 =
  // 全串（双拼与既有行为逐位一致）。候选 span 是对**原始 input** 的，
  // FM 判定须与段边界 [seg_start, seg_end) 比较，切片随构造参数传递。
  size_t seg_start = 0, seg_end = engine_->context()->input().length();
  {
    const Composition &comp = engine_->context()->composition();
    if (!comp.empty()) {
      const Segment &seg = comp.back();
      // 2026-11-24 修（真机报障：确认第一个词后切错/乱候选/不再确认）：
      // `ctx->Select` 之后 `Context::input()` 只保留**未确认部分**，而段的
      // start/end 仍是**原串坐标** → `seg.end > input.size()` 即坐标错位。
      // 此时把活动段当作"整个当前 input"（基址归零）——候选 span 也是相对
      // 当前 input 的，FM 比较随之一致。
      if (seg.end > seg_end) {
        seg_start = 0;
        // seg_end 保持 = 当前 input 长度
      } else if (seg.end > seg.start && seg.end <= seg_end &&
                 seg.start < seg_end) {
        seg_start = seg.start;
        seg_end = seg.end;
      }
    }
  }
  const std::string code =
      engine_->context()->input().substr(seg_start, seg_end - seg_start);
  g_last_input = code;

  // 参数在部署期快照（方案 llm_rerank 节）——改配置需重新部署，无热重载
  if (!g_enabled)
    return translation;  // enabled=false -> pass-through

#ifdef _WIN32
  // COM 旁路线程提前启动（2026-09-10 用户定案：统一 WPS 徽章，消除 lagging
  // 首中词回落 AI·历史 的冷启动窗口）。开销核算（讨论定案）：未装 WPS =
  // CLSID 解析失败线程即退，零开销；WPS 在而无文档 = GetActiveObject 连败
  // 8s 退避（微秒级本地 ROT 查询）；有文档 = 2s 周期 × ~5.5ms 读链 ≈ 0.3%
  // CPU。好应用打字不受影响——COM 消费被粘性降级门控，好应用走 TSF 分支。
  if (g_com_ctx_enabled)
    comctx::ensure_started();
#endif

  {
    // 触发条件（2026-09-30）：编码串全串匹配 code_pattern（与 speller/
    // auto_select_pattern 同引擎同语义）。空模式 = 不限制。
    // 2026-11-22 整句模式：匹配对象 = 活动段切片 code（多段续打正确）。
    bool matched = true;
    if (!g_code_pattern.empty()) {
      matched = g_code_re_ok && boost::regex_match(code, g_code_re);
    }
    if (!matched) {
      // 实时调试（2026-11-10）：跳过原因 S 行——"为什么没触发"是调试窗口
      // 的主要用途之一（pattern / ctx_empty / model_loading / few_candidates）
      live_send(std::string("S|") + now_hms() + "|pattern|" +
                live_esc(code) + "|-");
      return translation;  // code outside code_pattern
    }
  }

  // context source: TSF caret text, or commit history fallback
  auto [ctx, src] = GetContextTextPair();
  // 组合区已确认前缀追加到 ctx（2026-11-23，§11.8 协同）：已确认词是确定
  // token，LLM 上下文必须包含；P 在末尾、截断取末 max_ctx token 时不会被
  // 挤掉（仅 P 自身超长才会被截尾）。
  if (g_prefix_confirm) {
    std::string conf = ConfirmedPrefixText(engine_->context());
    if (!conf.empty()) ctx += conf;
  }
  if (ctx.empty()) {
    // 无上文（2026-11-24 用户定案）：CE 不可算，但整句模式下前缀确认仍要跑
    // ——融合分按剩余项（β·log1p(eff) + log(uni+1) + 词码对修正）计算，
    // 组句 lead 仍与窗口融合序第 1 比较（Collect 内 freq_only 分支）。
    // 组合区已确认前缀已在上面追加进 ctx：确认过 1 个 phrase 后 ctx 非空，
    // 下一键自动回到完整 CE 融合（"选一次 phrase 就有了临时上文"）。
    // 其余开关组合保持旧行为（skip ctx_empty）。
    const bool freq_only_ok = g_sentence_mode && g_prefix_confirm &&
                              g_ngram_group && code.size() >= 4;
    if (!freq_only_ok) {
      // 常开语料：满码但无上文的 skip 事件（W 行，status=skip）
      std::string wid = new_event_id("W", g_user_tick);
      write_w(wid, src, code, "", {}, {}, {}, g_user_tick, "skip", "ctx_empty",
              false, "");
      live_send(std::string("S|") + now_hms() + "|ctx_empty|" +
                live_esc(code) + "|" + src);
      return translation;  // no context, keep original order
    }
  }

  if (!g_loaded.load()) {
    std::string wid = new_event_id("W", g_user_tick);
    write_w(wid, src, code, ctx, {}, {}, {}, g_user_tick, "skip",
            "model_loading", false, "");
    live_send(std::string("S|") + now_hms() + "|model_loading|" +
              live_esc(code) + "|" + src);
    return translation;  // model still loading, keep original order
  }

  return New<LlmRerankTranslation>(translation, code, seg_start, seg_end,
                                   ctx, src, engine_);
}

}  // namespace rime
