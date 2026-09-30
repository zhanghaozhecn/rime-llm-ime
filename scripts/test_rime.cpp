// test_rime.cpp - minimal librime smoke test (LoadLibrary + GetProcAddress)
// 运行前把 cwd 切到 exe 所在目录（bin\），默认按**相对路径**加载 .\rime.dll；
// 也可用 argv[1] 指定任意 rime.dll（绝对/相对皆可）——2026-09-30 去绝对路径。
#include <cstdio>
#include <windows.h>
#include "rime_api.h"  // for RimeApi struct layout only

/* 把工作目录切到 exe 所在目录（bin\）——相对路径基准，仓库搬目录后照样跑 */
static void chdir_to_exe_dir(void) {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  char* slash = strrchr(path, '\\');
  if (slash) { *slash = '\0'; SetCurrentDirectoryA(path); }
}

int main(int argc, char** argv) {
  chdir_to_exe_dir();   /* cwd = exe 所在目录（bin\ 或 scripts\，相对路径基准） */
  /* 默认先找同目录 rime.dll；exe 落在 scripts\ 时回退 ..\bin\rime.dll（2026-09-30） */
  const char* dll = (argc > 1) ? argv[1] : "rime.dll";
  HMODULE h = LoadLibraryA(dll);
  if (!h && argc <= 1) {
    dll = "..\\bin\\rime.dll";
    h = LoadLibraryA(dll);
  }
  if (!h) {
    printf("LoadLibrary FAILED err=%lu (dll=%s)\n", GetLastError(), dll);
    return 1;
  }
  printf("loaded=%s\n", dll);
  RimeApi* (*get_api)() = (RimeApi * (*)())GetProcAddress(h, "rime_get_api");
  if (!get_api) {
    printf("GetProcAddress(rime_get_api) FAILED\n");
    return 1;
  }
  RimeApi* api = get_api();
  if (!api) {
    printf("rime_get_api returned null\n");
    return 1;
  }
  printf("version=%s\n", api->get_version());
  api->initialize(NULL);
  RimeSessionId sid = api->create_session();
  printf("create_session=%d\n", (int)sid);
  if (sid) {
    char schema_id[256] = {0};
    api->get_current_schema(sid, schema_id, sizeof(schema_id));
    printf("schema=%s\n", schema_id);
    Bool sel = api->select_schema(sid, "pdsp");
    printf("select_schema(pdsp)=%d\n", (int)sel);
    api->destroy_session(sid);
    RimeSessionId sid2 = api->create_session();
    printf("create_session2=%d\n", (int)sid2);
    if (sid2) {
      api->select_schema(sid2, "pdsp");
      char schema_id2[256] = {0};
      api->get_current_schema(sid2, schema_id2, sizeof(schema_id2));
      printf("schema2=%s\n", schema_id2);
      api->destroy_session(sid2);
    }
  }
  api->finalize();
  return 0;
}
