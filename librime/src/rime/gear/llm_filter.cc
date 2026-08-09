//
// llm_filter.cc - LLM candidate rerank filter
//
// Context text (rime_api get_context_text, collected by TSF frontend)
// + llama.cpp inference to rerank candidates.
// Core algorithm ported from llm_rerank project (rime_llm.cpp):
//   ctx decode once -> KV copy -> parallel candidate decode -> CE score
//
#include <rime/gear/llm_filter.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
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
//   backend: off | cpu | gpu — off disables rerank entirely (pass-through)
//   min_code_len: input code length below this -> no rerank
static std::string g_model_path = "d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf";
static std::string g_backend = "off";
static int g_min_code_len = 4;
static int g_min_tokens = 1;
static int g_max_ctx_tokens = 10;  // tok=10: 93.4% acc, 10->17 gains only +1.1pp
static int g_n_threads = 6;        // default; user may set via bench_threads

static void log_msg(const char *fmt, ...);  // defined below (fwd decl)
static std::vector<llama_token> tokenize(const char *text);  // fwd decl
static double cross_entropy(float *logits, int vs, int target_id);  // fwd decl

// ── load-adaptive threads (simplified two-tier) ─────────────────────
// llama_set_n_threads() takes effect on the next decode with no context
// rebuild, so we can adapt at runtime:
//   high tier: g_max_threads - decided ONCE at startup by a quick scan
//     (reuses the loaded context, sweeps {4,6,..,16} via set_n_threads,
//     ~3s in the model-load thread). Picks the SMALLEST thread count
//     whose throughput reaches 95% of the measured optimum, so the
//     common case uses fewer threads for nearly the same speed.
//   low tier: 4 - fixed floor. Heavy system load (busy>85%) switches
//     straight to 4; back to the high tier when busy<60%.
//   sampler: background thread, 2.5s cadence, GetSystemTimes delta.
//   10s change throttle prevents thrashing.
static bool      g_auto_adapt = true;
static double    g_cpu_busy = 0.0;
static int       g_cur_threads = 0;
static int       g_max_threads = 0;  // 95% tier, set by thread_scan()
static long long g_last_adapt_ms = 0;

static void sampler_loop() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    FILETIME idle0, kern0, user0, idle1, kern1, user1;
    if (!GetSystemTimes(&idle0, &kern0, &user0))
      continue;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!GetSystemTimes(&idle1, &kern1, &user1))
      continue;
    auto sub = [](ULONGLONG a, ULONGLONG b) { return a > b ? a - b : 0ULL; };
    ULONGLONG idl = sub((((ULONGLONG)idle1.dwHighDateTime) << 32) | idle1.dwLowDateTime,
                        (((ULONGLONG)idle0.dwHighDateTime) << 32) | idle0.dwLowDateTime);
    ULONGLONG ker = sub((((ULONGLONG)kern1.dwHighDateTime) << 32) | kern1.dwLowDateTime,
                        (((ULONGLONG)kern0.dwHighDateTime) << 32) | kern0.dwLowDateTime);
    ULONGLONG usr = sub((((ULONGLONG)user1.dwHighDateTime) << 32) | user1.dwLowDateTime,
                        (((ULONGLONG)user0.dwHighDateTime) << 32) | user0.dwLowDateTime);
    ULONGLONG total = ker + usr;
    if (total > 0)
      g_cpu_busy = (double)(total - idl) / (double)total;
  }
}

// Score-batch-shaped micro-benchmark (same as bench_threads.exe).
// S1 (ctx decode) is pre-computed by prepare() in production, so it is
// NOT timed: only S2+S3 (the real per-keystroke cost) enters the scan.
static void scan_ctx_once(llama_context *ctx,
                          const std::vector<llama_token> &ctx_ids) {
  int ctx_len = (int)ctx_ids.size();
  llama_memory_clear(llama_get_memory(ctx), false);
  llama_batch b1 = llama_batch_init(ctx_len, 0, 1);
  for (int j = 0; j < ctx_len; j++) {
    b1.token[j] = ctx_ids[j];
    b1.pos[j] = j;
    b1.n_seq_id[j] = 1;
    b1.seq_id[j][0] = 0;
  }
  b1.logits[ctx_len - 1] = 1;
  b1.n_tokens = ctx_len;
  llama_decode(ctx, b1);
  llama_batch_free(b1);
}

static void scan_cand_once(llama_context *ctx,
                           const std::vector<std::vector<llama_token>> &cands,
                           int ctx_len, int vs) {
  int n = (int)cands.size();
  for (int s = 0; s < n; s++)
    llama_memory_seq_cp(llama_get_memory(ctx), 0, s + 1, 0, -1);
  llama_batch b2 = llama_batch_init(n, 0, n);
  for (int s = 0; s < n; s++) {
    b2.token[s] = cands[s][0];
    b2.pos[s] = ctx_len;
    b2.n_seq_id[s] = 1;
    b2.seq_id[s][0] = s + 1;
    b2.logits[s] = 1;
  }
  b2.n_tokens = n;
  if (llama_decode(ctx, b2) == 0) {
    for (int s = 0; s < n; s++) {
      float *l = llama_get_logits_ith(ctx, s);
      if (l)
        cross_entropy(l, vs, cands[s][1]);
    }
  }
  llama_batch_free(b2);
  // S3: 3-token candidates continue decoding
  std::vector<int> idx3;
  for (int s = 0; s < n; s++)
    if ((int)cands[s].size() >= 3)
      idx3.push_back(s);
  if (!idx3.empty()) {
    llama_batch b3 = llama_batch_init((int)idx3.size(), 0, (int)idx3.size());
    for (size_t k = 0; k < idx3.size(); k++) {
      int s = idx3[k];
      b3.token[k] = cands[s][1];
      b3.pos[k] = ctx_len + 1;
      b3.n_seq_id[k] = 1;
      b3.seq_id[k][0] = s + 1;
      b3.logits[k] = 1;
    }
    b3.n_tokens = (int)idx3.size();
    if (llama_decode(ctx, b3) == 0) {
      for (size_t k = 0; k < idx3.size(); k++) {
        float *l = llama_get_logits_ith(ctx, (int)k);
        if (l)
          cross_entropy(l, vs, cands[idx3[k]][2]);
      }
    }
    llama_batch_free(b3);
  }
}

// Run once right after model load (inside the load thread, before
// g_loaded). Sweeps {4,6,...,16} via set_n_threads on the live context
// and sets g_max_threads to the smallest count reaching 95% of the
// measured optimum. Falls back to the configured g_n_threads.
static void thread_scan(llama_context *ctx) {
  g_max_threads = g_n_threads;  // config fallback
  log_msg("scan: start ctx=%p", (void *)ctx);
  if (!ctx)
    return;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  int cores = (int)si.dwNumberOfProcessors;
  log_msg("scan: cores=%d", cores);
  std::vector<int> sweep;
  for (int t = 4; t <= cores && t <= 16; t += 2)
    sweep.push_back(t);
  if (sweep.empty())
    return;
  const char *ctx_text = "今天天气不错我们去公园散步聊聊天然后回家吃晚饭";
  std::vector<llama_token> ctx_ids = tokenize(ctx_text);
  if ((int)ctx_ids.size() > 10)
    ctx_ids.erase(ctx_ids.begin(), ctx_ids.end() - 10);
  // heaviest real typing case: 3 x 2-token + 2 x 3-token candidates
  // (S3 decode). Common words are single tokens; select from the pool
  // and verify counts (2/3) at runtime.
  const char *cand_pool[] = {
      "错事", "侧式", "测速", "仄声", "佚名", "怅惘", "缱绻", "龌龊",
      "邂逅", "蹉跎", "饕餮", "犄角", "旮旯", "囫囵", "氤氲", "黢黑",
      "计算机", "图书馆", "摄像头", "咖啡机", "高跟鞋", "潜台词", "老字号",
      "双刃剑", "里程碑", "橄榄枝", "绊脚石", "遮羞布", "紧箍咒", "试金石"};
  std::vector<std::vector<llama_token>> cands;
  int got2 = 0, got3 = 0;
  for (auto *w : cand_pool) {
    auto ids = tokenize(w);
    if (ids.size() == 2 && got2 < 3) {
      cands.push_back(ids);
      got2++;
    } else if (ids.size() == 3 && got3 < 2) {
      cands.push_back(ids);
      got3++;
    }
  }
  if (cands.size() < 5)
    return;  // pool failed; keep the config fallback
  int vs = llama_n_vocab(g_vocab);
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  double best_ms = 1e18;
  int best_t = sweep[0];
  std::vector<double> ms_of(sweep.size());
  int ctx_len = (int)ctx_ids.size();
  for (size_t i = 0; i < sweep.size(); i++) {
    llama_set_n_threads(ctx, sweep[i], sweep[i]);
    scan_ctx_once(ctx, ctx_ids);  // S1 not timed (prepare absorbs it)
    scan_cand_once(ctx, cands, ctx_len, vs);  // warmup (graph build)
    double best = 1e18;
    for (int k = 0; k < 3; k++) {
      LARGE_INTEGER t0, t1;
      scan_ctx_once(ctx, ctx_ids);  // S1: outside the timed window
      QueryPerformanceCounter(&t0);
      scan_cand_once(ctx, cands, ctx_len, vs);  // S2+S3: timed
      QueryPerformanceCounter(&t1);
      double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
      if (ms < best)
        best = ms;
    }
    ms_of[i] = best;
    if (best < best_ms) {
      best_ms = best;
      best_t = sweep[i];
    }
  }
  // smallest thread count reaching >= 90% of the optimum
  // (first satisfying sweep entry; must be <= the optimum)
  int max_t = sweep[0];
  for (size_t i = 0; i < sweep.size(); i++) {
    if (ms_of[i] <= best_ms / 0.90) {
      max_t = sweep[i];
      break;
    }
  }
  g_max_threads = max_t;
  llama_set_n_threads(ctx, g_max_threads, g_max_threads);
  log_msg("scan: opt thr=%d (%.0fms) 90%%tier thr=%d", best_t, best_ms,
          g_max_threads);
}

static void adapt_threads(llama_context *ctx) {
  if (!g_auto_adapt || !ctx)
    return;
  long long now = (long long)GetTickCount64();
  if (now - g_last_adapt_ms < 10000)
    return;  // throttle: at most one change per 10s
  double busy = g_cpu_busy;
  int high = g_max_threads > 0 ? g_max_threads : g_n_threads;
  int cur = g_cur_threads;
  int target = cur;
  if (busy > 0.85 && cur != 4)
    target = 4;                 // heavy system load: drop to the floor
  else if (busy < 0.60 && cur != high)
    target = high;              // system idle: back to the 95% tier
  if (target != cur) {
    llama_set_n_threads(ctx, target, target);
    g_cur_threads = target;
    g_last_adapt_ms = now;
    log_msg("adapt: cpu=%.0f%% threads %d->%d", busy * 100.0, cur, target);
  }
}
static int g_min_free_mem_mb = 2560;  // skip model load when free RAM below this
                                       // (0.8B Q4 needs ~2GB: freeze beats no rerank)
static int g_n_ctx = 128;          // KV: 11 seqs x (ctx 10 + cand 2) = 132, 64 overflows
static int g_n_seq_max = 12;       // template seq 0 + up to 11 worker seqs
static int g_max_candidates = 5;   // candidates participating in scoring

// ============================================================
// commit-history fallback state (engine thread only: OnCommit sink
// callback and Apply both run on the engine thread, no lock needed)
// ============================================================
static std::string g_fallback_buffer;  // session committed texts
static int g_fallback_gen_seen = 0;    // consumed reset generation

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

static void log_msg(const char *fmt, ...) {
  char buf[512];
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
  if (g_loaded.load() || g_loading.load())
    return;
  // low-memory guard: LLM needs ~2GB (model + KV + compute buffers); on
  // small machines loading can swap the system to a freeze — skip instead
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms) &&
      ms.ullAvailPhys < (DWORD64)g_min_free_mem_mb * 1024 * 1024) {
    log_msg("WARN: free RAM %llu MB < %d MB, LLM rerank disabled",
            (unsigned long long)(ms.ullAvailPhys / (1024 * 1024)),
            g_min_free_mem_mb);
    return;
  }
  g_loading.store(true);

  std::thread([]() {
#ifdef _WIN32
    // 加载期 (2GB 模型读取 + 反量化 + warmup 全线程 decode) 会吃满
    // CPU/IO, 期间新进程启动 (QQ 音乐等 CEF 应用) 会被拖到秒级挂起;
    // 降低本线程 (及 warmup 创建的 llama worker 线程) 优先级让位系统,
    // 加载完成后恢复
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
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

    if (g_auto_adapt)
      thread_scan(g_ctx);  // ~3s: decides the 95% high tier
    g_cur_threads = g_max_threads > 0 ? g_max_threads : g_n_threads;
    g_loaded.store(true);
    g_loading.store(false);
    if (g_auto_adapt)
      std::thread(sampler_loop).detach();
    log_msg("model ready (n_ctx=%d threads=%d auto_adapt=%d)",
            g_n_ctx, g_cur_threads, g_auto_adapt ? 1 : 0);
  }).detach();
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

  // load-adaptive thread count (cheap cached check, 10s throttle)
  adapt_threads(g_ctx);

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
      double ev_ms =
          std::chrono::duration<double, std::milli>(ev_t1 - ev_t0).count();

      if (scores.size() == candidates_.size()) {
        // snapshot original order for the event log
        std::string before;
        for (auto &c : candidates_) {
          if (!before.empty())
            before += ",";
          before += c->text();
        }
        std::vector<int> order(scores.size());
        for (size_t i = 0; i < order.size(); i++)
          order[i] = (int)i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return scores[a] > scores[b]; });
        std::vector<an<Candidate>> reranked;
        for (int i : order)
          reranked.push_back(candidates_[i]);
        candidates_ = reranked;
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
    string s;
    if (config->GetString("llm_rerank/backend", &s) && !s.empty())
      g_backend = s;
    if (config->GetString("llm_rerank/model_path", &s) && !s.empty())
      g_model_path = s;
    int v = 0;
    if (config->GetInt("llm_rerank/min_code_len", &v))
      g_min_code_len = v;
    if (config->GetInt("llm_rerank/min_tokens", &v))
      g_min_tokens = v;
    if (config->GetInt("llm_rerank/max_tokens", &v))
      g_max_ctx_tokens = v;
    if (config->GetInt("llm_rerank/max_candidates", &v))
      g_max_candidates = v;
    if (config->GetInt("llm_rerank/cpu_cores", &v))
      g_n_threads = v;
    bool b = true;
    if (config->GetBool("llm_rerank/auto_adapt", &b))
      g_auto_adapt = b;
    if (config->GetInt("llm_rerank/min_free_mem_mb", &v))
      g_min_free_mem_mb = v;
    // cap threads at hardware cores: low-core machines shouldn't
    // oversubscribe (slowdown + extra per-thread memory)
    unsigned hw = std::thread::hardware_concurrency();
    if (hw > 0 && (unsigned)g_n_threads > hw)
      g_n_threads = (int)hw;
    log_msg("config: backend=%s min_code_len=%d min_tokens=%d "
            "max_tokens=%d max_candidates=%d cpu_cores=%d "
            "min_free_mem_mb=%d model=%s",
            g_backend.c_str(), g_min_code_len, g_min_tokens,
            g_max_ctx_tokens, g_max_candidates, g_n_threads,
            g_min_free_mem_mb, g_model_path.c_str());
  }
  // hook engine commit sink: pre-decode the upcoming context after commit
  commit_conn_ = engine_->sink().connect(
      [this](const std::string &text) { OnCommit(text); });
  // register context-change callback: pre-decode on every TSF caret text
  // delivery (covers window switch / model-load cases with no commit)
  const RimeApi *api2 = rime_get_api();
  if (api2 && api2->set_context_changed_callback)
    api2->set_context_changed_callback(&on_context_changed);
  if (g_backend != "off")
    load_model_async();
  else
    log_msg("config: backend off, LLM rerank disabled");
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
  if (api && api->get_context_text) {
    const char *text = api->get_context_text();
    if (text && *text)
      return {text, "tsf"};  // TSF caret text available
    if (api->context_text_valid && api->context_text_valid()) {
      // TSF collection works but the caret is at an empty spot:
      // a legitimately empty context -> no rerank, no fallback
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

  if (g_backend == "off")
    return translation;  // backend off -> pass-through

  if (engine_->context() &&
      (int)engine_->context()->input().size() < g_min_code_len)
    return translation;  // input code length below min_code_len -> no rerank

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
