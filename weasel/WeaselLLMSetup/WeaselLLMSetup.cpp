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
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>

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
#define IDC_SCHEMAREF    1032
#define IDC_SCHEMAADD    1033
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
static HWND g_hwnd;
static HFONT g_font;
static HFONT g_font_bold;  // 分节标题

// ---- 工具 ----
static std::wstring yaml_path() {
  wchar_t dir[MAX_PATH];
  if (!GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH))
    return std::wstring();
  return std::wstring(dir) + L"\\Rime";
}

static std::wstring default_model_path() {
  // 默认 = RIME 用户文件夹根（2026-08-31 用户澄清：指小狼毫右键的用户
  // 文件夹——方案配置所在处，模型直接放根、不套子文件夹；8-27 曾误用
  // %USERPROFILE%\gguf_models\。自定义位置在 GUI/schema 里显式填）
  wchar_t dir[MAX_PATH];
  if (GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH))
    return std::wstring(dir) + L"\\Rime\\Qwen3.5-0.8B-Q4_K_M.gguf";
  return L"Qwen3.5-0.8B-Q4_K_M.gguf";
}

static std::wstring shown_model() {
  return g_p.model_path.empty() ? default_model_path() : g_p.model_path;
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

// ---- llm_rerank.yaml 读写（与 llm_filter.cc 的扁平解析同构）----
static void load_params() {
  std::wstring dir = yaml_path();
  if (dir.empty()) return;
  FILE* f = NULL;
  _wfopen_s(&f, (dir + L"\\llm_rerank.yaml").c_str(), L"rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    std::string ln = trim(line);
    if (ln.empty() || ln[0] == '#') continue;
    size_t c = ln.find(':');
    if (c == std::string::npos) continue;
    std::string key = trim(ln.substr(0, c));
    std::string val = trim(ln.substr(c + 1));
    // 双引号（历史 model_path 风格）与单引号（code_pattern 正则风格，2026-09-30）
    // 都要去壳——只认双引号会让正则值带壳
    if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
      size_t e = val.find(val[0], 1);
      val = (e == std::string::npos) ? val.substr(1) : val.substr(1, e - 1);
    } else {
      size_t h = val.find('#');
      if (h != std::string::npos) val = trim(val.substr(0, h));
    }
    if (key == "enabled") g_p.enabled = (val == "true");
    else if (key == "code_pattern") g_p.code_pattern = val;
    // 旧键 min_code_len / max_code_len 有意忽略（触发条件已改为 code_pattern，
    // 2026-09-30）：保存时自然消失；等价换算 min=4,max=0 ⇒ '.{4}'
    else if (key == "min_code_len") { /* 旧键忽略 */ }
    else if (key == "max_code_len") { /* 旧键忽略 */ }
    else if (key == "expected_length_weight") g_p.elw = atof(val.c_str());
    else if (key == "freq_beta") g_p.freq_beta = atof(val.c_str());
    // 旧行 "min_tokens" 有意忽略（用户配置面移除，固定 C++ 默认 1，2026-09-30）
    else if (key == "max_tokens") g_p.max_tokens = atoi(val.c_str());
    else if (key == "max_candidates") g_p.max_candidates = atoi(val.c_str());
    else if (key == "cpu_cores") g_p.cpu_cores = atoi(val.c_str());
    else if (key == "debug_fusion") g_p.debug_fusion = (val == "true");
    else if (key == "model_path") g_p.model_path = utf8_to_wide(val);
  }
  fclose(f);
}

static bool save_params() {
  std::wstring dir = yaml_path();
  if (dir.empty()) return false;
  CreateDirectoryW(dir.c_str(), NULL);
  std::wstring model = shown_model();
  char buf[64];
  std::string out;
  out += "# LLM 重排全局配置（WeaselLLMSetup 写入；llm_filter 热重载即时生效）\n";
  out += "# 优先级：方案内 llm_rerank 节 > 本文件 > 内置默认\n";
  out += g_p.enabled ? "enabled: true\n" : "enabled: false\n";
  out += "code_pattern: " + yaml_scalar(g_p.code_pattern) + "\n";
  sprintf_s(buf, "expected_length_weight: %.2f\n", g_p.elw); out += buf;
  sprintf_s(buf, "freq_beta: %.2f\n", g_p.freq_beta); out += buf;
  sprintf_s(buf, "max_tokens: %d\n", g_p.max_tokens); out += buf;
  sprintf_s(buf, "max_candidates: %d\n", g_p.max_candidates); out += buf;
  sprintf_s(buf, "cpu_cores: %d\n", g_p.cpu_cores); out += buf;
  out += g_p.debug_fusion ? "debug_fusion: true\n" : "debug_fusion: false\n";
  out += "model_path: " + wide_to_utf8(model) + "\n";
  std::wstring tmp = dir + L"\\llm_rerank.yaml.tmp";
  FILE* f = NULL;
  _wfopen_s(&f, tmp.c_str(), L"wb");
  if (!f) return false;
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  // 原子替换：热重载按 mtime|size 指纹感知；tmp 同目录保证同卷 rename
  if (!MoveFileExW(tmp.c_str(), (dir + L"\\llm_rerank.yaml").c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
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

// ---- 模型路径下拉框：扫描常见位置的 .gguf（Rime 用户目录 = 默认下载
// 落点；%USERPROFILE%\gguf_models = 插件版约定位置——从插件版迁移的
// 用户模型已在盘上，下拉选一下即可，不必重新下载）----
static void scan_models(HWND combo) {
  wchar_t cur[512];
  GetWindowTextW(combo, cur, 512);
  wchar_t base[MAX_PATH];
  std::wstring dirs[2];
  if (GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH))
    dirs[0] = std::wstring(base) + L"\\Rime";
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
static void refresh_model_status() {
  wchar_t buf[512];
  GetDlgItemTextW(g_hwnd, IDC_MODEL, buf, 512);
  unsigned long long sz = 0;
  if (buf[0] && file_size(buf, &sz))
    set_status(GetDlgItem(g_hwnd, IDC_MSTATUS), L"模型已就绪：%s（%llu MB）",
               buf, sz >> 20);
  else
    set_status(GetDlgItem(g_hwnd, IDC_MSTATUS),
               L"模型文件不存在——重跑安装包可选择下载，或点“浏览…”选已有"
               L" .gguf 文件");
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
    return;
  }
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  wchar_t cmd[MAX_PATH] = L"";
  wcscpy_s(cmd, deployer.c_str());
  if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_TIMEOUT)
      set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
                 L"重新部署仍在后台进行；若候选异常请托盘手动重新部署");
    CloseHandle(pi.hProcess);
  } else {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[提示] 自动重新部署失败，请手动：托盘小狼毫 → 重新部署");
  }
}

static void scan_schemas() {
  HWND combo = GetDlgItem(g_hwnd, IDC_SCHEMA);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  std::wstring dir = yaml_path();
  if (dir.empty()) return;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.schema.yaml").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0)
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

static std::wstring selected_schema_path(bool* ok) {
  wchar_t name[MAX_PATH];
  if (GetDlgItemTextW(g_hwnd, IDC_SCHEMA, name, MAX_PATH) && name[0]) {
    *ok = true;
    return yaml_path() + L"\\" + name;
  }
  *ok = false;
  return std::wstring();
}

static void on_schema_add() {
  bool ok;
  std::wstring path = selected_schema_path(&ok);
  if (!ok) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"请先在下拉框选择方案文件（%APPDATA%\\Rime\\*.schema.yaml）");
    return;
  }
  bool had_bom;
  std::vector<std::string> lines;
  if (!read_lines(path, &had_bom, &lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"读取方案失败：%s", path.c_str());
    return;
  }
  int removed = strip_llm(&lines);
  std::wstring where;
  if (!insert_llm_filter(&lines, &where)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"[失败] 方案内未找到 engine/filters 块，无法插入组件");
    return;
  }
  if (!write_lines(path, had_bom, lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"写入方案失败：%s", path.c_str());
    return;
  }
  if (!write_lines(path, had_bom, lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"写入方案失败：%s", path.c_str());
    return;
  }
  wchar_t msg[512];
  swprintf_s(msg, L"已接入 llm_filter（%s）%s", where.c_str(),
             removed ? L"，旧 LLM 组件已剥离" : L"");
  set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), msg);
  trigger_redeploy();
}

static void on_schema_remove() {
  bool ok;
  std::wstring path = selected_schema_path(&ok);
  if (!ok) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"请先在下拉框选择方案文件（%APPDATA%\\Rime\\*.schema.yaml）");
    return;
  }
  bool had_bom;
  std::vector<std::string> lines;
  if (!read_lines(path, &had_bom, &lines)) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT), L"读取方案失败：%s", path.c_str());
    return;
  }
  int removed = strip_llm(&lines);
  if (!removed) {
    set_status(GetDlgItem(g_hwnd, IDC_SCHEMSTAT),
               L"未发现 LLM 组件，方案未改动");
    return;
  }
  write_lines(path, had_bom, lines);
  wchar_t msg[128];
  swprintf_s(msg, L"已移除 LLM 组件（含配置节）共 %d 行", removed);
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
// 叠字事故根因：勾选框 w430 与下行标签矩形相交 + 空状态静态框横贯
// 首行）。分节标题与每行参数独占一个水平带，行内 label 止于 edit x 前。
// 分组（2026-09-29，按用途）：总控（启用+模型）/ 触发条件（何时打分）/
// 推理规模（每次算多少）/ 候选排序融合（分数怎么合成）/ 排障（诊断）。
static void make_ui() {
  g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_font_bold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  // ── 总控（不加标题，自明）──────────────────────
  mk(0, L"启用 LLM 重排（保存后立即生效，无需重新部署）",
     BS_AUTOCHECKBOX | WS_TABSTOP, 15, 12, 470, 22, IDC_ENABLED);
  // 模型路径下拉框（可编辑：当前值 + 扫描到的 .gguf；高度含下拉列表，
  // 闭合时只占顶部 ~24px，展开覆盖下方是组合框固有行为）
  mk(1, L"模型路径:", 0, 15, 46, 68, 20, 0);
  mk(3, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP, 86, 41, 355, 200,
     IDC_MODEL);
  mk(0, L"浏览…", WS_TABSTOP, 447, 42, 56, 25, IDC_BROWSE);
  // 模型状态（文件存在性/大小；路径一变即刷新）
  mk(1, L"", 0, 15, 74, 539, 18, IDC_MSTATUS);
  // ── 方案接入（一次性；语义同 installer\schema_add.ps1）──
  HWND hdr = mk(1, L"方案接入 —— 写入选中方案并自动重新部署（幂等，跨版自动转换）",
                0, 15, 96, 539, 18, 0);
  SendMessageW(hdr, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
  mk(1, L"方案文件:", 0, 15, 120, 68, 20, 0);
  mk(3, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 86, 115, 240, 200, IDC_SCHEMA);
  mk(0, L"刷新", WS_TABSTOP, 332, 116, 56, 25, IDC_SCHEMAREF);
  mk(0, L"接入 LLM", WS_TABSTOP, 394, 116, 80, 25, IDC_SCHEMAADD);
  mk(0, L"剥离", WS_TABSTOP, 480, 116, 60, 25, IDC_SCHEMAREM);
  mk(1, L"", 0, 15, 146, 539, 18, IDC_SCHEMSTAT);
  // ── 触发条件 ─────────────────────────────────
  hdr = mk(1, L"触发条件 —— 何时打分", 0, 15, 172, 300, 18, 0);
  SendMessageW(hdr, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
  mk(1, L"编码匹配（正则，全串）", 0, 15, 196, 160, 20, 0);
  mk(2, L"", WS_BORDER | WS_TABSTOP, 180, 193, 150, 22, IDC_CODE_PAT);
  mk(1, L"4 码 .{4}｜4 码以上 .{4,}｜3-4 码 .{3,4}｜[abcde]{4}｜空 = 不限",
     0, 15, 218, 539, 18, 0);
  // ── 推理规模 ─────────────────────────────────
  hdr = mk(1, L"推理规模 —— 每次算多少、多快", 0, 15, 244, 340, 18, 0);
  SendMessageW(hdr, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
  mk(1, L"上文 token 上限", 0, 15, 268, 106, 20, 0);
  mk(2, L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 125, 265, 52, 22, IDC_MAX_TOK);
  mk(1, L"候选数上限", 0, 200, 268, 96, 20, 0);
  mk(2, L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 374, 265, 52, 22, IDC_MAX_CAND);
  mk(1, L"CPU 线程数", 0, 15, 298, 98, 20, 0);
  mk(2, L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, 125, 295, 52, 22, IDC_CORES);
  // ── 候选排序融合：公式两行，β/elw 挖空与作用项对齐（两框同列 x=127）──
  hdr = mk(1, L"候选排序融合 —— 分数怎么合成", 0, 15, 326, 380, 18, 0);
  SendMessageW(hdr, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
  mk(1, L"融合分 = score + ", 0, 15, 350, 108, 20, 0);
  mk(2, L"", WS_BORDER | WS_TABSTOP, 127, 347, 50, 22, IDC_FREQ_W);
  mk(1, L"·log(1+eff)", 0, 181, 350, 82, 20, 0);
  mk(1, L"+", SS_RIGHT, 15, 378, 108, 20, 0);
  mk(2, L"", WS_BORDER | WS_TABSTOP, 127, 375, 50, 22, IDC_ELW);
  mk(1, L"·span·匹配词长", 0, 181, 378, 130, 20, 0);
  mk(1, L"β = 词频系数（0=关闭）；elw = 预期词长权重（0=关闭）",
     0, 15, 406, 420, 18, 0);
  mk(1, L"elw 仅两码一字方案生效：词长=码长/2 的候选获得 span×elw 加成",
     0, 15, 426, 480, 18, 0);
  // ── 保存 / 状态 ──────────────────────────────
  mk(0, L"保存并生效", WS_TABSTOP | BS_DEFPUSHBUTTON, 15, 452, 110, 30,
     IDC_SAVE);
  mk(0, L"关闭", WS_TABSTOP, 133, 452, 70, 30, IDC_CLOSE);
  mk(0, L"打开用户文件夹", WS_TABSTOP, 440, 454, 114, 26, IDC_OPENDIR);
  mk(1, L"", 0, 213, 458, 215, 18, IDC_STATUS);
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  wchar_t note2[128];
  swprintf_s(note2,
             L"参数键名与 llm_rerank.yaml 相同；修改保存后立即生效（本机逻辑核 %lu）",
             si.dwNumberOfProcessors);
  mk(1, note2, 0, 15, 488, 539, 18, 0);
  // ── 排障 ────────────────────────────────────
  // 诊断开关（2026-09-04 GUI 化；此前仅 yaml 手改）：排障时逐步评分
  mk(0, L"诊断日志 debug_fusion（逐块评分明细写用户文件夹 rime_llm_debug.txt）",
     BS_AUTOCHECKBOX | WS_TABSTOP, 15, 512, 539, 22, IDC_DEBUG);
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
}

static void on_open_dir() {
  std::wstring dir = yaml_path();
  if (dir.empty()) {
    set_status(GetDlgItem(g_hwnd, IDC_STATUS), L"无法定位用户文件夹（APPDATA 缺失）");
    return;
  }
  CreateDirectoryW(dir.c_str(), NULL);
  ShellExecuteW(g_hwnd, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_hwnd = h;
      make_ui();
      load_params();
      params_to_ui();
      scan_schemas();
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
      switch (LOWORD(wp)) {
        case IDC_BROWSE: on_browse(); return 0;
        case IDC_OPENDIR: on_open_dir(); return 0;
        case IDC_SCHEMAREF: scan_schemas(); return 0;
        case IDC_SCHEMAADD: on_schema_add(); return 0;
        case IDC_SCHEMAREM: on_schema_remove(); return 0;
        case IDC_SAVE: {
          ui_to_params();
          if (save_params())
            set_status(GetDlgItem(h, IDC_STATUS), L"已保存，立即生效");
          else
            set_status(GetDlgItem(h, IDC_STATUS), L"保存失败（无法写入用户目录）");
          return 0;
        }
        case IDC_CLOSE: DestroyWindow(h); return 0;
        case IDCANCEL:            // IsDialogMessage 把 ESC 映射为 IDCANCEL
          DestroyWindow(h);
          return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  // 主线程 STA：Vista+ 打开/保存对话框内部走 COM，缺初始化会静默失败
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"WeaselLLMSetup";
  RegisterClassW(&wc);
  HWND h = CreateWindowExW(WS_EX_APPWINDOW, L"WeaselLLMSetup",
                           L"LLM 重排设置 — 小狼毫", WS_OVERLAPPEDWINDOW &
                               ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, 585, 582, NULL, NULL,
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
