# rime-llm-ime — 会用 AI 重排候选的小狼毫

LLM 候选重排**源码级集成**进 RIME 小狼毫：本地小语言模型（Qwen3.5-0.8B，约 500MB）依据你正在输入的句子实时给候选排序——刚打过的字决定下一个词谁排第一。全离线运行，无云端无账号，普通 CPU 即可（参考延迟 ~43ms/键）。

## 效果

| 能力 | 说明 |
|------|------|
| 光标前文上下文 | 读取光标前面的整句作为排序依据；刚上屏的词立即参与下一词排序 |
| 自动回退 | 光标文本采集不可用的应用（WPS、部分 32 位应用）自动改用上屏历史，功能等价 |
| 生效可见 | 重排后的首选候选注释位置带来源标记：`AI·TSF`（光标文本）/ `AI·历史`（上屏历史） |
| 按方案启用 | 在方案的 `engine/filters` 列出 `llm_filter` 才生效（Rime 惯例，位置由方案定）；参数零改动热调 |
| 参数热改 | 设置界面保存即生效，无需重新部署 |
| 长词不挤压 | 4+ 字长候选按尾部评分外推，不挤占短词 |

> 与 [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank)（插件版）功能等效，二选一；光标前文采集仅本版支持。

## 研究主要结论

重排核心 = 对同码候选做交叉熵评分 `argmax P(wᵢ|上文)`（只排序、不生成、编码无关）。三项关键技术：**分层并行解码**（候选共享上文、≤3 次 decode，与候选数无关）、**预解码 + KV 代次**（commit 后异步算上文，按键延迟 ~43ms）、**长词尾部外推**（λ=0.6）。主要数字：

- 首选率 **93.4%**（10 token 上文 / 5 候选，20000 样本），单字 96.8%；模型上限 ~94.3%；
- 0.8B 即最优（2B 仅 +0.3pp 但延迟 3×、体积 2.6×）；10 tok / 5 cand 为性价比最优点；
- 用户词频对数融合（`freq_beta=1.5`，fused = LLM分 + β·log(1+eff)）：词频无上限、强个人高频词可翻盘（本机真实窗回放事前口径 +0.4pp）；

完整研究文档（方法细节、扫参全表、词频融合研究）在本地研究资料库 `D:\llm-rerank-research\`。

## 安装

### 安装包（推荐）

1. 下载 `weasel-llm-setup-<日期>.exe`（约 10MB：[v2026.08.31](https://github.com/zhanghaozhecn/rime-llm-ime/releases/download/v2026.08.31/weasel-llm-setup-2026.08.31.exe)｜[全部版本](https://github.com/zhanghaozhecn/rime-llm-ime/releases)）双击。全新机器无需先装官方小狼毫；已有官方小狼毫则原地升级、不动注册
2. **在要启用的方案里加组件**（一次性，见下"方案接入"），托盘右键 → **重新部署**
3. 托盘右键小狼毫 → **"LLM 重排设置"**：首次打开提示下载模型（约 500MB，ModelScope 断点续传，中断重试自动续传）→ 勾选**启用 LLM 重排** → **保存并生效**（立即热重载）
4. 验证：打满 4 码，首选候选出现 `AI·TSF` / `AI·历史` 标记即已生效

> 已运行的应用需重启（或注销重登）托盘里才有新菜单项；重装/修复 = 重跑安装包（幂等）；输入法图标消失（罕见）→ 右键安装目录下的 `repair_tsf.ps1` → "使用 PowerShell 运行"。

### 设置界面

托盘右键 → "LLM 重排设置"。所有参数可选，默认值开箱即用：

| 参数 | 默认 | 说明 |
|------|------|------|
| 启用 LLM 重排 | 关 | 总开关，保存即热生效 |
| 模型路径 | `%APPDATA%\Rime\` | "浏览…"选已有文件，或"下载"从 ModelScope 断点续传到该路径 |
| 最小/最大编码长度 | 4 / 0 | 编码长度触发区间（最大 0=不限）；四码定长方案用默认 |
| 预期词长权重 | 0.2 | 词长==码长/2 的候选在融合分上获得"分数跨度×权重"加成（与词频β同一结合公式）。冷启动标定（2026-09-03，新用户视角字典窗+零词频）：真实打字 96.3% 双字提交（远高于语料 75.4%，长词倾向拆分），argmax [0.15,0.2]，≥0.4 转负；0 关闭 |
| 词频β | 1.5 | 对数融合（公式见下）——词频无上限，越常上屏的词越能翻盘；0 关闭 |
| 最少/最大上文 token | 1 / 10 | 上文 token 区间（不足不推理、超出截尾） |
| 候选数上限 | 5 | 参与打分的候选数 |
| CPU 线程数 | 4 | 推理线程数，不超过物理核数 |

GUI 写的是全局配置 `%APPDATA%\Rime\llm_rerank.yaml`。优先级：**方案内 `llm_rerank:` 节 > 全局 yaml > 内置默认**——要为某个方案单独定制时，在方案文件里加 `llm_rerank:` 节（键名与上表一致）即可，其余方案不受影响。所有改动（含开关与模型路径）保存后**下一次按键即时生效**：改模型路径自动卸载旧模型并按新路径重载；关闭开关即卸载释放内存。

### 词频与词长融合公式

"词频β"（`freq_beta`，默认 1.5）与"预期词长权重"（`expected_length_weight`，默认 0.2；均 0=关闭）按统一结合公式在**对数域三项相加**：

```
fused(w) = score(w) + β · log(1 + eff(w)) + elw · span · [ 词长(w) == ⌊码长/2⌋ ]
```

- `score(w)` = 原始 LLM 分，即候选词在上文条件下的负交叉熵（−CE，对数概率域，越大越好）
- `eff(w)` = Rime 同源的时间衰减计数（librime `algo::formula_d`，与引擎调频 userdb 同一公式）：

```
提交（每次含中文上屏，全局 tick ← tick + 1）:   dee ← 1 + dee · exp((t_w − t) / τ)
查询（打分时刻）:                               eff = dee · exp((t_w − t) / τ)
τ = 200 tick，半衰期 ≈ 139 次提交
```

- `dee` = 该词的衰减计数，`t_w` = 该词最后提交的 tick，`t` = 当前全局 tick。近期常打的词 eff 高，久未使用自动消退
- `span` = 窗内有效原始分的 max−min；词长项只在"两码一字"方案匹配期望词长时生效
- 三项可组合：冷启动 eff≡0 自动退化为 `score + elw·span·匹配`；成熟期三项叠加（2026-09-03 起词长加成作用于融合分，修复旧实现按原始分重排导致两项互斥的缺陷）
- 词频由上屏自动统计（RIME 用户目录 `user_freq.tsv`，仅中文词，每 20 词落盘；格式 `词\t累计\tdee\ttick`，首行 `#tick=N`）
- 标定与设计取舍的完整记录见本地研究资料库 `freq-fusion-research.md`

### 方案接入（llm_filter 为显式组件）

`llm_filter` 遵循 Rime 惯例：**方案在 `engine/filters` 里显式列出才参与重排**，未列出的方案完全不受影响。位置由方案与其他 filter 的先后关系决定（重排拿到的是其上方 filter 处理过的候选；一般放在简化/去重类 filter 之后）：

```yaml
# <方案>.schema.yaml
engine:
  filters:
    - simplifier
    - uniquifier
    - llm_filter      # 加这一行，位置按需调整
```

改完**重新部署**生效。开/关有两层：去掉这一行（需重新部署）= 该方案彻底关闭；保留这一行用设置界面的"启用 LLM 重排"开关（热生效，无需重新部署）。

### 手动安装（无法运行安装包时）

1. 下载 GGUF 模型（建议 ≤2B Q4；本机验证用 `Qwen3.5-0.8B-Q4_K_M.gguf`）放到 `%APPDATA%\Rime\`（RIME 用户文件夹根，模型直接放根）
2. 将 `bin/` 下 **9 个文件**复制到小狼毫安装目录（`C:\Program Files\Rime\weasel-0.17.4`）：`rime.dll`、`weaselx64.dll`、`weasel32.dll`（改名为 `weasel.dll`）、`WeaselServer.exe`、`WeaselDeployer.exe`、`WeaselLLMSetup.exe`、`opencc.dll`、`vcomp140.dll`、`WinSparkle.dll`（后三个是运行依赖，官方安装目录有前例可拷）
3. 原位替换系统 TSF 组件（不碰注册表）：`weaselx64.dll` → `C:\Windows\System32\weasel.dll`，`weasel32.dll` → `C:\Windows\SysWOW64\weasel.dll`。先停 `WeaselServer.exe`/`WeaselDeployer.exe`，旧文件改名 `*.llm_old` 腾位后复制。**切勿运行 `WeaselSetup /u`**——它会删 TSF 注册且难以恢复（应急：`installer\repair_tsf.ps1`）
4. 托盘右键 → **重新部署**（词典 build 必须由本版 librime 编译）

### 切换到插件版

重装官方小狼毫 → 运行[插件版安装器](https://github.com/zhanghaozhecn/rime-llm-rerank)（其"方案配置加 LLM"自动剥离本版配置再插入，无需手动还原）。

## 常见问题

- **Win+Space 没有小狼毫**：设置 → 时间和语言 → 语言 → 中文 → 键盘 → 添加键盘 → 小狼毫
- **某些应用里标记是 `AI·历史`**：该应用不支持光标文本采集（32 位/WPS 等），自动用上屏历史，属正常回退
- **托盘没有"LLM 重排设置"**：菜单由各应用进程内的 TSF 组件提供，已运行的应用需重启后可见，新开应用立即可见
- **内存紧张（≤4GB）**：模型加载后服务进程约占 2GB，建议保持关闭（模型路径留空 + 不启用即不占内存）
- **延迟偏高**：看日志（见下）total 是否 ~40ms 量级；~80ms 说明预解码未命中或 `cpu_cores` 超物理核数

## 日志（排查）

`%APPDATA%\Rime\rime_llm_filter_log.txt`：每次推理一行延迟分解（wait/S1/KV/S2/total/prep/ctx_tok/cand）与事件行（编码/候选/上文/排序结果/来源）；会话启动的 `config:` 行显示当前生效参数与配置来源。反馈问题时附上此文件。

## 从源码构建（开发者）

### 依赖

| 依赖 | 版本 | 获取 |
|------|------|------|
| Visual Studio 2022 Build Tools | v143（含 vcvars64.bat） | 官方安装器，C++ 桌面开发工作负载 |
| Boost | 1.84.0 | `weasel\install_boost.bat`（官方脚本自动下载） |
| librime 依赖 | marisa / opencc / leveldb / yaml-cpp / glog | `cd librime && git submodule update --init --recursive` 后官方 `build.bat` |
| llama.cpp | master | MT 静态构建（命令见下） |
| Inno Setup 6 | 6.x | 仅打包安装包时需要 |

### 目录结构

```
rime-llm-ime/
├── librime/              # librime 源码（上游 1.17.0 + llm_filter 显式组件）
│   └── src/rime/gear/llm_filter.{h,cc}
├── weasel/               # weasel 源码（上游 0.17.4 + TSF 上文采集 / IPC 扩展）
│   ├── WeaselTSF/          # 光标上文采集 + 托盘菜单
│   └── WeaselLLMSetup/     # 设置 GUI
├── bin/                  # 本地构建产物（gitignore；打包前同步到 installer\source\）
├── installer/            # setup.iss + 载荷 source\（入库）+ repair_tsf.ps1 → dist\ 安装包
└── scripts/              # 构建 / 打包 / 测试脚本
```

### 构建步骤

weasel 基底为 **0.17.4 release tag**（勿用 master——IPC 协议有差异，混用会输入失效）。相对上游的完整改动清单与维护陷阱（rime_api.h 双副本同步、include 路径等）见项目记忆 `memory\upstream-diff.md`，不在本文展开。

```bat
:: 0. 首次：boost + librime 依赖
weasel\install_boost.bat
cd librime
git submodule update --init --recursive
build.bat
cd ..

:: 1. llama.cpp MT 静态构建（任选目录，记为 <llama目录>）
git clone https://github.com/ggml-org/llama.cpp <你的 llama 目录>
cd <你的 llama 目录>
cmake -B build-mt -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF
cmake --build build-mt --config Release
cd ..

:: 2. rime.dll（llm_filter 链接 llama）
set LLAMA_ROOT=<你的 llama 目录>
scripts\cmake_rime.bat          :: → librime\build\*.sln
scripts\build_rime.bat          :: → librime\build\bin\Release\rime.dll

:: 3. weasel（首次：weasel\weasel.props 从 template 复制并设 BOOST_ROOT；
::    rime.lib 入库用 scripts\gen_rime_lib.bat）
scripts\build_tsf.bat           :: x64 TSF → weasel\output\weaselx64.dll
scripts\build_server.bat        :: → weasel\output\WeaselServer.exe
scripts\build_llm_setup.bat     :: → bin\WeaselLLMSetup.exe
:: 32 位 TSF（可选）：scripts\build_boost32.bat + scripts\build_tsf32.bat
```

`cmake_rime.bat` 的依赖探测优先 `deps\prebuilt\`（本机布局），回退 librime 官方安装位置；`RIME_DEPS`/`BOOST_ROOT`/`LLAMA_ROOT` 环境变量可覆盖。仅 CPU 路线。

### 打包

产物放进 `bin\`（32 位 TSF 改名 `weasel32.dll`；`opencc.dll`/`vcomp140.dll`/`WinSparkle.dll` 从官方安装目录拷入）→ `installer\make_installer.ps1` 同步载荷到 `installer\source\` → `scripts\build_pkg.bat` 编译 → `installer\dist\` 下的 setup.exe。**载荷变动后必须 `dumpbin /DEPENDENTS` 复核全部导入**（流程与教训见项目记忆）。

## 许可证

| 部分 | 许可证 |
|------|--------|
| 本项目新增代码（llm_filter、WeaselLLMSetup 等） | **GPL-3.0**（本仓库 LICENSE） |
| weasel（含其内 librime 头文件副本） | GPL-3.0（weasel/LICENSE.txt） |
| librime | BSD-3-Clause（librime/LICENSE） |
| llama.cpp（仅链接） | MIT |

## 相关项目

- [拼读双拼](https://github.com/zhanghaozhecn/rime-pindu-double-pinyin) — 本方案使用的编码方案（带调双拼）
- [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank) — 插件版（lua + DLL 路线），功能等效，上文仅用上屏历史
