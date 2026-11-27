// rime_cand_probe.cpp — 无头 rime 候选枚举探针（2026-11-23）
// 用途：给定共享数据目录（含 luna_quanpin/luna_pinyin 方案与词典）与临时用户
// 目录，部署后用真实引擎枚举某个全拼编码的全部候选（引擎原序），输出每码的
// 首次候选构造耗时与总枚举耗时 —— 用于①wiki 全拼候选位置分析；②引擎深枚举
// 定位（滑窗改造前后对比）。
//
// 用法：rime_cand_probe.exe [--cls] <rime.dll> <shared_data_dir>
//       <user_data_dir> [schema_id]（--cls = Q/P 行附 type/start-end/comp 类标）
//       从 stdin 逐行读编码（小写字母），每行输出：
//       Q\t<code>\t<begin_ms>\t<total_ms>\t<count>\t<text\x1ftext...>
//       空行 = 仅清空 composition。
// 源码纯 ASCII；候选文本按 UTF-8 字节原样输出。
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <windows.h>
#include "rime_api.h"

static RimeApi* g_api = nullptr;

static double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(
             steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  // --cls = 附带每候选的 type/start-end/comp 类标（需 2026-11-27 后的
  // rime.dll，导出 RimeGetCandidateCls；旧 dll 直接报错退出）。
  bool cls_mode = false;
  const char* args[4] = {0, 0, 0, 0};
  int nargs = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--cls") == 0) {
      cls_mode = true;
      continue;
    }
    if (nargs < 4)
      args[nargs++] = argv[i];
  }
  if (nargs < 3) {
    fprintf(stderr,
            "usage: rime_cand_probe [--cls] <rime.dll> <shared_data_dir> "
            "<user_data_dir> [schema_id]\n");
    return 2;
  }
  const char* dll = args[0];
  const char* shared = args[1];
  const char* user = args[2];
  const char* schema = (nargs > 3) ? args[3] : "luna_quanpin";

  HMODULE h = LoadLibraryA(dll);
  if (!h) {
    fprintf(stderr, "LoadLibrary failed err=%lu (dll=%s)\n", GetLastError(), dll);
    return 1;
  }
  fprintf(stderr, "[probe] dll loaded\n");
  RimeApi* (*get_api)() = (RimeApi * (*)())GetProcAddress(h, "rime_get_api");
  if (!get_api) {
    fprintf(stderr, "GetProcAddress(rime_get_api) failed\n");
    return 1;
  }
  RimeApi* api = get_api();
  g_api = api;

  typedef Bool (*GetClsFn)(RimeSessionId, int, char*, size_t, int*, int*,
                           int*);
  GetClsFn get_cls = nullptr;
  if (cls_mode) {
    get_cls = (GetClsFn)GetProcAddress(h, "RimeGetCandidateCls");
    if (!get_cls) {
      fprintf(stderr, "[probe] --cls: RimeGetCandidateCls not exported "
                      "(dll predates 2026-11-27 build)\n");
      return 1;
    }
    fprintf(stderr, "[probe] --cls on\n");
  }

  RimeTraits traits = {};
  RIME_STRUCT_INIT(RimeTraits, traits);
  traits.shared_data_dir = shared;
  traits.user_data_dir = user;
  traits.distribution_name = "Rime";
  traits.distribution_code_name = "weasel";
  traits.distribution_version = "0.17.4";
  traits.app_name = "rime.cand_probe";
  traits.min_log_level = 2;  // ERROR：部署/引擎噪音不进 stderr
  bool null_init = (strcmp(shared, "-") == 0);
  if (null_init) {
    // 默认路径模式（%APPDATA%\\Rime 已部署好目标方案时使用）——
    // 与 test_rime.exe 同路径，绕过自定义 traits 初始化
    fprintf(stderr, "[probe] initialize(default)\n");
    api->initialize(nullptr);
  } else {
    fprintf(stderr, "[probe] initializing shared=%s user=%s\n", shared, user);
    api->initialize(&traits);
  }
  fprintf(stderr, "[probe] initialized\n");
  if (api->get_shared_data_dir)
    fprintf(stderr, "[probe] shared=%s\n", api->get_shared_data_dir());
  if (api->get_user_data_dir)
    fprintf(stderr, "[probe] user=%s\n", api->get_user_data_dir());

  if (!null_init && api->start_maintenance) {
    fprintf(stderr, "[probe] maintenance start\n");
    api->start_maintenance(True);
    api->join_maintenance_thread();
    fprintf(stderr, "[probe] maintenance done\n");
  }

  RimeSessionId sid = api->create_session();
  if (!sid) {
    fprintf(stderr, "create_session failed\n");
    return 1;
  }
  Bool sel = api->select_schema(sid, schema);
  fprintf(stderr, "select_schema(%s)=%d\n", schema, (int)sel);
  char cur[256] = {0};
  api->get_current_schema(sid, cur, sizeof(cur));
  fprintf(stderr, "schema=%s\n", cur);
  {
    RimeStatus st;
    RIME_STRUCT_INIT(RimeStatus, st);
    if (api->get_status(sid, &st)) {
      fprintf(stderr, "[probe] status disabled=%d ascii=%d composing=%d "
              "id='%s' name='%s'\n",
              (int)st.is_disabled, (int)st.is_ascii_mode,
              (int)st.is_composing, st.schema_id ? st.schema_id : "",
              st.schema_name ? st.schema_name : "");
      if (api->free_status) api->free_status(&st);
    }
  }

  char line[512];
  while (fgets(line, sizeof(line), stdin)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    // 清 composition（上一码可能残留）
    api->process_key(sid, 0xFF1B /*Escape*/, 0);
    if (n == 0) {
      printf("Q\t\t0\t0\t0\t0\t\n");
      fflush(stdout);
      continue;
    }
    // '词码>目标词' = 只查目标在候选序中的位置（找到即停，快）
    {
      char* tab = strchr(line, '>');
      if (tab) {
        *tab = 0;
        const char* code = line;
        const char* target = tab + 1;
        double q0 = now_ms();
        if (api->simulate_key_sequence) api->simulate_key_sequence(sid, code);
        else {
          for (const char* p = code; *p; ++p)
            api->process_key(sid, (int)(unsigned char)*p, 0);
        }
        double q1 = now_ms();
        int pos = -1, cnt = 0;
        RimeCandidateListIterator it2;
        memset(&it2, 0, sizeof(it2));
        if (api->candidate_list_begin(sid, &it2)) {
          while (api->candidate_list_next(&it2)) {
            if (it2.candidate.text) {
              if (strcmp(it2.candidate.text, target) == 0) {
                pos = cnt;
                cnt++;
                break;
              }
              cnt++;
            }
            if (cnt >= 400) break;
          }
          api->candidate_list_end(&it2);
        }
        double q2 = now_ms();
        if (get_cls && pos >= 0) {
          char tb[64];
          int cs = -1, ce = -1, cp = -1;
          if (get_cls(sid, pos, tb, sizeof(tb), &cs, &ce, &cp)) {
            printf("P\t%s\t%d\t%d\t%.2f\t%.2f\t%s/%d-%d/%d\n", code, pos, cnt,
                   q1 - q0, q2 - q0, tb, cs, ce, cp);
            fflush(stdout);
            continue;
          }
        }
        printf("P\t%s\t%d\t%d\t%.2f\t%.2f\n", code, pos, cnt, q1 - q0,
               q2 - q0);
        fflush(stdout);
        continue;
      }
    }
    // '!词码' = 打码后空格上屏（建立上下文，供后续 Q 触发 LLM Collect）
    if (line[0] == '!') {
      const char* code = line + 1;
      if (api->simulate_key_sequence) api->simulate_key_sequence(sid, code);
      else {
        for (const char* p = code; *p; ++p)
          api->process_key(sid, (int)(unsigned char)*p, 0);
      }
      api->process_key(sid, 0x20 /*space*/, 0);
      api->process_key(sid, 0xFF1B, 0);
      printf("C\t%s\n", code);
      fflush(stdout);
      continue;
    }
    // 用 simulate_key_sequence（如有）——按 key_sequence 常规解析，等价
    // 于真实逐键；缺失则回退逐键 process_key。
    // ⚠ 2026-11-23 修：原实现二者都跑（输入被打两遍 → "wo" 的候选变成
    // "窝窝/我我"，Q 行耗时/候选数全部对翻倍码串），改为二选一。
    double t0 = now_ms();
    int keys_ok = 0;
    if (api->simulate_key_sequence) {
      api->simulate_key_sequence(sid, line);
      keys_ok = (int)n;
    } else {
      for (size_t i = 0; i < n; i++) {
        if (api->process_key(sid, (int)(unsigned char)line[i], 0)) keys_ok++;
      }
    }
    double t1 = now_ms();
    {
      RimeContext rctx;
      RIME_STRUCT_INIT(RimeContext, rctx);
      if (api->get_context(sid, &rctx)) {
        fprintf(stderr, "[probe] preedit='%s' composing=%d keys_ok=%d/%d\n",
                rctx.composition.preedit ? rctx.composition.preedit : "",
                (int)rctx.composition.length, keys_ok, (int)n);
        if (api->free_context) api->free_context(&rctx);
      }
    }

    std::string out;
    int count = 0;
    double t10 = 0.0;
    RimeCandidateListIterator it;
    memset(&it, 0, sizeof(it));
    if (api->candidate_list_begin(sid, &it)) {
      while (api->candidate_list_next(&it)) {
        if (count == 10) t10 = now_ms();
        if (it.candidate.text) {
          if (!out.empty()) out.push_back('\x1f');
          // 协议字段内的 TAB/换行替换为空格（候选词不含这些，防御性）
          for (const char* p = it.candidate.text; *p; ++p) {
            if (*p == '\t' || *p == '\n' || *p == '\r') out.push_back(' ');
            else out.push_back(*p);
          }
          count++;
        }
        if (count >= 2000) break;   // 安全上限
      }
      api->candidate_list_end(&it);
    }
    double t2 = now_ms();
    if (t10 == 0.0) t10 = t2;
    if (get_cls) {
      // 类标字段：type/start-end/comp，条目间同 \x1f，与候选一一对应
      std::string cls_out;
      for (int i = 0; i < count; ++i) {
        char tb[64];
        int cs = -1, ce = -1, cp = -1;
        if (!cls_out.empty())
          cls_out.push_back('\x1f');
        if (get_cls(sid, i, tb, sizeof(tb), &cs, &ce, &cp)) {
          cls_out += tb;
          char span[64];
          snprintf(span, sizeof(span), "/%d-%d/%d", cs, ce, cp);
          cls_out += span;
        } else {
          cls_out.push_back('?');
        }
      }
      printf("Q\t%s\t%.2f\t%.2f\t%.2f\t%d\t%s\t%s\n", line, t1 - t0, t10 - t0,
             t2 - t0, count, out.c_str(), cls_out.c_str());
    } else {
      printf("Q\t%s\t%.2f\t%.2f\t%.2f\t%d\t%s\n", line, t1 - t0, t10 - t0,
             t2 - t0, count, out.c_str());
    }
    fflush(stdout);
  }

  api->destroy_session(sid);
  api->finalize();
  return 0;
}
