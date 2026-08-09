# rime-llm-ime — 基于 LLM 候选重排的输入法

将 LLM 候选重排**源码级集成**进 RIME 小狼毫（weasel + librime）的中文输入方案：不需要外挂进程或 lua 插件，`llm_filter` 作为 librime 原生 filter 组件（C++）编译进 `rime.dll`，TSF 原生采集光标上文。

- **开发者**：仓库含修改后的完整 weasel + librime 源码树，可按本 README 构建
- **普通用户**：`bin/` 提供预编译产物，替换小狼毫安装文件即可使用（需自行下载 GGUF 模型）

## 特性

| 特性 | 说明 |
|------|------|
| 原生 filter 组件 | `llm_filter`（gear/llm_filter.{h,cc}）编译进 rime.dll，无外部进程、无 lua 依赖 |
| TSF 光标上文 | 原生采集光标前 ~64 字符，隔断规则：切换窗口、换行、编辑键（BackSpace/Delete/方向键/Home/End/Enter）后上文重置 |
| prepare 预解码 | commit 后异步预解码上文（KV cache 复用），score 命中跳过重复解码（prep=1，2ms vs ~50ms） |
| 长候选外推 | 4+ token 候选按尾部 CE 外推（λ=0.5，语料模拟调参），不增加 decode 次数，3-token 词不受长词挤压 |
| 性能日志 | `rime_llm_filter_log.txt` 每推理一行：wait/S1/KV/S2/total/prep/ctx_tok/cand |
| AI 首选标记 | 重排后首选候选 comment 显示 `AI·TSF`（光标前上文）/ `AI·历史`（上屏历史回退），**强调色**渲染（`style/ai_comment_text_color`，默认金色） |

## 目录结构

```
rime-llm-ime/
├── librime/           # 修改后的完整 librime 源码（上游 1.17.0 @ 1d0df6e）
│   └── src/rime/gear/llm_filter.{h,cc}   # LLM filter 组件（新增）
├── weasel/            # 修改后的完整 weasel 源码（上游 0.17.4）
│   ├── WeaselTSF/     # TSF 上文采集（GetTextBeforeCaret、KeyUp 路径、SetThreadFocus 刷新）
│   ├── WeaselIPC/     # SET_CONTEXT_TEXT / RESET_CONTEXT IPC
│   └── librime/       # 仅补丁头文件（rime_api.h），完整版用顶层 librime/
├── pdsp.schema.yaml   # 拼读双拼方案 LLM 版（含 llm_rerank 配置节示例）
├── bin/               # 预编译产物（rime.dll / weaselx64.dll / WeaselServer.exe / WeaselDeployer.exe / opencc.dll / vcomp140.dll）+ deploy_llm.bat 一键部署脚本 + bench_threads.exe 线程数测定工具
└── sync.py            # 本机维护脚本（编译源 → 项目目录 → GitHub 树）
```

## 快速开始（普通用户）

前置：已安装官方小狼毫 0.17.x（x64）。

### 一键部署（推荐）

双击 `bin/deploy_llm.bat`（自动请求管理员权限）→ **重启系统** → 完成。

脚本自动：停止 WeaselServer → 复制 6 个文件到安装目录 → System32 TSF 组件延迟替换（重启生效）→ 重启 Server。

### 手动步骤

1. 下载 GGUF 模型（本机验证使用 `Qwen3.5-0.8B-Q4_K_M.gguf`，任意小模型均可，建议 ≤2B Q4）
2. 将 `bin/` 下 **6 个文件**复制到小狼毫安装目录（`C:\Program Files\Rime\weasel-0.17.4`）：
   - `rime.dll` / `weaselx64.dll` / `WeaselServer.exe` / `WeaselDeployer.exe` — 本方案产物
   - `opencc.dll` — rime.dll 的动态依赖（缺失会报"找不到 opencc.dll"）
   - `vcomp140.dll` — VC OpenMP 运行时（无 VS 运行库的机器必需）
3. `weaselx64.dll` 同时复制为 `C:\Windows\System32\weasel.dll`（TSF 组件，被占用时用延迟替换——**切勿重命名替换**，会导致 TSF 注销、语言栏图标消失）
4. 将 `pdsp.schema.yaml` 复制到 RIME 用户目录（`%APPDATA%\Rime\`），修改 `llm_rerank` 配置节的 `model_path`
5. 托盘右键 → **重新部署** → **重启系统**（TSF 加载新版组件）

### 常见问题

- **Win+Space 无小狼毫**：设置 → 时间和语言 → 中文 → 键盘 → 添加键盘 → 小狼毫
- **32 位应用（QQ 音乐等）**：保持官方安装器的 32 位组件（勿删 SysWOW64\weasel.dll）
- **内存 ≤4GB**：`backend: off` 或调小 `min_free_mem_mb`（模型需约 2GB）
- **语言栏图标消失**：多为 System32 DLL 被重命名替换导致，恢复方式：重装官方包 → 设置添加键盘 → 重新部署

> 使用其他方案：把 `llm_rerank` 配置节加入任意 RIME 方案，并确认其 filters 链末尾（uniquifier 后）有 `llm_filter`。配置节仅依赖四码输入编码，与具体方案无关。

### llm_rerank 配置节（全部可选）

```yaml
llm_rerank:
  backend: cpu          # off | cpu | gpu — off 时透传不推理
  min_code_len: 4       # 输入编码长度小于此值时不重排
  min_tokens: 1         # 上文 token 数小于此值时不推理
  max_tokens: 10        # 上文 token 上限
  max_candidates: 5     # 参与打分的候选数上限
  cpu_cores: 6          # 默认线程数（bench_threads.exe 实测后自行修改）
  auto_adapt: true      # 运行时按系统 CPU 负载动态调整线程数（>85% 切到 4 让路，<60% 恢复默认；10s 节流）
  min_free_mem_mb: 2560 # 可用内存低于此值时不加载模型（防小内存机器系统卡死）
  model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf
```

> **最优线程数（一键）**：运行 `bin/bench_threads.exe --apply` 即可在本机实测各线程数的推理耗时，**自动写入** RIME 用户目录 schema 的 `cpu_cores`（无需手动编辑），提示重新部署即可。扫描 1..逻辑核数约半分钟。配合 `auto_adapt: true`（默认开启），运行时还会根据系统负载动态让出/恢复线程，兼顾打字流畅与其他应用。

### 线程数优化（可选，普通用户）

`bin/bench_threads.exe` 在**本机实测**推理性能，给出针对这台设备的线程数建议：

```
双击/命令行运行 bin/bench_threads.exe [--apply] [模型路径]
```

1. **建议在系统空闲时运行**（有编译/下载/游戏在跑会压平曲线、建议值失真）
2. 约 1 分钟后输出：
   ```
   optimal thread count: 12 (54 ms/pass)
   suggested default (90%): 6 (57 ms/pass)
   config suggestion:
     llm_rerank:
       cpu_cores: 6
   ```
   - `optimal`：该设备理论最快的线程数（全速档）
   - `suggested default`：达到最优 90% 性能的**最小**线程数（推荐日常使用，省线程、留余量）
3. 两种方式采纳建议：
   - **自动**：加 `--apply` 参数运行，工具直接改写 RIME 用户目录里含 `llm_rerank` 节的 schema（`cpu_cores` 行）
   - **手动**：把 `suggested default` 的数字填入方案 schema 的 `llm_rerank.cpu_cores`
4. 托盘右键 → 重新部署 → 生效

`auto_adapt`（默认开启）在运行时会再次兜底：系统负载 >85% 自动切到 4 线程让路，<60% 恢复。不跑本工具也可以直接用默认值 6。

> 内存需求：0.8B Q4 模型加载后 WeaselServer 占用约 2GB。**内存 ≤4GB 的机器建议 `backend: off`**（输入法照常使用，仅无 LLM 重排），或调小 `min_free_mem_mb` 谨慎尝试。

## 构建（开发者）

### 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| Visual Studio 2022 Build Tools | v143 | 编译 |
| Boost | 1.84.0 | weasel 构建（`weasel/deps/` 下，官方 `install_boost.bat` 可获取） |
| librime 依赖 | marisa-trie / opencc / leveldb / yaml-cpp / glog | `cd librime && git submodule update --init --recursive`，按 librime 官方流程构建（产出 install 目录：marisa.lib/opencc.lib/leveldb.lib/yaml-cpp.lib） |
| llama.cpp | master（MT 静态构建） | llm_filter 推理后端 |
| GGUF 模型 | 任意小模型 | 推理模型 |

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

:: 4. 构建 weasel
:: 将顶层 librime/include/rime_api.h 覆盖到 weasel/librime/include/
:: 将 librime 构建的 rime.lib 放入 weasel/librime/build/lib/Release/
cd weasel
env.bat
msbuild weasel.sln /p:Configuration=Release /p:Platform=x64
```

> `src/CMakeLists.txt` 中 `LLAMA_ROOT` 为 CMake CACHE 变量，可用 `-DLLAMA_ROOT=<你的路径>` 覆盖。
> CUDA 后端（`backend: gpu`）需自行接入 llama.cpp CUDA 构建，本仓库验证的是 CPU 路线。

### 替换安装文件

产物：`librime/build/src/Release/rime.dll`、`weasel/output/Release/weaselx64.dll`、`WeaselServer.exe`、`WeaselDeployer.exe`，另需 `librime install/bin/opencc.dll`（rime.dll 的动态依赖）和 `vcomp140.dll`（VC OpenMP 运行时）。替换小狼毫安装目录同名文件后，注销重登（TSF 注册表缓存）或重启系统。

## 候选窗 AI 标记

LLM 重排生效时，首选候选的 comment 显示来源徽章：

| 标记 | 含义 |
|------|------|
| `AI·TSF` | 上文来自 TSF 光标前文本（首选通道） |
| `AI·历史` | 上文来自上屏历史回退（TSF 不可用场景） |

样式可配置（weasel.custom.yaml）：

```yaml
style:
  # AI 首选标记色（默认金色；设透明色 0x00000000 可关闭）
  # 注意: 默认 color_format: abgr, 8 位按 AABBGGRR 字节序 → 金色写 0xFF00B8E8
  ai_comment_text_color: 0xFF00B8E8
```

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
