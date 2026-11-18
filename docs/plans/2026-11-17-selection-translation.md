# 划词翻译浮窗实现计划

> For implementation agent: use subagent-driven-development. Do not commit.

**目标：** 划词后由 SumatraPDF 在选区旁自动显示简体中文译文，不加入“询问 AI”。

**架构：** Windows 原版新增无模态浮窗；网络工作在线程内执行，通过 UI 队列报告流式文本；任务独立取消，旧结果不得覆盖新选区。复用 HttpPostStream、JSON 解析器、选区和窗口布局代码。

**技术：** C++ / Win32、现有 base/gui、兼容 chat completions 的 API、Bun 定向测试。

**基线：** sumatrapdfreader/sumatrapdf @ b1a8e22577c9e35064315d279fc33ba6b67c5bef。保留上游许可。新增个人交付脚本为 GPL-3.0-only。开发目录为 sumatra-native；sumatrapdf-translate 保留上游原始检出。

## 验收

- 鼠标选区完成自动显示原生浮窗，不依赖剪贴板或浏览器。
- 逐步显示中文；支持复制、重试、固定、设置、关闭；无“询问 AI”。
- 选区变化、清除、标签切换、滚动、窗口移动和销毁时正确更新、隐藏或取消。
- 配置 API 地址、模型、密钥、开关；未配置时显示设置入口。密钥不进入诊断和测试产物。
- 构建 Windows 可执行文件；本地模拟 API 验证协议、中文分片、错误、取消和过时结果。
- 保存版本、构建与测试记录、截图、SHA-256；如未验证真实模型，明确记录。

## Task 1：翻译协议层

新增 `src/LlmTranslate.h`、`src/LlmTranslate.cpp`。

```cpp
struct LlmTranslateRequest {
    Str url;
    Str model;
    Str apiKey;
    Str text;
};
struct LlmTranslateProgress {
    virtual ~LlmTranslateProgress() = default;
    virtual void OnText(Str cumulativeText) = 0;
};
bool RunLlmTranslate(const LlmTranslateRequest&, HttpStreamCancel*, LlmTranslateProgress*, Str& error);
```

1. 验证 URL、模型、输入；使用项目工具转义 JSON 和鉴权头。
2. 复用 HttpPostStream；系统提示固定翻译为简体中文，保留公式变量和引用。
3. 接受 SSE `choices[0].delta.content`，以及非流式 JSON `choices[0].message.content`。
4. 处理 UTF-8/SSE 网络分片、空响应、取消、HTTP/API 错误和响应上限。
5. OnText 在线程内调用；字符串仅在回调内有效。error 为调用者释放的 owned Str。
6. 不使用 STL；修改 C++ 后格式化。先规格审查，再代码审查。

## Task 2：原生浮窗和设置

新增 `src/TranslatePopup.h`、`src/TranslatePopup.cpp`；必要时更新 `cmd/gen-settings.ts`、`cmd/ng-gen-settings.ts` 及生成文件。

1. 建立 owner 属于阅读器、不抢焦点的无模态浮窗，遵守主题和 DPI。
2. 对外提供选区完成、刷新位置、隐藏、销毁和测试诊断接口。
3. 中文状态、译文、复制、重试、固定、设置、关闭。
4. 原生设置界面编辑 URL、模型、密钥和自动翻译开关。
5. 工作线程使用字符串快照，不能访问窗口、标签和可变全局设置。
6. UI 回调验证递增任务标识；取消和关闭不能产生悬空引用。
7. 未配置显示设置说明，不自动选择任意云模型。先规格审查，再代码审查。

## Task 3：选区与生命周期接入

修改 `src/shared/Selection.cpp`、`src/SelectionToolbar.cpp`、窗口生命周期文件、`premake5.files.lua` 和 VS 项目。

1. 完成选区触发一次翻译，拖动时隐藏，不反复请求。
2. 清除、切换标签、滚动、移动窗口、Esc、销毁时同步浮窗。
3. 与现有选区工具栏协调位置，保留现有阅读行为。
4. 更新构建列表；编译前格式化 C++。先规格审查，再代码审查。

## Task 4：定向验证与交付

新增 `tests/ad-hoc-llm-translate.ts`、`docs/selection-translation.md`；必要时扩展 dbg-control。

1. 本地临时 HTTP 服务、生成的小 PDF，无需云密钥。
2. 复用 `-dbg-control` 和 Bun GUI helpers 验证原生面板。
3. 验证转义、中文流、取消、过时结果、错误、关闭、复制和定位。
4. 只运行相关测试，不运行发行全套。
5. 构建发布版、打开 PDF、保存实际浮窗截图。
6. 输出源码补丁、运行目录、配置说明、验证记录和校验值。

## 构建准备

已有 MSVC 14.50、SDK 10.0.26100。Bun、clang-format 下载到忽略的 agent-tools，不安装系统组件。设置进程 PATH 以便原有构建入口识别 BuildTools。

```powershell
agent-tools/bun.exe cmd/build.ts -dbg
agent-tools/bun.exe tests/ad-hoc-llm-translate.ts --no-build
agent-tools/bun.exe cmd/build.ts -rel
```

测试启动带 `-for-testing`。不提交、不推送、不替换用户现有阅读器。
