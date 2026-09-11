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
#include <condition_variable>
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
//   min_code_len: input code length below this -> no rerank
//   max_code_len: input code length above this -> no rerank (0 = unlimited);
//                 [min, max] = rerank trigger window
//   expected_length_weight: >0 = bonus candidates whose word length equals
//                 floor(code_len/2) (两码一字), weighted by current score span
//   freq_beta: 用户词频对数融合 fused = score + β·log(1+eff)（score=原始
//                 LLM 分 −CE；对数域加法=词频无上限可翻盘），eff=Rime
//                 formula_d 时间衰减计数（tick 每词提交+1，与引擎调频同源；
//                 user_freq.tsv 持久化）。β=1.5 标定自本机打字真实窗回放
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
static double g_expected_length_weight = 0.2;  // 预期词长加权 (冷启动标定 2026-09-03; 两版统一)
// 用户词频对数融合 (2026-09-02): fused = score + β·log(1+eff), 默认 β=1.5
// (本机打字真实窗回放标定: β∈[1,2] 平台, 大 β 为标签偏好假象; 见 Collect 段)
static double g_freq_beta = 1.5;
// debug_fusion 诊断 (2026-09-03 插件版先行、本文件移植): true = 逐块记录
// 评分→词频融合→词长加成全过程写 rime_llm_debug.txt (与插件版同名同格式;
// 差异: commit 行无码字段 — OnCommit 只收到上屏文本; reset 行按代次触发,
// 无法区分具体编辑键)。默认 false, 关闭时仅一次 bool 读零开销。
static bool g_debug_fusion = false;
// COM 文档模型光标上文旁路开关 (2026-09-10 WPS 实验移植, 默认开; 定义与
// 模块见下方 comctx 节 / 消费点 GetContextTextPair)
static bool g_com_ctx_enabled = true;
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
  bool has_elw = false;          double elw = 0.2;
  bool has_freq_beta = false;    double freq_beta = 1.5;
  bool has_min_tokens = false;   int min_tokens = 1;
  bool has_max_tokens = false;   int max_tokens = 10;
  bool has_max_cand = false;     int max_cand = 5;
  bool has_cpu_cores = false;    int cpu_cores = 4;
  bool has_debug_fusion = false; bool debug_fusion = false;
  bool has_com_ctx = false;      bool com_ctx = true;
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
      s.has_elw ? s.elw : (y.has_elw ? y.elw : 0.2);
  g_freq_beta = s.has_freq_beta ? s.freq_beta
                                : (y.has_freq_beta ? y.freq_beta : 1.5);
  g_debug_fusion = s.has_debug_fusion
                       ? s.debug_fusion
                       : (y.has_debug_fusion ? y.debug_fusion : false);
  g_com_ctx_enabled = s.has_com_ctx
                          ? s.com_ctx
                          : (y.has_com_ctx ? y.com_ctx : true);
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
    else if (key == "freq_beta") { p.has_freq_beta = true; p.freq_beta = atof(val.c_str()); }
    else if (key == "min_tokens") { p.has_min_tokens = true; p.min_tokens = atoi(val.c_str()); }
    else if (key == "max_tokens") { p.has_max_tokens = true; p.max_tokens = atoi(val.c_str()); }
    else if (key == "max_candidates") { p.has_max_cand = true; p.max_cand = atoi(val.c_str()); }
    else if (key == "cpu_cores") { p.has_cpu_cores = true; p.cpu_cores = atoi(val.c_str()); }
    else if (key == "debug_fusion") { p.has_debug_fusion = true; p.debug_fusion = (val == "true"); }
    else if (key == "com_context") { p.has_com_ctx = true; p.com_ctx = (val == "true"); }
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
  log_msg("llm_rerank.yaml reloaded: enabled=%d elw=%.2f freq_beta=%.2f "
          "debug_fusion=%d tok=%d/%d cand=%d cores=%d model=%s",
          g_enabled ? 1 : 0, g_expected_length_weight, g_freq_beta,
          g_debug_fusion ? 1 : 0, g_min_tokens, g_max_ctx_tokens,
          g_max_candidates, g_n_threads, g_model_path.c_str());
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
// 失败/过期/com_context 关闭 → 原 {hist, "rime"} (八轮方案零改动)。
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

static void ensure_started() {  // 首次粘性降级命中时调用 (引擎线程)
  bool expect = false;
  if (g_started.compare_exchange_strong(expect, true))
    std::thread(thread_proc).detach();
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
// 用户词频 (freq_beta 对数融合, 2026-09-02; 衰减 2026-08-21 起 Rime 时间衰减):
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
// 日志轮转（2026-09-04）：>5MB 改名 .old（单文件轮转）防无限增长；
// 改名失败（被占等罕见）则继续追加，不丢日志
static void rotate_if_large(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) &&
      ((((unsigned long long)fa.nFileSizeHigh << 32) |
        (unsigned long long)fa.nFileSizeLow) > 5ull * 1024 * 1024)) {
    std::string oldp = std::string(path) + ".old";
    DeleteFileA(oldp.c_str());
    MoveFileA(path, oldp.c_str());
  }
}

static FILE *open_log_file(const char *filename) {
  char path[MAX_PATH];
  const RimeApi *api = rime_get_api();
  if (api && api->get_user_data_dir) {
    const char *ud = api->get_user_data_dir();
    if (ud && *ud && strlen(ud) < MAX_PATH - 64) {
      snprintf(path, sizeof(path), "%s\\%s", ud, filename);
      rotate_if_large(path);
      return fopen(path, "a");
    }
  }
#ifdef _WIN32
  GetTempPathA(sizeof(path), path);
  strncat(path, filename, sizeof(path) - strlen(path) - 1);
  rotate_if_large(path);
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
  for (auto &kv : g_user_freq) {
    // 落盘修剪（2026-09-04）：衰减有效计数 <1e-3 的冷词条不再写（对
    // 融合分影响 <0.002 nats，远低于任何实际分差）——文件有界，旧冷
    // 词条自然淘汰（内存 map 会话期保留，下次 save 重新评估）
    if (kv.second.dee *
            exp((double)(kv.second.tick - g_user_tick) / kFreqTau) < 1e-3)
      continue;
    fprintf(f, "%s\t%lld\t%.3f\t%lld\n", kv.first.c_str(), kv.second.commits,
            kv.second.dee, kv.second.tick);
  }
  fclose(f);
}

static void user_freq_bump(const std::string &w) {
  // 仅计非 ASCII 上屏（>=0x80）：与 Rime userdb "任何词条提交推 tick"
  // 同源口径——全角标点也计，有意为之（2026-09-03 tick 含一字词定案的
  // 同源原则；严格汉字判定反而偏离引擎口径）
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

static void event_log(const std::string &input, const std::string &before,
                      const std::string &ctx, const std::string &after,
                      double elapsed_ms, const std::string &src) {
  FILE *f = open_log_file("rime_llm_filter_log.txt");
  if (!f)
    return;
  long n = ++g_event_cnt;
  fprintf(f, "%s|%ld|%s|%s|%s|%s|%.0fms|%s\n", now_hms().c_str(), n,
          input.c_str(), before.c_str(), ctx.c_str(), after.c_str(),
          elapsed_ms, src.c_str());
  fclose(f);
}

// ============================================================
// debug_fusion 诊断输出 (与插件版 llm_filter.lua/llm_processor.lua
// 同名同格式, 写 rime_llm_debug.txt): score 块 = 头行 + 负路径行 +
// 逐候选明细 + 名次变化 + span; commit 行 = 词频 bump 前后; reset 行 =
// 代次变化清缓存。块尾空行分隔。
// ============================================================
static void debug_fusion_write(const std::vector<std::string> &lines) {
  if (lines.empty())
    return;
  FILE *f = open_log_file("rime_llm_debug.txt");
  if (!f)
    return;
  for (auto &l : lines)
    fprintf(f, "%s\n", l.c_str());
  fprintf(f, "\n");
  fclose(f);
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
      // 失败自愈：prep 命中路径无 memory_clear，decode 失败多为 KV 耗尽
      // （见下方 worker 清理说明）——置无效强制下次 score 走全流程重建
      g_prep_ready = false;
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
      g_prep_ready = false;  // 同 Step2：失败自愈
    }
    llama_batch_free(b3);
    auto ts3_1 = std::chrono::high_resolution_clock::now();
    ms3 = std::chrono::duration<double, std::milli>(ts3_1 - ts3_0).count();
  }

  // worker 序列 KV 清理（2026-09-04 修"推理自停"根因，与插件版同步）：
  // seq_cp 是共享标记非复制，Step2/3 的 decode cell 评分后仍归属 seq 1..M；
  // prep 命中路径无 memory_clear，同 ctx 连续评分（退格重打/改码不换 ctx/
  // 失败重试）每轮净耗 M+K cell——n_ctx=128 约 14 轮耗尽 → decode 静默
  // 失败 → 全哨兵分且 prep_ready 仍真 → 卡死失败循环，仅 commit 触发
  // prepare 或重部署可解（非本机实测）。评分尾部立即释放 worker 归属：
  // 共享 cell 只去掉一个归属方，seq0 与 prep 状态不受影响，prep 命中
  // 照常，每次评分净耗归零。
  if (M > 0) {
    auto *mem = llama_get_memory(g_ctx);
    for (int s = 1; s <= M; s++)
      llama_memory_seq_rm(mem, s, 0, -1);
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
      // 插件版在 processor 编辑键回调记 reset 行; 源码版无 lua 层, 在下次
      // Collect 见到代次变化时补记 (首块 s_cache_gen=-1 不记, 免启动噪音)
      if (g_debug_fusion && s_cache_gen != -1)
        debug_fusion_write({now_hms() + "|reset|代次 " +
                            std::to_string(s_cache_gen) + "→" +
                            std::to_string(gen) +
                            " → 上文重置+缓存清空 (编辑键/切窗)"});
      s_cache_valid = false;
      s_cache_gen = gen;
    }

    bool did_score = false;
    double ev_ms = 0;
    std::vector<int> order;                     // candidate indices, score desc
    std::vector<double> score_of(candidates_.size(), 0.0);  // 词频融合重放用
    std::vector<char> has_score(candidates_.size(), 0);

    // debug_fusion 块缓冲: 头行先写, 明细在各段补, 末尾统一落盘
    std::vector<std::string> dbg;
    double dbg_span = 0;
    bool dbg_span_on = false;
    std::vector<double> dbg_eb(candidates_.size(), 0.0);
    const bool cache_hit = s_cache_valid && ctx == s_cache_ctx &&
                           input_ == s_cache_input && !s_cache_ranked.empty();
    if (g_debug_fusion) {
      user_freq_ensure_loaded();  // tick 读数须在懒加载后 (插件版同修)
      std::string tail =
          ctx.size() > 16 ? "…" + ctx.substr(ctx.size() - 15) : ctx;
      char head[512];
      snprintf(head, sizeof(head),
               "%s|score|input=%s|cache=%s|ctx=[%s]|src=%s|tick=%lld|ready=1"
               "|n=%d",
               now_hms().c_str(), input_.c_str(), cache_hit ? "HIT" : "MISS",
               tail.c_str(), src_.c_str(), (long long)g_user_tick,
               (int)candidates_.size());
      dbg.push_back(head);
    }

    if (cache_hit) {
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
        } else if (g_debug_fusion) {
          dbg.push_back("  评分异常: scores 数组长度不符 (推理失败)");
        }
      } else if (g_debug_fusion) {
        dbg.push_back("  score=nil (ctx tokens " +
                      std::to_string((int)ctx_ids.size()) +
                      " < min_tokens " + std::to_string(g_min_tokens) +
                      " — 上文空/过短跳过)");
      }
    }

    // CE 序名次 (诊断): 此刻 order = 评分序 (缓存命中 = 缓存的评分序)
    std::vector<int> ce_rank(candidates_.size(), 0);
    for (size_t k = 0; k < order.size(); k++)
      ce_rank[(size_t)order[k]] = (int)k + 1;

    // 对数词频融合 (freq_beta, 2026-09-02; 词频 Rime 时间衰减 2026-08-21):
    // fused = score + β·log(1+eff)。score = 原始 LLM 分 (−CE, 对数概率域),
    // eff = librime algo::formula_d 指数衰减计数 (τ=200 tick, tick=每词提交
    // +1, 引擎调频同源) — 近期常打的词权重高, 久未使用的自动消退。
    // 对数域加法 = 词频无上限: 强个人高频词可翻盘 CE 分差 (工业输入法通行
    // 结构, 替代旧凸组合 (1-w)·minmax+w·eff/(eff+k) 的结构性封顶)。
    // β=1.5 标定 (本机打字真实候选窗回放, 事前 eff 口径): 真实窗子集
    // β∈[1,2] 平台 +0.38pp, 大 β 为标签偏好假象; 改错数随 β 单调下降
    // (高频首选获自保护)。失败哨兵/缺分不参与融合, 保序排尾。
    // 应用于评分/缓存顺序之上、expected_length 之前 (与插件版一致);
    // 缓存只存分数序, 融合每次按当前衰减重放。稳定排序保同分原序 (CE 序)。
    std::vector<double> fused_of(candidates_.size(), -1e308);
    if (order.size() > 1) {
      user_freq_ensure_loaded();
      std::vector<std::pair<double, int>> fused;  // (total, idx)
      fused.reserve(order.size());
      for (int i : order) {
        double t = has_score[i]
                       ? score_of[i] +
                             g_freq_beta * std::log(1.0 + user_freq_eff(
                                                 candidates_[i]->text()))
                       : -1e308;  // 失败哨兵/缺分 → 排尾
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

    // expected-length weighting: 加成作用于融合分 (2026-09-03 统一结合公式,
    // 冷启动标定 elw=0.2): 两码一字方案 L 码对应 floor(L/2) 字 — 词长等于
    // 期望词长的候选在融合分上加 weight·span (span = 原始有效分跨度);
    // 修复旧实现按原始分重排导致 ELW 架空词频融合的组成缺陷。失败哨兵/
    // 缺分候选不得奖励; 任一候选无有效融合分则整体跳过 (与插件版一致)。
    // 稳定排序: 加权分降序 → 匹配词长优先, 全同分保持原序 (融合序)。
    if (g_expected_length_weight > 0 && order.size() > 1) {
      int expected_len = (int)(input_.length() / 2);
      double lo = 1e300, hi = -1e300;
      bool all_valid = true;
      for (int i : order) {
        if (!has_score[i] || fused_of[i] <= -1e307) {
          all_valid = false;
          break;
        }
        if (score_of[i] < lo) lo = score_of[i];
        if (score_of[i] > hi) hi = score_of[i];
      }
      double span = hi - lo;
      if (all_valid && expected_len >= 1 && span > 1e-9) {
        struct ELItem { double score; bool match; int idx; };
        std::vector<ELItem> items;
        items.reserve(order.size());
        for (int i : order) {
          bool match = utf8_len(candidates_[i]->text()) == expected_len;
          double bonus = match ? g_expected_length_weight * span : 0.0;
          if (g_debug_fusion && match)
            dbg_eb[i] = bonus;
          items.push_back({fused_of[i] + bonus, match, i});
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const ELItem &a, const ELItem &b) {
                           if (a.score != b.score) return a.score > b.score;
                           return (int)a.match > (int)b.match;
                         });
        order.clear();
        for (auto &it : items) order.push_back(it.idx);
        if (g_debug_fusion) {
          dbg_span = span;
          dbg_span_on = true;
        }
      } else if (g_debug_fusion && !all_valid) {
        dbg.push_back("  词长段跳过: 存在无效融合分候选");
      }
    }

    // 诊断明细: 逐候选 CE/eff/频+/长+/key 与名次变化 (最终序前 8 个)
    if (g_debug_fusion && !order.empty()) {
      std::vector<int> final_rank(candidates_.size(), 0);
      for (size_t k = 0; k < order.size(); k++)
        final_rank[(size_t)order[k]] = (int)k + 1;
      size_t shown = order.size() < 8 ? order.size() : 8;
      for (size_t k = 0; k < shown; k++) {
        int i = order[k];
        double eff = has_score[i] ? user_freq_eff(candidates_[i]->text()) : 0;
        double fb = has_score[i] ? g_freq_beta * std::log1p(eff) : 0;
        char line[256];
        snprintf(line, sizeof(line),
                 "  #%d %-6s CE=%s eff=%6.3f 频+%6.3f 长+%5.2f key=%s"
                 "  原次序%d→%d",
                 (int)k + 1, candidates_[i]->text().c_str(),
                 dbg_num8(has_score[i] ? score_of[i] : -1e308).c_str(), eff,
                 fb, dbg_eb[i], dbg_num(fused_of[i] + dbg_eb[i]).c_str(),
                 ce_rank[i], final_rank[i]);
        dbg.push_back(line);
      }
      std::string moved;
      for (size_t k = 0; k < order.size(); k++) {
        int i = order[k];
        if (ce_rank[i] && final_rank[i] && ce_rank[i] != final_rank[i]) {
          if (!moved.empty())
            moved += ", ";
          moved += candidates_[i]->text() + " " +
                   std::to_string(ce_rank[i]) + "→" +
                   std::to_string(final_rank[i]);
        }
      }
      dbg.push_back(moved.empty() ? "  名次变化: 无 (CE 序即融合序)"
                                  : "  名次变化: " + moved);
      if (dbg_span_on)
        dbg.push_back("  词长span=" + dbg_num(dbg_span));
    }
    if (g_debug_fusion)
      debug_fusion_write(dbg);

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
        // 来源标记: tsf = TSF 光标前文; com = COM 文档模型旁路 (WPS 类受限
        // 应用, 2026-09-10); 其余 = 历史上屏记录。weasel 端按 "AI·" 前缀
        // 统一强调渲染, 徽章文案区分来源。
        std::string tag = (src_ == "tsf")   ? "AI·TSF"
                          : (src_ == "com") ? "AI·COM"
                                            : "AI·历史";
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
    if (config->GetBool("llm_rerank/debug_fusion", &b)) {
      p.has_debug_fusion = true;
      p.debug_fusion = b;
    }
    if (config->GetBool("llm_rerank/com_context", &b)) {
      p.has_com_ctx = true;
      p.com_ctx = b;
    }
    int v = 0;
    if (config->GetInt("llm_rerank/min_code_len", &v)) { p.has_min_code_len = true; p.min_code_len = v; }
    if (config->GetInt("llm_rerank/max_code_len", &v)) { p.has_max_code_len = true; p.max_code_len = v; }
    double dw = 0.0;
    if (config->GetDouble("llm_rerank/expected_length_weight", &dw) && dw >= 0) { p.has_elw = true; p.elw = dw; }
    if (config->GetDouble("llm_rerank/freq_beta", &dw) && dw >= 0) { p.has_freq_beta = true; p.freq_beta = dw; }
    if (config->GetInt("llm_rerank/min_tokens", &v)) { p.has_min_tokens = true; p.min_tokens = v; }
    if (config->GetInt("llm_rerank/max_tokens", &v)) { p.has_max_tokens = true; p.max_tokens = v; }
    if (config->GetInt("llm_rerank/max_candidates", &v)) { p.has_max_cand = true; p.max_cand = v; }
    if (config->GetInt("llm_rerank/cpu_cores", &v)) { p.has_cpu_cores = true; p.cpu_cores = v; }
    llm_load_global_params();
    g_yaml_stamp = llm_yaml_stamp();
    llm_apply_params();
    log_msg("config: enabled=%d min_code_len=%d max_code_len=%d "
            "expected_length_weight=%.2f freq_beta=%.2f debug_fusion=%d "
            "min_tokens=%d "
            "max_tokens=%d max_candidates=%d cpu_cores=%d "
            "model=%s",
            g_enabled ? 1 : 0, g_min_code_len, g_max_code_len,
            g_expected_length_weight, g_freq_beta, g_debug_fusion ? 1 : 0,
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
  // 用户词频累计 (freq_beta 对数融合; 仅含中文的词, 每 20 词落盘)。
  // debug_fusion commit 行: eff/tick/脏计数 bump 前后 (码字段插件版有、
  // 源码版无 — OnCommit 只收到上屏文本, 拿不到已提交的编码)
  if (g_debug_fusion) {
    user_freq_ensure_loaded();  // before 读数须在懒加载后 (插件版同修)
    bool has_cjk = false;
    for (unsigned char ch : commit_text)
      if (ch >= 0x80) {
        has_cjk = true;
        break;
      }
    double eff_before = 0;
    long long tick_before = g_user_tick;
    int dirty_before = g_user_freq_dirty;
    if (has_cjk)
      eff_before = user_freq_eff(commit_text);
    user_freq_bump(commit_text);
    if (has_cjk) {
      char line[256];
      snprintf(line, sizeof(line),
               "%s|commit|词=%s|eff %.3f→%.3f|tick %lld→%lld|flush脏%d/20",
               now_hms().c_str(), commit_text.c_str(), eff_before,
               user_freq_eff(commit_text), tick_before, (long long)g_user_tick,
               dirty_before);
      debug_fusion_write({line});
    }
  } else {
    user_freq_bump(commit_text);
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

an<Translation> LlmFilter::Apply(an<Translation> translation,
                                 CandidateList *candidates) {
  if (!translation)
    return translation;

  llm_reload_global_if_changed();  // llm_rerank.yaml 热重载（GUI 保存即生效）

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
