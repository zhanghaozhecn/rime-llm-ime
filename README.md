# rime-llm-ime — 基于 LLM 候选重排的输入法

将 LLM 候选重排**源码级集成**进 RIME 小狼毫（weasel + librime）的中文输入方案：不需要外挂进程或 lua 插件，`llm_filter` 作为 librime 原生 filter 组件（C++）编译进 `rime.dll`，TSF 原生采集光标上文。

- **开发者**：仓库含修改后的完整 weasel + librime 源码树，可按本 README 构建
- **普通用户**：`bin/` 提供预编译产物 + 一键部署脚本，替换小狼毫安装文件即可使用（模型由部署脚本自动检查/下载）

## 特性

| 特性 | 说明 |
|------|------|
| 原生 filter 组件 | `llm_filter`（gear/llm_filter.{h,cc}）编译进 rime.dll，无外部进程、无 lua 依赖 |
| TSF 光标上文 | 原生采集光标前 ~64 字符：文档变化触发 + **提交后立即采集**（跳过去抖，下一词首键前上文已到位） |
| 上文来源自适应 | TSF 采集正常 → `AI·TSF`；采集不可用（32 位应用等）→ commit history 回退 `AI·历史`；**残留检测**：TSF 文本不含最近上屏词时判为其他应用残留，自动改用历史 |
| WPS 兼容 | WPS 系应用自动退化到上屏历史（`AI·历史`），打字稳定 |
| prepare 预解码 | commit 后异步预解码上文（KV cache 复用），score 命中跳过重复解码（prep=1，2ms vs ~50ms） |
| 长候选外推 | 4+ token 候选按尾部 CE 外推（λ=0.5，语料模拟调参），不增加 decode 次数，3-token 词不受长词挤压 |
| 性能日志 | `rime_llm_filter_log.txt` 每推理一行：wait/S1/KV/S2/total/prep/ctx_tok/cand |
| AI 首选标记 | 重排后首选候选 comment 显示 `AI·TSF` / `AI·历史` |

## 目录结构

```
rime-llm-ime/
├── librime/           # 修改后的完整 librime 源码（上游 1.17.0 @ 1d0df6e）
│   └── src/rime/gear/llm_filter.{h,cc}   # LLM filter 组件（新增，含上文来源自适应）
├── weasel/            # 修改后的完整 weasel 源码（上游 0.17.4 release tag）
│   ├── WeaselTSF/     # TSF 上文采集（文档变化 + 提交后立即采集 + WPS 黑名单）
│   ├── WeaselIPC/     # SET_CONTEXT_TEXT / RESET_CONTEXT IPC（枚举尾部追加，前缀兼容官方）
│   ├── WeaselIPCServer/  # OnSetContextText / OnResetContext handler
│   └── librime/       # 仅补丁头文件（rime_api.h，LLM 版含 set_context_text 等扩展 API）
├── bin/               # 预编译产物 + 部署/验证脚本（见下）
│   ├── rime.dll / weaselx64.dll / weasel32.dll / WeaselServer.exe / WeaselDeployer.exe / opencc.dll / vcomp140.dll / bench_threads.exe
│   ├── deploy_llm.bat          # 一键部署（模型检查 → 组件复制 → WeaselSetup 官方部署 → 注册兜底 → schema 插入）
│   ├── deploy_llm_model.ps1    # 模型检查/下载（ModelScope，断点续传）
│   ├── deploy_llm_schema.ps1   # 方案配置插入（llm_filter + llm_rerank 节，幂等，位置校验）
│   └── verify_deploy.bat       # 部署验证（与源包自动对比 md5）
├── scripts/           # 构建/部署/测试脚本（发布清单，见"从源码构建"）
└── deploy/            # 发布部署包打包目录（gitignore，不进 git）
```

> 方案（`pdsp.schema.yaml`）不在本仓库——部署脚本在 RIME 用户目录的方案中幂等插入 `llm_filter` 组件与 `llm_rerank` 配置节。

## 快速开始（普通用户）

前置：已安装官方小狼毫 0.17.x（x64）。

### 一键部署（推荐）

1. 从 [Releases](../../releases) 下载部署包 zip（`rime-llm-deploy-*.zip`），解压到任意目录
   > 仓库 zip / `git clone` **不含**预编译二进制（bin 二进制不进 git）。源码构建见文末"从源码构建"节。
2. 双击 `deploy_llm.bat`（自动请求管理员权限），流程：
   - **[1/7] 模型检查**：默认路径 `d:\gguf_models\Qwen3.5-0.8B-Q4_K_M.gguf` 已有则跳过；缺失则询问是否下载（约 500MB，ModelScope，断点续传；跳过则 LLM 不工作但输入法正常）
   - **[2/7] 停止 WeaselServer → [3/7] 复制 7 个组件到安装目录（含 32 位 TSF）→ [4/7] WeaselSetup /u+/i 官方部署（SysWOW64 + System32 双 TSF）→ [5/7] 32 位视图注册兜底 → [6/7] 重启 Server**
   - **[7/7] 方案配置插入**：在 RIME 用户目录的 `pdsp.schema.yaml` 中幂等插入 `- llm_filter`（uniquifier 后、pin_fix 前，位置校验）与 `llm_rerank:` 配置节
3. **重启系统**（System32/SysWOW64 TSF 组件生效）
4. **托盘右键 → 重新部署**（重建词典 build，必须）

> **托盘重新部署必须执行**：词典 build 必须由 LLM 版 librime 编译（官方部署会覆盖为官方格式，LLM librime 读不了 → 一码字词全部为空）。

### 手动步骤（脚本不可用时）

1. 下载 GGUF 模型（本机验证使用 `Qwen3.5-0.8B-Q4_K_M.gguf`，任意小模型均可，建议 ≤2B Q4）放到 `d:\gguf_models\`
2. 将 `bin/` 下 **7 个文件**复制到小狼毫安装目录（`C:\Program Files\Rime\weasel-0.17.4`）：
   - `rime.dll` / `weaselx64.dll` / `weasel32.dll`（复制为 `weasel.dll`）/ `WeaselServer.exe` / `WeaselDeployer.exe` — 本方案产物
   - `opencc.dll` — rime.dll 的动态依赖（缺失会报"找不到 opencc.dll"）
   - `vcomp140.dll` — VC OpenMP 运行时（无 VS 运行库的机器必需）
3. 运行 `WeaselSetup.exe /u + /i`（官方部署流程：安装目录 `weasel.dll` → SysWOW64、`weaselx64.dll` → System32，并注册；32 位视图注册缺失时用 `SysWOW64\regsvr32.exe /s C:\Windows\SysWOW64\weasel.dll` + `TEXTSERVICE_PROFILE=hans` 补注册）；System32 被占用时用延迟替换（**切勿重命名替换**，会导致 TSF 注销、语言栏图标消失）
4. 方案配置插入（二选一）：
   - `powershell -ExecutionPolicy Bypass -File bin\deploy_llm_schema.ps1`（推荐，幂等）
   - 手动在 `%APPDATA%\Rime\pdsp.schema.yaml` 的 filters 块 uniquifier 之后加 `- llm_filter`，并添加 `llm_rerank:` 配置节
5. 托盘右键 → **重新部署** → **重启系统**

### 部署验证

`bin/verify_deploy.bat`（与部署文件放同一目录）逐项检查：安装目录 3 组件 md5（与源包自动对比）、System32 TSF、注册表、Server 进程。

### 常见问题

- **Win+Space 无小狼毫**：设置 → 时间和语言 → 中文 → 键盘 → 添加键盘 → 小狼毫
- **32 位应用（QQ 音乐、WPS 等）**：加载 32 位 LLM TSF（SysWOW64\weasel.dll，部署包 `weasel32.dll`）；32 位视图注册缺失时输入英文，用部署脚本 [5/7] 兜底（`SysWOW64\regsvr32.exe /s` + `TEXTSERVICE_PROFILE=hans`）
- **内存 ≤4GB**：`enabled: false` 或调小 `min_free_mem_mb`（0.8B Q4 模型加载后 WeaselServer 约占 2GB）
- **语言栏图标消失**：多为 System32 DLL 被重命名替换导致，恢复方式：重装官方包 → 设置添加键盘 → 重新部署

> 使用其他方案：`deploy_llm_schema.ps1 -SchemaName <你的方案>.schema.yaml`，或手动加 `llm_rerank` 配置节 + filters 链 uniquifier 后的 `- llm_filter`。配置节仅依赖四码输入编码，与具体方案无关。

### llm_rerank 配置节（全部可选）

```yaml
llm_rerank:
  enabled: true         # true=启用 LLM 重排 | false=关闭（组件透传，不推理）
  min_code_len: 4       # 输入编码长度小于此值时不重排
  max_code_len: 0       # 编码长度上限（0=不限制）；超出不推理，与 min_code_len 组成触发区间
  long_word_first: false # true=long-word-first: 候选算完 CE 后按词长降序, 同词长按 CE 评分序
  freq_weight: 0.25  # 用户词频融合权重 (0=关闭): total=(1-w)·LLM(窗内min-max)+w·count/(count+k)
  freq_k: 5          # 词频饱和常数; 词频由 OnCommit 自动统计 (RIME 用户目录 user_freq.tsv)
  min_tokens: 1         # 上文 token 数小于此值时不推理
  max_tokens: 10        # 上文 token 上限
  max_candidates: 5     # 参与打分的候选数上限
  cpu_cores: 4          # 默认线程数（=GGML 默认，适用旧设备；用发布包内 bench_threads.exe 实测后自行修改）
  min_free_mem_mb: 2560 # 可用内存低于此值时不加载模型（防小内存机器系统卡死）
  model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf
```

## 构建（开发者）

### 依赖

| 依赖 | 版本 | 获取方式 |
|------|------|---------|
| Visual Studio 2022 Build Tools | v143（含 vcvars64.bat） | 官方安装器，C++ 桌面开发工作负载 |
| Boost | 1.84.0 | 仓库 `weasel/install_boost.bat`（官方脚本，自动下载）→ `weasel/deps/boost_1_84_0` |
| librime 依赖 | marisa / opencc / leveldb / yaml-cpp / glog | `cd librime && git submodule update --init --recursive`，官方 `build.bat` 构建并安装到 `librime/` 与 `librime/dist/` |
| llama.cpp | master | clone + MT 静态构建（命令见下），路径经 `-DLLAMA_ROOT=` 传入 |
| GGUF 模型 | 任意小模型（建议 ≤2B Q4） | 部署脚本自动检查/下载 |

### weasel 源码说明（0.17.4 基底）

**重要**：weasel 源码基于 **0.17.4 release tag**（`git clone --depth 1 --branch 0.17.4 https://github.com/rime/weasel.git`），**不是 master 分支**——master 与 0.17.4 存在 IPC 协议差异（UIStyle 序列化中间插字段等），混用官方 0.17.4 组件会导致输入法失效（WPS/Office 英文直出、IPC 错误）。

本仓库 weasel/ 相对官方 0.17.4 的改动：
1. `include/WeaselIPC.h`：枚举**尾部追加** `SET_CONTEXT_TEXT`/`RESET_CONTEXT`（前缀编号不变，与官方组件任意混用兼容）+ Client 接口 `SetContextText`/`ResetContext`
2. `WeaselIPC/WeaselClientImpl.{h,cpp}`：`SendContextText`/`SendContextReset` 实现 + **recursive_mutex 互斥锁**（TSF 采集线程与主线程共享管道 buffer，防消息体错位）
3. `WeaselIPCServer/WeaselServerImpl.{h,cpp}`：`OnSetContextText`/`OnResetContext` handler（调 librime `set_context_text`/`reset_context_text`）
4. `WeaselTSF/`：光标上文采集——`CGetTextBeforeCaretEditSession`（GetSelection → GetText 分块，kMaxIter=128 防死循环）+ TextEditSink 文档变化触发（100ms 去抖合并防 CEF 风暴）+ **提交后立即采集**（`immediate` 跳过去抖，下一词首键前 TSF 上文到位）+ **编辑键 reset**（有编码时退格豁免——方案将其转 ESC 清码，上文不变）+ **焦点切换 reset+采集** + **ShiftStart(-64) 主路径**（MOVESTART 兜底）+ **WPS 滞后检测**（TSF 文本明显短于上屏历史且为其尾部 → 自动改用完整历史）
5. **UIStyle 不加字段**（`ai_comment_text_color` 未移植——保持与官方 0.17.4 序列化 100% 兼容；AI 标记以 comment 文本显示）

librime 侧（`gear/llm_filter.cc`，新增组件）：
- 上文来源自适应（`GetContextTextPair`）：TSF 文本优先；TSF 文本空/滞后 → commit history 兜底（提交后同步累积，必有最近上屏词）；**残留检测**——TSF 文本不含 commit history 尾部（最近上屏词）时判为其他应用残留，改用历史
- 预解码（`prepare`）：commit/上下文变化后异步 decode 上文，score 命中复用 KV
- 触发区间 `[min_code_len, max_code_len]`（0=不限制）+ 重排后 `long_word_first` long-word-first 排序（与插件版参数对齐）
- 用户词频融合 `freq_weight`/`freq_k`（默认 0.25/5）：`total=(1-w)·LLM评分(窗内min-max归一) + w·count/(count+k)`，词频由 OnCommit 自动统计并持久化（user_freq.tsv）。实证（真实候选窗回放 6000 抽样）：w=0.25 首选率 97.08%→98.20%，纯 LLM 排错事件 87% 的选中词用户词频≥2。融合应用于评分序之上、long_word_first 之前（与插件版一致）

### 构建步骤

仓库自带 `scripts/` 构建链（已发布，路径相对仓库根，任意目录 clone 后可直接使用）。以下按顺序执行：

```bat
:: 0. 官方 0.17.4 安装版 weasel 一次获取依赖（boost + 官方二进制）——仅首次
weasel\install_boost.bat        :: 自动下载 boost 1.84.0 → weasel\deps\boost_1_84_0

:: 1. librime 依赖 + 官方构建（产出 librime\dist\lib\rime.lib 等）
cd librime
git submodule update --init --recursive
build.bat                       :: 官方流程（依赖 install 到 librime\，rime 装到 dist\）
cd ..

:: 2. llama.cpp MT 静态构建（MT 运行时，与 librime 一致）
git clone https://github.com/ggml-org/llama.cpp <你的 llama 目录>
cd <你的 llama 目录>
cmake -B build-mt -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build build-mt --config Release
:: 产出 build-mt/src/Release/llama.lib + build-mt/ggml/src/Release/ggml*.lib
cd ..

:: 3. 配置 + 构建 rime.dll（llm_filter 链接 llama）
set LLAMA_ROOT=<你的 llama 目录>
scripts\cmake_rime.bat          :: 生成 librime\build\*.sln（deps 探测见下）
scripts\build_rime.bat          :: 产物 librime\build\bin\Release\rime.dll

:: 4. weasel 依赖准备（首次）
::   - weasel\weasel.props：从 weasel.props.template 复制并替换 BOOST_ROOT 为本地 boost 路径
::   - weasel\librime\include\rime_api.h：仓库内已与 librime\src\rime_api.h 同步
::     （LLM 版含 set_context_text/context_text_age_ms 等扩展 API——头布局必须与 rime.dll
::      一致，改动 rime_api.h 后需重新覆盖到 weasel 树）
::   - rime.lib 入库：scripts\gen_rime_lib.bat（从 librime\dist\lib 拷到 weasel\lib + lib64）
::   - 注：WeaselIPCServer.vcxproj 的 include 路径已在仓库内改好（$(SolutionDir)\librime\include）

:: 5. 构建 weasel
scripts\build_tsf.bat           :: x64 TSF → weasel\output\weaselx64.dll
scripts\build_server.bat        :: server → weasel\output\WeaselServer.exe
:: 32 位 TSF（可选，WPS 等 32 位应用）：
scripts\build_boost32.bat       :: 32 位 boost（-x32- 后缀库，首次）
scripts\build_tsf32.bat         :: Win32 TSF → weasel\output\weasel.dll（官方命名 32 位）
```

**`cmake_rime.bat` 的 deps 解析**：优先探测 `deps\prebuilt\`（本地预编译库，本机布局）；不存在则回退 librime 官方 build.bat 的安装位置（`librime\` 本身）。两者都可用 `RIME_DEPS` 环境变量覆盖。`BOOST_ROOT`/`LLAMA_ROOT` 同理（环境变量优先，默认 `deps\boost_1_84_0` / `D:/llama.cpp-mirror`）。

> `src/CMakeLists.txt` 中 `LLAMA_ROOT` 为 CMake CACHE 变量；仅 CPU 路线（llama.cpp MT 静态构建），GPU 版已退役不发布。

### 替换安装文件

产物齐后，把 `librime\build\bin\Release\rime.dll`、`weasel\output\weaselx64.dll`、`weasel\output\weasel.dll`（32 位）、`weasel\output\WeaselServer.exe` 放进 `bin\`（连同 opencc.dll / vcomp140.dll），然后按上面"一键部署"流程（`deploy_llm.bat` 或 `scripts\deploy_tsf32.bat` 官方 WeaselSetup 流程）部署，重启系统（TSF 注册表缓存），最后**托盘重新部署**（LLM librime 重建词典 build）。

## 候选窗 AI 标记

LLM 重排生效时，首选候选的 comment 显示来源徽章（文本形式，跟随候选窗 comment 样式）：

| 标记 | 含义 |
|------|------|
| `AI·TSF` | 上文来自 TSF 光标前文本（Word/记事本等 TSF 采集正常的应用） |
| `AI·历史` | 上文来自上屏历史回退（WPS、32 位应用等 TSF 采集不可用的场景，自动退化） |

> 注：0.17.4 基底未加独立强调色样式（保持与官方 UIStyle 序列化完全兼容）；如需自定义 comment 颜色，用候选窗 comment 样式即可。

## 日志

`%APPDATA%\Rime\rime_llm_filter_log.txt`：

```
score: wait=12ms S1=48ms KV=3ms S2=9ms total=72ms prep=1 ctx_tok=9 cand=5
ctx: [上文]              # 推理时用的上文（normalize 后）
ctx raw: [原始上文]       # normalize 前后不一致时记录原始值
RESET: context cleared (gen=N reason=...)   # TSF 上下文被清空（编辑键/切窗）
config: enabled=1 min_code_len=4 max_code_len=0 long_word_first=0 ...   # 部署/会话启动时的配置
```

事件日志（`时间|计数|编码|候选|上文|排序后候选|延迟|来源`）用于选词质量排查。

## 许可证

| 部分 | 许可证 |
|------|--------|
| 本项目新增代码（llm_filter 等） | **GPL-3.0**（本仓库 LICENSE） |
| weasel（含其内 librime 头文件副本） | GPL-3.0（weasel/LICENSE.txt） |
| librime | BSD-3-Clause（librime/LICENSE） |
| llama.cpp（仅链接） | MIT |

## 相关项目

- [拼读双拼](https://github.com/zhanghaozhecn/rime-pindu-double-pinyin) — 本方案使用的编码方案（带调双拼）
- [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank) — 插件版（lua + DLL 路线），功能与此仓库等效，上文仅用上屏历史；源码版与插件版二选一
