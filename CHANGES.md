# 改动清单：在原版 weasel + librime 上增加 LLM 功能

对应上游版本：**weasel 0.17.4**（GitHub rime/weasel）+ **librime 1.17.0 @ 1d0df6e**（GitHub rime/librime）。

LLM 候选重排为**源码级集成**：`llm_filter` 作为 librime 原生 filter 组件（C++）编译进 rime.dll，llama.cpp 静态链接做推理；上文由 weasel TSF 原生采集（光标前 64 字符），经 IPC 传入 librime。无 lua 插件、无外挂进程。

| 组件 | 修改文件 | 新增文件 | 规模 |
|------|---------|---------|------|
| librime | 4 个（+85 行） | 2 个（llm_filter，919 行） | ~1000 行 |
| weasel | 11 个（功能）+ 3 个（构建配置） | 1 个（afxres.h stub） | ~400 行 |

---

## 一、librime 改动（6 个文件）

### 1. 新增 `src/rime/gear/llm_filter.h` / `.cc`（核心，919 行）

原生 filter 组件 `LlmFilter`，在 schema 的 filters 链中注册（uniquifier 之后），配置节 `llm_rerank`（全部可选，默认与旧 lua 插件项目一致）：

```yaml
llm_rerank:
  enabled: true         # true=启用 LLM 重排 | false=关闭（组件透传，不推理）
  min_code_len: 4       # 输入编码长度小于此值时不重排
  max_code_len: 0       # 编码长度上限（0=不限制）；超出不推理，与 min_code_len 组成触发区间
  multi_char_first: false # true=long-word-first: 候选算完 CE 后按词长降序, 同词长按 CE 评分序
  min_tokens: 1         # 上文 token 数小于此值时不推理
  max_tokens: 10        # 上文 token 上限
  max_candidates: 5     # 参与打分的候选数上限
  cpu_cores: 4          # 推理线程数（自动不超过物理核数）
  min_free_mem_mb: 2560 # 可用内存低于此值时不加载模型（防小内存机器系统卡死）
  model_path: d:/gguf_models/Qwen3.5-0.8B-Q4_K_M.gguf
```

> 注：`backend` 选项已随 GPU 版退役移除，`enabled` 承担开关职责（08-14 与插件版参数对齐）。

功能模块：

| 模块 | 说明 |
|------|------|
| `score_batch` | 三步打分：Step1 上文 ctx decode → KV cache copy → 并行候选 decode（n_seq_max=12 序列）→ 逐 token 交叉熵 CE 评分 |
| `prepare` 预解码 | commit 后异步线程执行 Step1 并保存 logits（带请求代数防过期）；下次 score 命中直接复用（prep=1，Step1 从 ~50ms → 0ms） |
| 长候选外推 | 4+ token 候选按尾部 CE 外推 `λ=0.5`（语料模拟调参），不增加 decode 次数，3-token 词不受长词挤压 |
| 上文双通道 | TSF 光标前文本（`get_context_text`）优先；TSF 采集不可用时回退 commit_history（`context_text_valid` 区分"合法空"与"不可用"） |
| 隔断处理 | 编辑键/窗口切换 → reset 代次变化 → 上屏历史作废；换行（CRLF/LF/CR）只取最后一段为上文 |
| 防护 | 可用内存 < `min_free_mem_mb` 时跳过模型加载；线程数不超过物理核数 |
| 日志 | `%APPDATA%\Rime\rime_llm_filter_log.txt`：每推理一行 score（wait/S1/KV/S2/total/prep/ctx_tok/cand）+ 推理事件行（时间\|序号\|input\|候选\|上文\|结果\|耗时\|来源） |

### 2. `src/rime_api.h`（+17 行）— 6 个新 C API

```c
void (*set_context_text)(const char* text);            // server 写入光标前上文（TSF 送达）
const char* (*get_context_text)(void);                 // 读取上文（组件/脚本消费）
Bool (*context_text_valid)(void);                      // TSF 是否成功采集过
void (*reset_context_text)(void);                      // 清缓存 + reset 代次++
int (*context_reset_generation)(void);                 // 代次查询（代次变 = 旧上屏历史作废）
void (*set_context_changed_callback)(void (*cb)(const char*));  // 文本更新回调（预解码触发）
```

### 3. `src/rime_api_impl.h`（+49 行）— 上述 API 实现

mutex 保护的全局上下文缓存 + `thread_local` 读缓冲（锁外安全返回指针）+ reset 代次 + 回调注册，在 `rime_get_api()` 中挂载。

### 4. `src/rime/gear/gears_module.cc`（+2 行）

```cpp
r.Register("llm_filter", new Component<LlmFilter>);   // 组件注册
```

### 5. `src/CMakeLists.txt`（+17 行）

llama.cpp 集成：`LLAMA_ROOT` CACHE 变量（默认 `D:/llama.cpp-mirror`，可 `-DLLAMA_ROOT=` 覆盖）+ 静态链接 `llama.lib` + `ggml.lib`/`ggml-base.lib`/`ggml-cpu.lib`（llm_filter 推理后端，MT 静态运行时构建）。

---

## 二、weasel 改动（14 个文件）

### 1. TSF 上文采集（WeaselTSF）

| 文件 | 改动 |
|------|------|
| `WeaselTSF/WeaselTSF.cpp` | 新增 `CGetTextBeforeCaretEditSession`：文档锁内从 [doc_start, caret] 扩展 + `ShiftStart(-64)` 采集光标前 **64 字符**（LLM 上文 20 token × 1-2 字/token 绰绰有余；空 range 直接负向 ShiftStart 实测返回 0 字符，故从 doc_start 扩展）。新增 `_RequestContextText`（`TF_ES_READ\|TF_ES_ASYNCDONTCARE` 只读异步编辑会话）、`_OnContextTextReady`（**detach 独立线程**调 `m_client.SetContextText`，禁在 TSF 文档锁内同步 IPC）、`_Utf8FromWide`。`OnSetThreadFocus` 追加 `ResetContext`（窗口切换 → commit-history 兜底作废）+ 采集新窗口上文 |
| `WeaselTSF/KeyEventSink.cpp` | 新增 `_HandleEditKeyReset`：composition 为空时，退格/删除/方向键/Home/End/PageUp/Down/回车 → `ResetContext`。**四个按键回调**（KeyDown/KeyUp/TestKeyDown/TestKeyUp）全部补 `_RequestContextText`（KeyUp 路径覆盖只走 KeyUp 派发按键的应用，如聊天窗口） |
| `WeaselTSF/WeaselTSF.h` | 新增方法/成员声明（`_RequestContextText`、`_OnContextTextReady`、`_HandleEditKeyReset`、`_Utf8FromWide`、`m_textBeforeCaret`）+ `friend class CGetTextBeforeCaretEditSession` |

### 2. IPC 协议（WeaselIPC / WeaselIPCServer）

| 文件 | 改动 |
|------|------|
| `include/WeaselIPC.h` | 枚举新增 `WEASEL_IPC_SET_CONTEXT_TEXT`、`WEASEL_IPC_RESET_CONTEXT`（插在 CHANGE_PAGE 与 LAST_COMMAND 之间）；`Client` 接口新增 `SetContextText`（UTF-8 管道 body 传递）/ `ResetContext` |
| `WeaselIPC/WeaselClientImpl.cpp` | `SendContextText`：UTF-8 → wstring → `ClearBufferStream` + `Write` + `Transact(SET_CONTEXT_TEXT, body 字节数)`；**刻意不查 `_Active()`**——该方法可能从 TSF 回调 detach 的独立发送线程调用，thread_local 管道句柄未连接会误判 false，`Transact` 内部按需建连。`SendContextReset`：`Transact(RESET_CONTEXT)`。Client 包装层新增 `SetContextText`/`ResetContext` |
| `WeaselIPC/WeaselClientImpl.h` | 两个新方法声明 |
| `WeaselIPCServer/WeaselServerImpl.cpp` | 新增 `OnSetContextText`（body 限 4096 宽字符 → UTF-8 → `rime_get_api()->set_context_text`，不经 RequestHandler）、`OnResetContext`（`reset_context_text`）；`HandlePipeMessage` 挂两条分派；**`_ProcessPipeThread` 改单次 `ReadFile` 读入头+body**（消息模式原子性，绕开 `_Receive` 部分读取后二次读 body 的死锁；容忍 183/234 错误码），头拷入 msg 后 body memcpy 到 `SendBuffer()` |
| `WeaselIPC/PipeChannel.cpp` | `_Receive`：`ReadFile` 失败且错误码 `ERROR_MORE_DATA(234)` 或 `ERROR_ALREADY_EXISTS(183)` 时不再抛错，fallback 二次 `ReadFile` 读完剩余消息（头+body 消息的健壮性修复） |

### 3. rime_api.h 补丁副本（weasel 树内 `librime/` 子目录）

| 文件 | 改动 |
|------|------|
| `librime/include/rime_api.h` | 追加 5 个函数指针（set/get_context_text、context_text_valid、reset_context_text、context_reset_generation）——weasel 编译 `#include <rime_api.h>` 实际命中的是此副本 |
| `librime/src/rime_api.h` | 早期补丁副本（仅 set/get_context_text，演进遗留，不影响构建） |

### 4. 构建配置（非功能改动）

| 文件 | 改动 |
|------|------|
| `weasel.props` | BOOST_ROOT 改为本机绝对路径（上游为相对形式） |
| `env.bat` | 与上游一致（样板保留） |
| `include/afxres.h` | 新增 3 行 stub（BuildTools 无 MFC 组件，用 winres.h 替代官方 afxres.h） |

---

## 三、数据流

```
按键 / 切换窗口 (TSF)
  → _RequestContextText 采集光标前 64 字（文档锁内 ShiftStart）
  → _OnContextTextReady 独立线程发 IPC WEASEL_IPC_SET_CONTEXT_TEXT（body=UTF-8）
  → server: rime_get_api()->set_context_text() → librime 上下文缓存（mutex+TLS）
  → llm_filter: get_context_text 读取（TSF 不可用回退 commit_history，reset 代次同步）
  → score_batch（prepare 命中跳过 Step1）→ 候选按 CE 重排（长候选外推 λ=0.5）

编辑键 / 窗口切换 → WEASEL_IPC_RESET_CONTEXT → reset_context_text()（缓存清空 + 代次++）
  → llm_filter 的 commit_history 兜底从此从头积累
```

## 四、构建依赖（新增）

| 依赖 | 说明 |
|------|------|
| llama.cpp（MT 静态） | clone + cmake `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` 构建，产出 llama.lib + ggml*.lib，`-DLLAMA_ROOT=` 指定 |
| GGUF 模型 | 本机验证 `Qwen3.5-0.8B-Q4_K_M.gguf`，任意小模型均可 |
| opencc.dll / vcomp140.dll | rime.dll 的动态依赖（librime 官方构建链），部署时随 bin/ 分发 |

---

## 五、2026-08-08 修复记录（真机联调）

### 1. 管道互锁死锁（部分应用打不开，如 QQ 音乐）

**症状**：输入法运行期间部分应用（QQ 音乐等）无法打开；退出算法服务立即打开。

**根因**：管道 `FlushFileBuffers` 互锁——客户端写入+Flush 等待 Server 读取，Server 响应+Flush 等待客户端读取，互相阻塞（WinDbg 栈捕获：Server 线程卡在 `_WritePipe → FlushFileBuffers`）。

**修复**（`WeaselIPCServer/WeaselServerImpl.cpp` + `WeaselIPC/PipeChannel.cpp`）：`_WritePipe`/`_Send` 增加 `flush` 参数，**Server 响应 `_Send(pipe, resp, false)` 不 Flush**。

### 2. 模型加载饥饿（加载 13 分钟卡死）

**根因**：加载阶段 `THREAD_PRIORITY_BELOW_NORMAL` 使 llama worker 线程饿死（warmup 阶段多线程解码）。

**修复**（`src/rime/gear/llm_filter.cc`）：只在**单线程加载**阶段降优先级，warmup 前恢复 NORMAL。

### 3. TSF 注册链（语言栏/输入法切换失效）

部署方式教训（详见 README 常见问题）：CTF\TIP 是 32/64 共享键、System32 rename 会触发 TSF 注销、手动注册缺 Category 结构不识别、恢复后需设置添加键盘。**最终部署方式**：官方安装器 → 设置添加键盘 → `bin/deploy_llm.bat`（System32 组件用 MoveFileEx **延迟替换**，重启生效）→ 重启系统。

### 4. ESC 清编码后无推理

**根因**：客户端去抖"相同文本不重发"与 Server `reset_context_text` 清空不一致——重发被跳过，Server 上下文永远空。

**修复**（`WeaselTSF/WeaselTSF.cpp` + `.h`）：新增 `_OnContextReset()`，在 `ResetContext` 后清掉"已发送"标记，下次采集强制重发。

### 5. 快速打字上文滞后（`试剂`→`事迹`错词 + 无 AI 标记）

**症状**：三行"英雄事迹/化学试剂/二十世纪"慢速正确；快速打字时"化学试剂"→"化学事迹"、"二十世纪"→"二十事迹"，且无 AI·TSF/历史标记。

**根因**：上屏后客户端等 300ms 去抖才发送光标上文；快速打字（~100ms/键）下一词 4 码打满（~350ms）早于上文送达 → Server 上下文为空 → 重排跳过（无标记）→ 词库默认顺序顶上错词。

**修复**（`WeaselTSF/WeaselTSF.cpp`）：去抖 **300ms → 100ms**（Server 端 `on_context_changed` 另有 300ms 节流 + 上下文去重防解码风暴，文档更新风暴场景不受影响）。

---

## 六、2026-08-14 修复记录（TSF 上文采集四模块 + 参数对齐 + 发布体系）

### 1. TSF 光标上文四模块

| 模块 | 说明 |
|------|------|
| OnEndEdit selection-change 采集 | 非 composing 时 selection 变化也触发上文采集（此前只监听 TextEditSink 文档变化，光标移动后上文滞后） |
| 编辑键 reset 恢复 | 08-11 因 WPS 停用（同步 Transact 拉长 TSF 处理链）→ 改**异步线程发送**恢复；判定在按键送达 server **之前**（`_status.composing` 是上一拍状态——有编码时退格被方案转 ESC 清码，事后判定 composing 已变 false 会误清上文） |
| OnSetFocus(DocumentMgr) reset+collect | 仅 DocumentMgr 真变化时执行（同应用内重聚焦频繁，否则打字期间缓存反复被清） |
| ShiftStart(-64) 主路径 | 负移只在真移动时 honored（IsEqualStart 判据）；transitory context 回退 MOVESTART 大块方案（迭代上限 128 防死循环） |

### 2. WPS 滞后检测（llm_filter）

WPS 自研适配层只暴露最近 composition 文本（连续打字只采到 2 字符）+ ShiftStart 负移不 honored。判定层新增**滞后检测**：TSF 文本是上屏历史的尾部子串且长度 < 一半 → 自动改用更全的 fallback（标 AI·历史但上文完整）。WPS 全历史 + 完整上文，记事本/VSCode 全 TSF 无误伤。

### 3. 参数对齐（插件版 → 源码版）

`max_code_len`（触发区间上限，0=不限制）+ `multi_char_first`（long-word-first：候选算完 CE 后按词长降序，同词长按 CE 评分序）。

### 4. 关键 bug 修复

- **RimeApi 头布局不同步**：`weasel\librime\include\rime_api.h` 与 `librime\src\rime_api.h` 必须一致（函数指针表错位 → 调用错误函数 → 字母直出）
- **UTF-8 8 字节截断**切在多字节字符中间 → 残留检测恒误判；需字符边界对齐
- **IPC buffer 竞争**：异步 reset 线程与 flush/主线程并发写 channel buffer → 消息体错位（"focus:switch"→"cus:switch"）；ClientImpl 全部发送加 recursive_mutex
- **空文本语义**：打字期间 selection-change 采集到 transient 空 → 客户端 800ms 去抖 + 服务端 age 判定区分 transient 空 vs 真空
- **bin\data 缺失** → detect_modifications 抛异常 → 永久维护循环（每键部署弹窗）

### 5. 构建/发布体系

- **32/64 位双构建**：Win32 平台 → `weasel.dll`（32 位），x64 → `weaselx64.dll`（64 位）；32 位 boost 用 `b2 architecture=x86 address-model=32`（-x32- 后缀库）；`scripts/` 构建链全部 %~dp0 相对化并发布
- **部署**：WeaselSetup /u + /i 官方流程（双 TSF + 64 位注册）；32 位视图注册失效需手动 `SysWOW64\regsvr32.exe /s` + `TEXTSERVICE_PROFILE=hans`；System32 被锁用 MoveFileEx DELAY_UNTIL_REBOOT
- **GitHub Releases**：bin 二进制不进 git，发布走 tag + Releases 部署包 zip（`rime-llm-deploy-*.zip`：7 二进制含 bench_threads.exe + 4 脚本，`deploy_llm.bat` 7 步一键部署）
- **测试工具**：`scripts/test_ipc.ps1`（IPC 直发验证）、`scripts/test_rime.cpp`（LoadLibrary 冒烟，从 bin 运行）、`scripts/tsf_switch.ps1`（注册表切换测试 DLL 免重启）

---

## 七、2026-08-16 优化记录

### 1. score 结果缓存（翻页/候选窗重建不重复推理）

对齐插件版 `_G.llm_filter_cache`：`Collect()` 缓存「(ctx, input) → 评分顺序（候选文本）」，
同一编码 + 同文的重排直接复用，不再跑 S2/S3 候选 decode（~36ms/次）。
- 只存评分顺序，`multi_char_first` long-word-first 排序与 AI 徽章每次按当前配置重放（改配置后缓存仍正确）
- 失效：reset 代次变（编辑键/窗口切换）→ 缓存作废；ctx/input 变 → key 不匹配自然失效
- 事件日志仅真实推理时写（缓存命中省日志 IO）

### 2. 上文来源判定纯逻辑 + 回归测试

`GetContextTextPair` 的判定（滞后检测/残留检测/空文本分类/UTF-8 边界对齐）抽取为
`ctx_logic` 命名空间（不依赖引擎/模型），新增 `scripts/test_llm_context.cpp` 复制同函数
做 20 个 gold 断言（`scripts/build_test_llm_context.bat` 编译运行）。改判定必须同步测试。

### 3. enabled=false 释放模型

filter 重建（重新部署）时 enabled=false 分支调用 `unload_model()` 释放已加载模型
（WeaselServer ~2GB），不再常驻到进程退出。加 `lagging` 空串防御（生产调用侧已排除空 TSF）。

### 4. bench_threads.exe 入发布包

`scripts/bench_threads.cpp`（自插件版同源）+ `scripts/build_bench_threads.bat`（LLAMA_ROOT 环境变量，
默认 `D:/llama.cpp-mirror`）；预编译 exe 进 Releases 部署包 zip（7 二进制）。
