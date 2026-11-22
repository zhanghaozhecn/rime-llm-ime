// symresolve.c — RVA -> 符号名（DbgHelp + rime.pdb）
// 用法: symresolve.exe <rime.dll全路径> <pdb目录> <rva_hex> [rva_hex ...]
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#pragma comment(lib, "dbghelp.lib")

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: %s <rime.dll> <pdbdir> <rva_hex>...\n", argv[0]);
        return 1;
    }
    const DWORD64 BASE = 0x180000000;   // rime.dll 64 位默认镜像基址
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(GetCurrentProcess(), argv[2], FALSE)) {
        printf("SymInitialize failed %lu\n", GetLastError());
        return 1;
    }
    if (!SymInitialize(GetCurrentProcess(), argv[2], FALSE)) {
        printf("SymInitialize failed %lu\n", GetLastError());
        return 1;
    }
    DWORD64 mod = SymLoadModuleEx(GetCurrentProcess(), NULL, argv[1], NULL,
                                  BASE, 0, NULL, 0);
    if (!mod) {
        printf("SymLoadModuleEx(full) failed %lu\n", GetLastError());
        SetLastError(0);
        mod = SymLoadModuleEx(GetCurrentProcess(), NULL, "rime.dll", NULL,
                              BASE, 0, NULL, 0);
        if (!mod) {
            printf("SymLoadModuleEx(name) failed %lu\n", GetLastError());
            return 1;
        }
    }
    IMAGEHLP_MODULEW64 mi = {0};
    mi.SizeOfStruct = sizeof(mi);
    if (SymGetModuleInfoW64(GetCurrentProcess(), BASE, &mi)) {
        printf("module: SymType=%d LoadedPdbName=%ls\n",
               (int)mi.SymType, mi.LoadedPdbName);
    } else {
        printf("SymGetModuleInfoW64 failed %lu\n", GetLastError());
    }
    for (int i = 3; i < argc; i++) {
        DWORD64 rva = strtoull(argv[i], NULL, 16);
        char buf[sizeof(SYMBOL_INFO) + 512] = {0};
        SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 511;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), BASE + rva, &disp, si))
            printf("rva=0x%llx -> %s +0x%llx\n", rva, si->Name, disp);
        else
            printf("rva=0x%llx -> ? (err %lu)\n", rva, GetLastError());
    }
    return 0;
}
