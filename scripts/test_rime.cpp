// test_rime.cpp - minimal librime smoke test (LoadLibrary + GetProcAddress)
#include <cstdio>
#include <windows.h>
#include "rime_api.h"  // for RimeApi struct layout only

int main(int argc, char** argv) {
  const char* dll = (argc > 1) ? argv[1] : "D:\\rime-llm-ime\\bin\\rime.dll";
  HMODULE h = LoadLibraryA(dll);
  if (!h) {
    printf("LoadLibrary FAILED err=%lu\n", GetLastError());
    return 1;
  }
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
