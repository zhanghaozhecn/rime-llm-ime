# weasel/librime 说明

本目录包含 weasel 编译所需的 librime 接口头文件补丁（`rime_api.h`，含 LLM 新 API：`set_context_text` / `get_context_text` / `reset_context_text` 等）。

**完整 librime 源码见仓库顶层 `librime/`**（含 `llm_filter` 组件补丁）。构建 weasel 前：

1. 将顶层 `librime/`（或其构建后的 `include/rime_api.h`）复制/覆盖到此目录，使 weasel 的 `$(SolutionDir)\librime\include` 指向含补丁的头文件
2. 将 librime 构建出的 `rime.lib` 放入 `weasel\librime\build\lib\Release\`（vcxproj 的 AdditionalLibraryDirectories 默认查找此路径）

本目录仅维护两个补丁头文件，其余 librime 内容以顶层 `librime/` 为准。
