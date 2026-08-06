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
├── scripts/           # 构建脚本（Windows）
├── bin/               # 预编译产物（rime.dll / weaselx64.dll / WeaselServer.exe / WeaselDeployer.exe）
└── sync.py            # 本机维护脚本（编译源 → 项目目录 → GitHub 树）
```

## 快速开始（普通用户）

前置：已安装小狼毫 0.17.x（x64）。

1. 下载 GGUF 模型（本机验证使用 `Qwen3.5-0.8B-Q4_K_M.gguf`，任意小模型均可，建议 ≤2B Q4）
2. 将 `bin/` 下 4 个文件复制到小狼毫安装目录，覆盖同名文件（如 `C:\Program Files\Rime\weasel-0.17.4`）
3. 将 `pdsp.schema.yaml` 复制到 RIME 用户目录（`%APPDATA%\Rime\`），按需修改 `llm_rerank` 配置节（至少改 `model_path`）
4. 托盘右键 → **重新部署**；若替换过 `weaselx64.dll`/`WeaselServer.exe` 需注销重登或重启

> 使用其他方案：把 `llm_rerank` 配置节加入任意 RIME 方案，并确认其 filters 链末尾（uniquifier 后）有 `llm_filter`。配置节仅依赖四码输入编码，与具体方案无关。

### llm_rerank 配置节（全部可选）

```yaml
llm_rerank:
  backend: cpu          # off | cpu | gpu — off 时透传不推理
  min_code_len: 4       # 输入编码长度小于此值时不重排
  min_tokens: 1         # 上文 token 数小于此值时不推理
  max_tokens: 10        # 上文 token 上限
  max_candidates: 5     # 参与打分的候选数上限
  cpu_cores: 7          # 推理线程数
  model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf
```

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

产物：`librime/build/src/Release/rime.dll`、`weasel/output/Release/weaselx64.dll`、`WeaselServer.exe`、`WeaselDeployer.exe`。替换小狼毫安装目录同名文件后，注销重登（TSF 注册表缓存）或重启系统。

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
