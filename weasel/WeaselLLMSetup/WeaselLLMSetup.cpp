// WeaselLLMSetup.cpp — LLM 重排设置（托盘菜单“LLM 重排设置”启动）
//
// 直接安装版（2026-08-27）：读写 %APPDATA%\Rime\llm_rerank.yaml（平面
// key: value）。llm_filter（librime）对该文件热重载——保存即生效，无需
// 重新部署。参数三级优先级：schema llm_rerank 节 > 本文件 > 内置默认。
// 2026-09-04 改版：模型下载移入安装包（装时可选、默认不下载），本界面
// 只管配置——去掉下载按钮/首次下载提示；模型路径改下拉框（扫描 Rime
// 用户目录与 %USERPROFILE%\gguf_models 的 .gguf + 浏览），新增模型状态
// 行（文件存在性/大小）、打开用户文件夹、debug_fusion 诊断开关。
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <deque>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")  // 悬停提示（tooltips_class32）
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")  // RegGetValue（WeaselRoot 定位部署器）
// 视觉样式（Common Controls v6）：否则按钮/勾选框呈 Win2000 经典浮雕样式
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- 控件 ID ----
#define IDC_ENABLED      1001
#define IDC_MODEL        1002
#define IDC_BROWSE       1003
#define IDC_OPENDIR      1004
#define IDC_MSTATUS      1005
#define IDC_MSTATUS_HINT 1006   // 模型状态第二行（灰色小字）
#define IDC_DOWNLOAD     1007   // 下载模型（2026-10-01 用户定案：从安装器移到配置 GUI）
// 数值参数（列1/列2 各自顺序）
#define IDC_MIN_CODE     1011
#define IDC_MAX_CODE     1012
#define IDC_MIN_TOK      1013
#define IDC_MAX_TOK      1014
#define IDC_CORES        1015
#define IDC_CODE_PAT     1016   // 编码匹配正则（2026-09-30 取代 min/max_code_len）
#define IDC_ELW          1021
#define IDC_FREQ_W       1022
#define IDC_DEBUG        1023
#define IDC_MAX_CAND     1024
// 方案接入（2026-09-29 GUI 化，语义同 installer\schema_add.ps1）
#define IDC_SCHEMA       1031
#define IDC_SCHEMAREM    1034
#define IDC_SCHEMSTAT    1035
#define IDC_SAVE         1101
#define IDC_CLOSE        1102
#define IDC_STATUS       1103

struct Params {
  bool enabled = false;
  // 编码匹配正则（2026-09-30 取代 min_code_len/max_code_len）：全串匹配，
  // 语义同 Rime speller/auto_select_pattern。默认恰 4 码。
  std::string code_pattern = ".{4}";
  // min_tokens 不再暴露给用户（2026-09-30 定案：最少上文 token 恒为 1，
  // 代码内默认值保留，yaml/schema 里的旧行读取时忽略、写入时不再产出）
  int max_tokens = 10, cpu_cores = 4;
  double elw = 0.2, freq_beta = 1.5;
  int max_candidates = 5;
  bool debug_fusion = false;
  std::wstring model_path;  // 显示值（空 = 默认路径）
};
static Params g_p;
// 方案下拉的"已加载项"序号 + 抑制标志（2026-10-01：切换方案时若有未保存改动，
// 选"否"要把下拉拨回这一项且不再触发重读）
static std::wstring g_loaded_name;   // 当前已读入界面的方案文件名（切换取消时按名拨回）
static bool g_sel_guard = false;
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_font_hint;  // 灰色小字（状态副行 / 底部动态注）
static HFONT g_font_help;  // 「?」徽标里的问号（小号加粗）
static int g_ui_height = 0; // make_ui() 算出的内容总高（设备像素；用于定窗口高）
// 悬停提示：g_tip = tooltips_class32 窗口；g_tips 存文案（deque：push_back
// 不搬移已有元素，c_str() 指针长期有效——vector 会因扩容失效）
static HWND g_tip = NULL;
static std::deque<std::wstring> g_tips;

// ---- 工具 ----
// Rime 用户文件夹（= 小狼毫右键"用户文件夹"；方案与模型都在这里）。
// 优先级：环境变量 RIME_LLM_USER_DIR（便携/测试显式指定）→ 注册表 RimeUserDir
//（便携模式/自定义位置的小狼毫写的就是它）→ %APPDATA%\Rime
//（2026-10-01：此前硬编码 APPDATA，便携部署下 GUI 显示的默认路径会与运行期不符）
static std::wstring rime_user_dir() {
  wchar_t env[MAX_PATH] = L"";
  if (GetEnvironmentVariableW(L"RIME_LLM_USER_DIR", env, MAX_PATH) && env[0] &&
      GetFileAttributesW(env) != INVALID_FILE_ATTRIBUTES)
    return std::wstring(env);
  HKEY k = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel", 0, KEY_READ, &k) ==
      ERROR_SUCCESS) {
    wchar_t v[MAX_PATH] = L"";
    DWORD cb = sizeof(v), type = 0;
    LONG r = RegQueryValueExW(k, L"RimeUserDir", NULL, &type, (LPBYTE)v, &cb);
    RegCloseKey(k);
    if (r == ERROR_SUCCESS && type == REG_SZ && v[0] &&
        GetFileAttributesW(v) != INVALID_FILE_ATTRIBUTES)
      return std::wstring(v);
  }
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH))
    return std::wstring();
  return std::wstring(dir) + L"\\Rime";
}

// 旧全局配置 %APPDATA%\Rime\llm_rerank.yaml（2026-09-30 起废弃：配置回归
// 方案 llm_rerank 节）。**只作一次性迁移**——方案节里没有 model_path 时，
// 用它把老用户已配好的模型路径带进界面，保存即写入方案节。
// （实现放在 trim/utf8_to_wide 之后：见下方 legacy_model_path）

static std::wstring default_model_path() {
  // 默认 = RIME 用户文件夹根（2026-08-31 用户澄清：指小狼毫右键的用户
  // 文件夹——方案配置所在处，模型直接放根、不套子文件夹；8-27 曾误用
  // %USERPROFILE%\gguf_models\。自定义位置在 GUI/schema 里显式填）
  // 2026-10-01：改用 rime_user_dir()（注册表 RimeUserDir 优先），与运行期
  // 默认（deployer().user_data_dir）在便携部署下保持一致
  std::wstring dir = rime_user_dir();
  if (!dir.empty()) return dir + L"\\Qwen3.5-0.8B-Q4_K_M.gguf";
  return L"Qwen3.5-0.8B-Q4_K_M.gguf";
}

static std::wstring shown_model() {
  return g_p.model_path.empty() ? default_model_path() : g_p.model_path;
}

// 绝对路径判定（③ 用）：X:\ / X:/ / \\server\share
static bool is_abs_path(const std::wstring& p) {
  if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
    return true;
  if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true;
  return false;
}

static std::string wide_to_utf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
  return s;
}
static std::wstring utf8_to_wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// YAML 标量转义（2026-09-30，code_pattern 引入）：
// 含特殊字符（{[}]:,#&*!|>%@`"' 空格 或反斜杠）时用**单引号**包起——
// 双引号标量里 \d 这类非法转义在 YAML 里会直接报错，单引号则原样保留正则。
static std::string yaml_scalar(const std::string& s) {
  if (!s.empty() && s.find_first_of("{}[]:,#&*!|>%@`\"' \\") == std::string::npos)
    return s;  // 安全裸标量
  std::string q = "'";
  for (char ch : s) {
    if (ch == '\'') q += "''";  // 单引号内的单引号写两遍
    else q += ch;
  }
  return q + "'";
}

// 旧全局配置的一次性迁移读取（%APPDATA%\Rime\llm_rerank.yaml，已废弃）。
// 只在该方案**没有 llm_rerank 节**时调用：把老用户配好的参数带进界面，
// 点『接入 LLM』即写进方案节——运行期（llm_filter）完全不读这个文件。
static bool load_legacy_params(Params* p) {
  std::wstring dir = rime_user_dir();
  if (dir.empty()) return false;
  FILE* f = NULL;
  _wfopen_s(&f, (dir + L"\\llm_rerank.yaml").c_str(), L"rb");
  if (!f) return false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string ln = trim(line);
    if (ln.empty() || ln[0] == '#') continue;
    size_t c = ln.find(':');
    if (c == std::string::npos) continue;
    std::string key = trim(ln.substr(0, c));
    std::string val = trim(ln.substr(c + 1));
    if (val.empty() || val[0] == '#') continue;
    if (val[0] == '"' || val[0] == '\'') {
      size_t e = val.find(val[0], 1);
      val = (e == std::string::npos) ? val.substr(1) : val.substr(1, e - 1);
    } else {
      size_t h = val.find('#');
      if (h != std::string::npos) val = trim(val.substr(0, h));
    }
    if (val.empty()) continue;
    if (key == "enabled") p->enabled = (val == "true");
    else if (key == "code_pattern") p->code_pattern = val;
    else if (key == "expected_length_weight") p->elw = atof(val.c_str());
    else if (key == "freq_beta") p->freq_beta = atof(val.c_str());
    else if (key == "max_tokens") p->max_tokens = atoi(val.c_str());
    else if (key == "max_candidates") p->max_candidates = atoi(val.c_str());
    else if (key == "cpu_cores") p->cpu_cores = atoi(val.c_str());
    else if (key == "debug_fusion") p->debug_fusion = (val == "true");
    else if (key == "model_path") {
      for (auto& ch : val)
        if (ch == '/') ch = '\\';
      p->model_path = utf8_to_wide(val);
    }
  }
  fclose(f);
  return true;
}

// ---- 方案内 llm_rerank: 配置节读写（2026-09-30 用户定案：两版统一回归
// Rime 原生做法——配置作为节写在方案 schema.yaml 里，取消源码版全局
// %APPDATA%\Rime\llm_rerank.yaml）----
// 节内键序与插件版 installer\install_plugin.ps1 的 Get-LlmCfgLines /
// Update-LlmSection **逐字一致**：enabled / code_pattern / max_tokens /
// max_candidates / cpu_cores / freq_beta / expected_length_weight /
// debug_fusion / model_path —— 两版方案配置节相同，仅组件行不同。
static const char* kCfgSection = "llm_rerank:";

// 节起始行下标（-1 = 无节）
static int llm_section_start(const std::vector<std::string>& lines) {
  for (size_t i = 0; i < lines.size(); ++i)
    if (!lines[i].compare(0, strlen(kCfgSection), kCfgSection)) return (int)i;
  return -1;
}

// 解析节内 key: value 到 g_p；返回是否有节。旧键（min/max_code_len、
// min_tokens、com_context）有意忽略——保存时自然消失。
static bool parse_llm_section(const std::vector<std::string>& lines) {
  int start = llm_section_start(lines);
  if (start < 0) return false;
  for (size_t i = start + 1; i < lines.size(); ++i) {
    const std::string& raw = lines[i];
    if (raw.empty()) continue;
    if (raw[0] != ' ' && raw[0] != '\t') break;  // 节结束（下一个顶层键）
    std::string ln = trim(raw);
    if (ln.empty() || ln[0] == '#') continue;
    size_t c = ln.find(':');
    if (c == std::string::npos) continue;
    std::string key = trim(ln.substr(0, c));
    std::string val = trim(ln.substr(c + 1));
    if (val.empty() || val[0] == '#') continue;   // 注释占位行（空 model_path）
    if (val[0] == '"' || val[0] == '\'') {        // 引号去壳（单/双都要）
      size_t e = val.find(val[0], 1);
      val = (e == std::string::npos) ? val.substr(1) : val.substr(1, e - 1);
    } else {
      size_t h = val.find('#');
      if (h != std::string::npos) val = trim(val.substr(0, h));
    }
    if (val.empty()) continue;
    if (key == "enabled") g_p.enabled = (val == "true");
    else if (key == "code_pattern") g_p.code_pattern = val;
    else if (key == "min_code_len" || key == "max_code_len" ||
             key == "min_tokens" || key == "com_context") { /* 旧键忽略 */ }
    else if (key == "expected_length_weight") g_p.elw = atof(val.c_str());
    else if (key == "freq_beta") g_p.freq_beta = atof(val.c_str());
    else if (key == "max_tokens") g_p.max_tokens = atoi(val.c_str());
    else if (key == "max_candidates") g_p.max_candidates = atoi(val.c_str());
    else if (key == "cpu_cores") g_p.cpu_cores = atoi(val.c_str());
    else if (key == "debug_fusion") g_p.debug_fusion = (val == "true");
    else if (key == "model_path") g_p.model_path = utf8_to_wide(val);
  }
  return true;
}

// 节内容（含 "llm_rerank:" 行本身；model_path 为空写注释占位，与插件版同款）
static std::vector<std::string> build_llm_section() {
  std::vector<std::string> sec;
  char buf[64];
  sec.push_back(kCfgSection);
  sec.push_back(g_p.enabled ? "  enabled: true" : "  enabled: false");
  sec.push_back("  code_pattern: " + yaml_scalar(g_p.code_pattern));
  sprintf_s(buf, "  max_tokens: %d", g_p.max_tokens); sec.push_back(buf);
  sprintf_s(buf, "  max_candidates: %d", g_p.max_candidates); sec.push_back(buf);
  sprintf_s(buf, "  cpu_cores: %d", g_p.cpu_cores); sec.push_back(buf);
  sprintf_s(buf, "  freq_beta: %.2f", g_p.freq_beta); sec.push_back(buf);
  sprintf_s(buf, "  expected_length_weight: %.2f", g_p.elw); sec.push_back(buf);
  sec.push_back(g_p.debug_fusion ? "  debug_fusion: true" : "  debug_fusion: false");
  if (!g_p.model_path.empty()) {
    std::string p = wide_to_utf8(g_p.model_path);
    bool quote = p.find(' ') != std::string::npos;
    for (auto& ch : p) if (ch == '\\') ch = '/';
    sec.push_back("  model_path: " + (quote ? ("\"" + p + "\"") : p));
  } else {
    sec.push_back("  # model_path: <绝对路径；默认 = " +
                  wide_to_utf8(rime_user_dir()) +
                  "\\Qwen3.5-0.8B-Q4_K_M.gguf>");
  }
  return sec;
}

// 原位重写节内容（节必须已存在——没节说明方案未接入 LLM）；返回是否成功
static bool update_llm_section(std::vector<std::string>* lines) {
  int start = llm_section_start(*lines);
  if (start < 0) return false;
  int end = start + 1;
  while (end < (int)lines->size() && !(*lines)[end].empty() &&
         ((*lines)[end][0] == ' ' || (*lines)[end][0] == '\t'))
    end++;
  std::vector<std::string> sec = build_llm_section();
  std::vector<std::string> out;
  out.insert(out.end(), lines->begin(), lines->begin() + start);
  out.insert(out.end(), sec.begin(), sec.end());
  out.insert(out.end(), lines->begin() + end, lines->end());
  *lines = out;
  return true;
}

static bool file_size(const std::wstring& p, unsigned long long* sz) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa))
    return false;
  *sz = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
  return true;
}

static void set_status(HWND ctrl, const wchar_t* fmt, ...) {
  wchar_t buf[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  SetWindowTextW(ctrl, buf);
}

// ---- 错误日志（2026-09-30 用户定案：界面不留日志框，**只在出错时**写文件）----
// 位置 = GUI 程序所在目录（安装目录）；该目录通常需要管理员权限，写不进时
// 回退到用户文件夹 %APPDATA%\Rime。正常流程只更新状态行，不产生任何文件。
static std::wstring error_log_path() {
  static std::wstring cached;
  if (!cached.empty()) return cached;
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(NULL, exe, MAX_PATH);
  std::wstring dir = exe;
  size_t p = dir.find_last_of(L'\\');
  if (p != std::wstring::npos) dir = dir.substr(0, p);
  std::wstring cand = dir + L"\\WeaselLLMSetup_error.log";
  FILE* f = NULL;
  _wfopen_s(&f, cand.c_str(), L"ab");   // 探测可写性（追加模式不留空文件）
  if (f) {
    fclose(f);
    cached = cand;
    return cached;
  }
  std::wstring userdir = rime_user_dir();
  cached = userdir.empty() ? cand : (userdir + L"\\WeaselLLMSetup_error.log");
  return cached;
}

static void log_error(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
  va_end(ap);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t head[128];
  swprintf_s(head, L"===== %04d-%02d-%02d %02d:%02d:%02d =====", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  std::string text = wide_to_utf8(std::wstring(head) + L"\n" + buf + L"\n");
  FILE* f = NULL;
  _wfopen_s(&f, error_log_path().c_str(), L"ab");
  if (!f) return;
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
}

// ---- 模型路径下拉框：扫描常见位置的 .gguf（Rime 用户目录 = 默认下载
// 落点；%USERPROFILE%\gguf_models = 插件版约定位置——从插件版迁移的
// 用户模型已在盘上，下拉选一下即可，不必重新下载）----
static void scan_models(HWND combo) {
  wchar_t cur[512];
  GetWindowTextW(combo, cur, 512);
  wchar_t base[MAX_PATH];
  std::wstring dirs[2];
  dirs[0] = rime_user_dir();   // 注册表 RimeUserDir 优先（便携部署）
  if (GetEnvironmentVariableW(L"USERPROFILE", base, MAX_PATH))
    dirs[1] = std::wstring(base) + L"\\gguf_models";
  int added = 0;
  for (auto& dir : dirs) {
    if (dir.empty()) continue;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.gguf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::wstring p = dir + L"\\" + fd.cFileName;
      if (_wcsicmp(p.c_str(), cur) == 0) continue;  // 当前值已在编辑框
      SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)p.c_str());
      if (++added >= 8) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (added >= 8) break;
  }
}

// 模型状态行：开关开着但文件不存在是最常见的静默失败——实时反映
// （2026-10-01：小于完整体量的文件不再显示成"已就绪"——半截模型会让重排
//   静默失效且用户找不到原因）
static const unsigned long long kModelMinBytes = 100ULL * 1024 * 1024;
static void refresh_model_status() {
  wchar_t buf[512];
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  unsigned long long sz = 0;
  if (buf[0] && file_size(buf, &sz)) {
    if (sz >= kModelMinBytes) {
      set_status(GetDlgItem(g_hwnd, IDC_MSTATUS), L"模型已就绪：%llu MB", sz >> 20);
      set_status(GetDlgItem(g_hwnd, IDC_MSTATUS_HINT), L"");
    } else {
      set_status(GetDlgItem(g_hwnd, IDC_MSTATUS), L"模型文件可疑：仅 %llu MB", sz >> 20);
      set_status(GetDlgItem(g_hwnd, IDC_MSTATUS_HINT),
                 L"疑似未下完或损坏（完整约 508MB）——点“下载模型”会删除后重下，或点“浏览…”换一个");
    }
  } else {
    // 拆两行：主状态（正文）+ 后续操作（灰色小字）——一行放不下会被截断
    set_status(GetDlgItem(g_hwnd, IDC_MSTATUS), L"模型文件不存在");
    set_status(GetDlgItem(g_hwnd, IDC_MSTATUS_HINT),
               L"点“下载模型”下载（ModelScope 断点续传），或点“浏览…”选已有的 .gguf");
  }
}

// ---- 方案接入（语义同 installer\schema_add.ps1，2026-09-29 GUI 化）----
// 幂等插入/剥离 llm_filter 组件行；先剥后插——插件版 lua 组件行与
// llm_rerank 节一并剥净（跨版自动转换）。schema 文件按 UTF-8 读写
//（原 BOM 状态保留），行尾统一 CRLF（yaml-cpp 兼容）。
static bool read_lines(const std::wstring& path, bool* had_bom,
                       std::vector<std::string>* lines) {
  FILE* f = NULL;
  _wfopen_s(&f, path.c_str(), L"rb");
  if (!f) return false;
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  fclose(f);
  *had_bom = data.compare(0, 3, "\xEF\xBB\xBF") == 0;
  if (*had_bom) data.erase(0, 3);
  lines->clear();
  size_t start = 0;
  while (start <= data.size()) {
    size_t e = data.find('\n', start);
    std::string ln = data.substr(start, (e == std::string::npos ? data.size()
                                                            : e) - start);
    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
    lines->push_back(ln);
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return true;
}

static bool write_lines(const std::wstring& path, bool had_bom,
                        const std::vector<std::string>& lines) {
  std::string data = had_bom ? "\xEF\xBB\xBF" : "";
  for (auto& ln : lines) { data += ln; data += "\r\n"; }
  FILE* f = NULL;
  _wfopen_s(&f, path.c_str(), L"wb");
  if (!f) return false;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  return true;
}

// 行是否为列表项 "- <item>"（允许任意缩进/空白）
static bool is_list_item(const std::string& ln, const char* item) {
  size_t i = ln.find_first_not_of(" \t");
  if (i == std::string::npos || ln[i] != '-') return false;
  i = ln.find_first_not_of(" \t", i + 1);
  if (i == std::string::npos) return false;
  size_t n = strlen(item);
  if (ln.compare(i, n, item) != 0) return false;
  i += n;
  return ln.find_first_not_of(" \t", i) == std::string::npos;
}

// ── 外部改动保护（2026-10-01 用户定案）────────────────────────────
// 文件内容指纹（FNV-1a 64）；同内容重写/触摸文件不会误报"被外部修改"
static std::wstring g_file_stamp;
static std::wstring file_stamp(const std::wstring& path) {
  FILE* f = NULL;
  _wfopen_s(&f, path.c_str(), L"rb");
  if (!f) return std::wstring();
  unsigned long long h = 1469598103934665603ULL;
  int c;
  while ((c = fgetc(f)) != EOF) {
    h ^= (unsigned char)c;
    h *= 1099511628211ULL;
  }
  fclose(f);
  wchar_t buf[32];
  swprintf_s(buf, L"%016llX", h);
  return std::wstring(buf);
}

// 剥离两版 LLM 组件行与 llm_rerank 节（含节前空行）；返回删除行数
static int strip_llm(std::vector<std::string>* lines) {
  std::vector<std::string> out;
  int removed = 0;
  bool in_cfg = false;
  for (auto& ln : *lines) {
    if (in_cfg) {
      if (!ln.empty() && !isspace((unsigned char)ln[0])) { in_cfg = false; }
      else { removed++; continue; }
    }
    if (is_list_item(ln, "lua_processor@*llm_processor") ||
        is_list_item(ln, "lua_filter@*llm_filter") ||
        is_list_item(ln, "llm_filter")) {
      removed++; continue;
    }
    // 顶格 llm_rerank: 节
    size_t first = ln.find_first_not_of(" \t");
    if (first != std::string::npos && ln.compare(first, 11, "llm_rerank:") == 0) {
      removed++;
      if (!out.empty() && out.back().empty()) { out.pop_back(); removed++; }
      in_cfg = true;
      continue;
    }
    out.push_back(ln);
  }
  if (removed) *lines = out;
  return removed;
}

// filters 块内幂等插入 "    - llm_filter"：
// uniquifier 后 → simplifier 后 → 块末；无 filters 块返回 false
static bool insert_llm_filter(std::vector<std::string>* lines,
                              std::wstring* where) {
  for (auto& ln : *lines) {
    std::string t = trim(ln);
    if (t == "llm_filter" || t == "- llm_filter") {
      *where = L"（已存在）";
      return true;
    }
  }
  int filt_start = -1, filt_end = -1, uniquifier = -1, simplifier = -1;
  bool in_filt = false;
  for (int i = 0; i < (int)lines->size(); i++) {
    const std::string& ln = (*lines)[i];
    if (!in_filt) {
      size_t first = ln.find_first_not_of(" \t");
      if (first != std::string::npos && ln[first] != ' ' && ln[first] != '\t' &&
          ln.compare(first, 8, "filters:") == 0) {
        // 仅 engine 下的 filters（缩进 ≥1）；顶格 filters: 不是 engine 键
        if (first > 0) { in_filt = true; filt_start = i; }
      }
      continue;
    }
    // 顶格键 = 块结束（注意不能用 ln[first]!=' ' 判顶格——first 来自
    // find_first_not_of，恒非空白；2026-09-29 沙箱测试抓出的真 bug）
    if (!ln.empty() && !isspace((unsigned char)ln[0])) {
      filt_end = i - 1;
      break;
    }
    if (ln.empty()) continue;  // 块尾空行：插入点不越过它
    std::string t = trim(ln);
    if (t.rfind("- ", 0) == 0) {
      std::string item = trim(t.substr(2));
      if (item == "uniquifier") uniquifier = i;
      if (simplifier < 0 && item == "simplifier") simplifier = i;
    }
    filt_end = i;
  }
  if (filt_start < 0 || filt_end < filt_start) return false;
  int at = uniquifier >= 0 ? uniquifier : simplifier;
  *where = uniquifier >= 0 ? L"uniquifier 之后"
                           : (simplifier >= 0 ? L"simplifier 之后" : L"filters 块末尾");
  lines->insert(lines->begin() + (at >= 0 ? at : filt_end) + 1, "    - llm_filter");
  return true;
}

// 方案是否需要"重建接入"（缺节 / 缺本版组件行 / 残留插件版组件行）
static bool schema_needs_rebuild(const std::vector<std::string>& lines, wchar_t* why) {
  bool has_sec = llm_section_start(lines) >= 0;
  bool has_filter = false, has_plugin = false;
  for (auto& ln : lines) {
    std::string t = trim(ln);
    if (t == "llm_filter" || t == "- llm_filter") has_filter = true;
    if (t.find("lua_processor@*llm_processor") != std::string::npos ||
        t.find("lua_filter@*llm_filter") != std::string::npos)
      has_plugin = true;
  }
  std::vector<std::wstring> parts;
  if (!has_sec) parts.push_back(L"无配置节");
  if (!has_filter) parts.push_back(L"缺 llm_filter 组件行");
  if (has_plugin) parts.push_back(L"残留插件版组件行");
  std::wstring w;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) w += L"、";
    w += parts[i];
  }
  if (why) wcscpy_s(why, 128, w.c_str());
  return !w.empty();
}

// 触发重新部署：WeaselRoot 注册表定位 WeaselDeployer，15s 有界等待
//（deployer 曾在 EndMaintenance 管道应答中挂死——超时留后台不杀）
static void trigger_redeploy() {
  // 测试开关：静默跳过（结果状态行不被覆盖）
  if (GetEnvironmentVariableW(L"WEASEL_LLM_SETUP_NO_REDEPLOY", NULL, 0)) return;
  wchar_t root[MAX_PATH] = L"";
  DWORD sz = sizeof(root);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rime\\Weasel", L"WeaselRoot",
                   RRF_RT_REG_SZ, NULL, root, &sz) != ERROR_SUCCESS || !root[0]) {
    sz = sizeof(root);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rime\\Weasel",
                 L"WeaselRoot", RRF_RT_REG_SZ, NULL, root, &sz);
  }
  std::wstring deployer = std::wstring(root) + L"\\WeaselDeployer.exe";
  if (!root[0] || GetFileAttributesW(deployer.c_str()) == INVALID_FILE_ATTRIBUTES) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[提示] 未定位到 WeaselDeployer，请手动：托盘小狼毫 → 重新部署");
    log_error(L"[部署] 未定位到 WeaselDeployer（WeaselRoot=%s）", root);
    return;
  }
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  wchar_t cmd[MAX_PATH] = L"";
  wcscpy_s(cmd, deployer.c_str());
  if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_TIMEOUT) {
      set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
                 L"重新部署仍在后台进行；若候选异常请托盘手动重新部署");
      log_error(L"[部署] WeaselDeployer 15s 未退出（仍在后台）");
    }
    CloseHandle(pi.hProcess);
  } else {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[提示] 自动重新部署失败，请手动：托盘小狼毫 → 重新部署");
    log_error(L"[部署] CreateProcess 失败：%s", deployer.c_str());
  }
}

// ── 程序文件夹（预装数据）里的方案 ──────────────────────────────
// Rime 的配置分两处：**用户文件夹** %APPDATA%\Rime（可写、优先）与**程序文件夹**
// <小狼毫安装目录>\data（预装方案 luna_pinyin / cangjie5 / bopomofo …，随安装包更新）。
// 两处的 *.schema.yaml 都要能选；但只有用户文件夹里的才算"用户的方案"：写程序
// 文件夹既需管理员、又会被下次升级覆盖 → 对预装方案一律**先复制到用户文件夹再改**
// （Rime 解析顺序本就是用户文件夹优先，这也是 Rime 官方的手改做法）。
static std::wstring rime_shared_dir() {
  static std::wstring cached;
  static bool tried = false;
  if (tried) return cached;
  tried = true;
  wchar_t root[MAX_PATH] = L"";
  DWORD sz = sizeof(root);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Rime\\Weasel", L"WeaselRoot",
                   RRF_RT_REG_SZ, NULL, root, &sz) != ERROR_SUCCESS || !root[0]) {
    sz = sizeof(root);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Rime\\Weasel",
                 L"WeaselRoot", RRF_RT_REG_SZ, NULL, root, &sz);
  }
  if (root[0]) {
    std::wstring d = std::wstring(root) + L"\\data";
    if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) {
      cached = d;
      return cached;
    }
  }
  // 回退：%ProgramFiles%\Rime\weasel-*\data（取字典序最大 = 最新版本）
  wchar_t pf[MAX_PATH] = L"";
  if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
    std::wstring pat = std::wstring(pf) + L"\\Rime\\weasel-*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          std::wstring d = std::wstring(pf) + L"\\Rime\\" + fd.cFileName + L"\\data";
          if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) cached = d;
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  return cached;
}

static const wchar_t* kSharedSuffix = L"（程序）";   // 预装方案在下拉里的后缀

static const wchar_t* base_name(const std::wstring& path);   // 定义在后（先声明）

// 方案下拉项 → 完整路径（与 combo 项一一对应）
static std::vector<std::wstring> g_schema_paths;
static std::vector<std::wstring> g_schema_names;

static void scan_schemas() {
  HWND combo = GetDlgItem(g_hwnd, IDC_SCHEMA);
  // 记住当前选中项（按文件名，忽略「（程序）」后缀）——刷新/接入后保持，不跳回第一项
  std::wstring prev;
  int prevSel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (prevSel >= 0 && prevSel < (int)g_schema_names.size()) prev = g_schema_names[prevSel];
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  g_schema_paths.clear();
  g_schema_names.clear();
  std::wstring user = rime_user_dir();
  if (user.empty()) return;
  // ① 用户文件夹（无后缀）
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((user + L"\\*.schema.yaml").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      g_schema_names.push_back(fd.cFileName);
      g_schema_paths.push_back(user + L"\\" + fd.cFileName);
      SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  // ② 程序文件夹预装方案（带「（程序）」后缀；同名以用户文件夹为准，不重复列出）
  std::wstring shared = rime_shared_dir();
  if (!shared.empty()) {
    h = FindFirstFileW((shared + L"\\*.schema.yaml").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        bool dup = false;
        for (size_t i = 0; i < g_schema_names.size(); i++)
          if (g_schema_names[i] == fd.cFileName) { dup = true; break; }
        if (dup) continue;
        g_schema_names.push_back(fd.cFileName);
        g_schema_paths.push_back(shared + L"\\" + fd.cFileName);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)(std::wstring(fd.cFileName) + kSharedSuffix).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0) {
    int restore = 0;
    if (!prev.empty()) {
      for (size_t i = 0; i < g_schema_names.size(); i++)
        if (g_schema_names[i] == prev) { restore = (int)i; break; }
    }
    SendMessageW(combo, CB_SETCURSEL, restore, 0);
  }
  // 下拉列表按最长项加宽（框内放不下没关系，列表可以更宽）——预装方案名 + 「（程序）」较长
  HDC dc = GetDC(combo);
  HGDIOBJ old = SelectObject(dc, g_font);
  int wmax = 0;
  int n = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
  for (int i = 0; i < n; i++) {
    wchar_t item[260] = L"";
    SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)item);
    SIZE sz = {0};
    if (GetTextExtentPoint32W(dc, item, (int)wcslen(item), &sz) && sz.cx > wmax) wmax = sz.cx;
  }
  SelectObject(dc, old);
  ReleaseDC(combo, dc);
  if (wmax > 0) SendMessageW(combo, CB_SETDROPPEDWIDTH, wmax + 24, 0);
}

// 下拉里按文件名（忽略后缀）重新选中——复制到用户文件夹后用
static void select_schema_named(const std::wstring& name) {
  for (size_t i = 0; i < g_schema_names.size(); i++) {
    if (g_schema_names[i] == name) {
      SendMessageW(GetDlgItem(g_hwnd, IDC_SCHEMA), CB_SETCURSEL, i, 0);
      return;
    }
  }
}

// 写入前确保方案在用户文件夹：程序文件夹里的预装方案先复制过来（返回用户文件夹路径；
// 第二个出参标记是否发生了复制，供状态行提示）
static std::wstring ensure_user_schema(const std::wstring& path, bool* copied) {
  if (copied) *copied = false;
  std::wstring user = rime_user_dir();
  if (user.empty()) return path;
  if (_wcsnicmp(path.c_str(), (user + L"\\").c_str(), user.size() + 1) == 0) return path;
  const wchar_t* leaf = base_name(path);
  std::wstring dest = user + L"\\" + leaf;
  CreateDirectoryW(user.c_str(), NULL);
  if (!CopyFileW(path.c_str(), dest.c_str(), FALSE)) {
    log_error(L"[复制] 预装方案复制到用户文件夹失败：%s → %s", path.c_str(), dest.c_str());
    return path;   // 复制失败仍返回原路径（写盘会失败并报错，日志已留痕）
  }
  if (copied) *copied = true;
  return dest;
}

static std::wstring selected_schema_path(bool* ok) {
  int sel = (int)SendMessageW(GetDlgItem(g_hwnd, IDC_SCHEMA), CB_GETCURSEL, 0, 0);
  if (sel >= 0 && sel < (int)g_schema_paths.size()) {
    *ok = true;
    return g_schema_paths[sel];
  }
  *ok = false;
  return std::wstring();
}

// 选中的是不是程序文件夹里的预装方案（未被复制到用户文件夹）
static bool selected_is_shared() {
  int sel = (int)SendMessageW(GetDlgItem(g_hwnd, IDC_SCHEMA), CB_GETCURSEL, 0, 0);
  if (sel < 0 || sel >= (int)g_schema_paths.size()) return false;
  std::wstring user = rime_user_dir();
  if (user.empty()) return false;
  return _wcsnicmp(g_schema_paths[sel].c_str(), (user + L"\\").c_str(), user.size() + 1) != 0;
}

static const wchar_t* base_name(const std::wstring& path) {
  const wchar_t* p = wcsrchr(path.c_str(), L'\\');
  return p ? p + 1 : path.c_str();
}

static void params_to_ui();  // fwd decl（定义在下方）
static void ui_to_params();  // fwd decl（定义在下方）

// 把选中方案的 llm_rerank 节读进界面（选方案/刷新/接入/剥离后调用）。
// 无节 → 界面上是默认值 + 提示"未接入"；模型路径若方案节里没有而旧全局
// 配置里有，则带过来（一次性迁移，保存即写入方案节）。
static void load_schema_params() {
  if (!g_hwnd) return;
  bool ok = false;
  std::wstring path = selected_schema_path(&ok);
  if (!ok) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"请先在下拉框选择方案文件（%%APPDATA%%\\Rime\\*.schema.yaml）");
    return;
  }
  g_p = Params();  // 先落默认值
  bool had_bom = false;
  std::vector<std::string> lines;
  bool has_section = read_lines(path, &had_bom, &lines) && parse_llm_section(lines);
  // 方案节里还没有配置 → 用旧全局 llm_rerank.yaml 一次性迁移界面值
  // （该文件已废弃：运行期不读它，只有这里为老用户带出参数）
  bool migrated = false;
  if (!has_section) migrated = load_legacy_params(&g_p);
  // 无节 = 该方案还没接入：默认勾上「启用」，让『保存并生效』一步完成接入
  // （用户取消勾选再保存 = 显式写入 enabled: false）
  if (!has_section && !migrated) g_p.enabled = true;
  if (has_section) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"已加载 %s 的 llm_rerank 配置节",
               base_name(path));
  } else if (selected_is_shared()) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[未接入] %s 是程序文件夹里的预装方案——点『保存并生效』会先复制到用户文件夹，"
               L"再补组件行 + 配置节",
               base_name(path));
  } else if (migrated) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[未接入] %s 没有 llm_rerank 节——已带出旧全局配置的值，"
               L"点『保存并生效』写进方案",
               base_name(path));
  } else {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[未接入] %s 里没有 llm_rerank 节——显示默认值（已默认勾选启用），"
               L"点『保存并生效』即接入",
               base_name(path));
  }
  params_to_ui();
  g_file_stamp = file_stamp(path);   // 外部改动保护：记下读入时的文件指纹
  g_loaded_name = base_name(path);
}

// 保存界面参数 → 选中方案的 llm_rerank 节（节必须已存在）+ 触发重新部署
static void save_schema_params() {
  bool ok = false;
  std::wstring path = selected_schema_path(&ok);
  if (!ok) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"[失败] 请先选择方案文件");
    return;
  }
  bool copied = false;
  std::wstring name = base_name(path);
  path = ensure_user_schema(path, &copied);   // 预装方案先落到用户文件夹
  if (copied) {
    scan_schemas();
    select_schema_named(name);
  }
  bool had_bom = false;
  std::vector<std::string> lines;
  if (!read_lines(path, &had_bom, &lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"[失败] 读取方案失败：%s", path.c_str());
    log_error(L"[保存] 读取方案失败：%s", path.c_str());
    return;
  }
  ui_to_params();
  // ③ 模型路径必须是绝对路径（运行期直接交给 llama——不做 %VAR% 展开、不解析
  //    相对路径，相对值会按服务进程 cwd 解析，几乎必然加载失败）
  if (!g_p.model_path.empty() && !is_abs_path(g_p.model_path)) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS),
               L"[失败] 『模型路径』必须是绝对路径（如 D:\\gguf_models\\xxx.gguf）或留空用默认：%s",
               g_p.model_path.c_str());
    log_error(L"[保存] 模型路径不是绝对路径，拒绝保存：%s", g_p.model_path.c_str());
    return;
  }
  // 外部改动保护（2026-10-01）：读入后文件又被别处改过 → 先问再覆盖，避免陈旧界面值冲掉外部编辑
  // （预装方案刚被复制过来时指纹基准是程序文件夹那份，故 copied 时跳过比对）
  if (!copied && !g_file_stamp.empty() && file_stamp(path) != g_file_stamp) {
    if (MessageBoxW(g_hwnd,
                    L"方案文件已被外部修改。\n\n用界面上的值覆盖它？（选『否』= 丢弃界面改动并重新读取）",
                    L"文件已变更", MB_YESNO | MB_ICONWARNING) != IDYES) {
      load_schema_params();
      set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已取消保存并重新读取磁盘上的方案");
      return;
    }
  }
  // 保存 = 自适应（2026-10-01 用户定案，替代原「接入 LLM」+「保存」两按钮）：
  //   缺节 / 缺本版组件行 / 残留另一版组件行 → 重建接入（剥净 + 插组件 + 按界面值写节）
  //   否则                                    → 只重写节
  wchar_t why[128] = L"";
  bool need_rebuild = schema_needs_rebuild(lines, why);
  if (need_rebuild) {
    int removed = strip_llm(&lines);          // 剥净两版组件行 + 旧节
    std::wstring where;
    if (!insert_llm_filter(&lines, &where)) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS),
                 L"[失败] 方案内未找到 engine/filters 块，无法补齐组件行");
      log_error(L"[保存] 未找到 engine/filters 块：%s", path.c_str());
      return;
    }
    if (!lines.empty() && !lines.back().empty()) lines.push_back("");
    std::vector<std::string> sec = build_llm_section();
    lines.insert(lines.end(), sec.begin(), sec.end());
    (void)removed;
  } else if (!update_llm_section(&lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"[失败] 重写 llm_rerank 节失败：%s", path.c_str());
    log_error(L"[保存] update_llm_section 失败：%s", path.c_str());
    return;
  }
  if (!write_lines(path, had_bom, lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"[失败] 写入方案失败：%s", path.c_str());
    log_error(L"[保存] 写入方案失败：%s", path.c_str());
    return;
  }
  g_file_stamp = file_stamp(path);
  load_schema_params();   // 回读：方案接入组状态行从"未接入"变"已加载"，界面与磁盘一致
  set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"正在重新部署…");
  refresh_model_status();  // 模型路径可能刚改
  trigger_redeploy();
  const wchar_t* act = need_rebuild ? L"已接入并保存" : L"已保存";
  wchar_t note[256] = L"";
  if (copied && need_rebuild)
    swprintf_s(note, L"（原为程序文件夹预装方案，已复制到用户文件夹；补齐了组件行：%s）", why);
  else if (copied)
    swprintf_s(note, L"（原为程序文件夹预装方案，已复制到用户文件夹）");
  else if (need_rebuild)
    swprintf_s(note, L"（补齐了组件行：%s）", why);
  set_status(GetDlgItem(g_hwnd, IDC_STATUS),
             L"%s %s%s 并触发重新部署——部署完成后参数生效", act, base_name(path), note);
}

// on_schema_add 已删除（2026-10-01 用户定案：并入『保存并生效』的自适应重建，
// 见 save_schema_params；CLI/脚本侧的 schema_add.ps1 仍在，供无人值守安装用）


static void on_schema_remove() {
  bool ok;
  std::wstring path = selected_schema_path(&ok);
  if (!ok) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"请先在下拉框选择方案文件（%APPDATA%\\Rime\\*.schema.yaml）");
    return;
  }
  if (selected_is_shared()) {
    // 程序文件夹预装方案从未被改过 → 剥离无意义，也不该动它
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[提示] 该方案是程序文件夹预装方案（未被修改过）——无可剥离；"
               L"若要改它请先『接入 LLM』（会复制到用户文件夹）");
    return;
  }
  bool had_bom;
  std::vector<std::string> lines;
  if (!read_lines(path, &had_bom, &lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"读取方案失败：%s", path.c_str());
    log_error(L"[剥离] 读取方案失败：%s", path.c_str());
    return;
  }
  int removed = strip_llm(&lines);
  if (!removed) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"未发现 LLM 组件，方案未改动");
    return;
  }
  if (!write_lines(path, had_bom, lines))
    log_error(L"[剥离] 写入方案失败：%s", path.c_str());
  wchar_t msg[128];
  swprintf_s(msg, L"已移除 LLM 组件（含配置节）共 %d 行", removed);
  load_schema_params();  // 回默认值 + "未接入" 提示
  set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), msg);
  trigger_redeploy();
}

// ---- 控件读写 ----
static void ui_to_params() {
  g_p.enabled = SendMessageW(GetDlgItem(g_hwnd, IDC_ENABLED), BM_GETCHECK, 0, 0) == BST_CHECKED;
  g_p.debug_fusion = SendMessageW(GetDlgItem(g_hwnd, IDC_DEBUG), BM_GETCHECK, 0, 0) == BST_CHECKED;
  wchar_t buf[512];
  struct { int id; int* v; } ints[] = {
      {IDC_MAX_TOK, &g_p.max_tokens},
      {IDC_CORES, &g_p.cpu_cores},
      {IDC_MAX_CAND, &g_p.max_candidates}};
  for (auto& r : ints) {
    GetDlgItemTextW(g_hwnd, r.id, buf, 64);
    *r.v = (int)wcstol(buf, NULL, 10);
  }
  // 编码匹配正则（原样收，不做数值解析；空串 = 不限制）。
  // 正则语法本身是 ASCII，按 ASCII 收窄即可（中文只在注释里，不入模式）
  GetDlgItemTextW(g_hwnd, IDC_CODE_PAT, buf, 512);
  {
    std::string pat;
    for (const wchar_t* p = buf; *p; ++p)
      if (*p < 128) pat += (char)*p;
    g_p.code_pattern = trim(pat);
  }
  struct { int id; double* v; } dbls[] = {{IDC_ELW, &g_p.elw},
                                          {IDC_FREQ_W, &g_p.freq_beta}};
  for (auto& r : dbls) {
    GetDlgItemTextW(g_hwnd, r.id, buf, 64);
    *r.v = wcstod(buf, NULL);
  }
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  std::wstring def = default_model_path();
  g_p.model_path = (wcscmp(buf, def.c_str()) == 0) ? std::wstring() : buf;
}

// 界面签名 / 脏标记（2026-10-01）：与"上次读入界面时"的签名比对——用于
// "打开下拉时是否重读当前文件"（有未保存改动就不覆盖用户输入）。
// 比挂 TextChanged 事件简单可靠：跨线程/程序化赋值都不会误判。
static std::wstring g_ui_sig;
static std::wstring ui_signature() {
  std::wstring s;
  s += SendMessageW(GetDlgItem(g_hwnd, IDC_ENABLED), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
  s += SendMessageW(GetDlgItem(g_hwnd, IDC_DEBUG), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
  const int ids[] = {IDC_MODEL,   IDC_CODE_PAT, IDC_MAX_TOK, IDC_MAX_CAND,
                     IDC_CORES,   IDC_FREQ_W,   IDC_ELW};
  wchar_t buf[512];
  for (int id : ids) {
    buf[0] = 0;
    GetDlgItemTextW(g_hwnd, id, buf, 512);
    s += buf;
    s += L"|";
  }
  return s;
}
static bool ui_dirty() { return g_hwnd && ui_signature() != g_ui_sig; }

static void params_to_ui() {
  SendMessageW(GetDlgItem(g_hwnd, IDC_ENABLED), BM_SETCHECK,
               g_p.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(GetDlgItem(g_hwnd, IDC_DEBUG), BM_SETCHECK,
               g_p.debug_fusion ? BST_CHECKED : BST_UNCHECKED, 0);
  SetDlgItemTextW(g_hwnd, IDC_MODEL, shown_model().c_str());
  SetDlgItemTextW(g_hwnd, IDC_CODE_PAT, utf8_to_wide(g_p.code_pattern).c_str());
  wchar_t buf[64];
  struct { int id; int v; } rows[] = {
      {IDC_MAX_TOK, g_p.max_tokens},
      {IDC_CORES, g_p.cpu_cores},       {IDC_MAX_CAND, g_p.max_candidates}};
  for (auto& r : rows) {
    swprintf_s(buf, L"%d", r.v);
    SetDlgItemTextW(g_hwnd, r.id, buf);
  }
  swprintf_s(buf, L"%.2f", g_p.freq_beta);
  SetDlgItemTextW(g_hwnd, IDC_FREQ_W, buf);
  swprintf_s(buf, L"%.2f", g_p.elw);
  SetDlgItemTextW(g_hwnd, IDC_ELW, buf);
  scan_models(GetDlgItem(g_hwnd, IDC_MODEL));
  refresh_model_status();
  g_ui_sig = ui_signature();   // 脏标记基准（打开下拉是否重读当前文件）
}

// ---- 控件创建 ----
static HWND mk(int cls, const wchar_t* text, DWORD style, int x, int y, int w,
               int h, int id) {
  static const wchar_t* C[] = {L"BUTTON", L"STATIC", L"EDIT", L"COMBOBOX"};
  HWND ctl = CreateWindowW(C[cls], text,
                           WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd,
                           (HMENU)(INT_PTR)id, NULL, NULL);
  SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
  return ctl;
}

// 布局铁律：任何控件的矩形不得与其他控件相交——不透明子控件按 z 序
// 覆盖先画者，会把被覆盖控件的文字"局部擦除"成叠字残片（2026-08-27
// 叠字事故根因：勾选框 w430 与下行标签矩形相交 + 空状态静态框横贯首行）。
// 2026-09-30 第一次改版（"丑、自解释性不强"）：分组改 BS_GROUPBOX、
//   列宽按 tw() 实测、清单加 dpiAware（位图拉伸发虚 = 黑边框很丑的根因）。
// 2026-09-30 第二次改版（用户定案）：**一个配置项一行，说明不直出**——
//   每行 = 标签 + 输入框 + 右对齐「?」徽标，说明只在鼠标悬停时弹出
//   （tooltips_class32 + TTF_SUBCLASS，见 tip_init/tip_add/mk_help）。
// 新增控件请沿用 tw() 排栅格与 HX 徽标列；改完务必截图核对。
static HWND mk_group(const wchar_t* title, int x, int y, int w, int h);
static HWND mk_hint(const wchar_t* text, int x, int y, int w);
static HWND mk_help(int x, int y, const wchar_t* tip);  // 「?」徽标
static void tip_add(HWND ctl, const wchar_t* text);     // 控件挂悬停提示
static void tip_init();                                 // tooltips_class32 窗口
static int  tw(const wchar_t* s, HFONT f);   // 文字像素宽
static int  ts(int v);                        // DPI 缩放

// ---- 参数说明文案（与插件版 install_plugin.ps1 的 $tip* 逐字对齐）----
// \n 手工断行：tooltips_class32 未设最大宽度时单行会长到出屏（另有
// TTM_SETMAXTIPWIDTH 兜底自动折行）。注意路径里的 \ 要写成 \\。
static const wchar_t* const TIP_ENABLED =
    L"总开关：开 = 加载模型参与候选重排；关 = 卸载模型释放内存。\n"
    L"保存后立即生效，无需重新部署。";
static const wchar_t* const TIP_MODEL =
    L"GGUF 模型文件路径（留空 = 用户文件夹里的默认名）。\n"
    L"下拉列出用户文件夹与本机 gguf_models 下的模型；\n"
    L"换模型保存后会自动卸载并重载。";
static const wchar_t* const TIP_BROWSE = L"浏览…：选择 .gguf 模型文件。";
static const wchar_t* const TIP_DOWNLOAD =
    L"下载模型：ModelScope 断点续传（curl -C -），落点 = 左边的模型路径框\n"
    L"（留空 = 用户文件夹里的默认名 Qwen3.5-0.8B-Q4_K_M.gguf）。\n"
    L"失败会保留 .download 分片，再点一次即续传。";
static const wchar_t* const TIP_SCHEMA =
    L"配置就写在选中的方案文件里。下拉列出**用户文件夹** %APPDATA%\\Rime 与\n"
    L"**程序文件夹** <小狼毫目录>\\data（预装方案，带「（程序）」后缀）两处的 *.schema.yaml；\n"
    L"**打开下拉即重扫两处**（外部新增/删除方案后无需手动刷新），界面无改动时还会重读当前文件。\n"
    L"预装方案在写入前会自动复制到用户文件夹（Rime 解析顺序：用户文件夹优先）。";
static const wchar_t* const TIP_SCHEMAREM =
    L"剥离：把选中方案里的 LLM 组件行与 llm_rerank 配置节整个删掉\n"
    L"（方案回到未接入状态），并自动重新部署。\n"
    L"日常只想改参数请用『保存并生效』。";
static const wchar_t* const TIP_CODE_PAT =
    L"触发条件：编码串全串正则匹配，只有匹配上的编码才交给 LLM 重排\n"
    L"（写法与 Rime speller/auto_select_pattern 一致）。\n"
    L"默认 .{4} = 恰 4 码。例：\n"
    L"　.{4,} 4 码以上　　.{3,4} 3~4 码\n"
    L"　[abcde]{4} 指定首码　　空 = 不限制\n"
    L"含 \\ 的写法要用单引号，如 '\\d{4}'。";
static const wchar_t* const TIP_MAX_TOK =
    L"上文长度上限：取光标前多少个 token 作为重排依据（默认 10）。\n"
    L"越大越准，但每次都更慢。";
static const wchar_t* const TIP_MAX_CAND =
    L"每次按键参与 LLM 打分的候选数上限（默认 5）。\n"
    L"一般不用改——调大更准但更慢。";
static const wchar_t* const TIP_CORES =
    L"推理用的 CPU 线程数（默认 4）。不要超过本机物理核；\n"
    L"可用 ..\\rime-llm-rerank\\cpp\\build_bench_threads.bat 实测最优值。";
static const wchar_t* const TIP_BETA =
    L"用户词频权重 β（默认 1.5，0 = 关闭）。\n"
    L"融合分 = CE 分 + β·log(1+词频计数) + elw·词长加成\n"
    L"越常上屏的词加分越多；加分在 log 域，可翻盘 LLM 的分差。";
static const wchar_t* const TIP_ELW =
    L"预期词长权重 elw（默认 0.2，0 = 关闭）。\n"
    L"融合分 = CE 分 + β·log(1+词频计数) + elw·词长加成\n"
    L"按 词长 = 码长÷2 给候选加成，只对两码一字的方案有意义；\n"
    L"成熟机器建议 0。";
static const wchar_t* const TIP_DEBUG =
    L"诊断日志：开启后每次重排都往用户文件夹写 rime_llm_debug.txt\n"
    L"（逐候选 CE / 词频 / 词长与名次变化）。排障用，平时关闭。";
static const wchar_t* const TIP_SAVE =
    L"把上面的参数写进选中方案文件的 llm_rerank 配置节（键名与该节里相同），\n"
    L"随后自动触发重新部署——部署完成即生效（配置在方案里，不再有全局文件）。\n"
    L"方案还没接入（缺配置节 / 缺组件行 / 残留插件版组件行）时，\n"
    L"本按钮会先剥净再补齐组件行 + 写入配置节（原「接入 LLM」已并入这里）。";
static const wchar_t* const TIP_CLOSE = L"关闭窗口（不保存未保存的改动）。";
static const wchar_t* const TIP_OPENDIR =
    L"打开小狼毫用户文件夹（%APPDATA%\\Rime）——模型与方案文件都在这里。";

// 一行 = 标签 + 输入框 + 行末「?」徽标（全界面共用一个标签列宽与徽标列）
static void row_input(int& y, int label_w, const wchar_t* label, int id,
                      int box_w, DWORD style, const wchar_t* tip, int hx) {
  mk(1, label, 0, ts(24), y + ts(3), label_w + ts(4), ts(20), 0);
  HWND e = mk(2, L"", WS_BORDER | WS_TABSTOP | style, ts(24) + label_w + ts(10),
              y, ts(box_w), ts(22), id);
  tip_add(e, tip);
  mk_help(hx, y + ts(2), tip);
  y += ts(30);
}

static void make_ui() {
  g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_font_hint = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_font_help = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  tip_init();

  const int PAD = ts(12);   // 分组框外边距（左右对称：客户区 620 - 2×12 = 596）
  const int CW = ts(596);   // 分组框宽
  const int LX = ts(24);    // 组内左起点（PAD + 12）
  const int HX = ts(576);   // 「?」徽标列（全界面统一，行右对齐）
  const int RX = ts(566);   // 组内行内容右边界（给徽标列留位）
  int y = ts(8);

  // ═══ 方案接入（语义同 installer\schema_add.ps1）——放最前：
  // 配置写在方案里，**先选方案再改参数**（2026-09-30 用户定案，与插件版同序）═══
  {
    int gy = y; y += ts(26);
    const wchar_t* lbl = L"方案文件:";
    int lw = tw(lbl, g_font);
    mk(1, lbl, 0, LX, y + ts(3), lw + ts(4), ts(20), 0);
    int x = LX + lw + ts(10);
    // 2026-10-01 用户定案：删「刷新」（打开下拉即重扫两处方案）与「接入 LLM」
    // （并入『保存并生效』的自适应重建）；『剥离』挪到保存按钮右侧 → 本行只剩下拉
    int comboW = RX - x - ts(18);
    HWND cmb = mk(3, L"", CBS_DROPDOWNLIST | WS_TABSTOP, x, y - ts(3), comboW,
                  ts(200), IDC_SCHEMA);
    tip_add(cmb, TIP_SCHEMA);
    mk_help(HX, y + ts(2), TIP_SCHEMA);
    y += ts(30);
    mk(1, L"", 0, LX, y, ts(528), ts(18), IDC_SCHEMSTAT);
    y += ts(20);
    mk_group(L"方案接入", PAD, gy, CW, y - gy + ts(8));
  }
  y += ts(10);

  // ═══ 总控：开关 + 模型路径 ═══════════════════════════════
  {
    int gy = y; y += ts(26);
    HWND cb = mk(0, L"启用 LLM 重排", BS_AUTOCHECKBOX | WS_TABSTOP, LX, y,
                 tw(L"启用 LLM 重排", g_font) + ts(26), ts(22), IDC_ENABLED);
    tip_add(cb, TIP_ENABLED);
    mk_help(HX, y + ts(2), TIP_ENABLED);
    y += ts(30);
    const wchar_t* lbl = L"模型路径:";
    int lw = tw(lbl, g_font);
    mk(1, lbl, 0, LX, y + ts(3), lw + ts(4), ts(20), 0);
    int ex = LX + lw + ts(10);
    int btnW = tw(L"浏览…", g_font) + ts(26);
    int dlW = tw(L"下载模型", g_font) + ts(26);
    // 2026-10-01 用户定案：模型下载功能两版统一，放这一行（源码版安装器不再提示下载）
    int comboW = (RX - btnW - dlW - ts(16)) - ex;
    HWND cmb = mk(3, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP, ex,
                  y - ts(3), comboW, ts(200), IDC_MODEL);
    tip_add(cmb, TIP_MODEL);
    tip_add(mk(0, L"浏览…", WS_TABSTOP, ex + comboW + ts(8), y - ts(3), btnW,
               ts(26), IDC_BROWSE), TIP_BROWSE);
    tip_add(mk(0, L"下载模型", WS_TABSTOP, ex + comboW + btnW + ts(12), y - ts(3),
               dlW, ts(26), IDC_DOWNLOAD), TIP_DOWNLOAD);
    mk_help(HX, y + ts(2), TIP_MODEL);
    y += ts(30);
    mk(1, L"", 0, LX, y, ts(140), ts(18), IDC_MSTATUS);
    // 第二段（灰色小字，紧跟状态后）：ID 供 refresh_model_status 填字
    HWND mh = mk_hint(L"", LX + ts(144), y, ts(404));
    SetWindowLongPtrW(mh, GWLP_ID, IDC_MSTATUS_HINT);
    y += ts(20);
    mk_group(L"总控", PAD, gy, CW, y - gy + ts(8));
  }
  y += ts(10);

  // ═══ 触发条件（单行）════════════════════════════════════
  {
    int gy = y; y += ts(26);
    row_input(y, tw(L"编码匹配:", g_font), L"编码匹配:", IDC_CODE_PAT, 240, 0,
              TIP_CODE_PAT, HX);
    mk_group(L"触发条件", PAD, gy, CW, y - gy + ts(8));
  }
  y += ts(10);

  // ═══ 推理规模 + 候选排序融合：五个参数各一行，标签列用同一个实测宽度
  // （2026-09-30 第二次定案：融合分不再平铺公式——两个权重按普通配置项列出，
  //  公式移进「?」悬停说明，避免"公式反而更难懂"）═══
  {
    struct { const wchar_t* label; int id; const wchar_t* tip; } rows[] = {
        {L"上文 token 上限:", IDC_MAX_TOK, TIP_MAX_TOK},
        {L"参与打分的候选数:", IDC_MAX_CAND, TIP_MAX_CAND},
        {L"CPU 线程数:", IDC_CORES, TIP_CORES},
        {L"用户词频权重:", IDC_FREQ_W, TIP_BETA},
        {L"预期词长权重:", IDC_ELW, TIP_ELW}};
    int lw = 0;
    for (int i = 0; i < 5; ++i) {
      int w = tw(rows[i].label, g_font);
      if (w > lw) lw = w;
    }
    int gy = y; y += ts(26);
    for (int i = 0; i < 3; ++i)
      row_input(y, lw, rows[i].label, rows[i].id, 64, ES_NUMBER, rows[i].tip, HX);
    mk_group(L"推理规模", PAD, gy, CW, y - gy + ts(8));
    y += ts(10);

    gy = y; y += ts(26);
    for (int i = 3; i < 5; ++i)
      row_input(y, lw, rows[i].label, rows[i].id, 64, 0, rows[i].tip, HX);
    mk_group(L"候选排序融合", PAD, gy, CW, y - gy + ts(8));
  }
  y += ts(12);

  // ═══ 诊断 + 保存 ═════════════════════════════════════════
  {
    HWND cb = mk(0, L"诊断日志 debug_fusion", BS_AUTOCHECKBOX | WS_TABSTOP, LX, y,
                 tw(L"诊断日志 debug_fusion", g_font) + ts(26), ts(22), IDC_DEBUG);
    tip_add(cb, TIP_DEBUG);
    mk_help(HX, y + ts(2), TIP_DEBUG);
    y += ts(30);
  }
  y += ts(4);
  // 2026-10-01 用户定案：『剥离』= 保存的破坏性补充，并排放在保存右侧
  tip_add(mk(0, L"保存并生效", WS_TABSTOP | BS_DEFPUSHBUTTON, LX, y, ts(110),
             ts(32), IDC_SAVE), TIP_SAVE);
  tip_add(mk(0, L"剥离", WS_TABSTOP, LX + ts(114), y, ts(76), ts(32), IDC_SCHEMAREM),
          TIP_SCHEMAREM);
  tip_add(mk(0, L"关闭", WS_TABSTOP, LX + ts(198), y, ts(76), ts(32), IDC_CLOSE),
          TIP_CLOSE);
  tip_add(mk(0, L"打开用户文件夹", WS_TABSTOP, RX - ts(126), y, ts(126),
             ts(32), IDC_OPENDIR), TIP_OPENDIR);
  y += ts(34);
  // 状态行独占一行（保存/部署消息较长，放按钮行右侧会被截断）
  mk(1, L"", 0, LX, y, ts(540), ts(20), IDC_STATUS);
  y += ts(22);
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  wchar_t note[200];
  swprintf_s(note,
             L"保存后自动重新部署生效　|　本机逻辑核 %lu　|　出错日志：WeaselLLMSetup_error.log",
             si.dwNumberOfProcessors);
  mk_hint(note, LX, y, ts(596));
  y += ts(22);
  g_ui_height = y + ts(10);          // 内容总高（调用方据此定窗口高度）
}

// DPI 缩放：清单已声明 system 感知，此处把设计像素换算成设备像素
static int ts(int v) {
  static int dpi = 0;
  if (!dpi) {
    HDC dc = GetDC(NULL);
    dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    if (dpi <= 0) dpi = 96;
  }
  return MulDiv(v, dpi, 96);
}

// 文字像素宽（按实测排栅格，避免估算导致标签被截断）。
// 注意：make_ui() 在 WM_CREATE 里调用——此时 g_hwnd **尚未赋值**（它在
// CreateWindowExW 返回后才设），故用桌面 DC + 显式选字体实测，可靠。
static int tw(const wchar_t* s, HFONT f) {
  HDC dc = GetDC(NULL);
  if (!dc) return (int)wcslen(s) * 13;
  HGDIOBJ old = SelectObject(dc, f);
  SIZE sz = {0, 0};
  GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  return sz.cx;
}

// 原生分组框（BS_GROUPBOX）：标题画在边框左上角，视觉上把参数分组
static HWND mk_group(const wchar_t* title, int x, int y, int w, int h) {
  HWND c = CreateWindowW(L"BUTTON", title,
                         WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h,
                         g_hwnd, NULL, NULL, NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
  return c;
}

// 灰色小字（状态副行 / 底部动态注）
static HWND mk_hint(const wchar_t* text, int x, int y, int w) {
  HWND c = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, 18,
                         g_hwnd, NULL, NULL, NULL);
  SendMessageW(c, WM_SETFONT, (WPARAM)g_font_hint, TRUE);
  return c;
}

// ---- 悬停提示（2026-09-30：说明只在这里出现，界面不直出）----
// tooltips_class32 + TTF_IDISHWND|TTF_SUBCLASS：由 tooltip 自己 subclass
// 目标控件收鼠标消息，父窗口不用转发任何消息。清单已声明 comctl32 v6，
// 界面上是圆角气泡样式。
static void tip_init() {
  g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                          WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                          g_hwnd, NULL, NULL, NULL);
  if (!g_tip) return;
  SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SendMessageW(g_tip, WM_SETFONT, (WPARAM)g_font_hint, TRUE);
  // 最大宽度 = 手工断行之外的兜底：超宽自动折行，绝不长到出屏
  SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, ts(480));
  SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELPARAM(300, 0));
}

static void tip_add(HWND ctl, const wchar_t* text) {
  if (!g_tip || !ctl || !text || !*text) return;
  g_tips.push_back(text);          // deque：指针不会被后续 push_back 搬走
  TOOLINFOW ti;
  ZeroMemory(&ti, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = g_hwnd;
  ti.uId = (UINT_PTR)ctl;
  ti.lpszText = (LPWSTR)g_tips.back().c_str();
  SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// 「?」徽标：SS_OWNERDRAW 小圆 + 问号，自身挂悬停提示（WM_DRAWITEM 里画）。
// 坑：静态控件**不带 SS_NOTIFY 时 WM_NCHITTEST 返回 HTTRANSPARENT**（鼠标
// 消息直接穿透给父窗口）→ tooltip 的 TTF_SUBCLASS 永远收不到 WM_MOUSEMOVE，
// 悬停没反应（2026-09-30 实测：徽标画得出、提示不出）。故必须带 SS_NOTIFY。
static HWND mk_help(int x, int y, const wchar_t* tip) {
  HWND c = CreateWindowW(L"STATIC", L"?",
                         WS_CHILD | WS_VISIBLE | SS_OWNERDRAW | SS_NOTIFY, x, y,
                         ts(18), ts(18), g_hwnd, NULL, NULL, NULL);
  tip_add(c, tip);
  return c;
}

// WM_DRAWITEM（ODT_STATIC）：画浅蓝圆底 + 深蓝问号。
// 窗口类背景是 COLOR_WINDOW（白），故先用白刷擦底再画圆。
static void draw_help(const DRAWITEMSTRUCT* d) {
  RECT r = d->rcItem;
  FillRect(d->hDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
  HBRUSH fill = CreateSolidBrush(RGB(228, 235, 244));
  HPEN edge = CreatePen(PS_SOLID, 1, RGB(150, 172, 200));
  HGDIOBJ ob = SelectObject(d->hDC, fill);
  HGDIOBJ op = SelectObject(d->hDC, edge);
  Ellipse(d->hDC, r.left, r.top, r.right, r.bottom);
  SelectObject(d->hDC, ob);
  SelectObject(d->hDC, op);
  DeleteObject(fill);
  DeleteObject(edge);
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, RGB(45, 78, 120));
  HGDIOBJ of = SelectObject(d->hDC, g_font_help);
  DrawTextW(d->hDC, L"?", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(d->hDC, of);
}

// ── 下载模型（2026-10-01 用户定案：功能两版统一，从安装器移到配置 GUI）──────
// 与插件版逐字同源：curl.exe（Win10 1803+ 自带）断点续传 -L -C - -o <目标>.download，
// 轮询分片大小打进度；完成后剥掉 .download 后缀转正；失败保留分片可重试续传。
static const wchar_t* const kModelUrl =
    L"https://modelscope.cn/models/unsloth/Qwen3.5-0.8B-GGUF/resolve/master/Qwen3.5-0.8B-Q4_K_M.gguf";
static HANDLE g_dl_proc = NULL;
static std::wstring g_dl_tmp, g_dl_target;
#define DL_TIMER_ID 1

static unsigned long long file_size_ull(const std::wstring& path) {
  unsigned long long sz = 0;
  return file_size(path, &sz) ? sz : 0;
}

// ② 上次遗留的 curl（关窗没杀干净 / 异常退出）会和新的一次同时写同一个分片。
// 每次起下载前记 <分片>.pid；下次起下载时按它结束旧进程（分片保留，续传接管）。
static bool terminate_pid_if_curl(DWORD pid) {
  if (!pid) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
  if (!h) return false;
  wchar_t path[MAX_PATH] = L"";
  DWORD n = MAX_PATH;
  bool is_curl = QueryFullProcessImageNameW(h, 0, path, &n) && wcsstr(path, L"curl.exe");
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  bool killed = false;
  if (is_curl && alive) killed = TerminateProcess(h, 1) != 0;
  CloseHandle(h);
  return killed;
}

static bool kill_stale_curl(const std::wstring& tmp) {
  std::wstring pf = tmp + L".pid";
  FILE* f = NULL;
  _wfopen_s(&f, pf.c_str(), L"rb");
  if (!f) return false;
  char buf[64] = "";
  size_t got = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (!got) return false;
  DWORD pid = (DWORD)strtoul(buf, NULL, 10);
  DeleteFileW(pf.c_str());
  return terminate_pid_if_curl(pid);
}

static void write_dl_pid(DWORD pid) {
  if (g_dl_tmp.empty()) return;
  FILE* f = NULL;
  _wfopen_s(&f, (g_dl_tmp + L".pid").c_str(), L"wb");
  if (!f) return;
  fprintf(f, "%lu", (unsigned long)pid);
  fclose(f);
}

static void download_finish(bool ok, DWORD code) {
  if (g_dl_proc) { CloseHandle(g_dl_proc); g_dl_proc = NULL; }
  KillTimer(g_hwnd, DL_TIMER_ID);
  if (!g_dl_tmp.empty()) DeleteFileW((g_dl_tmp + L".pid").c_str());
  if (ok) {
    // >100MB 才认（与插件版同一判据）：避免把错误页面/半截文件当模型转正
    unsigned long long sz = file_size_ull(g_dl_tmp);
    if (sz > 100ULL * 1024 * 1024 && MoveFileExW(g_dl_tmp.c_str(), g_dl_target.c_str(),
                                                MOVEFILE_REPLACE_EXISTING)) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"模型下载完成（%llu MB）→ %s",
                 sz >> 20, g_dl_target.c_str());
      log_error(L"[下载] 完成：%s（%llu MB）", g_dl_target.c_str(), sz >> 20);
    } else {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS),
                 L"[失败] 下载内容异常（%llu MB）或转正失败，分片保留可重试续传",
                 sz >> 20);
      log_error(L"[下载] 内容异常或转正失败：%s（%llu 字节）", g_dl_tmp.c_str(), sz);
    }
  } else {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS),
               L"[失败] 模型下载失败（curl 退出码 %lu）——分片已保留，再点可续传；"
               L"或手动下载：%s", code, kModelUrl);
    log_error(L"[下载] curl 退出码 %lu；分片保留：%s", code, g_dl_tmp.c_str());
  }
  refresh_model_status();
}

static void on_download_model() {
  if (g_dl_proc) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已有下载在进行中…");
    return;
  }
  wchar_t buf[512];
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  std::wstring target = buf;
  if (target.empty()) {   // 留空 = 用户文件夹里的默认名（与 llm_filter 兜底一致）
    std::wstring dir = rime_user_dir();
    if (dir.empty()) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"[失败] 无法定位用户文件夹（APPDATA 缺失）");
      log_error(L"[下载] APPDATA 缺失，无法定默认落点");
      return;
    }
    target = dir + L"\\Qwen3.5-0.8B-Q4_K_M.gguf";
  }
  for (wchar_t* c = &target[0]; *c; ++c)
    if (*c == L'/') *c = L'\\';
  unsigned long long exist = 0;
  if (file_size(target, &exist)) {
    if (exist >= kModelMinBytes) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"模型已存在（%llu MB），无需下载：%s",
                 exist >> 20, target.c_str());
      return;
    }
    // ① 小于完整体量 = 疑似半截/损坏：不再当成"已存在"跳过，确认后删除重下
    wchar_t q[768];
    swprintf_s(q, L"目标路径已有文件，但只有 %llu MB（疑似未下完或损坏）：\n%s\n\n"
                  L"删除它并重新下载？", exist >> 20, target.c_str());
    if (MessageBoxW(g_hwnd, q, L"模型文件可疑", MB_YESNO | MB_ICONWARNING) != IDYES) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已取消下载（保留现有文件）");
      return;
    }
    if (!DeleteFileW(target.c_str())) {
      set_status(GetDlgItem(g_hwnd, IDC_STATUS),
                 L"[失败] 无法删除可疑文件（可能被占用）：%s", target.c_str());
      log_error(L"[下载] 删除可疑文件失败：%s（err=%lu）", target.c_str(), GetLastError());
      return;
    }
  }
  size_t slash = target.find_last_of(L'\\');
  if (slash != std::wstring::npos) CreateDirectoryW(target.substr(0, slash).c_str(), NULL);
  g_dl_target = target;
  g_dl_tmp = target + L".download";
  // ② 同目标还有上次遗留的 curl（关窗时旧版没杀干净 / 异常退出）→ 结束它再接管，
  //    否则同一分片会有两个写入者（断点续传也救不了）
  if (kill_stale_curl(g_dl_tmp))
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已结束上次遗留的下载进程，由本次续传接管…");
  std::wstring cmd = L"curl.exe -L -C - -s -S -o \"" + g_dl_tmp + L"\" \"" + kModelUrl + L"\"";
  std::vector<wchar_t> cl(cmd.begin(), cmd.end());
  cl.push_back(0);
  STARTUPINFOW si = {sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cl.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                      &si, &pi)) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS),
               L"[失败] 无法启动 curl.exe（Win10 1803+ 自带）——可手动下载：%s", kModelUrl);
    log_error(L"[下载] CreateProcess(curl.exe) 失败：%lu", GetLastError());
    return;
  }
  CloseHandle(pi.hThread);
  g_dl_proc = pi.hProcess;
  write_dl_pid(pi.dwProcessId);
  SetTimer(g_hwnd, DL_TIMER_ID, 700, NULL);
  set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"开始下载模型 → %s（断点续传）",
             target.c_str());
}

// 进度轮询（下载期间每秒级刷新状态行；结束时收尾）
static void on_download_tick() {
  if (!g_dl_proc) { KillTimer(g_hwnd, DL_TIMER_ID); return; }
  DWORD code = 0;
  if (WaitForSingleObject(g_dl_proc, 0) == WAIT_OBJECT_0) {
    GetExitCodeProcess(g_dl_proc, &code);
    download_finish(code == 0, code);
    return;
  }
  set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"下载中… %llu MB（目标 %s，约 508 MB）",
             file_size_ull(g_dl_tmp) >> 20, g_dl_target.c_str());
}

static void on_browse() {
  wchar_t buf[512];
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  // 打开对话框对正斜杠初始路径报 FNERR_INVALIDFILENAME(0x3002) 静默失败——
  // 统一为反斜杠（显示/保存/yaml 随之一致，Windows API 两者都接受）
  for (wchar_t* c = buf; *c; ++c)
    if (*c == L'/') *c = L'\\';
  SetDlgItemTextW(g_hwnd, IDC_MODEL, buf);
  OPENFILENAMEW ofn = {sizeof(ofn)};
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"GGUF 模型 (*.gguf)\0*.gguf\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 512;
  // 选已有模型文件用打开对话框（原 Save 对话框选现有文件会弹覆盖确认）
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (GetOpenFileNameW(&ofn)) {
    SetDlgItemTextW(g_hwnd, IDC_MODEL, buf);
    refresh_model_status();
    return;
  }
  // 打开失败：状态行给出 CommDlg 错误码，便于用户机排查
  wchar_t msg[64];
  swprintf_s(msg, L"打开对话框失败（CommDlg 错误码 %lu）",
             CommDlgExtendedError());
  set_status(GetDlgItem(g_hwnd, IDC_STATUS), msg);
  log_error(L"%s", msg);
}

static void on_open_dir() {
  std::wstring dir = rime_user_dir();
  if (dir.empty()) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"无法定位用户文件夹（APPDATA 缺失）");
    log_error(L"打开用户文件夹：APPDATA 缺失");
    return;
  }
  CreateDirectoryW(dir.c_str(), NULL);
  ShellExecuteW(g_hwnd, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

// 实际消息处理（外面再套一层 WndProc 捕获异常 → 写错误日志）
static LRESULT CALLBACK WndProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_hwnd = h;
      make_ui();
      scan_schemas();        // 填方案下拉（默认选第一个）
      load_schema_params();  // 读该方案的 llm_rerank 节 → 界面
      // 内容高由 make_ui() 算出 → 此刻按它校正窗口尺寸（设计宽 620 + 内容高）
      if (g_ui_height > 0) {
        RECT cr = {0, 0, ts(620), g_ui_height};
        AdjustWindowRect(&cr, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), FALSE);
        SetWindowPos(h, NULL, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      // 忘关提醒（2026-09-04）：诊断开着会持续写盘，打开设置时点一下
      if (g_p.debug_fusion)
        set_status(GetDlgItem(h, IDC_STATUS),
                   L"提醒：诊断日志开着（rime_llm_debug.txt 持续增长，排障完建议关闭）");
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == IDC_MODEL &&
          (HIWORD(wp) == CBN_EDITUPDATE || HIWORD(wp) == CBN_SELCHANGE)) {
        refresh_model_status();  // 手输/下拉选择/浏览返回即刷新
        return 0;
      }
      // 切换方案 = 切换配置（配置节在方案里，2026-09-30 定案）
      if (LOWORD(wp) == IDC_SCHEMA && HIWORD(wp) == CBN_SELCHANGE) {
        if (g_sel_guard) return 0;
        // ④ 有未保存改动先问；选"否"把下拉拨回已加载的那一项，界面值原样保留
        if (ui_dirty() && MessageBoxW(g_hwnd,
                L"当前方案有未保存的改动。\n\n切换方案会丢弃这些改动，继续吗？",
                L"未保存的改动", MB_YESNO | MB_ICONWARNING) != IDYES) {
          // 按**名字**拨回而不是按序号：列表可能刚被重扫过（序号会错位）
          g_sel_guard = true;
          if (!g_loaded_name.empty()) select_schema_named(g_loaded_name);
          g_sel_guard = false;
          set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"已取消切换（保留未保存的改动）");
          return 0;
        }
        load_schema_params();
        return 0;
      }
      if (LOWORD(wp) == IDC_SCHEMA && HIWORD(wp) == CBN_DROPDOWN) {
        // 打开下拉即重扫两处方案列表（外部新增/删除方案无需手动刷新）；
        // 界面没有未保存改动时顺带重读当前文件（吸收外部编辑），有改动则只重扫列表
        scan_schemas();
        if (!ui_dirty()) load_schema_params();
        return 0;
      }
      switch (LOWORD(wp)) {
        case IDC_BROWSE: on_browse(); return 0;
        case IDC_DOWNLOAD: on_download_model(); return 0;
        case IDC_OPENDIR: on_open_dir(); return 0;
        case IDC_SCHEMAREM: on_schema_remove(); return 0;
        case IDC_SAVE: save_schema_params(); return 0;
        case IDC_CLOSE: DestroyWindow(h); return 0;
        case IDCANCEL:            // IsDialogMessage 把 ESC 映射为 IDCANCEL
          DestroyWindow(h);
          return 0;
      }
      break;
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
      if (d && d->CtlType == ODT_STATIC) {  // 只有「?」徽标是 owner-draw static
        draw_help(d);
        return TRUE;
      }
      break;
    }
    case WM_TIMER:
      if (wp == DL_TIMER_ID) { on_download_tick(); return 0; }
      break;
    case WM_DESTROY:
      KillTimer(h, DL_TIMER_ID);
      // ② 关窗连带结束 curl：它每次都是新进程，留着会和下次下载同时写同一个分片。
      //    分片本身保留 → 下次点『下载模型』照样断点续传。
      if (g_dl_proc) { TerminateProcess(g_dl_proc, 0); CloseHandle(g_dl_proc); g_dl_proc = NULL; }
      if (!g_dl_tmp.empty()) kill_stale_curl(g_dl_tmp);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// 顶层异常兜底（2026-09-30：界面不留日志框，异常写 WeaselLLMSetup_error.log）
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  try {
    return WndProcInner(h, msg, wp, lp);
  } catch (const std::exception& e) {
    log_error(L"GUI 未捕获异常（msg=0x%04X）：%S", (unsigned)msg, e.what());
  } catch (...) {
    log_error(L"GUI 未捕获异常（msg=0x%04X）：未知类型", (unsigned)msg);
  }
  set_status(GetDlgItem(h, IDC_STATUS),
             L"[失败] 界面异常，已写错误日志：WeaselLLMSetup_error.log");
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  // 主线程 STA：Vista+ 打开/保存对话框内部走 COM，缺初始化会静默失败
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  // 悬停提示（tooltips_class32）需要初始化 common controls
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"WeaselLLMSetup";
  RegisterClassW(&wc);
  // 初始尺寸随意（WM_CREATE 里按 g_ui_height 校正）
  HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"WeaselLLMSetup",
                           L"LLM 重排设置 — 小狼毫", WS_OVERLAPPEDWINDOW &
                               ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, ts(620), ts(400), NULL, NULL,
                           inst, NULL);
  ShowWindow(h, show);
  UpdateWindow(h);
  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  CoUninitialize();
  return 0;
}
