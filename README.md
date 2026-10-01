# rime-llm-ime — 会用 AI 重排候选的小狼毫

## 快速开始（以朙月拼音为例，约 5 分钟）

> 目标：让 **朙月拼音 `luna_pinyin`**（Rime 自带默认方案）用上 LLM 候选重排。换别的方案同理：第 4 步的「编码匹配」按该方案的码长改，「预期词长权重」只对"两码一字"的定长方案保留默认 `0.2`（全拼要改 `0`）。

1. **装安装包**：到 [Releases 最新版](https://github.com/zhanghaozhecn/rime-llm-ime/releases/latest) 下载 `weasel-llm-setup-<日期>.exe` 双击（未签名——SmartScreen 弹窗选"更多信息 → 仍要运行"）。全新机器**无需**先装官方小狼毫；已有官方小狼毫则原地升级、不动注册表。装完向导不再问模型（下载在设置界面里）。
2. **打开设置**：托盘小狼毫 → 右键 → **「LLM 重排设置」**（本来就在运行的应用要重启/注销后才有这个菜单项）。
3. **方案接入**：`方案文件` 选 **`luna_pinyin.schema.yaml（程序）`**（带「（程序）」= 小狼毫自带的预装方案；保存时会先复制到用户文件夹再改，原件不动）。
4. **改参数 + 下模型**：
   - `触发条件` → `编码匹配` 填 **`.+`**（朙月拼音码长 1~6 不等，默认 `.{4}` 不会触发）。
   - `候选排序融合` → `预期词长权重` 填 **`0`** —— 该项按"词长 = 码长÷2"给候选加成，**只对"两码一字"的定长方案**（双拼、五笔、郑码…）有意义；全拼码长不固定，留着只会误伤（默认 0.2 是给定长方案的）。
   - `总控` → 点 **『下载模型』**（约 508 MB，ModelScope，断点续传）。
5. **点『保存并生效』** —— 写进该方案的 `llm_rerank:` 节并自动重新部署（缺组件行/配置节时会自动补齐）。
6. **验证**：**连打两个词**——先打 `nihao` 选「你好」上屏，再打 `shijie`，这时首选候选才会带上 `AI·TSF` / `AI·COM` / `AI·历史` 徽章。**光标前没有文字时不会有徽章**：重排至少要 1 个上文 token，刚装好（或空文档、刚切窗口）打第一个词时上文为空，引擎直接按原顺序放行，属预期。没生效：托盘小狼毫 → **重新部署**。

后续微调都在同一页：改 `推理规模`（上文 token / 候选数 / CPU 线程）与 `候选排序融合`（词频 β、词长权重）→ **保存并生效**；不想用了点 **『剥离』**（就在保存右侧：组件行与配置节一起删，自动重新部署）。

---

LLM 候选重排**源码级集成**进 RIME 小狼毫：本地小语言模型（Qwen3.5-0.8B，约 500MB）依据你正在输入的句子实时给候选排序——刚打过的字决定下一个词谁排第一。全离线运行，无云端无账号，普通 CPU 即可（参考延迟 ~43ms/键）。

## 效果

| 能力 | 说明 |
|------|------|
| 光标前文上下文 | 读取光标前面的整句作为排序依据；刚上屏的词立即参与下一词排序 |
| WPS COM 旁路 | WPS 文字/演示的 TSF 只暴露组合串、正文在自绘画布——本版经 **COM 文档模型**（宏系统同款对象模型）读取光标前 64 字真文，**含文档既有内容**（打开旧文档续写场景）；演示文稿文本框同样支持 |
| 统一标签页防串档 | WPS 多组件同窗标签（文字/演示/表格共用一个框架窗口、前台类名不随标签变）经**视图窗口判定 + 双组件探读 + 标题校验**正确区分活动文档，新建文档/演示标签自动跟随 |
| 自动回退 | 光标文本采集不可用/受限的应用（WPS 表格编辑态为 COM 盲区、部分 32 位应用）自动改用上屏历史，功能等价；受限判定粘性留存（WPS 内部切换不清除、只清历史） |
| 鼠标移动感知 | **任何鼠标点击即视为光标可能移动，自动重置兜底历史上文**（不做例外排除——上下文宁短不错）；编辑键（退格/导航/回车/撤销/重做/粘贴/剪切）与点击同时失效 COM 快照并触发 ~50ms 延迟重读；导航键（Home/方向键）后 TSF 客户端 80ms 主动重采集（纯光标移动不触发编辑事件，旧文本会冒充）。全部时序按设计标准校验：按键间隔 ≥50ms、光标移动后 ≥100ms——编辑后首词即恢复真文上文 |
| 生效可见 | 重排后的首选候选注释位置带来源标记：`AI·TSF`（光标文本）/ `AI·COM`（WPS 文档真文）/ `AI·历史`（上屏历史） |
| 按方案启用 | 在方案的 `engine/filters` 列出 `llm_filter` 才生效（Rime 惯例，位置由方案定） |
| 参数随方案 | 参数写在方案的 `llm_rerank:` 配置节里（与插件版同一套键，改完保存即自动重新部署生效） |
| 长词不挤压 | 4+ 字长候选只算前三 token 加权 CE（尾部外推已证伪删除） |

> 与 [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank)（插件版）功能等效，二选一；本版 TSF 采集 + COM 旁路，插件版 COM + UIA TextPattern + 历史三层。两版鼠标点击清兜底历史同语义。

## 安装

### 安装包（推荐）

1. 到 [Releases 最新版](https://github.com/zhanghaozhecn/rime-llm-ime/releases/latest) 下载 `weasel-llm-setup-<日期>.exe`（约 10MB）双击（未签名——SmartScreen 弹"Windows 已保护你的电脑"时选"更多信息 → 仍要运行"）。全新机器无需先装官方小狼毫；已有官方小狼毫则原地升级、不动注册。系统要求：Windows 10 或更高（更早版本未验证）
2. 安装向导**不再询问模型下载**（2026-10-01 定案：下载功能统一挪进配置 GUI）：装完在托盘「LLM 重排设置」→ **总控** → 点**「下载模型」**（约 508MB，ModelScope，curl 断点续传；落点 = 模型路径框，留空 = `%APPDATA%\Rime\Qwen3.5-0.8B-Q4_K_M.gguf`）。**安装过程不写任何配置文件**
3. **在要启用的方案里加组件与配置节**（一次性，三选一）：托盘右键 → "LLM 重排设置" → 选方案点"保存并生效"（缺组件行/配置节时会自动补齐：幂等插入 `llm_filter` + 写入 `llm_rerank:` 配置节 + 自动重新部署）；或运行安装目录下的 `schema_add.ps1`（右键"使用 PowerShell 运行"，交互同上、另支持 CLI 自动化）；或按下文"方案接入"手动加。之后托盘右键 → **重新部署**（用 GUI 保存则自动）
4. 若还没下模型：托盘右键 → **"LLM 重排设置"** → 总控 → 点**「下载模型」**（或"浏览…"选已有的 `.gguf`；状态行会提示文件是否存在）→ 点**保存并生效**（写入该方案并自动重新部署）
5. 验证：**先上屏一个词**（光标前无文字时不重排 → 首个词不带标记），再打满 4 码，首选候选出现 `AI·TSF` / `AI·COM` / `AI·历史` 标记即已生效

> 已运行的应用需重启（或注销重登）托盘里才有新菜单项；重装/修复 = 重跑安装包（幂等）；输入法图标消失（罕见）→ 右键安装目录下的 `repair_tsf.ps1` → "使用 PowerShell 运行"。

### 设置界面

托盘右键 → "LLM 重排设置"。**单页界面**（与插件版安装器同构）：**方案接入**（方案文件下拉）→ **总控**（启用开关 + 模型路径 + **下载模型** + 模型状态）→ **触发条件** → **推理规模** → **候选排序融合** → **诊断日志** → 底部 **保存并生效 / 剥离 / 关闭**（底色与插件版统一为白）。

**配置写在方案里**：先在上面选方案文件，再改参数，点"保存并生效"写进该方案的 `llm_rerank:` 节并**自动重新部署**（部署完成即生效）。**"保存并生效"是自适应的**：方案还没接入（缺配置节 / 缺 `llm_filter` 组件行 / 残留插件版组件行）时会先剥净再补齐组件行 + 写入配置节（原「接入 LLM」按钮已并入这里），已接入则只重写配置节；无配置节的方案默认勾选「启用」，点一次保存即完成接入。**"剥离"就在保存按钮右侧**，负责整体移除组件行与配置节。**"下载模型"在模型路径那一行**（curl 断点续传，失败保留 `.download` 分片可续传）。**方案下拉列出两处** `*.schema.yaml`：**用户文件夹** `%APPDATA%\Rime`（可写、Rime 优先）与**程序文件夹** `<小狼毫安装目录>\data`（预装方案 luna_pinyin / cangjie5 / bopomofo …，带「（程序）」后缀）；**打开下拉即重扫两处**（外部新增/删除方案后无需手动刷新），界面没有未保存改动时还会重读当前文件。预装方案**写入前会自动复制到用户文件夹**（写程序文件夹既要管理员、又会被下次升级覆盖），程序文件夹里的原件不受影响。若方案文件在界面读入后被外部改过，保存前会弹一次确认。界面**不留日志框**：正常只看状态行，出错才写 `WeaselLLMSetup_error.log`（程序所在目录；该目录不可写时回退 `%APPDATA%\Rime\`）。所有参数可选，默认值开箱即用；每项右侧「?」悬停看说明：

| 参数 | 默认 | 说明 |
|------|------|------|
| 启用 LLM 重排 | 关 | 总开关（写进方案节；关 = 卸载模型释放内存） |
| 模型路径 | `%APPDATA%\Rime\` | **下拉框**（可手输）：列出本机扫描到的 `.gguf`（Rime 用户目录 + `%USERPROFILE%\gguf_models`），"浏览…"选任意文件；状态行实时显示文件存在性/大小——**小于 100MB 显示"模型文件可疑"**（半截模型会让重排静默失效）。右侧**「下载模型」**就地下载（ModelScope 断点续传，失败留 `.download` 分片可重试；新下载会先结束上次遗留的 curl 进程）。**必须是绝对路径**（`D:\…` / UNC），相对路径保存会被拒绝；默认路径跟"用户目录"（`RIME_LLM_USER_DIR` → 注册表 `RimeUserDir` → `%APPDATA%\Rime`） |
| 编码匹配 | `.{4}` | **正则，全串匹配**——决定哪些编码触发重排。语法与语义同 Rime 自身 `speller/auto_select_pattern`（同一 boost::regex 引擎）：仅 4 码 `.{4}`｜4 码以上 `.{4,}`｜3-4 码 `.{3,4}`｜指定首码的 4 码 `[abcde]{4}`｜任意长度 `.+`｜**留空 = 不限制**。模式非法时回退默认并在 `rime_llm_filter_log.txt` 留痕。取代旧的"最小/最大编码长度"（旧键读取时忽略、保存后消失） |
| 预期词长权重 | 0.2 | 词长==码长/2 的候选在融合分上获得"分数跨度×权重"加成（与词频β同一结合公式）。冷启动标定（2026-09-03，新用户视角字典窗+零词频）：真实打字 96.3% 双字提交（远高于语料 75.4%，长词倾向拆分），argmax [0.15,0.2]，≥0.4 转负；0 关闭 |
| 词频β | 1.5 | 对数融合（公式见下）——词频无上限，越常上屏的词越能翻盘；0 关闭 |
| debug_fusion | 关 | 勾选"诊断日志"：逐块把评分→词频→词长全过程写入用户目录 `rime_llm_debug.txt`（与插件版同名同格式；commit 行无码字段、reset 行按代次记）；诊断排障用，平时关闭 |
| 上文 token 上限 | 10 | 截取的上文 token 数（最少上文 token 固定 1，不再是配置项） |
| 候选数上限 | 5 | 参与打分的候选数 |
| CPU 线程数 | 4 | 推理线程数，不超过物理核数 |

GUI 与接入助手写的都是**方案内的 `llm_rerank:` 配置节**（`%APPDATA%\Rime\*.schema.yaml`），键名与上表一致；**两版（本版与插件版 rime-llm-rerank）的方案配置节完全相同，只有组件行不同**（本版 `- llm_filter`，插件版 `lua_processor@*llm_processor` + `lua_filter@*llm_filter`）——切换版本时先接入会自动剥掉另一版组件行。参数没有内置默认之外的第二层来源（2026-09-30 起取消了旧的全局 `%APPDATA%\Rime\llm_rerank.yaml`；该文件若还在会被忽略，只有设置界面在"方案节没配 model_path"时会读它一次以迁移模型路径）。改动保存后自动**重新部署**生效：改模型路径按新路径加载；关闭开关即卸载释放内存。

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

llm_rerank:            # 参数节（GUI / schema_add.ps1 自动写入，键名同设置界面）
  enabled: true
  code_pattern: '.{4}'
  max_tokens: 10
  max_candidates: 5
  cpu_cores: 4
  freq_beta: 1.50
  expected_length_weight: 0.20
  debug_fusion: false
  # model_path: <绝对路径；留注释 = 默认 %APPDATA%\Rime\Qwen3.5-0.8B-Q4_K_M.gguf>
```

改完**重新部署**生效（设置界面保存会自动部署）。开/关有两层：去掉组件行（需重新部署）= 该方案彻底关闭；保留组件行、把节里 `enabled` 设为 false = 该方案关掉重排但保留参数。

### 手动安装（无法运行安装包时）

1. 下载 GGUF 模型（建议 ≤2B Q4；本机验证用 `Qwen3.5-0.8B-Q4_K_M.gguf`）放到 `%APPDATA%\Rime\`（RIME 用户文件夹根，模型直接放根）
2. 将 `bin/` 下 **9 个文件**复制到小狼毫安装目录（`C:\Program Files\Rime\weasel-0.17.4`）：`rime.dll`、`weaselx64.dll`、`weasel32.dll`（改名为 `weasel.dll`）、`WeaselServer.exe`、`WeaselDeployer.exe`、`WeaselLLMSetup.exe`、`opencc.dll`、`vcomp140.dll`、`WinSparkle.dll`（后三个是运行依赖，官方安装目录有前例可拷）
3. 原位替换系统 TSF 组件（不碰注册表）：`weaselx64.dll` → `C:\Windows\System32\weasel.dll`，`weasel32.dll` → `C:\Windows\SysWOW64\weasel.dll`。先停 `WeaselServer.exe`/`WeaselDeployer.exe`，旧文件改名 `*.llm_old` 腾位后复制。**切勿运行 `WeaselSetup /u`**——它会删 TSF 注册且难以恢复（应急：`installer\repair_tsf.ps1`）
4. 托盘右键 → **重新部署**（词典 build 必须由本版 librime 编译）

### 切换到插件版

重装官方小狼毫 → 运行[插件版安装器](https://github.com/zhanghaozhecn/rime-llm-rerank)（其"保存并生效"会自动剥离本版配置再插入插件版组件，无需手动还原）。

## 常见问题

- **Win+Space 没有小狼毫**：设置 → 时间和语言 → 语言 → 中文 → 键盘 → 添加键盘 → 小狼毫
- **某些应用里标记是 `AI·历史`**：该应用不支持光标文本采集（WPS 表格编辑态、部分 32 位应用），自动用上屏历史，属正常回退
- **WPS 里标记是 `AI·COM`**：WPS 文字/演示经 COM 文档模型读取光标前真文（TSF 只暴露组合串，COM 读的是文档本体）——这是预期行为，2026.09.11 起支持
- **第一个词没有 `AI·` 标记**：重排至少需要 1 个上文 token——光标前还没有任何已上屏文字时（刚装好、新开文档、刚切到某应用）上文为空，引擎保持原候选顺序（属预期）。上屏第一个词后，下一个词即带标记
- **托盘没有"LLM 重排设置"**：菜单由各应用进程内的 TSF 组件提供，已运行的应用需重启后可见，新开应用立即可见
- **内存紧张（≤4GB）**：模型加载后服务进程常驻约 1 GB（模型 ~500 MB + 推理缓存），低内存机器建议保持关闭（模型路径留空 + 不启用即不占内存）
- **延迟偏高**：看日志（见下）total 是否 ~40ms 量级；~80ms 说明预解码未命中或 `cpu_cores` 超物理核数

## 日志（排查）

`%APPDATA%\Rime\rime_llm_filter_log.txt`：每次推理一行延迟分解（wait/S1/KV/S2/total/prep/ctx_tok/cand）与事件行（编码/候选/上文/排序结果/来源）；会话启动的 `config:` 行显示当前生效参数与配置来源。反馈问题时附上此文件。

设置界面（`WeaselLLMSetup.exe`）自身**不留日志框**：正常只更新状态行，出错才写 `WeaselLLMSetup_error.log`——优先写程序所在目录（安装目录），该目录不可写时回退 `%APPDATA%\Rime\`；界面底部也标了文件名。

## 研究主要结论

重排核心 = 对同码候选做交叉熵评分 `argmax P(wᵢ|上文)`（只排序、不生成、编码无关）。三项关键技术：**分层并行解码**（候选共享上文、≤3 次 decode，与候选数无关）、**预解码 + KV 代次**（commit 后异步算上文，按键延迟 ~43ms）、**CE 位置权重**（前 3 token 按 (1.0, 1.13, 0.61) 加权；尾部外推经端到端扫描证伪删除）。主要数字：

- 首选率 **93.4%**（10 token 上文 / 5 候选，20000 样本），单字 96.8%；模型上限 ~94.3%；
- 0.8B 即最优（2B 仅 +0.3pp 但延迟 3×、体积 2.6×）；10 tok / 5 cand 为性价比最优点；
- 用户词频对数融合（`freq_beta=1.5`，fused = LLM分 + β·log(1+eff)）：词频无上限、强个人高频词可翻盘（本机真实窗回放事前口径 +0.4pp）；

完整研究文档（方法细节、扫参全表、词频融合研究）在本地研究资料库 `llm-rerank-research\`（与本仓库同级目录）。

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

### 真机自动化回归（test_ctx_auto.py）

`scripts\test_ctx_auto.py` 用 SendInput 合成真实键击走完整输入法链路（编码→候选→LLM 重排→上屏，不绕过任何环节），从 `rime_llm_events.txt` 逐词断言上文来源徽章（uia/com/rime）与内容——把退格 / 撤销 / 移光标 / 粘贴等上文信号场景变成一键回归（2026-09-13 真机 12/12 通过，验证插件版信号层修复）：

```bat
python scripts\test_ctx_auto.py               :: 记事本 5 场景（s1 基线/s2 退格/s3 撤销/s4 移光标/s5 粘贴）
python scripts\test_ctx_auto.py -s s2         :: 单场景
python scripts\test_ctx_auto.py --app wps     :: WPS：手动开好空文字文档置前再跑（断言 src=com）
```

运行期间手离开键鼠约 20 秒；自动清旧记事本起新窗口，内置输入法自愈（评分探针 + Win+Space 循环）。三个写进脚本注释的关键事实：满 4 码不自动顶屏（上屏由第 5 键或空格触发，编辑前须先空格顶屏否则删的是编码）；INPUT 结构 union 必须含 MOUSEINPUT（x64 上 40 字节，32B 被 SendInput 静默拒绝）；绝不能 taskkill WeaselServer（丢输入法绑定，恢复只能逐窗 Win+Space）。`--engine source` 切换源码版原生组件日志（`rime_llm_filter_log.txt`，行格式与插件版一致）；`--app wps` 验证 COM 通道（自动找 Office 窗口切前台）。两版回归基线：插件版记事本/WPS 各 12/12、源码版各 11/11（冷启动首词无评分行属行为差异）。

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
- [rime-llm-rerank](https://github.com/zhanghaozhecn/rime-llm-rerank) — 插件版（lua + DLL，跑在官方小狼毫上），功能等效；上文 COM + UIA + 历史（轮询快照），本版为 TSF 采集 + COM（事件驱动）
