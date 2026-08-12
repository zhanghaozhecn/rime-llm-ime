# rime-llm-ime — 基于 LLM 候选重排的输入法

将 LLM 候选重排**源码级集成**进 RIME 小狼毫（weasel + librime）的中文输入方案：不需要外挂进程或 lua 插件，`llm_filter` 作为 librime 原生 filter 组件（C++）编译进 `rime.dll`，TSF 原生采集光标上文。

- **开发者**：仓库含修改后的完整 weasel + librime 源码树，可按本 README 构建
- **普通用户**：`bin/` 提供预编译产物，替换小狼毫安装文件即可使用（需自行下载 GGUF 模型）

## 特性

| 特性 | 说明 |
|------|------|
| 原生 filter 组件 | `llm_filter`（gear/llm_filter.{h,cc}）编译进 rime.dll，无外部进程、无 lua 依赖 |
| TSF 光标上文 | 原生采集光标前 ~64 字符（TextEditSink 文档变化 + 提交时主动采集，0.17.4 基底） |
| WPS 兼容 | WPS 系应用（Kso 定制 Qt 的 TSF 实现不完整）自动禁用 TSF 采集，退化到上屏历史（`AI·历史`），打字稳定 |
| prepare 预解码 | commit 后异步预解码上文（KV cache 复用），score 命中跳过重复解码（prep=1，2ms vs ~50ms） |
| 长候选外推 | 4+ token 候选按尾部 CE 外推（λ=0.5，语料模拟调参），不增加 decode 次数，3-token 词不受长词挤压 |
| 性能日志 | `rime_llm_filter_log.txt` 每推理一行：wait/S1/KV/S2/total/prep/ctx_tok/cand |
| AI 首选标记 | 重排后首选候选 comment 显示 `AI·TSF`（光标前上文）/ `AI·历史`（上屏历史回退，WPS 等场景） |

## 目录结构

```
rime-llm-ime/
├── librime/           # 修改后的完整 librime 源码（上游 1.17.0 @ 1d0df6e）
│   └── src/rime/gear/llm_filter.{h,cc}   # LLM filter 组件（新增）
├── weasel/            # 修改后的完整 weasel 源码（上游 0.17.4 release tag）
│   ├── WeaselTSF/     # TSF 上文采集（TextEditSink 文档变化 + 提交时采集 + WPS 黑名单）
│   ├── WeaselIPC/     # SET_CONTEXT_TEXT / RESET_CONTEXT IPC（枚举尾部追加，前缀兼容官方）
│   ├── WeaselIPCServer/  # OnSetContextText / OnResetContext handler
│   └── librime/       # 仅补丁头文件（rime_api.h，LLM 版含 set_context_text 等扩展 API）
├── pdsp.schema.yaml   # 拼读双拼方案 LLM 版（含 llm_rerank 配置节示例）
├── bin/               # 预编译产物（rime.dll / weaselx64.dll / WeaselServer.exe / WeaselDeployer.exe / opencc.dll / vcomp140.dll）+ deploy_llm.bat 一键部署脚本 + bench_threads.exe 线程数测定工具
└── sync.py            # 本机维护脚本（编译源 → 项目目录 → GitHub 树）
```

## 快速开始（普通用户）

前置：已安装官方小狼毫 0.17.x（x64）。

### 一键部署（推荐）

双击 `bin/deploy_llm.bat`（自动请求管理员权限）→ **重启系统** → **托盘重新部署** → 完成。

脚本自动：停止 WeaselServer → 复制 6 个文件到安装目录 → System32 TSF 组件延迟替换（重启生效）→ 重启 Server。

> **托盘重新部署必须执行**：词典 build 必须由 LLM 版 librime 编译（官方部署会覆盖为官方格式，LLM librime 读不了 → 一码字词全部为空）。

### 手动步骤

1. 下载 GGUF 模型（本机验证使用 `Qwen3.5-0.8B-Q4_K_M.gguf`，任意小模型均可，建议 ≤2B Q4）
2. 将 `bin/` 下 **6 个文件**复制到小狼毫安装目录（`C:\Program Files\Rime\weasel-0.17.4`）：
   - `rime.dll` / `weaselx64.dll` / `WeaselServer.exe` / `WeaselDeployer.exe` — 本方案产物
   - `opencc.dll` — rime.dll 的动态依赖（缺失会报"找不到 opencc.dll"）
   - `vcomp140.dll` — VC OpenMP 运行时（无 VS 运行库的机器必需）
3. `weaselx64.dll` 同时复制为 `C:\Windows\System32\weasel.dll`（TSF 组件，被占用时用延迟替换——**切勿重命名替换**，会导致 TSF 注销、语言栏图标消失）
4. 将 `pdsp.schema.yaml` 复制到 RIME 用户目录（`%APPDATA%\Rime\`），修改 `llm_rerank` 配置节的 `model_path`
5. 托盘右键 → **重新部署**（LLM librime 重建词典 build）→ **重启系统**（TSF 加载新版组件）

### 常见问题

- **Win+Space 无小狼毫**：设置 → 时间和语言 → 中文 → 键盘 → 添加键盘 → 小狼毫
- **32 位应用（QQ 音乐等）**：保持官方安装器的 32 位组件（勿删 SysWOW64\weasel.dll）
- **内存 ≤4GB**：`enabled: false` 或调小 `min_free_mem_mb`（模型需约 2GB）
- **语言栏图标消失**：多为 System32 DLL 被重命名替换导致，恢复方式：重装官方包 → 设置添加键盘 → 重新部署

> 使用其他方案：把 `llm_rerank` 配置节加入任意 RIME 方案，并确认其 filters 链末尾（uniquifier 后）有 `llm_filter`。配置节仅依赖四码输入编码，与具体方案无关。

### llm_rerank 配置节（全部可选）

```yaml
llm_rerank:
  enabled: true         # true=启用 LLM 重排 | false=关闭（组件透传，不推理）
  min_code_len: 4       # 输入编码长度小于此值时不重排
  min_tokens: 1         # 上文 token 数小于此值时不推理
  max_tokens: 10        # 上文 token 上限
  max_candidates: 5     # 参与打分的候选数上限
  cpu_cores: 4          # 默认线程数（=GGML 默认，适用旧设备；bench_threads.exe 实测后自行修改）
  min_free_mem_mb: 2560 # 可用内存低于此值时不加载模型（防小内存机器系统卡死）
  model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf
```

> **各线程数延迟实测**：运行 `bin/bench_threads.exe` 即可在本机实测 1~10（或最大核数）各线程数的推理耗时，打印延迟表（中位数 + mid50 区间），自行选择后填入 schema 的 `cpu_cores`，提示重新部署即可。每档采样 99 次，约 2.5 分钟。结果同时写入 exe 同目录 `bench_threads_result.txt`（覆盖）。

### 线程数实测（可选，普通用户）

`bin/bench_threads.exe` 在**本机实测**各线程数的推理延迟：

```
双击/命令行运行 bin/bench_threads.exe [模型路径]
```

1. **建议在系统空闲时运行**（有编译/下载/游戏在跑会压平曲线、延迟失真）
2. 约 2.5 分钟后输出各线程数延迟表（每档采样 99 次，中位数 + mid50 区间）：
   ```
   == bench_threads: per-thread latency ==
   ...
     thr= 5: median  69.2 ms/pass (mid50  67.7- 71.6)
     thr= 6: median  65.6 ms/pass (mid50  64.2- 67.1)
     ...
   ```
   工具**不做推荐**——自行权衡"延迟 vs 线程占用"，把选定的数字填入方案 schema 的 `llm_rerank.cpu_cores`（通常选曲线拐点附近的最小线程数即可；`mid50` 区间窄 = 该档稳定）
3. 托盘右键 → 重新部署 → 生效

不跑本工具也可以直接用默认值 4（=GGML 默认，适用旧设备；线程数固定，无运行时动态调整）。

> 内存需求：0.8B Q4 模型加载后 WeaselServer 占用约 2GB。**内存 ≤4GB 的机器建议 `enabled: false`**（输入法照常使用，仅无 LLM 重排），或调小 `min_free_mem_mb` 谨慎尝试。

## 构建（开发者）

### 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| Visual Studio 2022 Build Tools | v143 | 编译 |
| Boost | 1.84.0 | weasel 构建（`weasel/deps/` 下，官方 `install_boost.bat` 可获取） |
| librime 依赖 | marisa-trie / opencc / leveldb / yaml-cpp / glog | `cd librime && git submodule update --init --recursive`，按 librime 官方流程构建（产出 install 目录：marisa.lib/opencc.lib/leveldb.lib/yaml-cpp.lib） |
| llama.cpp | master（MT 静态构建） | llm_filter 推理后端 |
| GGUF 模型 | 任意小模型 | 推理模型 |

### weasel 源码说明（0.17.4 基底）

**重要**：weasel 源码基于 **0.17.4 release tag**（`git clone --depth 1 --branch 0.17.4 https://github.com/rime/weasel.git`），**不是 master 分支**——master 与 0.17.4 存在 IPC 协议差异（UIStyle 序列化中间插字段等），混用官方 0.17.4 组件会导致输入法失效（WPS/Office 英文直出、IPC 错误）。

本仓库 weasel/ 相对官方 0.17.4 的改动：
1. `include/WeaselIPC.h`：枚举**尾部追加** `SET_CONTEXT_TEXT`/`RESET_CONTEXT`（前缀编号不变，与官方组件任意混用兼容）+ Client 接口 `SetContextText`/`ResetContext`
2. `WeaselIPC/WeaselClientImpl.{h,cpp}`：`SendContextText`/`SendContextReset` 实现 + **channel_mutex 互斥锁**（TSF 采集线程与主线程共享管道，替代 master 的 TLS 管道重构）
3. `WeaselIPCServer/WeaselServerImpl.{h,cpp}`：`OnSetContextText`/`OnResetContext` handler（调 librime `set_context_text`/`reset_context_text`）
4. `WeaselTSF/`：光标上文采集——`CGetTextBeforeCaretEditSession`（GetSelection → GetText 分块，kMaxIter=128 防死循环）+ TextEditSink 文档变化触发 + `CEndCompositionEditSession` 提交时采集 + **WPS 黑名单**（`_IsTSFCtxReliable()`：wps.exe 等禁用 TSF 采集 → librime 自动退化上屏历史）
5. **UIStyle 不加字段**（`ai_comment_text_color` 未移植——保持与官方 0.17.4 序列化 100% 兼容；AI 标记以 comment 文本显示）

### 步骤

```bat
:: 1. librime 依赖
cd librime
git submodule update --init --recursive
build.bat                      :: 官方流程，产出 build/install 等

:: 2. llama.cpp MT 静态构建（MT 运行时，与 librime 一致）
git clone https://github.com/ggml-org/llama.cpp
cmake -B build-mt -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build build-mt --config Release
:: 产出 build-mt/src/Release/llama.lib + build-mt/ggml/src/Release/ggml*.lib

:: 3. 构建 rime.dll（llm_filter 链接 llama）
cmake -B build -A x64 -DLLAMA_ROOT=D:/llama.cpp-mirror ^
      -DCMAKE_PREFIX_PATH=D:/rime-build/librime/install
cmake --build build --config Release --target rime

:: 4. 构建 weasel（0.17.4 基底）
:: 4a. 依赖准备
::   - boost_1_84_0 放入 weasel/deps/（install_boost.bat 或从其他 weasel 树复制）
::   - weasel.props 从 template 生成（替换 BOOST_ROOT/PLATFORM_TOOLSET=v143/VERSION=0.17.4.0）
::   - afxres.h 放入 weasel/include/（VS 无 MFC 时需 stub）
::   - LLM 版 rime_api.h（顶层 librime/src/rime_api.h，含 set_context_text 等扩展）
::     复制到 weasel/librime/include/rime_api.h
::   - rime.lib：librime 构建产物，或从 rime.dll 生成：
::       dumpbin /exports rime.dll > exports.txt  →  写 rime.def  →  lib /def:rime.def /machine:x64
::     放入 weasel/lib/ 和 weasel/lib64/（x64 Release 用 lib64）
::   - WeaselIPCServer.vcxproj 的 include 路径需含 $(SolutionDir)\librime\include
:: 4b. 编译
cd weasel
msbuild weasel.sln /p:Configuration=Release /p:Platform=x64 /t:WeaselTSF /t:WeaselServer
:: 产物在 weasel/output/weaselx64.dll + weasel/output/WeaselServer.exe
```

> `src/CMakeLists.txt` 中 `LLAMA_ROOT` 为 CMake CACHE 变量，可用 `-DLLAMA_ROOT=<你的路径>` 覆盖。
> 仅 CPU 路线（llama.cpp MT 静态构建）；GPU 版已退役不发布。

### 替换安装文件

产物：`librime/build/src/Release/rime.dll`、`weasel/output/weaselx64.dll`、`weasel/output/WeaselServer.exe`，另需 `librime install/bin/opencc.dll`（rime.dll 的动态依赖）和 `vcomp140.dll`（VC OpenMP 运行时）。替换小狼毫安装目录同名文件后，注销重登（TSF 注册表缓存）或重启系统，然后**托盘重新部署**（LLM librime 重建词典 build）。

## 候选窗 AI 标记

LLM 重排生效时，首选候选的 comment 显示来源徽章（文本形式，跟随候选窗 comment 样式）：

| 标记 | 含义 |
|------|------|
| `AI·TSF` | 上文来自 TSF 光标前文本（Word/记事本等 TSF 采集正常的应用） |
| `AI·历史` | 上文来自上屏历史回退（WPS 等 TSF 采集不可用的应用，自动退化） |

> 注：0.17.4 基底未加独立强调色样式（保持与官方 UIStyle 序列化完全兼容）；如需自定义 comment 颜色，用候选窗 comment 样式即可。

## 日志

`%APPDATA%\Rime\rime_llm_filter_log.txt` — 每次推理一行：

```
score: wait=12ms S1=48ms KV=3ms S2=9ms total=72ms prep=1 ctx_tok=9 cand=5
```

## 许可证

| 部分 | 许可证 |
|------|--------|
| 本项目新增代码（llm_filter 等） | **GPL-3.0**（本仓库 LICENSE） |
| weasel（含其内 librime 头文件副本） | GPL-3.0（weasel/LICENSE.txt） |
| librime | BSD-3-Clause（librime/LICENSE） |
| llama.cpp（仅链接） | MIT |

## 相关项目

- [拼读双拼](https://github.com/zhanghaozhecn/rime-pindu-double-pinyin) — 本方案使用的编码方案（带调双拼）
- [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank) — 旧版 lua 插件路线（已封存，功能与此仓库等效，上文仅用上屏历史）
