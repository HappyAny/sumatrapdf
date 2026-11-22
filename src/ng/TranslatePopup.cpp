/* Copyright 2026. License: GPL-3.0-only */

#include "gui/GpuiBridge.h"
#include "VirtKeys.h"

#include "base/Http.h"
#include "base/UITask.h"
#include "gui/Dpi.h"
#include "gui/PlatformFont.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "SumatraConfig.h"
#include "MainWindow.h"
#include "AIChatTypes.h"
#include "WindowTab.h"
#include "Selection.h"
#include "CommandAvailability.h"
#include "SelectionToolbarCommon.h"
#include "Theme.h"
#include "LlmTranslate.h"
#include "LlmRequestOptions.h"
#include "TranslateAnchor.h"
#include "gui/AppShell.h"
#include "gui/DocCanvas.h"
#include "gui/DialogWidgets.h"
#include "gui/ToolWindow.h"
#include "TranslatePopup.h"

constexpr int kMinWidth = 330;
constexpr int kMaxWidth = 1600;
constexpr int kMinHeight = 180;
constexpr int kMaxHeight = 1200;
constexpr int kMinFont = 8;
constexpr int kMaxFont = 32;
constexpr int kDefaultFont = 11;
constexpr int kFitInterval = 180;
constexpr int kFitMargin = 8;
constexpr int kMargin = 12;
constexpr int kGap = 6;
constexpr int kHeader = 26;
constexpr int kStatus = 22;
constexpr int kFooter = 28;
constexpr int kPadX = 8;
constexpr int kPadY = 4;
constexpr int kChrome = kMargin * 2 + kGap * 2 + kHeader + kStatus + kFooter + 2 * (kPadY + 1);
constexpr int kScrollbarWidth = 4;
constexpr Color kShell = MkRgb(242, 242, 242);
constexpr Color kCard = MkRgb(255, 255, 255);
constexpr Color kBorder = MkRgb(213, 213, 213);
constexpr Color kText = MkRgb(32, 32, 32);
constexpr Color kSecondary = MkRgb(96, 96, 96);

enum class PopupAction {
    Copy,
    Retry,
    Pin,
    Settings,
    Close,
    FontMinus,
    FontPlus,
    AutoSize,
    Bold,
};

enum class ConfigField {
    Url,
    Model,
    Key,
    Temperature,
    Prompt,
    Font,
    FontSize,
    LineSpacing,
    Width,
    Height,
    MaxHeight,
    Count,
};

enum class ConfigAction {
    Enable,
    Stream,
    Bold,
    AutoSize,
    Params,
    Cancel,
    Save,
    AddParam,
    RemoveParam,
    SaveParams,
    CancelParams,
};

constexpr Str kThinkingValues[] = {StrL("default"), StrL("off"),  StrL("low"),
                                   StrL("medium"),  StrL("high"), StrL("max")};
constexpr Str kThinkingLabels[] = {StrL("Default（服务默认）"), StrL("Off（关闭）"), StrL("Low（低）"),
                                   StrL("Medium（中）"),        StrL("High（高）"),  StrL("Max（最高）")};
constexpr TranslatePosition kPositions[] = {TranslatePosition::Auto, TranslatePosition::Top, TranslatePosition::Bottom,
                                            TranslatePosition::Left, TranslatePosition::Right};
constexpr Str kPositionLabels[] = {StrL("自动"), StrL("上方"), StrL("下方"), StrL("左侧"), StrL("右侧")};

struct PopupView {
    MainWindow* win = nullptr;
    static void OnAction(PopupView*, gp::Ctx*, const gp::ClickEvent*, int64_t action);
    static void OnConfig(PopupView*, gp::Ctx*, const gp::ClickEvent*, int64_t action);
    static void OnScroll(PopupView*, gp::Ctx*, const gp::ScrollEvent*);
    static void OnConfigScroll(PopupView*, gp::Ctx*, const gp::ScrollEvent*);
    static void OnParamsScroll(PopupView*, gp::Ctx*, const gp::ScrollEvent*);
    static void OnWheel(PopupView*, gp::Ctx*, const gp::ScrollWheelEvent*);
};

struct ConfigParam {
    gp::InputState* key;
    gp::InputState* value;
};

struct PopupDraft {
    gp::InputState* fields[(int)ConfigField::Count]{};
    DialogSelect from;
    DialogSelect to;
    DialogSelect thinking;
    DialogSelect position;
    DialogSelect fonts;
    gp::App* app = nullptr;
    bool enabled = false;
    bool stream = false;
    bool bold = false;
    bool autoSize = false;
    bool focus = true;
    float scroll = 0;
    float paramsScroll = 0;
    Vec<ConfigParam> params;
    Str customParams;
    Str note;
    Str paramsNote;

    gp::InputState* Input(ConfigField field) const { return fields[(int)field]; }
    Str Text(ConfigField field) const { return FromGpui(gp::InputValue(Input(field))); }
};

struct NativePopupJob;

struct NativeTranslatePopup {
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    DocController* document = nullptr;
    ToolWindow* popup = nullptr;
    ToolWindow* config = nullptr;
    ToolWindow* params = nullptr;
    gp::Entity<PopupView> view;
    PopupDraft* draft = nullptr;
    NativePopupJob* job = nullptr;
    u64 instance = 0;
    Str source;
    Str result;
    Str status;
    Rect anchor;
    TranslateCallout callout;
    Rect lastPlacement;
    Rect measuredArea;
    Size fittedSize;
    bool open = false;
    bool pinned = false;
    bool anchorVisible = false;
    bool showingSource = false;
    bool sizing = false;
    bool fitPending = false;
    int fitWait = 0;
    float scroll = 0;
    float lineHeight = 0;
};

static Vec<NativeTranslatePopup*> gPopups;
static u64 gNextInstance = 0;

static NativeTranslatePopup* FindPopup(MainWindow* win) {
    for (NativeTranslatePopup* p : gPopups) {
        if (p->win == win) {
            return p;
        }
    }
    return nullptr;
}

static NativeTranslatePopup* FindPopupById(u64 instance) {
    for (NativeTranslatePopup* p : gPopups) {
        if (p->instance == instance) {
            return p;
        }
    }
    return nullptr;
}

static bool CanTranslate() {
#if OS_WIN || OS_DARWIN || (defined(GPUI_HAVE_CURL) && GPUI_HAVE_CURL)
    return ToolWindowsAvailable() &&
           HasPermission(Perm::CopySelection | Perm::InternetAccess | Perm::AIChat | Perm::DiskAccess);
#else
    return false;
#endif
}

static bool CanCopy(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    return dm && (gDisableDocumentRestrictions || dm->GetEngine()->allowsCopyingText);
}

static bool IsCurrentPopup(NativeTranslatePopup* p) {
    return p->win->CurrentTab() == p->tab && p->win->ctrl == p->document;
}

static bool HasSelection(MainWindow* win) {
    return win->showSelection && win->AsFixed() && !IsRectangularSelection(win);
}

static Rect ReadingArea(MainWindow* win) {
    Rect canvas = AppShellCanvasScreenRect(win);
    Rect work = AppShellWorkArea(win);
    return work.IsEmpty() ? canvas : canvas.Intersect(work);
}

static Rect SelectionAnchor(MainWindow* win) {
    Rect layout;
    if (!HasSelection(win) || !GetSelectionScreenRect(win->CurrentTab(), layout)) {
        return {};
    }
    Rect canvas = AppShellCanvasScreenRect(win);
    if (canvas.IsEmpty() || win->canvasRc.IsEmpty()) {
        return {};
    }
    // DisplayModel uses document pixels and the tool window uses screen units.
    float scale = CanvasScale(win) * (float)canvas.dx / (float)win->canvasRc.dx;
    layout.x = canvas.x + (int)((float)layout.x * scale);
    layout.y = canvas.y + (int)((float)layout.y * scale);
    layout.dx = std::max(1, (int)((float)layout.dx * scale));
    layout.dy = std::max(1, (int)((float)layout.dy * scale));
    return layout.Intersect(ReadingArea(win));
}

static int FontSize() {
    return std::clamp(gSettings->translatePopupFontSize, kMinFont, kMaxFont);
}

static int LineSpacing() {
    int percent = gSettings->translatePopupLineSpacing;
    return std::clamp(percent > 0 ? percent : 100, 1, 200);
}

static Str FontFace() {
    Str text = gSettings->translatePopupFont;
    return str::IsEmptyOrWhiteSpace(text) ? StrL("Microsoft YaHei UI") : text;
}

static Str Body(NativeTranslatePopup* p) {
    return p->showingSource ? p->source : p->result;
}

static int MeasureBody(NativeTranslatePopup* p, int width, int dpi) {
    auto style = gSettings->translatePopupBold ? PlatformFontStyle::Bold : PlatformFontStyle::Regular;
    PlatformFont* font = GetPlatformFont(FontFace(), (float)FontSize(), style);
#if OS_WIN
    // GDI measures against the Windows screen DC.
    int fontDpi = dpi;
#else
    int fontDpi = 96;
#endif
    int lineHeight = std::max(1, PlatformFontLineHeight(font));
    int step = std::max(1, MulDiv(lineHeight, LineSpacing(), 100));
    p->lineHeight = (float)step * 96.f / (float)fontDpi;
    int innerWidth = std::max(1, MulDiv(width, fontDpi, dpi) - MulDiv((kMargin + kPadX + 1) * 2, fontDpi, 96));
    int measured = PlatformFontMeasureText(font, Body(p), innerWidth).dy;
    return std::max(1, MulDiv(measured, LineSpacing() * dpi, 100 * fontDpi));
}

static Size FitSize(NativeTranslatePopup* p, Rect area) {
    int dpi = std::max(1, AppShellWindowDpi(p->win));
    int width = std::min(area.dx, MulDiv(std::clamp(gSettings->translatePopupWidth, kMinWidth, kMaxWidth), dpi, 96));
    int body = MeasureBody(p, width, dpi);
    int manual = std::clamp(gSettings->translatePopupHeight, kMinHeight, kMaxHeight);
    if (!gSettings->translatePopupAutoSize) {
        return {width, std::min(area.dy, MulDiv(manual, dpi, 96))};
    }
    int minimum = std::min(area.dy, MulDiv(kMinHeight, dpi, 96));
    int limit = std::clamp(gSettings->translatePopupMaxHeight, 30, 90);
    int maximum = std::clamp(MulDiv(area.dy, limit, 100), minimum, area.dy);
    return {width, std::clamp(body + MulDiv(kChrome + kFitMargin, dpi, 96), minimum, maximum)};
}

static Rect PlacePopup(MainWindow* win, ToolWindow* tw) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->open || !IsCurrentPopup(p)) {
        return {};
    }
    Rect area = ReadingArea(win);
    if (area.IsEmpty()) {
        return {};
    }
    if (p->sizing) {
        return ToolWindowRect(tw);
    }
    if (area.Size() != p->measuredArea.Size() || p->fittedSize.IsEmpty()) {
        p->fittedSize = FitSize(p, area);
        p->measuredArea = area;
    }
    if (p->anchorVisible) {
        Rect toolbar = TranslateToolbarScreenRect(win);
        int dpi = std::max(1, AppShellWindowDpi(win));
        int minHeight = MulDiv(kMinHeight, dpi, 96);
        if (gSettings->translatePopupAutoSize && !toolbar.IsEmpty()) {
            minHeight = std::min(
                minHeight, MulDiv(kHeader + kStatus + kFooter + 2, dpi, 96) + (int)(p->lineHeight * (float)dpi / 96.f));
        }
        p->callout =
            PlaceTranslateCallout(p->anchor, p->fittedSize, area,
                                  ParseTranslatePosition(gSettings->translatePopupPosition), dpi, toolbar, minHeight);
        p->lastPlacement = p->callout.popup;
    } else {
        Rect placed = p->lastPlacement;
        placed.dx = p->fittedSize.dx;
        placed.dy = p->fittedSize.dy;
        placed.x = std::clamp(placed.x, area.x, area.Right() - placed.dx);
        placed.y = std::clamp(placed.y, area.y, area.Bottom() - placed.dy);
        p->lastPlacement = placed;
    }
    return p->lastPlacement;
}

static void Refit(NativeTranslatePopup* p, bool grow) {
    Rect area = ReadingArea(p->win);
    if (!area.IsEmpty() && !p->sizing) {
        Size fit = FitSize(p, area);
        if (grow && gSettings->translatePopupAutoSize && fit.dx == p->fittedSize.dx) {
            fit.dy = std::min(area.dy, std::max(fit.dy, p->fittedSize.dy));
        }
        p->fittedSize = fit;
        p->measuredArea = area;
    }
    if (p->popup) {
        ToolWindowsFollow(p->win);
        AppShellInvalidate(p->win);
    }
}

static void SetBody(NativeTranslatePopup* p, Str text, bool source = false) {
    if (source || p->showingSource) {
        p->scroll = 0;
    }
    p->showingSource = source;
    str::ReplaceWithCopy(&p->result, source ? Str{} : text);
    if (!source && p->job) {
        p->fitPending = true;
    } else {
        p->fitPending = false;
        Refit(p, false);
    }
    if (p->popup) {
        ToolWindowInvalidate(p->popup);
    }
}

static void UpdateJob(NativePopupJob* job);
static void FinishJob(NativePopupJob* job);

struct NativePopupJob : LlmTranslateProgress {
    AtomicInt refs = 2;
    Mutex lock;
    HttpStreamCancel cancel;
    LlmTranslateRequest request;
    u64 popupId = 0;
    Str latest;
    Str error;
    bool updateQueued = false;
    bool ok = false;

    ~NativePopupJob() override {
        str::Free(request.url);
        str::Free(request.model);
        str::Free(request.apiKey);
        str::Free(request.text);
        str::Free(request.langFrom);
        str::Free(request.langTo);
        str::Free(request.prompt);
        str::Free(request.thinking);
        str::Free(request.customParams);
        str::Free(latest);
        str::Free(error);
    }

    void OnText(Str text) override {
        if (cancel.IsCanceled()) {
            return;
        }
        bool queue;
        {
            AutoUnlockMutex autoLock(&lock);
            str::ReplaceWithCopy(&latest, text);
            queue = !updateQueued;
            updateQueued = true;
            if (queue) {
                AtomicIntInc(&refs);
            }
        }
        if (queue) {
            uitask::Post(MkFunc0(UpdateJob, this), "TranslatePopupUpdate");
        }
    }
};

static void ReleaseJob(NativePopupJob* job) {
    if (AtomicIntDec(&job->refs) == 0) {
        delete job;
    }
}

static void CancelJob(NativeTranslatePopup* p) {
    NativePopupJob* job = p->job;
    p->job = nullptr;
    p->fitPending = false;
    p->fitWait = 0;
    if (job) {
        job->cancel.Cancel();
        ReleaseJob(job);
    }
}

static void UpdateJob(NativePopupJob* job) {
    Str text;
    {
        AutoUnlockMutex lock(&job->lock);
        text = str::Dup(job->latest);
        job->updateQueued = false;
    }
    NativeTranslatePopup* p = FindPopupById(job->popupId);
    if (p && p->job == job && IsCurrentPopup(p) && !job->cancel.IsCanceled() &&
        (!p->showingSource || !str::IsEmptyOrWhiteSpace(text))) {
        if (p->showingSource) {
            str::ReplaceWithCopy(&p->status, fmt("正在翻译为%s…", LlmLanguageLabel(job->request.langTo)));
        }
        SetBody(p, text);
    }
    str::Free(text);
    ReleaseJob(job);
}

static void FinishJob(NativePopupJob* job) {
    NativeTranslatePopup* p = FindPopupById(job->popupId);
    if (p && p->job == job) {
        p->job = nullptr;
        if (IsCurrentPopup(p) && !job->cancel.IsCanceled()) {
            if (job->ok) {
                str::ReplaceWithCopy(&p->status, fmt("%s · 翻译完成", LlmLanguageLabel(job->request.langTo)));
                SetBody(p, job->latest);
            } else {
                str::ReplaceWithCopy(&p->status, StrL("翻译失败 · 可重试或修改设置"));
                SetBody(p, job->error);
            }
        }
        ReleaseJob(job);
    }
    ReleaseJob(job);
}

static void RunJob(NativePopupJob* job) {
    job->ok = RunLlmTranslate(job->request, &job->cancel, job, job->error);
    // The UI completion takes ownership of the worker reference.
    uitask::Post(MkFunc0(FinishJob, job), "TranslatePopupDone");
}

static void StartJob(NativeTranslatePopup* p) {
    if (!p->open || !IsCurrentPopup(p)) {
        return;
    }
    CancelJob(p);
    if (!CanTranslate() || !CanCopy(p->win)) {
        str::ReplaceWithCopy(&p->status, StrL("划词翻译不可用"));
        SetBody(p, StrL("当前阅读器权限不允许使用翻译服务。"));
        return;
    }
    if (str::IsEmptyOrWhiteSpace(gSettings->translatePopupUrl) ||
        str::IsEmptyOrWhiteSpace(gSettings->translatePopupModel)) {
        str::ReplaceWithCopy(&p->status, StrL("请先设置翻译服务"));
        SetBody(p, StrL("点击“设置”，填写 API 地址和模型，然后保存。\n\n支持 Chat Completions 和 Responses "
                        "兼容接口，译文显示在这里。"));
        return;
    }
    if (len(p->source) > kLlmSourceLimit) {
        str::ReplaceWithCopy(&p->status, StrL("翻译失败"));
        SetBody(p, StrL("选区过长，请分段翻译。"));
        return;
    }
    auto* job = new NativePopupJob();
    job->popupId = p->instance;
    job->request.url = str::Dup(gSettings->translatePopupUrl);
    job->request.model = str::Dup(gSettings->translatePopupModel);
    job->request.apiKey = str::Dup(gSettings->translatePopupApiKey);
    job->request.text = str::Dup(p->source);
    job->request.langFrom = str::Dup(gSettings->translatePopupLangFrom);
    job->request.langTo = str::Dup(gSettings->translatePopupLangTo);
    job->request.prompt = str::Dup(gSettings->translatePopupPrompt);
    job->request.thinking = str::Dup(gSettings->translatePopupThinking);
    job->request.customParams = str::Dup(gSettings->translatePopupCustomParams);
    job->request.temperature = gSettings->translatePopupTemperature;
    job->request.stream = gSettings->translatePopupStream;
    p->job = job;
    str::ReplaceWithCopy(&p->status, fmt("原文 · 正在翻译为%s…", LlmLanguageLabel(job->request.langTo)));
    SetBody(p, p->source, true);
    ThreadHandle thread = StartThread(MkFunc0(RunJob, job), StrL("LlmTranslate"));
    if (!thread) {
        ReleaseJob(job);
        CancelJob(p);
        str::ReplaceWithCopy(&p->status, StrL("无法启动翻译"));
        SetBody(p, StrL("工作线程启动失败，请重试。"));
        return;
    }
    SafeCloseThreadHandle(&thread);
}

static void ClosePopup(NativeTranslatePopup* p, bool force) {
    if (!p) {
        return;
    }
    p->anchorVisible = false;
    if (!force && p->pinned) {
        return;
    }
    p->open = false;
    p->sizing = false;
    CancelJob(p);
    if (p->popup) {
        ToolWindowSetVisible(p->popup, false);
    }
    AppShellInvalidate(p->win);
}

static void SetFontSize(NativeTranslatePopup* p, int size) {
    gSettings->translatePopupFontSize = std::clamp(size, kMinFont, kMaxFont);
    ScheduleSaveSettings();
    Refit(p, false);
}

static void SaveManualSize(NativeTranslatePopup* p, Size size) {
    if (size.IsEmpty()) {
        return;
    }
    int dpi = std::max(1, AppShellWindowDpi(p->win));
    gSettings->translatePopupWidth = std::clamp(MulDiv(size.dx, 96, dpi), kMinWidth, kMaxWidth);
    gSettings->translatePopupHeight = std::clamp(MulDiv(size.dy, 96, dpi), kMinHeight, kMaxHeight);
}

static void OpenConfig(NativeTranslatePopup* p);

void PopupView::OnAction(PopupView* view, gp::Ctx*, const gp::ClickEvent*, int64_t action) {
    NativeTranslatePopup* p = FindPopup(view->win);
    if (!p) {
        return;
    }
    switch ((PopupAction)action) {
        case PopupAction::Copy:
            if (!str::IsEmptyOrWhiteSpace(p->result) && HasPermission(Perm::CopySelection)) {
                CanvasSetClipboardText(p->win, p->result);
                str::ReplaceWithCopy(&p->status, StrL("译文已复制"));
            }
            break;
        case PopupAction::Retry:
            StartJob(p);
            break;
        case PopupAction::Pin:
            p->pinned = !p->pinned;
            break;
        case PopupAction::Settings:
            OpenConfig(p);
            break;
        case PopupAction::Close:
            ClosePopup(p, true);
            break;
        case PopupAction::FontMinus:
            SetFontSize(p, FontSize() - 1);
            break;
        case PopupAction::FontPlus:
            SetFontSize(p, FontSize() + 1);
            break;
        case PopupAction::AutoSize:
            if (gSettings->translatePopupAutoSize) {
                SaveManualSize(p, ToolWindowRect(p->popup).Size());
            }
            gSettings->translatePopupAutoSize = !gSettings->translatePopupAutoSize;
            ScheduleSaveSettings();
            Refit(p, false);
            break;
        case PopupAction::Bold:
            gSettings->translatePopupBold = !gSettings->translatePopupBold;
            ScheduleSaveSettings();
            Refit(p, false);
            break;
    }
    ToolWindowInvalidate(p->popup);
}

void PopupView::OnScroll(PopupView* view, gp::Ctx* cx, const gp::ScrollEvent* ev) {
    if (NativeTranslatePopup* p = FindPopup(view->win)) {
        p->scroll = ev->offsetY;
        gp::Notify(cx);
    }
}

void PopupView::OnWheel(PopupView* view, gp::Ctx*, const gp::ScrollWheelEvent* ev) {
    NativeTranslatePopup* p = FindPopup(view->win);
    if (p && ev->modifiers.control && ev->deltaY != 0) {
        SetFontSize(p, FontSize() + (ev->deltaY > 0 ? 1 : -1));
    }
}

static gp::El* PopupButton(NativeTranslatePopup* p, gp::Ctx* cx, Str label, PopupAction action, bool toggled = false) {
    auto* button = gpc::Button::New(cx, GpuiDup(cx->a, fmt("translate-action-%d", (int)action)))
                       ->Label(GpuiDup(cx->a, label))
                       ->WithSize(gp::UiSize::Small)
                       ->Disabled((action == PopupAction::Copy && str::IsEmptyOrWhiteSpace(p->result)) ||
                                  (action == PopupAction::FontMinus && FontSize() <= kMinFont) ||
                                  (action == PopupAction::FontPlus && FontSize() >= kMaxFont))
                       ->OnClick(gp::ListenTo(p->view, &PopupView::OnAction, (int64_t)action));
    return (toggled ? button->Primary() : button->Outline())->IntoEl();
}

static gp::El* PopupBuild(MainWindow* win, gp::Ctx* cx) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p) {
        return nullptr;
    }
    float fontPx = (float)FontSize() * 96.f / 72.f;
    float lineHeight = p->lineHeight > 0 ? p->lineHeight / fontPx : 1.f;
    gp::El* header = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->H(kHeader)->Shrink0()->ItemsCenter()->Gap(5);
    header->Child(gp::TextEl(cx->a, GStrL("划词翻译"))->Font(15)->Bold()->Fg(ToGpui(kText))->Flex1()->MinW(0));
    header->Child(gp::TextEl(cx->a, GpuiDup(cx->a, fmt("%dpt", FontSize())))->Font(14)->Fg(ToGpui(kText))->W(34));
    header->Child(PopupButton(p, cx, StrL("A−"), PopupAction::FontMinus)->W(30)->H(kHeader));
    header->Child(PopupButton(p, cx, StrL("A+"), PopupAction::FontPlus)->W(30)->H(kHeader));
    header->Child(PopupButton(p, cx, StrL("B"), PopupAction::Bold, gSettings->translatePopupBold)->W(30)->H(kHeader));
    header->Child(PopupButton(p, cx, StrL("自适应"), PopupAction::AutoSize, gSettings->translatePopupAutoSize)
                      ->W(72)
                      ->H(kHeader));

    Str body = str::ReplaceTemp(Body(p), StrL("\r"), StrL(""));
    gp::El* words = gp::SelectableText::New(cx, GStrL("translate-copy-text"), GpuiDup(cx->a, body))
                        ->IntoEl()
                        ->Font(fontPx)
                        ->FontFamily(GpuiDup(cx->a, FontFace()))
                        ->Weight(gSettings->translatePopupBold ? gp::FontWeight::Bold : gp::FontWeight::Normal)
                        ->LineHeight(lineHeight)
                        ->Fg(ToGpui(kText))
                        ->Shrink0();
    gp::El* content = gp::Div(cx->a)->FlexCol()->PadX(kPadX)->PadY(kPadY)->W(gp::kFill)->Shrink0()->Child(words);
    gp::El* card = gp::Div(cx->a)
                       ->Id(GStrL("translate-body"))
                       ->FlexCol()
                       ->W(gp::kFill)
                       ->Flex1()
                       ->MinH(0)
                       ->Bg(ToGpui(kCard))
                       ->Border(1, ToGpui(kBorder))
                       ->Radius(8)
                       ->ScrollY(p->scroll)
                       ->ScrollFromPath()
                       ->OnScroll(gp::ListenTo(p->view, &PopupView::OnScroll))
                       ->OnScrollWheel(gp::ListenTo(p->view, &PopupView::OnWheel))
                       ->Child(content);
    gp::ScrollbarStyles slim;
    slim.track = slim.track.Width(kScrollbarWidth);
    slim.thumb = slim.thumb.Width(kScrollbarWidth).Radius(kScrollbarWidth / 2);
    card = gp::Scrollbar::ApplyStyles(cx, card, slim);

    gp::El* footer = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->H(kFooter)->Gap(kGap)->Shrink0();
    footer->Child(PopupButton(p, cx, StrL("复制"), PopupAction::Copy)->Flex1()->MinW(0));
    footer->Child(PopupButton(p, cx, StrL("重试"), PopupAction::Retry)->Flex1()->MinW(0));
    footer->Child(PopupButton(p, cx, p->pinned ? StrL("已固定") : StrL("固定"), PopupAction::Pin)->Flex1()->MinW(0));
    footer->Child(PopupButton(p, cx, StrL("设置"), PopupAction::Settings)->Flex1()->MinW(0));
    footer->Child(PopupButton(p, cx, StrL("关闭"), PopupAction::Close)->Flex1()->MinW(0));

    return gp::Div(cx->a)
        ->SizeFull()
        ->FlexCol()
        ->Pad(kMargin)
        ->Bg(ToGpui(kShell))
        ->Child(header)
        ->Child(gp::TextEl(cx->a, GpuiDup(cx->a, p->status))->H(kStatus)->Shrink0()->Font(14)->Fg(ToGpui(kSecondary)))
        ->Child(gp::Div(cx->a)->H(kGap)->Shrink0())
        ->Child(card)
        ->Child(gp::Div(cx->a)->H(kGap)->Shrink0())
        ->Child(footer);
}

static bool PopupKey(MainWindow* win, gp::Ctx*, const gp::KeyEvent* event) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p) {
        return false;
    }
    if (event->vk == VK_ESCAPE) {
        ClosePopup(p, true);
        return true;
    }
    if (event->ctrl && (event->vk == '0' || event->vk == VK_NUMPAD0)) {
        SetFontSize(p, kDefaultFont);
        return true;
    }
    return false;
}

static void PopupTick(MainWindow* win, gp::Ctx*, int elapsed) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->fitPending) {
        return;
    }
    p->fitWait += elapsed;
    if (p->fitWait >= kFitInterval) {
        p->fitPending = false;
        p->fitWait = 0;
        Refit(p, true);
    }
}

static void PopupMoved(MainWindow* win, Rect outer) {
    NativeTranslatePopup* p = FindPopup(win);
    if (p && ToolWindowIsSizing(p->popup) && !p->lastPlacement.IsEmpty() && p->lastPlacement.Size() != outer.Size()) {
        p->sizing = true;
    }
}

static void PopupSized(MainWindow* win, Rect outer) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->sizing) {
        return;
    }
    SaveManualSize(p, outer.Size());
    gSettings->translatePopupAutoSize = false;
    p->sizing = false;
    p->lastPlacement = outer;
    ScheduleSaveSettings();
    Refit(p, false);
}

static void PopupClosed(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (p) {
        p->popup = nullptr;
        ClosePopup(p, true);
    }
}

static Str PopupTitle() {
    return StrL("划词翻译");
}

void ShowTranslatePopup(MainWindow* win) {
    if (!win || !gSettings->translatePopupEnabled || !CanTranslate() || !CanCopy(win) || !HasSelection(win)) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    bool textOnly = false;
    TempStr text = GetSelectedTextTemp(tab, StrL("\n"), textOnly);
    Rect anchor = SelectionAnchor(win);
    if (!textOnly || str::IsEmptyOrWhiteSpace(text) || anchor.IsEmpty()) {
        return;
    }
    NativeTranslatePopup* p = FindPopup(win);
    if (!p) {
        p = new NativeTranslatePopup();
        p->win = win;
        p->instance = ++gNextInstance;
        p->view = gp::EntityNewState<PopupView>(win->gpuiWin->app);
        auto* view = (PopupView*)gp::EntityGet(win->gpuiWin->app, p->view.id);
        view->win = win;
        VecAppend(gPopups, p);
    }
    bool same = p->open && IsCurrentPopup(p) && str::Eq(p->source, text);
    p->tab = tab;
    p->document = win->ctrl;
    p->anchor = anchor;
    p->anchorVisible = true;
    p->open = true;
    if (!same) {
        str::ReplaceWithCopy(&p->source, text);
        StartJob(p);
    }
    if (!p->popup) {
        auto desc = ToolWindowDockedDesc("translation-popup");
        desc.title = PopupTitle;
        desc.resize = ToolWinResize::Resizable;
        desc.minClient = {kMinWidth, kMinHeight};
        desc.maxClient = {kMaxWidth, kMaxHeight};
        desc.build = PopupBuild;
        desc.onCaptureKey = PopupKey;
        desc.onClosed = PopupClosed;
        desc.onTick = PopupTick;
        desc.onMoved = PopupMoved;
        desc.onExitSizeMove = PopupSized;
        desc.place = PlacePopup;
        p->popup = ToolWindowOpen(desc, win, PlacePopup(win, nullptr));
        if (!p->popup) {
            ClosePopup(p, true);
        }
    } else {
        ToolWindowSetVisible(p->popup, true);
        ToolWindowsFollow(win);
    }
    AppShellInvalidate(win);
}

void UpdateTranslatePopup(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->open) {
        return;
    }
    if (!gSettings->translatePopupEnabled || !IsCurrentPopup(p) || !CanTranslate() || !CanCopy(win)) {
        ClosePopup(p, true);
        return;
    }
    if (IsActivelySelecting(win) || !HasSelection(win)) {
        ClosePopup(p, false);
    } else {
        Rect anchor = SelectionAnchor(win);
        bool visible = !anchor.IsEmpty();
        bool changed = p->anchorVisible != visible || (visible && p->anchor != anchor);
        p->anchorVisible = visible;
        if (p->anchorVisible) {
            p->anchor = anchor;
        }
        if (changed) {
            AppShellInvalidate(win);
        }
    }
}

void HideTranslatePopup(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->open) {
        return;
    }
    if (!IsActivelySelecting(win) && IsCurrentPopup(p) && gSettings->translatePopupEnabled && CanTranslate() &&
        CanCopy(win) && HasSelection(win) && SelectionAnchor(win).IsEmpty()) {
        p->anchorVisible = false;
        return;
    }
    ClosePopup(p, false);
}

void DismissTranslatePopup(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (p && p->open) {
        ClosePopup(p, true);
    }
}

struct ArrowPaint {
    gp::Point vertices[3];
};

static void PaintArrow(gp::PaintCtx* ctx, gp::El*, void* user) {
    auto* a = (ArrowPaint*)user;
    gp::Path* shape = gp::PathNew(ctx, true);
    if (!shape) {
        return;
    }
    gp::PathMoveTo(shape, a->vertices[1].x, a->vertices[1].y);
    gp::PathLineTo(shape, a->vertices[0].x, a->vertices[0].y);
    gp::PathLineTo(shape, a->vertices[2].x, a->vertices[2].y);
    gp::PathFill(ctx, shape, ToGpui(kShell));
    gp::PathStroke(ctx, shape, 1.f, ToGpui(kBorder));
    gp::PathFree(shape);
}

gp::El* TranslateArrowBuild(MainWindow* win, gp::Ctx* cx) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->open || !p->anchorVisible || p->sizing || !ToolWindowIsVisible(p->popup)) {
        return nullptr;
    }
    Rect frame = ToolWindowRect(p->popup);
    Rect canvas = AppShellCanvasScreenRect(win);
    TranslateArrowShape shape;
    if (canvas.IsEmpty() || !MeasureTranslateArrow(p->callout, frame, ReadingArea(win), TranslateToolbarScreenRect(win),
                                                   AppShellWindowDpi(win), shape)) {
        return nullptr;
    }
    auto* paint = gp::ArenaNew<ArrowPaint>(cx->a);
    float scale = (float)win->canvasRc.dx / (float)canvas.dx;
    for (int i = 0; i < 3; i++) {
        paint->vertices[i] = {(float)(shape.vertices[i].x - canvas.x) * scale + (float)win->canvasRc.x,
                              (float)(shape.vertices[i].y - canvas.y) * scale + (float)win->canvasRc.y};
    }
    gp::El* overlay = gp::Div(cx->a)
                          ->Absolute()
                          ->Left((float)(shape.rect.x - canvas.x) * scale)
                          ->Top((float)(shape.rect.y - canvas.y) * scale)
                          ->W((float)shape.rect.dx * scale)
                          ->H((float)shape.rect.dy * scale);
    overlay->customPaint = PaintArrow;
    overlay->customUser = paint;
    return overlay;
}

static gp::InputState* NewField(gp::App* app, Str text, bool multiLine = false) {
    auto* field = new gp::InputState();
    field->focus = gp::FocusHandleNew(app);
    if (multiLine) {
        field->kind = gp::InputKind::Textarea;
    }
    gp::InputSetValue(field, ToGpui(text));
    return field;
}

static void FreeField(gp::InputState* field, MainWindow* win) {
    InputBlurWhereFocused(field, win->gpuiWin);
    delete field;
}

static void CloseParams(NativeTranslatePopup* p) {
    ToolWindow* tw = p->params;
    p->params = nullptr;
    if (tw) {
        ToolWindowClose(tw);
    }
    for (const ConfigParam& param : p->draft->params) {
        FreeField(param.key, p->win);
        FreeField(param.value, p->win);
    }
    VecReset(p->draft->params);
    str::Free(p->draft->paramsNote);
    p->draft->paramsNote = {};
    if (p->config) {
        ToolWindowInvalidate(p->config);
    }
}

static void CloseConfig(NativeTranslatePopup* p) {
    if (!p || !p->draft) {
        return;
    }
    CloseParams(p);
    ToolWindow* tw = p->config;
    p->config = nullptr;
    if (tw) {
        ToolWindowClose(tw);
    }
    PopupDraft* d = p->draft;
    p->draft = nullptr;
    for (gp::InputState* field : d->fields) {
        FreeField(field, p->win);
    }
    d->from.Free();
    d->to.Free();
    d->thinking.Free();
    d->fonts.Free();
    d->position.Free();
    str::Free(d->customParams);
    str::Free(d->note);
    delete d;
}

void DeleteTranslatePopup(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p) {
        return;
    }
    VecRemove(gPopups, p);
    CancelJob(p);
    CloseConfig(p);
    if (p->popup) {
        ToolWindowClose(p->popup);
    }
    gp::EntityDrop(AppShellGetApp(), p->view.id);
    str::Free(p->source);
    str::Free(p->result);
    str::Free(p->status);
    delete p;
}

static void SetSelect(DialogSelect& select, gp::App* app, const Str* values, const Str* labels, int count, Str value,
                      int fallback) {
    StrVec names;
    int selected = fallback;
    for (int i = 0; i < count; i++) {
        names.Append(labels[i]);
        if (str::EqI(value, values[i])) {
            selected = i;
        }
    }
    select.Init(app);
    select.SetItems(names, selected);
}

static void SetLanguages(DialogSelect& select, gp::App* app, Str code, int first) {
    StrVec names;
    int count = 0;
    const LlmLanguage* langs = LlmLanguages(count);
    int selected = 0;
    for (int i = first; i < count; i++) {
        names.Append(langs[i].label);
        if (str::EqI(code, langs[i].code)) {
            selected = i - first;
        }
    }
    select.Init(app);
    select.SetItems(names, selected);
}

static Str LanguageValue(const DialogSelect& select, int first, Str fallback) {
    int count = 0;
    const LlmLanguage* langs = LlmLanguages(count);
    int selected = select.sel + first;
    return selected >= first && selected < count ? langs[selected].code : fallback;
}

static Str PromptValue(Str prompt) {
    return str::IsEmptyOrWhiteSpace(prompt) ? LlmDefaultPrompt() : str::ReplaceTemp(prompt, StrL("\r\n"), StrL("\n"));
}

static void InitDraft(NativeTranslatePopup* p) {
    auto* d = new PopupDraft();
    p->draft = d;
    gp::App* app = p->win->gpuiWin->app;
    d->app = app;
    d->enabled = gSettings->translatePopupEnabled;
    d->stream = gSettings->translatePopupStream;
    d->bold = gSettings->translatePopupBold;
    d->autoSize = gSettings->translatePopupAutoSize;
    d->customParams = str::Dup(gSettings->translatePopupCustomParams);
    const Str values[] = {
        gSettings->translatePopupUrl,
        gSettings->translatePopupModel,
        gSettings->translatePopupApiKey,
        fmt("%g", (double)gSettings->translatePopupTemperature),
        PromptValue(gSettings->translatePopupPrompt),
        FontFace(),
        fmt("%d", FontSize()),
        fmt("%d%%", LineSpacing()),
        fmt("%d", std::clamp(gSettings->translatePopupWidth, kMinWidth, kMaxWidth)),
        fmt("%d", std::clamp(gSettings->translatePopupHeight, kMinHeight, kMaxHeight)),
        fmt("%d", std::clamp(gSettings->translatePopupMaxHeight, 30, 90)),
    };
    static_assert(dimofi(values) == (int)ConfigField::Count);
    for (int i = 0; i < dimofi(values); i++) {
        d->fields[i] = NewField(app, values[i], i == (int)ConfigField::Prompt);
    }
    d->Input(ConfigField::Key)->masked = true;
    SetLanguages(d->from, app, gSettings->translatePopupLangFrom, 0);
    SetLanguages(d->to, app, gSettings->translatePopupLangTo, 1);
    SetSelect(d->thinking, app, kThinkingValues, kThinkingLabels, dimofi(kThinkingValues),
              gSettings->translatePopupThinking, 1);
    Str keys[dimofi(kPositions)]{};
    for (int i = 0; i < dimofi(kPositions); i++) {
        keys[i] = TranslatePositionName(kPositions[i]);
    }
    SetSelect(d->position, app, keys, kPositionLabels, dimofi(keys), gSettings->translatePopupPosition, 0);
    int fontCount = 0;
    const gp::Str* fonts = gp::PaintInstalledFontNames(app->paint, &fontCount);
    StrVec names;
    int selected = -1;
    for (int i = 0; i < fontCount; i++) {
        Str name = FromGpui(fonts[i]);
        if (str::EqI(name, FontFace())) {
            selected = i;
        }
        names.Append(name);
    }
    d->fonts.Init(app);
    d->fonts.SetItems(names, selected);
}

static bool ReadInt(Str value, int minimum, int maximum, int& out, bool percent = false) {
    str::TrimWsBoth(value);
    if (percent && str::EndsWith(value, StrL("%"))) {
        value.len--;
        str::TrimWsBoth(value);
    }
    if (len(value) == 0) {
        return false;
    }
    char* start = CStrTemp(value);
    char* end = nullptr;
    long n = strtol(start, &end, 10);
    if (end != start + len(value) || n < minimum || n > maximum) {
        return false;
    }
    out = (int)n;
    return true;
}

static bool ConfigError(NativeTranslatePopup* p, Str msg) {
    str::ReplaceWithCopy(&p->draft->note, msg);
    ToolWindowInvalidate(p->config);
    return false;
}

static bool SaveConfig(NativeTranslatePopup* p) {
    PopupDraft* d = p->draft;
    if (p->params) {
        ToolWindowActivate(p->params);
        return false;
    }
    Str face = d->Text(ConfigField::Font);
    str::TrimWsBoth(face);
    if (len(face) == 0 || str::ContainsChar(face, '\0') || str::ContainsChar(face, '\r') ||
        str::ContainsChar(face, '\n') || len(ToWStrTemp(face)) >= 32) {
        return ConfigError(p, StrL("请选择字体名称，最多 31 个字符，不能包含换行。"));
    }
    int points, spacing, width, height, maxHeight;
    if (!ReadInt(d->Text(ConfigField::FontSize), kMinFont, kMaxFont, points)) {
        return ConfigError(p, StrL("译文字号须为 8 到 32 之间的整数。"));
    }
    if (!ReadInt(d->Text(ConfigField::LineSpacing), 1, 200, spacing, true)) {
        return ConfigError(p, StrL("文字行距须为 1% 到 200% 之间的整数百分比。"));
    }
    if (!ReadInt(d->Text(ConfigField::Width), kMinWidth, kMaxWidth, width)) {
        return ConfigError(p, StrL("阅读宽度须为 330 到 1600 之间的整数。"));
    }
    if (!ReadInt(d->Text(ConfigField::Height), kMinHeight, kMaxHeight, height)) {
        return ConfigError(p, StrL("手动高度须为 180 到 1200 之间的整数。"));
    }
    if (!ReadInt(d->Text(ConfigField::MaxHeight), 30, 90, maxHeight)) {
        return ConfigError(p, StrL("自动最大高度须为阅读区高度的 30% 到 90%。"));
    }
    Str url = d->Text(ConfigField::Url);
    Str model = d->Text(ConfigField::Model);
    Str key = d->Text(ConfigField::Key);
    str::TrimWsBoth(url);
    str::TrimWsBoth(model);
    str::TrimWsBoth(key);
    Str temperature = d->Text(ConfigField::Temperature);
    str::TrimWsBoth(temperature);
    if (len(temperature) == 0) {
        return ConfigError(p, StrL("温度须为 0 到 2 之间的数字。"));
    }
    char* start = CStrTemp(temperature);
    char* end = nullptr;
    double temp = strtod(start, &end);
    if (end != start + len(temperature) || !(temp >= 0 && temp <= 2)) {
        return ConfigError(p, StrL("温度须为 0 到 2 之间的数字。"));
    }
    Str from = LanguageValue(d->from, 0, StrL("auto"));
    Str to = LanguageValue(d->to, 1, StrL("zh-CN"));
    Str thinking = kThinkingValues[std::clamp(d->thinking.sel, 0, dimofi(kThinkingValues) - 1)];
    Str position = TranslatePositionName(kPositions[std::clamp(d->position.sel, 0, dimofi(kPositions) - 1)]);
    Str prompt = str::ReplaceTemp(d->Text(ConfigField::Prompt), StrL("\r\n"), StrL("\n"));
    bool requestChanged =
        !str::Eq(url, gSettings->translatePopupUrl) || !str::Eq(model, gSettings->translatePopupModel) ||
        !str::Eq(key, gSettings->translatePopupApiKey) ||
        !str::Eq(prompt, PromptValue(gSettings->translatePopupPrompt)) ||
        !str::Eq(from, gSettings->translatePopupLangFrom) || !str::Eq(to, gSettings->translatePopupLangTo) ||
        !str::Eq(thinking, gSettings->translatePopupThinking) ||
        !str::Eq(d->customParams, gSettings->translatePopupCustomParams) ||
        d->stream != gSettings->translatePopupStream || d->enabled != gSettings->translatePopupEnabled ||
        (float)temp != gSettings->translatePopupTemperature;
    bool appearanceChanged =
        !str::Eq(face, FontFace()) || points != FontSize() || spacing != LineSpacing() ||
        width != gSettings->translatePopupWidth || height != gSettings->translatePopupHeight ||
        maxHeight != gSettings->translatePopupMaxHeight || d->bold != gSettings->translatePopupBold ||
        d->autoSize != gSettings->translatePopupAutoSize ||
        !str::Eq(position, TranslatePositionName(ParseTranslatePosition(gSettings->translatePopupPosition)));
    if (d->enabled && (requestChanged || !appearanceChanged) && (len(url) == 0 || len(model) == 0)) {
        return ConfigError(p, StrL("启用自动翻译前，请填写完整 API 地址和模型。"));
    }
    if (len(url) && !str::StartsWithI(url, StrL("https://")) && !str::StartsWithI(url, StrL("http://"))) {
        return ConfigError(p, StrL("API 地址须以 https:// 或 http:// 开头。"));
    }
    if (str::ContainsChar(key, '\r') || str::ContainsChar(key, '\n')) {
        return ConfigError(p, StrL("密钥不能包含换行。"));
    }
    Str error;
    if (!CheckLlmPrompt(prompt, error)) {
        ConfigError(p, error);
        str::Free(error);
        return false;
    }
    Vec<LlmJsonParam> params;
    bool valid = ReadLlmParams(d->customParams, params, error);
    FreeLlmParams(params);
    if (!valid) {
        ConfigError(p, error);
        str::Free(error);
        return false;
    }
    str::ReplaceWithCopy(&gSettings->translatePopupUrl, url);
    str::ReplaceWithCopy(&gSettings->translatePopupModel, model);
    str::ReplaceWithCopy(&gSettings->translatePopupApiKey, key);
    str::ReplaceWithCopy(&gSettings->translatePopupPrompt, prompt);
    str::ReplaceWithCopy(&gSettings->translatePopupLangFrom, from);
    str::ReplaceWithCopy(&gSettings->translatePopupLangTo, to);
    str::ReplaceWithCopy(&gSettings->translatePopupThinking, thinking);
    str::ReplaceWithCopy(&gSettings->translatePopupCustomParams, d->customParams);
    str::ReplaceWithCopy(&gSettings->translatePopupPosition, position);
    str::ReplaceWithCopy(&gSettings->translatePopupFont, face);
    gSettings->translatePopupTemperature = (float)temp;
    gSettings->translatePopupFontSize = points;
    gSettings->translatePopupLineSpacing = spacing;
    gSettings->translatePopupWidth = width;
    gSettings->translatePopupHeight = height;
    gSettings->translatePopupMaxHeight = maxHeight;
    gSettings->translatePopupBold = d->bold;
    gSettings->translatePopupAutoSize = d->autoSize;
    gSettings->translatePopupEnabled = d->enabled;
    gSettings->translatePopupStream = d->stream;
    ScheduleSaveSettings();
    CloseConfig(p);
    if (!gSettings->translatePopupEnabled) {
        ClosePopup(p, true);
    } else if (p->open && IsCurrentPopup(p)) {
        if (requestChanged && HasSelection(p->win)) {
            bool textOnly = false;
            TempStr text = GetSelectedTextTemp(p->tab, StrL("\n"), textOnly);
            if (textOnly && str::Eq(p->source, text)) {
                StartJob(p);
            }
        }
        Refit(p, false);
    }
    for (NativeTranslatePopup* other : gPopups) {
        if (other != p && other->open) {
            if (gSettings->translatePopupEnabled) {
                Refit(other, false);
            } else {
                ClosePopup(other, true);
            }
        }
    }
    return true;
}

static gp::El* ConfigLabel(gp::Ctx* cx, Str label) {
    return gp::TextEl(cx->a, GpuiDup(cx->a, label))
        ->W(gp::kFill)
        ->MinH(24)
        ->Font(14)
        ->Wrap()
        ->Fg(ToGpui(ThemeWindowTextColor()))
        ->Shrink0();
}

static gp::El* ConfigInput(NativeTranslatePopup* p, gp::Ctx* cx, ConfigField field, bool disabled = false) {
    gp::InputState* state = p->draft->Input(field);
    gp::Str id = GpuiDup(cx->a, fmt("translation-field-%d", (int)field));
    if (field == ConfigField::Prompt) {
        return gpc::Textarea::New(cx, id, state)->H(140)->Disabled(disabled)->IntoEl();
    }
    auto* input = gpc::Input::New(cx, id, state)->Disabled(disabled)->W(gp::kFill);
    if (field == ConfigField::Key) {
        input->Masked(true)->ContentType(gpc::InputContentType::Password);
    }
    return input->IntoEl()->H(28)->Shrink0();
}

static gp::El* ConfigButton(NativeTranslatePopup* p, gp::Ctx* cx, Str label, ConfigAction action,
                            bool primary = false) {
    auto* button = gpc::Button::New(cx, GpuiDup(cx->a, fmt("translation-config-action-%d", (int)action)))
                       ->Label(GpuiDup(cx->a, label))
                       ->OnClick(gp::ListenTo(p->view, &PopupView::OnConfig, (int64_t)action));
    return (primary ? button->Primary() : button->Outline())->IntoEl()->H(29)->Shrink0();
}

static gp::El* ConfigFlag(NativeTranslatePopup* p, gp::Ctx* cx, Str label, ConfigAction action, bool checked) {
    return gpc::Checkbox::New(cx, GpuiDup(cx->a, fmt("translation-config-flag-%d", (int)action)))
        ->Label(GpuiDup(cx->a, label))
        ->Checked(checked)
        ->OnClick(gp::ListenTo(p->view, &PopupView::OnConfig, (int64_t)action))
        ->IntoEl()
        ->H(26)
        ->Shrink0();
}

static gp::El* ConfigFooter(NativeTranslatePopup* p, gp::Ctx* cx, ConfigAction cancel, ConfigAction save) {
    return gp::Div(cx->a)
        ->FlexRow()
        ->W(gp::kFill)
        ->H(49)
        ->Pad(10)
        ->Gap(10)
        ->Shrink0()
        ->JustifyEnd()
        ->Child(ConfigButton(p, cx, StrL("取消"), cancel)->W(80))
        ->Child(ConfigButton(p, cx, StrL("保存"), save, true)->W(80));
}

static gp::El* ConfigBuild(MainWindow* win, gp::Ctx* cx) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->draft) {
        return nullptr;
    }
    PopupDraft* d = p->draft;
    d->from.PollChanged(cx->app);
    d->to.PollChanged(cx->app);
    d->thinking.PollChanged(cx->app);
    d->position.PollChanged(cx->app);
    d->fonts.PollChanged(cx->app);
    if (d->focus) {
        d->focus = false;
        gp::InputFocus(d->Input(ConfigField::Url), cx->app, cx->win);
    }
    gp::El* content = gp::Div(cx->a)->FlexCol()->W(gp::kFill)->Pad(18)->Gap(7)->Shrink0();
    auto field = [&](Str label, ConfigField value, bool disabled = false) {
        content->Child(ConfigLabel(cx, label));
        content->Child(ConfigInput(p, cx, value, disabled));
    };
    field(StrL("API 地址（完整接口）"), ConfigField::Url);
    field(StrL("模型"), ConfigField::Model);
    field(StrL("API 密钥（本地服务可留空）"), ConfigField::Key);
    gp::El* languages = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->Gap(18)->Shrink0();
    languages->Child(gp::Div(cx->a)
                         ->FlexCol()
                         ->Flex1()
                         ->MinW(0)
                         ->Gap(3)
                         ->Child(ConfigLabel(cx, StrL("源语言")))
                         ->Child(d->from.Build(cx, StrL("translation-source-language"), gp::kFill)));
    languages->Child(gp::Div(cx->a)
                         ->FlexCol()
                         ->Flex1()
                         ->MinW(0)
                         ->Gap(3)
                         ->Child(ConfigLabel(cx, StrL("目标语言")))
                         ->Child(d->to.Build(cx, StrL("translation-target-language"), gp::kFill)));
    content->Child(languages);
    field(StrL("温度（0–2）"), ConfigField::Temperature);
    field(StrL("提示词"), ConfigField::Prompt);
    content->Child(ConfigLabel(cx, StrL("可用变量："
                                        "$"
                                        "{langFrom}、"
                                        "$"
                                        "{langTo}、"
                                        "$"
                                        "{sourceText}。必须包含 "
                                        "$"
                                        "{sourceText}。")));
    content->Child(ConfigFlag(p, cx, StrL("Stream（流式显示）"), ConfigAction::Stream, d->stream));
    content->Child(ConfigLabel(cx, StrL("Thinking（思考）")));
    content->Child(d->thinking.Build(cx, StrL("translation-thinking"), gp::kFill));
    content->Child(ConfigLabel(cx, StrL("按 API 地址识别思考参数。未识别的中转服务请在“自定义请求参数”中设置。")));
    content->Child(ConfigButton(p, cx, StrL("自定义请求参数…"), ConfigAction::Params)->W(190));
    content->Child(ConfigFlag(p, cx, StrL("划词后自动翻译"), ConfigAction::Enable, d->enabled));
    content->Child(ConfigLabel(cx, StrL("译文显示"))->Bold());
    content->Child(ConfigLabel(cx, StrL("译文字体（不改变界面字体）")));
    content->Child(
        d->fonts.BuildCombo(cx, StrL("translation-font"), d->Input(ConfigField::Font), gp::kFill, false, 28));
    field(StrL("译文字号（8–32 pt）"), ConfigField::FontSize);
    field(StrL("文字行距（1–200%，100% 为单倍）"), ConfigField::LineSpacing);
    content->Child(ConfigFlag(p, cx, StrL("译文加粗"), ConfigAction::Bold, d->bold));
    content->Child(ConfigFlag(p, cx, StrL("按译文自适应高度"), ConfigAction::AutoSize, d->autoSize));
    field(StrL("阅读宽度 / 手动宽度（330–1600）"), ConfigField::Width);
    field(StrL("手动高度（180–1200，仅手动模式）"), ConfigField::Height, d->autoSize);
    field(StrL("自动最大高度（占阅读区高度的 30–90%）"), ConfigField::MaxHeight, !d->autoSize);
    content->Child(ConfigLabel(cx, StrL("浮窗位置（空间不足自动换边）")));
    content->Child(d->position.Build(cx, StrL("translation-position"), gp::kFill));
    Str note =
        len(d->note)
            ? d->note
            : StrL("宽高以100%缩放为基准，随系统缩放变化。拖动边框后锁定尺寸，点击“自适应”恢复。密钥保存在本机。");
    content->Child(ConfigLabel(cx, note));
    gp::El* scroll = gp::Div(cx->a)
                         ->Id(GStrL("translation-settings-scroll"))
                         ->FlexCol()
                         ->Flex1()
                         ->W(gp::kFill)
                         ->MinH(0)
                         ->ScrollY(d->scroll)
                         ->ScrollMode(gp::ScrollbarMode::Always)
                         ->ScrollFromPath()
                         ->OnScroll(gp::ListenTo(p->view, &PopupView::OnConfigScroll))
                         ->Child(content);
    return gp::Div(cx->a)
        ->SizeFull()
        ->FlexCol()
        ->Bg(ToGpui(ThemeWindowBackgroundColor()))
        ->Child(scroll)
        ->Child(ConfigFooter(p, cx, ConfigAction::Cancel, ConfigAction::Save));
}

static void ConfigClosed(MainWindow* win) {
    if (NativeTranslatePopup* p = FindPopup(win)) {
        p->config = nullptr;
        CloseConfig(p);
    }
}

static bool ConfigKey(MainWindow* win, gp::Ctx* cx, const gp::KeyEvent* event) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->draft || p->params) {
        return false;
    }
    if (event->vk == VK_ESCAPE) {
        CloseConfig(p);
        return true;
    }
    if (event->vk == VK_RETURN && !event->ctrl && !event->alt &&
        !(cx->win->input && gp::InputIsMultiLine(cx->win->input))) {
        SaveConfig(p);
        return true;
    }
    return false;
}

static Str ConfigTitle() {
    return StrL("划词翻译设置");
}

static void OpenConfig(NativeTranslatePopup* p) {
    if (p->config) {
        ToolWindowActivate(p->config);
        return;
    }
    InitDraft(p);
    auto desc = ToolWindowModalDesc("translation-settings", ConfigTitle);
    desc.resize = ToolWinResize::Resizable;
    desc.minClient = {410, 300};
    desc.build = ConfigBuild;
    desc.onKey = ConfigKey;
    desc.onClosed = ConfigClosed;
    Rect outer = ToolWindowCenteredRect(desc, p->win, Size(606, 910));
    Rect work = AppShellWorkArea(p->win);
    if (!work.IsEmpty()) {
        outer.dx = std::min(work.dx, outer.dx);
        outer.dy = std::min(work.dy, outer.dy);
        outer = AppShellShiftToWorkArea(outer, p->win, true);
    }
    p->config = ToolWindowOpen(desc, p->win, outer);
    if (!p->config) {
        CloseConfig(p);
    }
}

static gp::El* ParamsBuild(MainWindow* win, gp::Ctx* cx) {
    NativeTranslatePopup* p = FindPopup(win);
    if (!p || !p->draft) {
        return nullptr;
    }
    PopupDraft* d = p->draft;
    gp::El* fields = gp::Div(cx->a)->FlexCol()->W(gp::kFill)->Pad(12)->Gap(8)->Shrink0();
    for (int i = 0; i < len(d->params); i++) {
        const ConfigParam& param = d->params[i];
        gp::El* row = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->Gap(8)->ItemsCenter()->Shrink0();
        row->Child(
            gpc::Input::New(cx, GpuiDup(cx->a, fmt("translation-param-key-%d", i)), param.key)->W(175)->IntoEl());
        row->Child(gpc::Input::New(cx, GpuiDup(cx->a, fmt("translation-param-value-%d", i)), param.value)
                       ->IntoEl()
                       ->Flex1()
                       ->MinW(0));
        row->Child(
            gpc::Button::New(cx, GpuiDup(cx->a, fmt("translation-param-remove-%d", i)))
                ->Label(GStrL("删除"))
                ->Outline()
                ->OnClick(gp::ListenTo(p->view, &PopupView::OnConfig, (int64_t)ConfigAction::RemoveParam + (i << 8)))
                ->IntoEl());
        fields->Child(row);
    }
    fields->Child(ConfigButton(p, cx, StrL("添加参数"), ConfigAction::AddParam)->W(120));
    gp::El* scroll = gp::Div(cx->a)
                         ->Id(GStrL("translation-params-scroll"))
                         ->FlexCol()
                         ->Flex1()
                         ->W(gp::kFill)
                         ->MinH(0)
                         ->ScrollY(d->paramsScroll)
                         ->ScrollMode(gp::ScrollbarMode::Always)
                         ->ScrollFromPath()
                         ->OnScroll(gp::ListenTo(p->view, &PopupView::OnParamsScroll))
                         ->Child(fields);
    return gp::Div(cx->a)
        ->SizeFull()
        ->FlexCol()
        ->Bg(ToGpui(ThemeWindowBackgroundColor()))
        ->Child(
            gp::Div(cx->a)
                ->Pad(12)
                ->W(gp::kFill)
                ->Shrink0()
                ->FlexCol()
                ->Child(ConfigLabel(
                    cx, StrL("参照服务商 API 文档填写参数。值必须是 JSON，可用对象、数组、字符串、数字、true、false 或 "
                             "null。\n同名参数覆盖标准请求参数。")))
                ->Child(ConfigLabel(cx, StrL("参数名称                      参数值（JSON）"))))
        ->Child(scroll)
        ->Child(ConfigLabel(cx, d->paramsNote))
        ->Child(ConfigFooter(p, cx, ConfigAction::CancelParams, ConfigAction::SaveParams));
}

static Str ParamsTitle() {
    return StrL("自定义请求参数");
}

static void ParamsClosed(MainWindow* win) {
    NativeTranslatePopup* p = FindPopup(win);
    if (p && p->draft) {
        p->params = nullptr;
        CloseParams(p);
    }
}

static void OpenParams(NativeTranslatePopup* p) {
    if (p->params) {
        ToolWindowActivate(p->params);
        return;
    }
    PopupDraft* d = p->draft;
    Vec<LlmJsonParam> values;
    Str error;
    if (ReadLlmParams(d->customParams, values, error)) {
        for (const LlmJsonParam& value : values) {
            VecAppend(d->params, ConfigParam{NewField(d->app, value.name), NewField(d->app, value.value)});
        }
    } else {
        str::ReplaceWithCopy(&d->paramsNote, error);
    }
    str::Free(error);
    FreeLlmParams(values);
    d->paramsScroll = 0;
    auto desc = ToolWindowModalDesc("translation-parameters", ParamsTitle);
    desc.resize = ToolWinResize::Resizable;
    desc.diagnosticText = ToolWinDiagnosticText::Omit;
    desc.minClient = {410, 240};
    desc.build = ParamsBuild;
    desc.onClosed = ParamsClosed;
    p->params = ToolWindowOpen(desc, p->win, ToolWindowCenteredRect(desc, p->win, Size(650, 430)));
    if (!p->params) {
        CloseParams(p);
    }
}

static void SaveParams(NativeTranslatePopup* p) {
    Vec<LlmJsonParam> rows;
    for (const ConfigParam& param : p->draft->params) {
        Str name = FromGpui(gp::InputValue(param.key));
        Str value = FromGpui(gp::InputValue(param.value));
        VecAppend(rows, LlmJsonParam{name, value});
    }
    Str object, error;
    if (WriteLlmParams(rows, object, error)) {
        str::ReplacePtr(&p->draft->customParams, object);
        CloseParams(p);
    } else {
        str::ReplacePtr(&p->draft->paramsNote, error);
        ToolWindowInvalidate(p->params);
    }
}

void PopupView::OnConfig(PopupView* view, gp::Ctx*, const gp::ClickEvent*, int64_t action) {
    NativeTranslatePopup* p = FindPopup(view->win);
    if (!p || !p->draft) {
        return;
    }
    PopupDraft* d = p->draft;
    switch ((ConfigAction)(action & 255)) {
        case ConfigAction::Enable:
            d->enabled = !d->enabled;
            break;
        case ConfigAction::Stream:
            d->stream = !d->stream;
            break;
        case ConfigAction::Bold:
            d->bold = !d->bold;
            break;
        case ConfigAction::AutoSize:
            d->autoSize = !d->autoSize;
            break;
        case ConfigAction::Params:
            OpenParams(p);
            break;
        case ConfigAction::Cancel:
            CloseConfig(p);
            break;
        case ConfigAction::Save:
            SaveConfig(p);
            break;
        case ConfigAction::AddParam:
            VecAppend(d->params, ConfigParam{NewField(d->app, {}), NewField(d->app, {})});
            break;
        case ConfigAction::RemoveParam: {
            int index = (int)(action >> 8);
            if (index >= 0 && index < len(d->params)) {
                const ConfigParam& param = d->params[index];
                FreeField(param.key, p->win);
                FreeField(param.value, p->win);
                VecRemoveAt(d->params, index);
            }
            break;
        }
        case ConfigAction::SaveParams:
            SaveParams(p);
            break;
        case ConfigAction::CancelParams:
            CloseParams(p);
            break;
    }
    if (p->params) {
        ToolWindowInvalidate(p->params);
    }
    if (p->config) {
        ToolWindowInvalidate(p->config);
    }
}

void PopupView::OnConfigScroll(PopupView* view, gp::Ctx* cx, const gp::ScrollEvent* ev) {
    NativeTranslatePopup* p = FindPopup(view->win);
    if (p && p->draft) {
        p->draft->scroll = ev->offsetY;
        gp::Notify(cx);
    }
}

void PopupView::OnParamsScroll(PopupView* view, gp::Ctx* cx, const gp::ScrollEvent* ev) {
    NativeTranslatePopup* p = FindPopup(view->win);
    if (p && p->draft) {
        p->draft->paramsScroll = ev->offsetY;
        gp::Notify(cx);
    }
}
