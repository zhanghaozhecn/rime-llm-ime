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
// 参数优先级 schema llm_rerank 节 > 全局 %APPDATA%\Rime\llm_rerank.yaml
//（GUI 写入，Apply 按 mtime|size 指纹热重载）。
//
#include <rime/gear/llm_filter.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime_api.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "llama.h"

#ifdef _WIN32
#include <windows.h>
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
//   min_code_len: input code length below this -> no rerank
//   max_code_len: input code length above this -> no rerank (0 = unlimited);
//                 [min, max] = rerank trigger window
//   expected_length_weight: >0 = bonus candidates whose word length equals
//                 floor(code_len/2) (两码一字), weighted by current score span
//   freq_weight/freq_k: 用户词频融合 total=(1-w)·LLM(窗内min-max归一)
//                 + w·eff/(eff+k)，eff=Rime formula_d 时间衰减计数
//                 （tick 每词提交+1，与引擎调频同源；user_freq.tsv 持久化）
// 排序管线（顺序固定，与插件版一致）: CE 评分序 → 词频融合（融合应用于
//   评分序之上）→ expected_length 加权 → 稳定排序（失败哨兵不参与
//   min-max 与加成；非有限分整块跳过）。
// 长候选外推: 4+ token 候选按尾部 CE 外推（λ=0.6，语料模拟调参），
//   不增加 decode 次数，3-token 词不受长词挤压。
// 全局配置（2026-08-27 直接安装版）: %APPDATA%\Rime\llm_rerank.yaml（GUI
// 写入, 平面 key: value, Apply 时按 mtime|size 热重载）; 优先级 schema 节 >
// 全局 yaml > 内置默认。2026-08-29 起为显式组件：方案 engine/filters 列出
// llm_filter 才参与重排（enabled 默认 false 纯透传）。
// 默认模型路径 = RIME 用户目录根\Qwen...gguf（2026-08-31 用户澄清定案：
// "用户文件夹"= 小狼毫右键的用户文件夹——方案配置所在处，模型直接放根
// 目录、不套子文件夹，与 simplifier 取 user_data_dir 同源；8-27 曾误用
// %USERPROFILE%\gguf_models\）。
// 自定义位置用 llm_rerank model_path 显式指向。
// 注意 Service.deployer().user_data_dir 在引擎启动后才就绪，故不能在
// 静态初始化求值——g_model_path 留空，加载时懒取默认。
static std::string default_model_path() {
  try {
    return (Service::instance().deployer().user_data_dir /
            "Qwen3.5-0.8B-Q4_K_M.gguf")
        .string();
  } catch (...) {
    return "Qwen3.5-0.8B-Q4_K_M.gguf";
  }
}
static std::string g_model_path;  // 空 = 未配置，load_model 时按默认兜底
static std::string g_loaded_from; // 当前已加载模型来自的路径（变更检测）
static bool g_enabled = false;  // CPU only; GPU build retired (not published)
static int g_min_code_len = 4;
static int g_max_code_len = 0;  // 0 = no upper limit (plugin-version parity)
static double g_expected_length_weight = 0.0;  // 预期词长加权 (两版统一 2026-08-27)
// 用户词频融合 (2026-08-19): total = (1-w)·LLM + w·count/(count+k),
// 默认 w=0.25/k=5 (真实候选窗回放实证, 见 Collect 融合段注释)
static double g_freq_weight = 0.25;
static int g_freq_k = 5;
static int g_min_tokens = 1;
static int g_max_ctx_tokens = 10;  // tok=10: 93.4% acc, 10->17 gains only +1.1pp
static int g_n_threads = 4;        // default = GGML_DEFAULT_N_THREADS; override via cpu_cores

static void log_msg(const char *fmt, ...);  // defined below (fwd decl)
static std::vector<llama_token> tokenize(const char *text);  // fwd decl
static double cross_entropy(float *logits, int vs, int target_id);  // fwd decl

static int g_n_ctx = 128;          // KV: 11 seqs x (ctx 10 + cand 2) = 132, 64 overflows
static int g_n_seq_max = 12;       // template seq 0 + up to 11 worker seqs
static int g_max_candidates = 5;   // candidates participating in scoring

// ==== 参数三级合并（2026-08-27 直接安装版）: schema llm_rerank 节 > 全局
// llm_rerank.yaml（%APPDATA%\Rime，GUI 写入）> 内置默认。全局文件热重载
//（Apply 时 stat mtime|size 指纹，变了重读合并；enabled 关→开触发模型异步
// 加载；model_path 热改不重载模型，需重启会话）。engine 线程调用。 ====
struct LlmParamSet {
  bool has_enabled = false;      bool enabled = false;
  bool has_min_code_len = false; int min_code_len = 4;
  bool has_max_code_len = false; int max_code_len = 0;
  bool has_elw = false;          double elw = 0.0;
  bool has_freq_weight = false;  double freq_weight = 0.25;
  bool has_freq_k = false;       int freq_k = 5;
  bool has_min_tokens = false;   int min_tokens = 1;
  bool has_max_tokens = false;   int max_tokens = 10;
  bool has_max_cand = false;     int max_cand = 5;
  bool has_cpu_cores = false;    int cpu_cores = 4;
  bool has_model_path = false;   std::string model_path;
};
static LlmParamSet g_schema_params;  // Initialize 时快照（部署期固定）
static LlmParamSet g_yaml_params;    // 全局 yaml（热重载）
static unsigned long long g_yaml_stamp = 0;  // mtime|size 变更指纹
static void load_model_async();      // fwd decl（定义在下方）

static void llm_apply_params() {
  const LlmParamSet &s = g_schema_params, &y = g_yaml_params;
  g_enabled = s.has_enabled ? s.enabled : (y.has_enabled ? y.enabled : false);
  g_min_code_len = s.has_min_code_len
                       ? s.min_code_len
                       : (y.has_min_code_len ? y.min_code_len : 4);
  g_max_code_len = s.has_max_code_len
                       ? s.max_code_len
                       : (y.has_max_code_len ? y.max_code_len : 0);
  g_expected_length_weight =
      s.has_elw ? s.elw : (y.has_elw ? y.elw : 0.0);
  g_freq_weight = s.has_freq_weight ? s.freq_weight
                                    : (y.has_freq_weight ? y.freq_weight : 0.25);
  g_freq_k = s.has_freq_k ? s.freq_k : (y.has_freq_k ? y.freq_k : 5);
  g_min_tokens =
      s.has_min_tokens ? s.min_tokens : (y.has_min_tokens ? y.min_tokens : 1);
  g_max_ctx_tokens = s.has_max_tokens
                         ? s.max_tokens
                         : (y.has_max_tokens ? y.max_tokens : 10);
  g_max_candidates =
      s.has_max_cand ? s.max_cand : (y.has_max_cand ? y.max_cand : 5);
  g_n_threads =
      s.has_cpu_cores ? s.cpu_cores : (y.has_cpu_cores ? y.cpu_cores : 4);
  // cap threads at hardware cores: 低核机器不应超订（变慢 + 每线程额外内存）
  unsigned hw = std::thread::hardware_concurrency();
  if (hw > 0 && (unsigned)g_n_threads > hw)
    g_n_threads = (int)hw;
  g_model_path = s.has_model_path
                     ? s.model_path
                     : (y.has_model_path ? y.model_path
                                         : default_model_path());
}

// 全局 llm_rerank.yaml 路径（与 user_freq.tsv 同目录解析）
static bool llm_global_file(char *path, size_t n) {
  const RimeApi *api = rime_get_api();
  if (api && api->get_user_data_dir) {
    const char *ud = api->get_user_data_dir();
    if (ud && *ud) {
      snprintf(path, n, "%s\\llm_rerank.yaml", ud);
      return true;
    }
  }
  return false;
}

static std::string llm_trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t\r\n");
  return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// 扁平 key: value 解析（GUI 生成的平面 yaml；坏行跳过；值支持行内 # 注释
// 与成对引号）
static void llm_load_global_params() {
  g_yaml_params = LlmParamSet();
  char path[MAX_PATH];
  if (!llm_global_file(path, sizeof(path)))
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string ln = llm_trim(line);
    if (ln.empty() || ln[0] == '#')
      continue;
    size_t c = ln.find(':');
    if (c == std::string::npos)
      continue;
    std::string key = llm_trim(ln.substr(0, c));
    std::string val = llm_trim(ln.substr(c + 1));
    if (!val.empty() && val[0] == '"') {  // 引号值: 取到闭引号
      size_t e = val.find('"', 1);
      val = (e == std::string::npos) ? val.substr(1) : val.substr(1, e - 1);
    } else {  // 行内注释
      size_t h = val.find('#');
      if (h != std::string::npos)
        val = llm_trim(val.substr(0, h));
    }
    LlmParamSet &p = g_yaml_params;
    if (key == "enabled") { p.has_enabled = true; p.enabled = (val == "true"); }
    else if (key == "min_code_len") { p.has_min_code_len = true; p.min_code_len = atoi(val.c_str()); }
    else if (key == "max_code_len") { p.has_max_code_len = true; p.max_code_len = atoi(val.c_str()); }
    else if (key == "expected_length_weight") { p.has_elw = true; p.elw = atof(val.c_str()); }
    else if (key == "freq_weight") { p.has_freq_weight = true; p.freq_weight = atof(val.c_str()); }
    else if (key == "freq_k") { p.has_freq_k = true; p.freq_k = atoi(val.c_str()); }
    else if (key == "min_tokens") { p.has_min_tokens = true; p.min_tokens = atoi(val.c_str()); }
    else if (key == "max_tokens") { p.has_max_tokens = true; p.max_tokens = atoi(val.c_str()); }
    else if (key == "max_candidates") { p.has_max_cand = true; p.max_cand = atoi(val.c_str()); }
    else if (key == "cpu_cores") { p.has_cpu_cores = true; p.cpu_cores = atoi(val.c_str()); }
    else if (key == "model_path") { p.has_model_path = true; p.model_path = val; }
  }
  fclose(f);
}

static unsigned long long llm_yaml_stamp() {
  char path[MAX_PATH];
  if (!llm_global_file(path, sizeof(path)))
    return 0;
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
    return 0;  // 文件不存在 = 空指纹（删除全局配置 → 回退 schema/默认）
  unsigned long long st =
      ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
      (unsigned long long)fa.ftLastWriteTime.dwLowDateTime;
  return st ^ ((unsigned long long)fa.nFileSizeLow << 1);
}

static void unload_model();  // 定义在后（enabled 关闭/路径变更时卸载，前置声明）

// 热重载: 指纹变化 → 重读合并; enabled 开关与路径变更即时生效
// （开 → 加载/重载；关 → 卸载释放内存。2026-09-01 修复：热路径此前
// 只处理"关→开加载"，"开→关卸载"漏了——GUI 开关不重新部署时模型
// 永远驻留，推理看起来"关不掉"）。
static void llm_reload_global_if_changed() {
  unsigned long long stamp = llm_yaml_stamp();
  if (stamp == g_yaml_stamp)
    return;
  g_yaml_stamp = stamp;  // 先记指纹（坏文件不反复重试）
  llm_load_global_params();
  llm_apply_params();
  log_msg("llm_rerank.yaml reloaded: enabled=%d elw=%.2f freq=%.2f/%d "
          "tok=%d/%d cand=%d cores=%d model=%s",
          g_enabled ? 1 : 0, g_expected_length_weight, g_freq_weight, g_freq_k,
          g_min_tokens, g_max_ctx_tokens, g_max_candidates, g_n_threads,
          g_model_path.c_str());
  if (g_enabled)
    load_model_async();  // 路径变更时内部自动卸载重载；同路径已载为 no-op
  else if (g_loaded.load() || g_loading.load())
    unload_model();
}

// ============================================================
// commit-history fallback state (engine thread only: OnCommit sink
// callback and Apply both run on the engine thread, no lock needed)
// ============================================================
static std::string g_fallback_buffer;  // session committed texts
static int g_fallback_gen_seen = 0;    // consumed reset generation

// 受限窗口粘性降级 (2026-08-18 八轮, 用户决策): lagging 命中一次即标记本
// 窗口"受限" (WPS 类应用 TSF 只暴露最近 composition), 之后整窗直接用
// 历史上文 (标 AI·历史), 直到编辑键/切窗 reset (代次变) 才清除重评。
// engine thread only (同上)。
static bool g_ctx_limited = false;
static int g_limited_gen_seen = 0;

// ============================================================
// prepare pre-decode state: after commit, asynchronously run
// Step 1 (ctx decode -> save logits) so the next score call can
// skip Step 1 when the ctx matches.
// ============================================================
static std::vector<llama_token> g_prep_ctx;     // pre-decoded ctx tokens
static std::vector<float>       g_prep_logits;  // ctx_last logits
static bool                     g_prep_ready = false;
static std::atomic<int>         g_prep_seq{0};  // request seq, stale requests skip
static long                     g_seq0_gen = 0; // seq0 KV generation: bumped by any decode covering seq0
static long                     g_prep_gen = 0; // generation at which prep was produced

// ============================================================
// score result cache: same (ctx, input) reuses the previous rerank
// (翻页/候选窗重建不重复推理 — 对齐插件版 _G.llm_filter_cache)。
// 只存评分顺序（候选文本），词频融合/expected_length 排序与 AI 徽章每次
// 按当前配置重放，改参数后重新部署缓存仍正确。reset 代次变
// （编辑键/窗口切换）→ 失效；ctx/input 变 → key 不匹配自然失效。
// engine 线程专用（Apply/Collect 均在引擎线程），无锁。
// ============================================================
static bool                      s_cache_valid = false;
static std::string               s_cache_ctx;
static std::string               s_cache_input;
static std::vector<std::string>  s_cache_ranked;  // 评分后的候选文本顺序
static std::vector<double>       s_cache_scores;  // 与 ranked 对齐的原始分 (词频融合重放用)
static int                       s_cache_gen = -1; // 缓存建立时的 reset 代次

// ============================================================
// 用户词频 (freq_weight 融合, 2026-08-19; 2026-08-21 改 Rime 时间衰减):
// OnCommit 累计 (仅含中文的词), RIME 用户目录 user_freq.tsv 持久化
// (每 20 词落盘, 崩溃最多丢 19 次)。engine thread only, 无锁。
// 衰减 = librime algo::formula_d (引擎调频同源):
//   提交: dee = 1 + dee·exp((t_old - t_now)/τ)
//   查询: eff = dee·exp((t_word - t_now)/τ)   未提交期间持续衰减
//   τ=200 tick; tick 每词提交 +1 (同 userdb UpdateTickCount)。
// 近期常打的词权重高, 久未使用的自动消退 (半衰期 ≈ 139 次提交)。
// 格式: 首行 "#tick=N"; 数据行 "词\t累计\tdee\ttick"; 兼容旧版 "词\t次数"
// (迁移: dee=次数, tick=当前 — 视为刚提交过)。
// ============================================================
struct UserFreqEntry {
  long long commits = 0;  // 累计提交次数 (记录/诊断用, 不参与评分)
  double dee = 0;         // 衰减计数 (formula_d 的 dee)
  long long tick = 0;     // 最后提交 tick
};
static std::map<std::string, UserFreqEntry> g_user_freq;
static long long g_user_tick = 0;
static bool g_user_freq_loaded = false;
static int  g_user_freq_dirty = 0;
static constexpr double kFreqTau = 200.0;  // rime formula_d 时间常数

// Log file: RIME user data dir, single file rime_llm_filter_log.txt
// (performance lines + per-inference event lines); falls back to %TEMP%.
// resolve log file path: RIME user data dir + filename, fallback %TEMP%
static FILE *open_log_file(const char *filename) {
  char path[MAX_PATH];
  const RimeApi *api = rime_get_api();
  if (api && api->get_user_data_dir) {
    const char *ud = api->get_user_data_dir();
    if (ud && *ud && strlen(ud) < MAX_PATH - 64) {
      snprintf(path, sizeof(path), "%s\\%s", ud, filename);
      return fopen(path, "a");
    }
  }
#ifdef _WIN32
  GetTempPathA(sizeof(path), path);
  strncat(path, filename, sizeof(path) - strlen(path) - 1);
  return fopen(path, "a");
#else
  (void)path;
  return nullptr;
#endif
}

// user_freq.tsv 路径 (RIME 用户目录; 与日志同目录解析)
static bool user_freq_file(char *path, size_t n) {
  const RimeApi *api = rime_get_api();
  if (api && api->get_user_data_dir) {
    const char *ud = api->get_user_data_dir();
    if (ud && *ud) {
      snprintf(path, n, "%s\\user_freq.tsv", ud);
      return true;
    }
  }
  return false;
}

static void user_freq_ensure_loaded() {
  if (g_user_freq_loaded)
    return;
  g_user_freq_loaded = true;
  char path[MAX_PATH];
  if (!user_freq_file(path, sizeof(path)))
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  long long max_tick = 0;
  std::vector<std::pair<std::string, long long>> legacy;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (strncmp(line, "#tick=", 6) == 0) {
      g_user_tick = atoll(line + 6);
      continue;
    }
    char *t1 = strchr(line, '\t');
    if (!t1)
      continue;
    *t1 = 0;
    char *t2 = strchr(t1 + 1, '\t');
    if (!t2) {  // 旧版 2 字段: 词\t次数
      long long n = atoll(t1 + 1);
      if (n > 0)
        legacy.emplace_back(std::string(line), n);
      continue;
    }
    *t2 = 0;
    char *t3 = strchr(t2 + 1, '\t');
    if (!t3)
      continue;
    *t3 = 0;
    UserFreqEntry e;
    e.commits = atoll(t1 + 1);
    e.dee = atof(t2 + 1);
    e.tick = atoll(t3 + 1);
    if (e.dee > 0) {
      g_user_freq[std::string(line)] = e;
      if (e.tick > max_tick)
        max_tick = e.tick;
    }
  }
  fclose(f);
  if (g_user_tick <= 0)
    g_user_tick = max_tick;
  for (auto &kv : legacy)  // 旧计数视为刚提交过
    g_user_freq[kv.first] = UserFreqEntry{kv.second, (double)kv.second, g_user_tick};
}

// 衰减有效计数 (融合用): rime formula_d 查询式
static double user_freq_eff(const std::string &w) {
  auto it = g_user_freq.find(w);
  if (it == g_user_freq.end() || it->second.dee <= 0)
    return 0;
  return it->second.dee *
         exp((double)(it->second.tick - g_user_tick) / kFreqTau);
}

static void user_freq_save() {
  char path[MAX_PATH];
  if (!user_freq_file(path, sizeof(path)))
    return;
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "#tick=%lld\n", g_user_tick);
  for (auto &kv : g_user_freq)
    fprintf(f, "%s\t%lld\t%.3f\t%lld\n", kv.first.c_str(), kv.second.commits,
            kv.second.dee, kv.second.tick);
  fclose(f);
}

static void user_freq_bump(const std::string &w) {
  // 仅计含中文的词 (与插件版训练语料同语义; 候选词均为中文词)
  bool has_cjk = false;
  for (unsigned char ch : w)
    if (ch >= 0x80) {
      has_cjk = true;
      break;
    }
  if (!has_cjk)
    return;
  user_freq_ensure_loaded();
  ++g_user_tick;
  UserFreqEntry &e = g_user_freq[w];
  e.dee = 1 + e.dee * exp((double)(e.tick - g_user_tick) / kFreqTau);
  e.commits += 1;
  e.tick = g_user_tick;
  if (++g_user_freq_dirty >= 20) {
    user_freq_save();
    g_user_freq_dirty = 0;
  }
}

static void log_msg(const char *fmt, ...) {  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  FILE *f = open_log_file("rime_llm_filter_log.txt");
  if (f) {
    fprintf(f, "%s\n", buf);
    fclose(f);
  }
}

// ============================================================
// per-inference event line (merged into rime_llm_filter_log.txt):
//   HH:MM:SS|seq|input|cands_before|ctx|result|elapsed_ms|src
// Written once per real inference; '|' -> '/', newline -> space.
// ============================================================
static long g_event_cnt = 0;

// Escape control chars for log lines: ctx may contain newlines (multi-line
// caret text) which would otherwise break line-based log viewing in
// Notepad. \n/\r/\t -> literal "\\n" etc.
static std::string escape_ctx(const std::string &s) {
  std::string t;
  t.reserve(s.size());
  for (char c : s) {
    if (c == '\n')
      t += "\\n";
    else if (c == '\r')
      t += "\\r";
    else if (c == '\t')
      t += "\\t";
    else
      t += c;
  }
  return t;
}

static std::string sanitize_field(const std::string &s) {
  std::string t = s;
  for (auto &c : t) {
    if (c == '|')
      c = '/';
    else if (c == '\n' || c == '\r')
      c = ' ';
  }
  return t;
}

static void event_log(const std::string &input, const std::string &before,
                      const std::string &ctx, const std::string &after,
                      double elapsed_ms, const std::string &src) {
  FILE *f = open_log_file("rime_llm_filter_log.txt");
  if (!f)
    return;
  long n = ++g_event_cnt;
  std::time_t t = std::time(nullptr);
  std::tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  char ts[16];
  std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);
  fprintf(f, "%s|%ld|%s|%s|%s|%s|%.0fms|%s\n", ts, n, input.c_str(),
          before.c_str(), ctx.c_str(), after.c_str(), elapsed_ms, src.c_str());
  fclose(f);
}

// ============================================================
// async model loading (non-blocking for the IME)
// ============================================================
static void load_model_async() {
  // 路径变更自动重载（2026-09-01）：GUI/schema 改 model_path 而模型已
  // 加载时，旧模型驻留内存、新路径被无视——此处检测变更即卸载，随后
  // 走正常加载。检测收敛在本函数，Apply 与 yaml 热路径调用点自动受益。
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
    mparams.use_mmap = 1;

    g_model = llama_model_load_from_file(g_model_path.c_str(), mparams);
    if (!g_model) {
      log_msg("ERROR: failed to load model");
      g_loading.store(false);
      return;
    }
    g_vocab = llama_model_get_vocab(g_model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = g_n_ctx;
    cparams.n_threads = g_n_threads;
    cparams.n_threads_batch = g_n_threads;
    cparams.n_seq_max = g_n_seq_max;

    g_ctx = llama_new_context_with_model(g_model, cparams);
    if (!g_ctx) {
      log_msg("ERROR: failed to create context");
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
// enabled=false（Apply 与 yaml 热路径）及路径变更重载（load_model_async）
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
    s_cache_valid = false;  // 模型已卸载, 评分结果缓存一并作废
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

// ============================================================
// CE helper: -log(softmax(x)[target])
// ============================================================
static double cross_entropy(float *logits, int vs, int target_id) {
  float m = -1e30f;
  for (int k = 0; k < vs; k++)
    if (logits[k] > m)
      m = logits[k];
  double se = 0;
  for (int k = 0; k < vs; k++)
    se += exp((double)(logits[k] - m));
  return -((double)(logits[target_id] - m) - log(se));
}

// Normalizer (max, logsumexp) for a logits vector, computed once and reused
// across multiple CE targets. Step 1 scores all candidates against the same
// ctx logits, so the O(vocab) scan was repeated per candidate (5x waste,
// ~5ms of the CE1 stage); with a shared normalizer it becomes O(vocab) once
// + O(1) per target.
static void logits_normalizer(float *logits, int vs, float &m, double &lse) {
  m = -1e30f;
  for (int k = 0; k < vs; k++)
    if (logits[k] > m)
      m = logits[k];
  double se = 0;
  for (int k = 0; k < vs; k++)
    se += exp((double)(logits[k] - m));
  lse = log(se);
}
static double ce_target(float *logits, int target_id, float m, double lse) {
  return -((double)(logits[target_id] - m) - lse);
}

// ============================================================
// core scoring:
//   Step 1: decode ctx -> save logits -> CE of cand[0] from it
//   Step 2: KV copy ctx -> M seqs, parallel decode cand[0] -> CE of cand[1]
//   Step 3: same seqs continue decode -> CE of cand[2]
// ============================================================
static void score_batch(const std::vector<llama_token> &ctx_ids,
                        const std::vector<std::vector<llama_token>> &cands,
                        std::vector<double> &scores_out) {
  scores_out.assign(cands.size(), -1e10);
  int n_cands = (int)cands.size();
  if (n_cands == 0)
    return;

  auto t0 = std::chrono::high_resolution_clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  auto t1 = std::chrono::high_resolution_clock::now();
  double wait_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();


  int ctx_len = (int)ctx_ids.size();
  int vs = llama_n_vocab(g_vocab);

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
  std::vector<int> cand_to_seq(n_cands, -1);

  // Step 1: decode ctx on seq 0, save logits.
  // Pre-decode hit: ctx tokens match the async prepare() result and seq0
  // was not overwritten since (generation match) -> reuse saved logits,
  // skip the decode entirely. The prep state is not consumed: the same
  // ctx keeps hitting until seq0 is covered by another decode.
  bool use_prep = g_prep_ready && ctx_ids == g_prep_ctx &&
                  g_seq0_gen == g_prep_gen;
  std::vector<float> ctx_logits;
  double ms1 = 0;

  if (!use_prep) {
    auto ts1_0 = std::chrono::high_resolution_clock::now();
    llama_memory_clear(llama_get_memory(g_ctx), false);
    llama_batch ctx_batch = llama_batch_init(ctx_len, 0, 1);
    for (int j = 0; j < ctx_len; j++) {
      ctx_batch.token[j] = ctx_ids[j];
      ctx_batch.pos[j] = j;
      ctx_batch.n_seq_id[j] = 1;
      ctx_batch.seq_id[j][0] = 0;
    }
    ctx_batch.logits[ctx_len - 1] = 1;
    ctx_batch.n_tokens = ctx_len;
    if (llama_decode(g_ctx, ctx_batch) == 0) {
      g_seq0_gen++;  // seq0 KV updated
      float *cl = llama_get_logits_ith(g_ctx, ctx_len - 1);
      if (cl)
        ctx_logits.assign(cl, cl + vs);
    }
    llama_batch_free(ctx_batch);
    if (ctx_logits.empty()) {
      log_msg("ERROR: ctx decode failed");
      return;
    }
    // self-refresh prep: seq0 just decoded (generation bumped), KV and
    // logits are consistent, so later scores for the same ctx (page turn,
    // candidate window rebuild) all hit instead of cascading misses
    g_prep_gen = g_seq0_gen;
    g_prep_ctx = ctx_ids;
    g_prep_logits = ctx_logits;
    g_prep_ready = true;
    auto ts1_1 = std::chrono::high_resolution_clock::now();
    ms1 = std::chrono::duration<double, std::milli>(ts1_1 - ts1_0).count();
  } else {
    ctx_logits = g_prep_logits;  // pre-decode hit: Step 1 skipped
  }

  // Step 1 CE: P(cand[0] | ctx) for all candidates
  // all candidates share the same ctx logits -> one normalizer scan
  auto ts_ce1_0 = std::chrono::high_resolution_clock::now();
  std::vector<double> ce_sum(n_cands, 0.0);
  float m0;
  double lse0;
  logits_normalizer(ctx_logits.data(), vs, m0, lse0);
  for (int i = 0; i < n_cands; i++) {
    ce_sum[i] = ce_target(ctx_logits.data(), cands[i][0], m0, lse0);
  }
  auto ts_ce1_1 = std::chrono::high_resolution_clock::now();
  double ms_ce1 =
      std::chrono::duration<double, std::milli>(ts_ce1_1 - ts_ce1_0).count();

  // Step 2: KV copy ctx -> worker seqs, decode cand[0], CE of cand[1]
  double ms2a = 0, ms2b = 0;
  if (M > 0) {
    auto ts2_0 = std::chrono::high_resolution_clock::now();
    for (int s = 0; s < M; s++) {
      llama_memory_seq_cp(llama_get_memory(g_ctx), 0, s + 1, 0, -1);
      cand_to_seq[idx2[s]] = s + 1;
    }
    auto ts2_kv = std::chrono::high_resolution_clock::now();

    llama_batch b2 = llama_batch_init(M, 0, M);
    for (int s = 0; s < M; s++) {
      int ci = idx2[s];
      b2.token[s] = cands[ci][0];
      b2.pos[s] = ctx_len;
      b2.n_seq_id[s] = 1;
      b2.seq_id[s][0] = s + 1;
      b2.logits[s] = 1;
    }
    b2.n_tokens = M;
    if (llama_decode(g_ctx, b2) == 0) {
      for (int s = 0; s < M; s++) {
        int ci = idx2[s];
        float *l = llama_get_logits_ith(g_ctx, s);
        if (l)
          ce_sum[ci] += cross_entropy(l, vs, cands[ci][1]);
        else
          ce_sum[ci] = -1e10;
      }
    } else {
      for (int ci : idx2)
        ce_sum[ci] = -1e10;
      log_msg("WARN: step2 decode failed");
    }
    llama_batch_free(b2);
    auto ts2_1 = std::chrono::high_resolution_clock::now();
    ms2a = std::chrono::duration<double, std::milli>(ts2_kv - ts2_0).count();
    ms2b = std::chrono::duration<double, std::milli>(ts2_1 - ts2_kv).count();
  }

  // Step 3: decode cand[1] on same seqs, CE of cand[2]
  double ms3 = 0;
  if (K > 0) {
    auto ts3_0 = std::chrono::high_resolution_clock::now();
    llama_batch b3 = llama_batch_init(K, 0, K);
    for (int s = 0; s < K; s++) {
      int ci = idx3[s];
      int seq_id = cand_to_seq[ci];
      b3.token[s] = cands[ci][1];
      b3.pos[s] = ctx_len + 1;
      b3.n_seq_id[s] = 1;
      b3.seq_id[s][0] = seq_id;
      b3.logits[s] = 1;
    }
    b3.n_tokens = K;
    if (llama_decode(g_ctx, b3) == 0) {
      for (int s = 0; s < K; s++) {
        int ci = idx3[s];
        float *l = llama_get_logits_ith(g_ctx, s);
        if (l)
          ce_sum[ci] += cross_entropy(l, vs, cands[ci][2]);
        else
          ce_sum[ci] = -1e10;
      }
    } else {
      for (int ci : idx3)
        ce_sum[ci] = -1e10;
      log_msg("WARN: step3 decode failed");
    }
    llama_batch_free(b3);
    auto ts3_1 = std::chrono::high_resolution_clock::now();
    ms3 = std::chrono::duration<double, std::milli>(ts3_1 - ts3_0).count();
  }

  // final scoring: CE sum + long-candidate tail extrapolation
  auto ts_sc_0 = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < n_cands; i++) {
    double score = ce_sum[i] > -1e9 ? -ce_sum[i] : -1e10;
    if (score > -1e9 && (int)cands[i].size() > 3) {
      // Only the first 3 token CEs are computed for 4+ token candidates.
      // Without compensation the truncation lets long words skip their
      // (negative) tail CEs, favoring them over 3-token words. Extrapolate
      // the missing tail CEs by the average CE - no extra decode needed.
      // lambda tuned on corpus (eval_long_cand, 187 long-cand samples,
      // 7-point scan 0.3-0.7): real tail CE / head CE measured at
      // mean 0.58 (len=4) / 0.62 (len=5+), so 0.6 sits on the plateau
      // (0.5-0.7 all ~94% first-choice agreement) with balanced
      // direction (up 5 / down 5 at 0.6 vs 6/4 at 0.5).
      double avg_ce = ce_sum[i] / 3.0;
      score = -ce_sum[i] - avg_ce * ((int)cands[i].size() - 3) * 0.6;
    }
    scores_out[i] = score;
  }

  auto t2 = std::chrono::high_resolution_clock::now();
  double total_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
  double ms_score =
      std::chrono::duration<double, std::milli>(t2 - ts_sc_0).count();
  // one score line per inference (prep hit check)
  // timing: wait=S1(lock) S1=ctx decode(0 on prep hit) CE1=P(cand0|ctx)
  //         KV=KV copy S2=decode cand0 S3=decode cand1 score=sum+extrap
  log_msg("score: wait=%.0fms S1=%.0fms CE1=%.0fms KV=%.0fms S2=%.0fms "
          "S3=%.0fms score=%.0fms total=%.0fms prep=%d ctx_tok=%d cand=%d",
          wait_ms, ms1, ms_ce1, ms2a, ms2b, ms3, ms_score, total_ms,
          use_prep ? 1 : 0, ctx_len, n_cands);
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
  if (!lock.owns_lock())
    return;

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
  int vs = llama_n_vocab(g_vocab);
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
    log_msg("prepare: ERROR ctx decode failed");
    return;  // score falls back to the full pass
  }
  float *cl = llama_get_logits_ith(g_ctx, ctx_len - 1);
  if (!cl) {
    llama_batch_free(ctx_batch);
    log_msg("prepare: ERROR no logits");
    return;
  }
  g_prep_logits.assign(cl, cl + vs);
  llama_batch_free(ctx_batch);

  g_seq0_gen++;  // seq0 KV updated (this pass)
  g_prep_gen = g_seq0_gen;
  g_prep_ctx = ctx_ids;
  g_prep_ready = true;
  auto t1 = std::chrono::high_resolution_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  log_msg("prepare: seq=%d ctx_tok=%d %.0fms", seq, ctx_len, ms);
}

// ============================================================
// LlmRerankTranslation
// ============================================================

LlmRerankTranslation::LlmRerankTranslation(an<Translation> translation,
                                           const std::string &input,
                                           const std::string &ctx,
                                           const std::string &src)
    : translation_(translation), input_(input), ctx_(ctx), src_(src) {
  Collect();
}

bool LlmRerankTranslation::Next() {
  if (index_ >= candidates_.size())
    return false;
  ++index_;
  return index_ < candidates_.size();
}

an<Candidate> LlmRerankTranslation::Peek() {
  if (index_ >= candidates_.size())
    return nullptr;
  return candidates_[index_];
}

void LlmRerankTranslation::Collect() {
  // collect up to max_candidates for scoring, keep the rest in original order
  const size_t kMaxRerank = (size_t)(g_max_candidates > 0 ? g_max_candidates : 5);
  std::vector<an<Candidate>> tail;

  size_t n = 0;
  while (!translation_->exhausted() && n < kMaxRerank) {
    auto cand = translation_->Peek();
    if (cand) {
      candidates_.push_back(cand);
      n++;
    }
    translation_->Next();
  }
  while (!translation_->exhausted()) {
    auto cand = translation_->Peek();
    if (cand)
      tail.push_back(cand);
    translation_->Next();
  }

  if (candidates_.size() >= 2 && g_loaded.load()) {
    // rerank with LLM (ctx_ computed by LlmFilter::Apply: TSF context
    // text, or commit history fallback when TSF is unavailable)
    std::string ctx = normalize_ctx(ctx_);

    // reset generation: edit keys / window switch invalidate cached results
    // (插件版由 processor 在编辑键时清缓存; 源码版无 lua 层, 用 reset 代次)
    int gen = 0;
    if (const RimeApi *api = rime_get_api())
      if (api->context_reset_generation)
        gen = api->context_reset_generation();
    if (gen != s_cache_gen) {
      s_cache_valid = false;
      s_cache_gen = gen;
    }

    bool did_score = false;
    double ev_ms = 0;
    std::vector<int> order;                     // candidate indices, score desc
    std::vector<double> score_of(candidates_.size(), 0.0);  // 词频融合重放用
    std::vector<char> has_score(candidates_.size(), 0);

    if (s_cache_valid && ctx == s_cache_ctx && input_ == s_cache_input &&
        !s_cache_ranked.empty()) {
      // cache hit: 同一 (ctx, input) 的评分结果复用 — 翻页/候选窗重建
      // 不再跑 S2/S3 (~36ms/次)。缓存存评分顺序+原始分, 词频融合与徽章按
      // 当前配置重放; 候选集变化时新候选未命中 → 落到尾部 (与插件版一致)。
      for (size_t k = 0; k < s_cache_ranked.size(); k++)
        for (size_t i = 0; i < candidates_.size(); i++)
          if (candidates_[i]->text() == s_cache_ranked[k]) {
            order.push_back((int)i);
            if (k < s_cache_scores.size()) {
              score_of[i] = s_cache_scores[k];
              has_score[i] = s_cache_scores[k] > -1e9;
            }
            break;
          }
    } else {
      if (ctx != ctx_)  // newline/whitespace differences worth showing
        log_msg("ctx raw: [%s]", escape_ctx(ctx_).c_str());
      log_msg("ctx: [%s]", escape_ctx(ctx).c_str());
      std::vector<llama_token> ctx_ids = tokenize(ctx.c_str());
      if ((int)ctx_ids.size() >= g_min_tokens) {
        if ((int)ctx_ids.size() > g_max_ctx_tokens)
          ctx_ids.erase(ctx_ids.begin(), ctx_ids.end() - g_max_ctx_tokens);

        std::vector<std::vector<llama_token>> cand_ids;
        for (auto &c : candidates_) {
          auto ids = tokenize(c->text().c_str());
          if (ids.empty())
            ids.push_back(0);
          cand_ids.push_back(ids);
        }

        std::vector<double> scores;
        auto ev_t0 = std::chrono::high_resolution_clock::now();
        score_batch(ctx_ids, cand_ids, scores);
        auto ev_t1 = std::chrono::high_resolution_clock::now();
        ev_ms =
            std::chrono::duration<double, std::milli>(ev_t1 - ev_t0).count();

        if (scores.size() == candidates_.size()) {
          for (size_t i = 0; i < scores.size(); i++) {
            score_of[i] = scores[i];
            has_score[i] = scores[i] > -1e9;
          }
          std::vector<int> ord(scores.size());
          for (size_t i = 0; i < ord.size(); i++)
            ord[i] = (int)i;
          std::sort(ord.begin(), ord.end(),
                    [&](int a, int b) { return scores[a] > scores[b]; });
          order = std::move(ord);
          // store cache: 评分顺序+原始分 (候选文本), 命中时按文本重放
          s_cache_valid = true;
          s_cache_ctx = ctx;
          s_cache_input = input_;
          s_cache_ranked.clear();
          s_cache_scores.clear();
          for (int i : order) {
            s_cache_ranked.push_back(candidates_[i]->text());
            s_cache_scores.push_back(scores[i]);
          }
          did_score = true;
        }
      }
    }

    // 用户词频融合 (freq_weight, 2026-08-19; 2026-08-21 词频改 Rime 时间衰减):
    // total = (1-w)·LLM_minmax + w·eff/(eff+k)。eff = librime algo::formula_d
    // 指数衰减计数 (τ=200 tick, tick=每词提交+1, 引擎调频同源) — 近期常打
    // 的词权重高, 久未使用的自动消退。实证 (17258 真实候选窗, 6000 抽样,
    // 事前计数口径): 纯 LLM 97.08%, 衰减融合约 +0.4pp; 融合的意义在个性化
    // 高频词 (纯 LLM 排错事件 87% 选中词词频 ≥2)。
    // 应用于评分/缓存顺序之上、expected_length 之前 (与插件版一致);
    // 缓存只存分数序, 融合每次按当前衰减重放。稳定排序保同分原序 (CE 序)。
    if (g_freq_weight > 0 && order.size() > 1) {
      user_freq_ensure_loaded();
      double lo = 1e300, hi = -1e300;
      for (int i : order)
        if (has_score[i]) {
          if (score_of[i] < lo)
            lo = score_of[i];
          if (score_of[i] > hi)
            hi = score_of[i];
        }
      if (hi - lo > 1e-9) {
        double span = hi - lo;
        std::vector<std::pair<double, int>> fused;  // (total, idx)
        fused.reserve(order.size());
        for (int i : order) {
          // 失败哨兵/缺分 → s_l=0 (排尾部, 词频仍可救)
          double sl = has_score[i] ? (score_of[i] - lo) / span : 0.0;
          double n = user_freq_eff(candidates_[i]->text());
          fused.emplace_back(
              (1.0 - g_freq_weight) * sl +
                  g_freq_weight * (n / (n + (double)g_freq_k)),
              i);
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
    }

    // expected-length weighting (expected_length_weight, 2026-08-27 两版统一,
    // 移植自插件版 lua): 两码一字方案 L 码对应 floor(L/2) 字 — 词长等于期望
    // 词长的候选加 weight·span (span = 本次有效分数跨度, 保留分差明显时的
    // 语义排序); 失败哨兵/缺分候选不得奖励、不参与 min/max, 任一候选分数非
    // 有限则整体跳过 (与插件版一致)。稳定排序: 加权分降序 → 匹配词长优先,
    // 全同分保持原序 (词频融合序)。
    if (g_expected_length_weight > 0 && order.size() > 1) {
      int expected_len = (int)(input_.length() / 2);
      double lo = 1e300, hi = -1e300;
      bool all_finite = true;
      for (int i : order) {
        if (!std::isfinite(score_of[i])) { all_finite = false; break; }
        if (has_score[i]) {
          if (score_of[i] < lo) lo = score_of[i];
          if (score_of[i] > hi) hi = score_of[i];
        }
      }
      double span = hi - lo;
      if (all_finite && expected_len >= 1 && span > 1e-9) {
        struct ELItem { double score; bool match; int idx; };
        std::vector<ELItem> items;
        items.reserve(order.size());
        for (int i : order) {
          bool match =
              has_score[i] && utf8_len(candidates_[i]->text()) == expected_len;
          double s =
              score_of[i] + (match ? g_expected_length_weight * span : 0.0);
          items.push_back({s, match, i});
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const ELItem &a, const ELItem &b) {
                           if (a.score != b.score) return a.score > b.score;
                           return (int)a.match > (int)b.match;
                         });
        order.clear();
        for (auto &it : items) order.push_back(it.idx);
      }
    }

    if (!order.empty()) {
      // 应用顺序 (缓存命中与真实评分共用): 未匹配候选(新候选)落尾部
      std::string before;  // 原始候选序 — 必须在重排前捕获 (原先构建于
      // 重排后, 恒等于 after, 日志失去对比意义, 2026-08-21 修复)
      for (auto &c : candidates_) {
        if (!before.empty())
          before += ",";
        before += c->text();
      }
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
      // AI 首选徽章: 重排后首候选 comment 追加来源标记 (与已有 comment 合并,
      // ShadowCandidate 包装避免污染原候选; weasel 端识别 "AI·" 用强调色渲染)
      if (!candidates_.empty()) {
        std::string tag = (src_ == "tsf") ? "AI·TSF" : "AI·历史";
        auto &c0 = candidates_[0];
        std::string merged =
            c0->comment().empty() ? tag : c0->comment() + " " + tag;
        candidates_[0] =
            New<ShadowCandidate>(c0, c0->type(), string(), merged, false);
      }
      // 事件日志仅在真实推理时写 (缓存命中不重复推理, 也省日志 IO)
      if (did_score) {
        std::string after;
        for (auto &c : candidates_) {
          if (!after.empty())
            after += ",";
          after += c->text();
        }
        event_log(sanitize_field(input_), sanitize_field(before), ctx,
                  sanitize_field(after), ev_ms, src_);
      }
    }
  }

  for (auto &c : tail)
    candidates_.push_back(c);
}

// ============================================================
// LlmFilter
// ============================================================

static void on_context_changed(const char *text);  // defined below

LlmFilter::LlmFilter(const Ticket &ticket) : Filter(ticket) {
  // read LLM config from the scheme's llm_rerank section.
  // The filter's position in the filter pipeline is scheme-defined, so the
  // config lives in the scheme: the LLM-enabled scheme is maintained in the
  // rime-llm-ime project, while the published generic scheme stays clean.
  if (Config *config = engine_->schema()->config()) {
    // schema llm_rerank 节快照（部署期固定）；三级合并与热重载见
    // LlmParamSet 注释（schema > 全局 llm_rerank.yaml > 内置默认）
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
    int v = 0;
    if (config->GetInt("llm_rerank/min_code_len", &v)) { p.has_min_code_len = true; p.min_code_len = v; }
    if (config->GetInt("llm_rerank/max_code_len", &v)) { p.has_max_code_len = true; p.max_code_len = v; }
    double dw = 0.0;
    if (config->GetDouble("llm_rerank/expected_length_weight", &dw) && dw >= 0) { p.has_elw = true; p.elw = dw; }
    if (config->GetDouble("llm_rerank/freq_weight", &dw) && dw >= 0) { p.has_freq_weight = true; p.freq_weight = dw; }
    if (config->GetInt("llm_rerank/freq_k", &v) && v >= 1) { p.has_freq_k = true; p.freq_k = v; }
    if (config->GetInt("llm_rerank/min_tokens", &v)) { p.has_min_tokens = true; p.min_tokens = v; }
    if (config->GetInt("llm_rerank/max_tokens", &v)) { p.has_max_tokens = true; p.max_tokens = v; }
    if (config->GetInt("llm_rerank/max_candidates", &v)) { p.has_max_cand = true; p.max_cand = v; }
    if (config->GetInt("llm_rerank/cpu_cores", &v)) { p.has_cpu_cores = true; p.cpu_cores = v; }
    llm_load_global_params();
    g_yaml_stamp = llm_yaml_stamp();
    llm_apply_params();
    log_msg("config: enabled=%d min_code_len=%d max_code_len=%d "
            "expected_length_weight=%.2f freq_weight=%.2f freq_k=%d "
            "min_tokens=%d "
            "max_tokens=%d max_candidates=%d cpu_cores=%d "
            "model=%s",
            g_enabled ? 1 : 0, g_min_code_len, g_max_code_len,
            g_expected_length_weight, g_freq_weight, g_freq_k,
            g_min_tokens,
            g_max_ctx_tokens, g_max_candidates, g_n_threads,
            g_model_path.c_str());
  }
  // hook engine commit sink: pre-decode the upcoming context after commit
  commit_conn_ = engine_->sink().connect(
      [this](const std::string &text) { OnCommit(text); });
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

LlmFilter::~LlmFilter() { commit_conn_.disconnect(); }

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
  // 用户词频累计 (freq_weight 融合; 仅含中文的词, 每 20 词落盘)
  user_freq_bump(commit_text);
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
  // commit-history fallback string, computed on the engine thread
  std::string fallback;
  if (!tsf_valid)
    fallback = g_fallback_buffer;

  std::thread([this, tsf_valid, fallback]() {
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
      g_limited_gen_seen = gen;
      g_ctx_limited = false;  // 编辑键/切窗 → 新窗口重新评估
    }
  }
  if (g_ctx_limited) {
    std::string hist = CommitHistoryText();
    if (!hist.empty())
      return {hist, "rime"};
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

an<Translation> LlmFilter::Apply(an<Translation> translation,
                                 CandidateList *candidates) {
  if (!translation)
    return translation;

  llm_reload_global_if_changed();  // llm_rerank.yaml 热重载（GUI 保存即生效）

  if (!g_enabled)
    return translation;  // enabled=false -> pass-through

  size_t code_len = engine_->context() ? engine_->context()->input().size() : 0;
  if ((int)code_len < g_min_code_len ||
      (g_max_code_len > 0 && (int)code_len > g_max_code_len))
    return translation;  // code length outside [min_code_len, max_code_len]

  // context source: TSF caret text, or commit history fallback
  auto [ctx, src] = GetContextTextPair();
  if (ctx.empty()) {
    // diag: ESC-clear then next word no rerank - print ctx state
    log_msg("Apply: ctx empty (src=%s input=[%s] len=%d) -> no rerank",
            src.c_str(), engine_->context()->input().c_str(),
            (int)engine_->context()->input().size());
    return translation;  // no context, keep original order
  }

  if (!g_loaded.load())
    return translation;  // model still loading, keep original order

  return New<LlmRerankTranslation>(translation, engine_->context()->input(),
                                   ctx, src);
}

}  // namespace rime
