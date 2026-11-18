# SumatraPDF 划词翻译定制版

这是在 SumatraPDF 源码中集成翻译浮窗的 Windows 定制版。鼠标选中文字后，译文在选区旁显示。新增浮窗只提供翻译，不包含“询问 AI”。

## 使用

1. 运行交付目录中的定制阅读器，打开有可选文字的 PDF。
2. 用鼠标选中文字，松开鼠标后查看浮窗。
3. 首次使用时点击浮窗的“设置”，填写完整 API 端点、模型和密钥，然后保存。
4. 重新选中文字，或点击“重试”，开始翻译。默认自动识别原语言，翻译为中文（简体），思考关闭，流式输出开启。

端点必须是完整地址。支持 Chat Completions（通常以 `/v1/chat/completions` 结尾）及 Responses（以 `/responses` 结尾）。例如图中的 DeepSeek 配置为 `https://api.deepseek.com/v1/responses`，模型为 `deepseek-flash`。模型名必须是服务商对该端点开放的模型。

空密钥可用于不要求鉴权的本地服务，例如已配置相应接口的本地模型服务器。程序默认不配置外部端点，也不会自动选择云服务。

浮窗提供复制、重试、固定、设置和关闭。

浮窗采用浅灰外框和白色圆角译文卡片，文字使用深色。该阅读卡片保持浅色，不改变 PDF 阅读器或设置窗口使用的主题。

标题行提供 A− / A+、B 和“自适应”。字号只影响译文，界面文字保持原字号。默认使用 Microsoft YaHei UI、11 pt；设置中可以选择字体和 8–32 pt 字号。B 控制整段译文是否加粗，默认关闭，也可以在设置中勾选“译文加粗”。字号、字体和加粗选择会记住。译文区域支持 Ctrl+滚轮调整字号，Ctrl+0 恢复 11 pt。修改这些显示选项不会重新调用翻译服务或改写译文内容。

默认启用自适应，按实际换行调整高度。流式输出期间窗口分段长高，完成后适配最终译文。默认最大高度为阅读区可见高度的 70%，超出的译文可以滚动查看。

拖动边框调整大小后，窗口切换为手动尺寸并记住宽高。点击“自适应”可恢复自动高度。宽高以 100% 系统缩放为基准保存。阅读区缩小时，窗口临时缩小，阅读区扩大后可以按保存的尺寸显示。

- 设置中的“浮窗位置”可选自动、上方、下方、左侧或右侧，默认自动。译文框在文档阅读区内摆放，避开标签栏、阅读器工具栏和侧栏。指定方向空间不足时会换边。修改位置只影响显示。
- 原选区快捷条和译文框会错开，必要时译文框缩短高度。空间不足以同时摆放时，快捷条临时收起。关闭译文框后，只要该选区的快捷条仍可用，就会重新显示。
- 小箭头指向当前可见选区，不抢焦点，也不阻挡鼠标操作。滚动页面和移动阅读器窗口时，浮窗与箭头跟随选区。
- 选区滚出视野时，浮窗保留译文和最后位置，箭头暂时隐藏，进行中的翻译继续。选区再次出现后恢复跟随，不发起新的翻译请求。
- 未固定时，真正清除选区或点击空白处会收起浮窗并取消当前请求。
- 固定后，清除选区会保留浮窗并隐藏箭头。再次划词仍会更新译文。
- 按 Esc 或点击关闭会强制收起浮窗，固定时也适用。
- 保存请求设置时，只有浮窗仍打开且当前文字选区与原文一致，才会自动重新翻译。
- 切换文档标签或替换当前文档会收起浮窗并取消请求，固定时也适用。

扫描 PDF 需要先有可用的文字层，才能用文字选区翻译。

## 配置

配置存放在阅读器使用的 `SumatraPDF-settings.txt` 中，位置遵循 SumatraPDF 的普通设置规则。使用菜单中的高级设置入口可找到当前设置文件。便携模式与安装模式可能使用不同位置。

本功能使用以下字段：

| 字段                         | 含义                                                                   |
| ---------------------------- | ---------------------------------------------------------------------- |
| `TranslatePopupEnabled`      | 是否在鼠标划词后自动翻译，默认开启                                     |
| `TranslatePopupUrl`          | 完整 Chat Completions 或 Responses 端点，默认留空                      |
| `TranslatePopupModel`        | 模型名                                                                 |
| `TranslatePopupApiKey`       | API 密钥；本地无鉴权服务可留空                                         |
| `TranslatePopupTemperature`  | 温度，0–2，默认 1                                                      |
| `TranslatePopupPrompt`       | 提示词模板；空值使用内置翻译提示词                                     |
| `TranslatePopupStream`       | 流式输出，默认开启                                                     |
| `TranslatePopupThinking`     | 思考模式，默认 `off`；也可选 `default`、`low`、`medium`、`high`、`max` |
| `TranslatePopupLangFrom`     | 原语言，默认 `auto`（Auto）                                            |
| `TranslatePopupLangTo`       | 目标语言，默认 `zh-CN`（中文（简体））                                 |
| `TranslatePopupCustomParams` | 自定义请求 JSON 对象，默认 `{}`                                        |

显示设置：

| 字段                      | 含义                                                             |
| ------------------------- | ---------------------------------------------------------------- |
| `TranslatePopupFont`      | 译文字体，默认 `Microsoft YaHei UI`                              |
| `TranslatePopupFontSize`  | 译文字号，默认 11 pt，范围 8–32                                  |
| `TranslatePopupBold`      | 译文加粗，默认关闭；与标题行 B 开关同步                          |
| `TranslatePopupPosition`  | 浮窗位置偏好，默认 `auto`；可选 `top`、`bottom`、`left`、`right` |
| `TranslatePopupAutoSize`  | 自动高度，默认开启；拖动边框后关闭                               |
| `TranslatePopupWidth`     | 阅读或手动宽度，默认 460，范围 330–1600（100% 缩放基准）         |
| `TranslatePopupHeight`    | 手动高度，默认 350，范围 180–1200（100% 缩放基准）               |
| `TranslatePopupMaxHeight` | 自动高度上限占阅读区可见高度的比例，默认 70%，范围 30–90%        |

提示词支持 `${langFrom}`、`${langTo}`、`${sourceText}`。必须包含 `${sourceText}`，才能将选区送入请求。变量只展开一次，选区里恰好出现的变量字样会保留。多行提示词中按 Enter 换行。

“自定义请求参数”窗口中，每行填写参数名和 JSON 值。例如 `reasoning` 与 `{"effort":"none"}`。字符串值需要 JSON 双引号；数字、`false`、`null`、对象和数组都按原类型发送。自定义参数覆盖同名的标准请求字段。子窗口保存后，仍需在主设置窗口点击“保存”才会生效；取消主窗口会丢弃草稿。

程序根据 DeepSeek、OpenAI 和 Azure 的端点设置思考参数。DeepSeek Chat Completions 的 Off 使用 `thinking.type = disabled`，Responses 的 Off 使用 `reasoning.effort = none`。服务商默认模式不主动发送思考参数。未识别的中转地址使用自定义参数设置。具体模型支持的温度和思考等级以服务商为准；不接受思考参数的模型可选 Default，并按需配置自定义参数。[DeepSeek 思考模式文档](https://api-docs.deepseek.com/guides/thinking_mode/)、[Responses API 文档](https://api-docs.deepseek.com/api/create-response/)和 [OpenAI Responses 文档](https://developers.openai.com/api/reference/resources/responses/methods/create)是请求格式依据。

密钥在本机设置界面填写，以明文保存在本机设置文件中。交付说明、示例配置和测试记录不包含真实密钥。

便携版先解压，再运行其中的 `SumatraPDF.exe`。同目录提供空白翻译配置；首次划词时通过“设置”填写自己的接口。关闭自动翻译后，可在高级设置中重新启用 `TranslatePopupEnabled`。

## 从源码构建

上游基线为 `sumatrapdfreader/sumatrapdf` 的 `b1a8e22577c9e35064315d279fc33ba6b67c5bef`。保留上游许可证；新增个人构建脚本采用 `GPL-3.0-only`。

正常编译需要 Windows、Visual Studio C++ 构建工具和完整的 Windows 10/11 SDK。源码中的生成项目和打包工具已经随源码提供。直接编译本静态目标不要求 Bun、Node.js、minio 或 clang-format。

在源码根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-translation.ps1 -Configuration Release
```

输出为 `out/rel64/SumatraPDF-static.exe`。脚本只构建，不启动阅读器，不安装，也不注册文件关联。日志与构建校验记录存放在 `out/build-translation/`。

构建 Debug，或先检查工具链：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-translation.ps1 -Configuration Debug
powershell -ExecutionPolicy Bypass -File tools/build-translation.ps1 -ValidateOnly
```

必要时指定工具集或 SDK：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-translation.ps1 -Configuration Release -PlatformToolset v145 -WindowsSdkVersion 10.0.26100.0
```

脚本优先用 `vswhere` 查找安装的 C++ 工具，也支持 `-VisualStudioPath` 指定 VS 根目录。它选择已安装的 x64 工具集与完整 SDK，并将选择作为 MSBuild 全局参数传入，不改写生成项目。

修改设置定义、命令定义或构建清单后，才需要按上游说明使用 Bun 重生成相应文件。修改 C++ 后需要 clang-format。当前工作区为这些开发操作准备了忽略目录 `agent-tools/`，其中的 Bun、formatter、npm 依赖和 `NODE_PATH` 设置只是本机开发准备，不是运行阅读器或直接编译静态目标的必要条件。

## 定向验证

测试使用本地模拟 API 和简单 PDF，不需要云密钥。开发环境有 Bun 时，先构建 Debug，再运行本功能的定向测试：

```powershell
$env:SUMATRA_TEST_EXE = (Resolve-Path out/dbg64/SumatraPDF-static.exe).Path
bun tests/ad-hoc-llm-translate.ts --no-build
bun tests/ad-hoc-translate-settings.ts --no-build
bun tests/ad-hoc-translate-options.ts --no-build
bun tests/ad-hoc-translate-appearance.ts --no-build
bun tests/ad-hoc-translate-style.ts --no-build
bun tests/ad-hoc-translate-anchor.ts --no-build
```

常规测试使用 `-for-testing`。配置持久化场景只在独立临时 appdata 中启用保存，随后重启并验证加载。

## 已有证据与验证边界

- 本机工具为 MSVC 14.50.35717、v145、SDK 10.0.26100.0。
- 修改前的上游基线 Debug、定制版 Debug 和发布版 x64 静态目标均已成功编译。
- 当前源码通过 24 个本地模拟接口场景、4 个原生设置场景、12 个请求选项场景、14 个字体／尺寸场景、5 个白卡片／加粗场景和 9 个定位／共存场景，共 68 项。此前便携包的验证回执记录了打包时的 65 项结果和程序校验值。
- 严格流式测试在服务器保持连接、尚未发送后续内容时，确认浮窗已经显示首个中文字。Windows 读取层已修复小响应等到结束才显示的问题。
- 63 项新增请求与协议检查和 14 项 Unicode 边界检查通过。Responses 的流式和非流式返回、完成快照去重、思考内容排除及失败／不完整结束都经过检查。提示词展开和多片段总译文均有长度上限。
- 原有选区工具栏的即时显示、SVG 布局和窗口跟随检查通过。移动旧测试在本机未声明 DPI 感知时，两套程序均出现相同的两倍坐标差，使用 DPI 感知测试进程后通过。
- 浮窗、主设置和自定义参数窗口均保存了实际截图。小窗口测试检查固定页脚的可见性和按钮绘制。深色主题的新建参数行经过独立图像检查。
- 外观测试检查实际字体行高、A− / A+、Ctrl+滚轮、Ctrl+0、字体输入框的 Tab / Enter / Esc、连续流式输出时的尺寸变化、最终高度、流式期间手动锁定尺寸及大尺寸被屏幕临时夹紧后保留偏好。只改外观不会发起新的 API 请求。
- 剪贴板内容读回单独记录为 `SKIP`，原因是当前宿主没有交互桌面；正常桌面仍保留严格读回断言。真实鼠标指针、跨显示器拖动和真实 250% DPI 切换未单独验证。小窗口布局使用当前 DPI 缩小客户区验证。
- 本机工具下载来源与校验保存在 `agent-tools/tools-receipt.json`；基线产物校验在 `agent-tools/baseline-binaries.json`。
- 每次使用本构建脚本成功构建后，会在 `out/build-translation/` 写入实际产物的 SHA-256、工具链和日志路径。
- 当前没有真实云模型凭据，本机 Ollama/LM Studio 也未运行。真实模型的翻译质量、延迟和服务商兼容性尚未验证。模拟 API 的协议测试不能代替真实模型验证。

交付目录中的 `MANIFEST.json` 汇总构建与测试回执，`SHA256SUMS.txt` 校验程序、源码和补丁。完整源码包可直接按上述命令构建；`source.patch` 用于对照固定上游基线审查改动。
