# 四向定位、选区跟随与箭头

> 延续已有白色卡片和 B 加粗任务；使用 subagent-driven-development 实现并完成规格、质量复查。不要提交 Git。

目标：浮窗可自动或按上／下／左／右偏好贴近选区，小箭头指向选区；选区滚出视野时保留浮窗，再出现时继续跟随，不重新翻译。

## 行为约定

- 新偏好 `TranslatePopupPosition = auto`，允许 `auto/top/bottom/left/right`；原版/ng 定义与序列化保持一致。设置控件 3127：“浮窗位置”，选项为自动、上方、下方、左侧、右侧。位置属于显示偏好，保存和取消均不触发翻译请求。
- 自动选择能放下窗口且尽量不覆盖选区的一边。指定方向空间不足时允许换边。整个浮窗与箭头保持屏幕工作区内。
- “选区可见”须同时满足实际选区存在、GetSelectionScreenRect 成功、选区与画布屏幕矩形有非空交集。现有 helper 在页面仍部分可见时可能返回画布外的文字坐标，不能只看返回布尔值。
- 选区滚出画布时保留当前译文、请求和最后位置，暂时隐藏箭头；重新滚入时更新位置和箭头。普通滚动不能取消流式请求或重复请求。
- 真正清除选区、切换／关闭文档、Esc、关闭按钮仍按原有固定／关闭规则处理。固定后选区真正清除时也不要留下指向旧位置的箭头。
- 箭头为无焦点、点击穿透的独立拥有窗口；保留主浮窗的原生缩放边框、圆角和阴影。隐藏／销毁与浮窗同步，不能拦截 PDF 或按钮输入。
- 原有白卡片、B、字号、手动／自适应、DPI、字体输入框键盘规则全部保留。小窗口 32 pt 加粗时需自适应收紧内边距，至少显示完整一行，不覆盖手动宽高。

## 模块约定

新建 `src/TranslateAnchor.h/.cpp`，公开以下接口；图形与几何模块不访问 gSettings、文档或网络：

```cpp
enum class TranslatePosition { Auto, Top, Bottom, Left, Right };
struct TranslateCallout {
    Rect popup;
    Point target;
    TranslatePosition side = TranslatePosition::Right;
};
TranslatePosition ParseTranslatePosition(Str value);
Str TranslatePositionName(TranslatePosition value);
TranslateCallout PlaceTranslateCallout(Rect anchor, Size popupSize, Rect work,
                                     TranslatePosition preferred, int dpi);
struct TranslateArrow;
TranslateArrow* CreateTranslateArrow(HWND owner);
void UpdateTranslateArrow(TranslateArrow*, const TranslateCallout&, int dpi, Color fill, Color border);
void HideTranslateArrow(TranslateArrow*);
void DeleteTranslateArrow(TranslateArrow*);
Rect TranslateArrowRect(TranslateArrow*);
bool TranslateArrowVisible(TranslateArrow*);
Point TranslateArrowTip(TranslateArrow*);
```

side 表示浮窗在选区的哪一侧，target 为选区边缘的指向点。箭头的实际基底应贴住可见窗口边缘，不能贴在 DWM 隐形缩放边框外留下缝隙。可以读取可见框范围，失败时退回窗口范围。分层箭头透明区域和所有像素均应点击穿透。

## 任务 1：几何与箭头绘制

文件：`src/TranslateAnchor.h/.cpp`。

1. 实现偏好解析、四边候选、工作区夹紧与避让选区。优先稳定，不因小幅滚动频繁换边。
2. 实现浅灰三角箭头窗口与拥有关系，使用传入颜色，处理 DPI、方向、透明、点击穿透及释放。
3. 对纯几何边界做有意义的独立检查；不要假装实际 GUI／跨屏验证。

## 任务 2：浮窗集成与偏好

文件：`src/TranslatePopup.cpp`、两个设置生成器与生成输出。

1. 接入模块，保存实际可见选区、方向偏好、箭头对象和最后位置。
2. 选择画布可见交集作为锚点。视野外只夹紧最后窗口位置，不使用过期锚点重新定位。
3. 箭头显隐与浮窗／选区生命周期同步，恢复跟随时不增加 request 或重试。
4. 新增 3127 下拉框和持久化。位置变更仅走显示应用，不进入请求参数比较。
5. 增加无敏感诊断：anchorVisible、anchor、position、side、arrowVisible、arrowRect、arrowTip。使用独立 fmt 追加块，每次少于 32 条格式指令。

## 任务 3：验收测试

文件：新建 `tests/ad-hoc-translate-anchor.ts`，更新共享测试控件和必要的旧跟随断言。

1. 旧版本新行为测试预期失败，先保存证据。
2. 实际原生窗口验证四方向偏好、自动换边与工作区边界、箭头方向／位置／点击穿透。
3. 用真实画布滚动令单词离开仍部分可见的页面，确认浮窗留存、箭头隐藏、译文／请求不变；滚回恢复跟随和箭头。
4. 在流式期间重复离开／返回，确保请求不被取消、首字／后续译文仍更新；不依靠“固定”才能成立。
5. 验证清除选区、固定后清除、关闭、切换文档和退出时无孤立箭头。
6. 设置方向保存／取消、独立 appdata 重启持久化，无 API 重试。保留白卡片、B 与既有 56 场景。

## 任务 4：合并验证与交付

文件：`premake5.files.lua`、动态／静态项目及 filters、`tools/package-translation.py`、文档和忽略目录中的回执脚本。

添加模块与源码白名单，格式化、生成项目、构建 Debug/Release。完成所有组件的规格和质量复查，集中运行相关测试并保留失败证据。更新原有“滚出视野隐藏”的说明；解压实际便携包检查白卡片、B、方向和箭头默认行为。保留旧交付批次，不使用真实模型凭据，不运行整套测试。
