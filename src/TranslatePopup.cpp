/* Copyright 2026. License: GPL-3.0-only */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Http.h"
#include "base/JsonParser.h"
#include "base/UITask.h"
#include "gui/Dpi.h"
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
#include "TextSelection.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "Theme.h"
#include "DarkMode.h"
#include "LlmTranslate.h"
#include "LlmRequestOptions.h"
#include "TranslateParams.h"
#include "TranslateAnchor.h"
#include "TranslatePopup.h"

constexpr WCHAR kPopupClass[] = L"SumatraTranslatePopup";
constexpr WCHAR kConfigClass[] = L"SumatraTranslateConfig";
constexpr int kPopupWidth = 460;
constexpr int kPopupHeight = 350;
constexpr int kPopupMinWidth = 330;
constexpr int kPopupMinHeight = 180;
constexpr int kPopupDefaultFontSize = 11;
constexpr int kPopupMinFontSize = 8;
constexpr int kPopupMaxFontSize = 32;
constexpr int kPopupMaxWidth = 1600;
constexpr int kPopupMaxHeight = 1200;
constexpr UINT_PTR kPopupFitTimer = 2;
constexpr UINT kPopupFitInterval = 180;
constexpr int kPopupMargin = 12;
constexpr int kPopupGap = 6;
constexpr int kPopupHeaderHeight = 26;
constexpr int kPopupStatusHeight = 22;
constexpr int kPopupButtonHeight = 28;
constexpr int kPopupCardPaddingX = 8;
constexpr int kPopupCardPaddingY = 4;
constexpr int kPopupCardRadius = 8;
constexpr Color kPopupShellColor = RGB(242, 242, 242);
constexpr Color kPopupCardColor = RGB(255, 255, 255);
constexpr Color kPopupBorderColor = RGB(213, 213, 213);
constexpr Color kPopupTextColor = RGB(32, 32, 32);
constexpr Color kPopupSecondaryColor = RGB(96, 96, 96);
constexpr int kBodyLimit = 1024 * 1024;
constexpr UINT_PTR kEscapeSubclass = 1;
constexpr int kConfigContentHeight = 1410;
constexpr int kConfigScrollLine = 24;

enum class PopupHide {
    Ordinary,
    Force
};
enum PopupButton {
    ButtonCopy = 3001,
    ButtonRetry,
    ButtonPin,
    ButtonSettings,
    ButtonClose,
    ButtonSave,
    ButtonCancel,
    ButtonFontMinus = 3010,
    ButtonFontPlus,
    ButtonAutoSize,
    ButtonBold
};

enum class PopupFit {
    Grow,
    Exact,
    Reset
};

enum ConfigControlId {
    ConfigUrl = 3101,
    ConfigModel,
    ConfigKey,
    ConfigTemperature,
    ConfigPrompt,
    ConfigStream,
    ConfigThinking,
    ConfigLangFrom,
    ConfigLangTo,
    ConfigEnabled,
    ConfigCustom,
    ConfigFont = 3120,
    ConfigFontSize,
    ConfigAutoSize,
    ConfigWidth,
    ConfigHeight,
    ConfigMaxHeight,
    ConfigBold,
    ConfigPosition
};

constexpr Str kThinkingValues[] = {StrL("default"), StrL("off"),  StrL("low"),
                                   StrL("medium"),  StrL("high"), StrL("max")};
constexpr const WCHAR* kThinkingLabels[] = {L"Default（服务默认）", L"Off（关闭）", L"Low（低）",
                                            L"Medium（中）",        L"High（高）",  L"Max（最高）"};
constexpr TranslatePosition kPositionValues[] = {TranslatePosition::Auto, TranslatePosition::Top,
                                                 TranslatePosition::Bottom, TranslatePosition::Left,
                                                 TranslatePosition::Right};
constexpr const WCHAR* kPositionLabels[] = {L"自动", L"上方", L"下方", L"左侧", L"右侧"};

struct PopupJob;

struct TranslatePopup {
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    DocController* document = nullptr;
    HWND hwnd = nullptr;
    HWND config = nullptr;
    HWND title = nullptr;
    HWND status = nullptr;
    HWND resultEdit = nullptr;
    HWND buttons[5]{};
    HWND fontMinus = nullptr;
    HWND fontPlus = nullptr;
    HWND fontSizeText = nullptr;
    HWND autoSizeButton = nullptr;
    HWND boldButton = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT textFont = nullptr;
    Str fontFace;
    int fontSize = 0;
    int fontDpi = 0;
    bool bold = false;
    int preferredWidth = kPopupWidth;
    int preferredHeight = kPopupHeight;
    int maxHeight = 70;
    bool autoSize = true;
    bool fitPending = false;
    bool fitting = false;
    bool sizing = false;
    Rect sizeStart;
    Rect workArea;
    Size fittedSize;
    bool open = false;
    u64 resizeCount = 0;
    HBRUSH brush = nullptr;
    HBRUSH bodyBrush = nullptr;
    Rect card;
    u64 instance = 0;
    u64 request = 0;
    int dpi = 96;
    Rect anchor;
    bool anchorVisible = false;
    TranslatePosition position = TranslatePosition::Auto;
    TranslateCallout callout;
    TranslateArrow* arrow = nullptr;
    Str source;
    Str result;
    Str state;
    bool pinned = false;
    bool busy = false;
    bool error = false;
    PopupJob* job = nullptr;
};

struct ConfigItem {
    HWND hwnd = nullptr;
    Rect rect;
    int id = 0;
};

struct PopupConfig {
    u64 instance = 0;
    HWND hwnd = nullptr;
    HWND content = nullptr;
    HWND url = nullptr;
    HWND model = nullptr;
    HWND key = nullptr;
    HWND temperature = nullptr;
    HWND prompt = nullptr;
    HWND stream = nullptr;
    HWND thinking = nullptr;
    HWND langFrom = nullptr;
    HWND langTo = nullptr;
    HWND params = nullptr;
    Str customParams;
    HWND fontFace = nullptr;
    HWND fontSize = nullptr;
    HWND bold = nullptr;
    HWND autoSize = nullptr;
    HWND width = nullptr;
    HWND height = nullptr;
    HWND maxHeight = nullptr;
    HWND position = nullptr;
    HWND enabled = nullptr;
    HWND note = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    Color background = kColorUnset;
    int dpi = 96;
    int scroll = 0;
    Vec<ConfigItem> items;
};

static Vec<TranslatePopup*> gPopups;
static u64 gPopupInstance = 0;
static u64 gPopupRequest = 0;

static TranslatePopup* FindPopup(MainWindow* win) {
    for (TranslatePopup* p : gPopups) {
        if (p->win == win) return p;
    }
    return nullptr;
}

static TranslatePopup* FindPopupById(u64 instance) {
    for (TranslatePopup* p : gPopups) {
        if (p->instance == instance) return p;
    }
    return nullptr;
}

static void CancelPopupJob(TranslatePopup* p);
static void HidePopup(TranslatePopup* p, PopupHide how);
static void StartPopupJob(TranslatePopup* p);
static void OpenPopupConfig(TranslatePopup* p);
static void LayoutConfig(PopupConfig* c);
static void RevealConfigControl(PopupConfig* c, HWND child);
static void JobUpdate(PopupJob* job);
static void JobFinished(PopupJob* job);
static void FitPopup(TranslatePopup* p, PopupFit how);
static void PlacePopup(TranslatePopup* p);
static void UpdatePopupTheme(TranslatePopup* p);
static void CancelPopupFit(TranslatePopup* p);
static void QueuePopupFit(TranslatePopup* p);
static void UpdateAppearance(TranslatePopup* p);
static void SetPopupFontSize(TranslatePopup* p, int value);
static void SetPopupAutoSize(TranslatePopup* p, bool enabled);
static void SetPopupBold(TranslatePopup* p, bool enabled);
static int PopupLineHeight(TranslatePopup* p);

struct PopupJob : LlmTranslateProgress {
    volatile LONG refs = 2;
    u64 instance = 0;
    u64 request = 0;
    LlmTranslateRequest input;
    HttpStreamCancel cancel;
    CRITICAL_SECTION lock;
    Str latest;
    Str error;
    bool updateQueued = false;
    bool ok = false;

    PopupJob() { InitializeCriticalSection(&lock); }
    ~PopupJob() override {
        DeleteCriticalSection(&lock);
        str::Free(input.url);
        str::Free(input.model);
        str::Free(input.apiKey);
        str::Free(input.text);
        str::Free(input.langFrom);
        str::Free(input.langTo);
        str::Free(input.prompt);
        str::Free(input.thinking);
        str::Free(input.customParams);
        str::Free(latest);
        str::Free(error);
    }

    void OnText(Str text) override {
        if (cancel.IsCanceled()) return;
        EnterCriticalSection(&lock);
        str::ReplaceWithCopy(&latest, text);
        bool queue = !updateQueued;
        updateQueued = true;
        if (queue) InterlockedIncrement(&refs);
        LeaveCriticalSection(&lock);
        if (queue) uitask::Post(MkFunc0(JobUpdate, this), "TranslatePopupUpdate");
    }
};

static void ReleaseJob(PopupJob* job) {
    if (InterlockedDecrement(&job->refs) == 0) delete job;
}

static void CancelPopupJob(TranslatePopup* p) {
    CancelPopupFit(p);
    PopupJob* job = p->job;
    p->job = nullptr;
    p->busy = false;
    if (job) {
        job->cancel.Cancel();
        ReleaseJob(job);
    }
}

static int ScalePopup(TranslatePopup* p, int value) {
    return DpiScaleByDpi(p->dpi, value);
}

static int PopupClamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static Str PopupFontName(Str name) {
    str::TrimWsBoth(name);
    if (len(name) == 0 || str::ContainsChar(name, '\0') || str::ContainsChar(name, '\r') ||
        str::ContainsChar(name, '\n') || len(ToWStrTemp(name)) >= LF_FACESIZE) {
        return StrL("Microsoft YaHei UI");
    }
    return name;
}

static HFONT NewPopupFont(int dpi, int points, int weight, Str name = {}) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(points, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, ToWStrTemp(PopupFontName(name)).s);
    return CreateFontIndirectW(&lf);
}

static void SetPopupStatus(TranslatePopup* p, Str text) {
    str::ReplaceWithCopy(&p->state, text);
    HwndSetText(p->status, text);
}

static void SetPopupResult(TranslatePopup* p, Str text) {
    str::ReplaceWithCopy(&p->result, text);
    int firstLine = (int)SendMessageW(p->resultEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(p->resultEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    TempStr normalized = str::ReplaceTemp(text, StrL("\r\n"), StrL("\n"));
    HwndSetText(p->resultEdit, str::ReplaceTemp(normalized, StrL("\n"), StrL("\r\n")));
    SendMessageW(p->resultEdit, EM_SETSEL, selStart, selEnd);
    int now = (int)SendMessageW(p->resultEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(p->resultEdit, EM_LINESCROLL, 0, firstLine - now);
    EnableWindow(p->buttons[0], !str::IsEmptyOrWhiteSpace(text));
    QueuePopupFit(p);
}

static bool IsCurrentPopup(TranslatePopup* p) {
    return p && IsWindow(p->hwnd) && p->win->CurrentTab() == p->tab && p->win->ctrl == p->document;
}

static void JobUpdate(PopupJob* job) {
    EnterCriticalSection(&job->lock);
    Str latest = str::Dup(job->latest);
    job->updateQueued = false;
    LeaveCriticalSection(&job->lock);
    TranslatePopup* p = FindPopupById(job->instance);
    if (p && p->job == job && p->request == job->request && IsCurrentPopup(p) && !job->cancel.IsCanceled()) {
        SetPopupResult(p, latest);
    }
    str::Free(latest);
    ReleaseJob(job);
}

static void JobFinished(PopupJob* job) {
    TranslatePopup* p = FindPopupById(job->instance);
    if (p && p->job == job && p->request == job->request) {
        p->job = nullptr;
        p->busy = false;
        if (IsCurrentPopup(p) && !job->cancel.IsCanceled()) {
            if (job->ok) {
                SetPopupResult(p, job->latest);
                SetPopupStatus(p, fmt("%s · 翻译完成", LlmLanguageLabel(job->input.langTo)));
            } else {
                p->error = true;
                SetPopupStatus(p, StrL("翻译失败 · 可重试或修改设置"));
                SetPopupResult(p, job->error);
            }
        }
        ReleaseJob(job);
    }
    ReleaseJob(job);
}

static void RunPopupJob(PopupJob* job) {
    job->ok = RunLlmTranslate(job->input, &job->cancel, job, job->error);
    InterlockedIncrement(&job->refs);
    uitask::Post(MkFunc0(JobFinished, job), "TranslatePopupFinished");
    ReleaseJob(job);
}

static bool CanTranslate() {
    return HasPermission(Perm::CopySelection | Perm::InternetAccess | Perm::AIChat | Perm::DiskAccess);
}

static bool CanCopySource(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    return dm && (gDisableDocumentRestrictions || dm->GetEngine()->allowsCopyingText);
}

static void StartPopupJob(TranslatePopup* p) {
    if (!IsCurrentPopup(p) || !p->open) return;
    CancelPopupJob(p);
    p->request = ++gPopupRequest;
    p->error = false;
    if (!CanTranslate() || !CanCopySource(p->win)) {
        p->error = true;
        SetPopupStatus(p, StrL("划词翻译不可用"));
        SetPopupResult(p, StrL("当前阅读器权限不允许使用翻译服务。"));
        return;
    }
    if (str::IsEmptyOrWhiteSpace(gSettings->translatePopupUrl) ||
        str::IsEmptyOrWhiteSpace(gSettings->translatePopupModel)) {
        SetPopupStatus(p, StrL("请先设置翻译服务"));
        SetPopupResult(p, StrL("点击“设置”，填写 API 地址和模型，然后保存。\n\n支持 Chat Completions 和 Responses "
                               "兼容接口，译文显示在这里。"));
        return;
    }
    auto* job = new PopupJob();
    job->instance = p->instance;
    job->request = p->request;
    job->input.url = str::Dup(gSettings->translatePopupUrl);
    job->input.model = str::Dup(gSettings->translatePopupModel);
    job->input.apiKey = str::Dup(gSettings->translatePopupApiKey);
    job->input.text = str::Dup(p->source);
    job->input.langFrom = str::Dup(gSettings->translatePopupLangFrom);
    job->input.langTo = str::Dup(gSettings->translatePopupLangTo);
    job->input.prompt = str::Dup(gSettings->translatePopupPrompt);
    job->input.thinking = str::Dup(gSettings->translatePopupThinking);
    job->input.customParams = str::Dup(gSettings->translatePopupCustomParams);
    job->input.temperature = gSettings->translatePopupTemperature;
    job->input.stream = gSettings->translatePopupStream;
    p->job = job;
    p->busy = true;
    FitPopup(p, PopupFit::Reset);
    SetPopupStatus(p, fmt("正在翻译为%s…", LlmLanguageLabel(job->input.langTo)));
    SetPopupResult(p, {});
    ThreadHandle thread = StartThread(MkFunc0(RunPopupJob, job), StrL("LlmTranslate"));
    if (!thread) {
        ReleaseJob(job);
        CancelPopupJob(p);
        p->error = true;
        SetPopupStatus(p, StrL("无法启动翻译"));
        SetPopupResult(p, StrL("工作线程启动失败，请重试。"));
        return;
    }
    SafeCloseThreadHandle(&thread);
}

static void LayoutPopup(TranslatePopup* p) {
    RECT rc{};
    GetClientRect(p->hwnd, &rc);
    int margin = ScalePopup(p, kPopupMargin);
    int gap = ScalePopup(p, kPopupGap);
    int width = rc.right - 2 * margin;
    int header = ScalePopup(p, kPopupHeaderHeight);
    int meta = ScalePopup(p, kPopupStatusHeight);
    int buttonHeight = ScalePopup(p, kPopupButtonHeight);
    int marginY = margin;
    int gapY = gap;
    if (p->textFont) {
        // Tighten vertical whitespace before clipping a line in a small manual window.
        int minimumBody = PopupLineHeight(p) + 2 * ScalePopup(p, 1);
        int whitespace = std::max(0, (int)rc.bottom - header - meta - buttonHeight - minimumBody);
        marginY = std::min(marginY, whitespace / 2);
        gapY = std::min(gapY, std::max(0, whitespace / 2 - marginY));
    }
    int footer = rc.bottom - marginY - buttonHeight;
    int bodyY = marginY + header + meta + gapY;
    int fontButtonWidth = ScalePopup(p, 30);
    int autoWidth = ScalePopup(p, 72);
    int pointsWidth = ScalePopup(p, 34);
    int headerGap = ScalePopup(p, 5);
    int right = rc.right - margin;
    MoveWindow(p->autoSizeButton, right - autoWidth, marginY, autoWidth, header, TRUE);
    right -= autoWidth + headerGap;
    MoveWindow(p->boldButton, right - fontButtonWidth, marginY, fontButtonWidth, header, TRUE);
    right -= fontButtonWidth + headerGap;
    MoveWindow(p->fontPlus, right - fontButtonWidth, marginY, fontButtonWidth, header, TRUE);
    right -= fontButtonWidth + headerGap;
    MoveWindow(p->fontMinus, right - fontButtonWidth, marginY, fontButtonWidth, header, TRUE);
    right -= fontButtonWidth + headerGap;
    MoveWindow(p->fontSizeText, right - pointsWidth, marginY, pointsWidth, header, TRUE);
    right -= pointsWidth + headerGap;
    MoveWindow(p->title, margin, marginY, std::max(1, right - margin), header, TRUE);
    MoveWindow(p->status, margin, marginY + header, width, meta, TRUE);
    p->card = Rect(margin, bodyY, std::max(1, width), std::max(1, footer - gapY - bodyY));
    int paddingX = std::min(ScalePopup(p, kPopupCardPaddingX), (p->card.dx - 1) / 2);
    int paddingY = std::min(ScalePopup(p, kPopupCardPaddingY), (p->card.dy - 1) / 2);
    if (p->textFont) {
        int spare = std::max(0, p->card.dy - PopupLineHeight(p));
        paddingY = std::min(paddingY, spare / 2);
    }
    MoveWindow(p->resultEdit, p->card.x + paddingX, p->card.y + paddingY, p->card.dx - 2 * paddingX,
               p->card.dy - 2 * paddingY, TRUE);
    int buttonWidth = (width - 4 * gap) / 5;
    for (int i = 0; i < 5; i++) {
        MoveWindow(p->buttons[i], margin + i * (buttonWidth + gap), footer, buttonWidth, buttonHeight, TRUE);
    }
    InvalidateRect(p->hwnd, nullptr, FALSE);
}

static void PaintPopup(TranslatePopup* p, HDC dc) {
    RECT client{};
    GetClientRect(p->hwnd, &client);
    FillRect(dc, &client, p->brush);
    if (p->card.dx <= 0 || p->card.dy <= 0) return;
    int saved = SaveDC(dc);
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, kPopupCardColor);
    SetDCPenColor(dc, kPopupBorderColor);
    int diameter = ScalePopup(p, kPopupCardRadius * 2);
    RoundRect(dc, p->card.x, p->card.y, p->card.x + p->card.dx, p->card.y + p->card.dy, diameter, diameter);
    RestoreDC(dc, saved);
}

static void UpdatePopupTheme(TranslatePopup* p) {
    int dpi = DpiGetForHwnd(p->hwnd);
    if (!p->font || p->dpi != dpi) {
        HFONT old = p->font;
        HFONT oldTitle = p->titleFont;
        p->dpi = dpi;
        p->font = NewPopupFont(dpi, 10, FW_NORMAL);
        p->titleFont = NewPopupFont(dpi, 11, FW_SEMIBOLD);
        HwndSetFontForWindowAndItsChildren(p->hwnd, p->font);
        HwndSetFont(p->title, p->titleFont);
        HwndSetFont(p->boldButton, p->titleFont);
        DeleteObject(old);
        DeleteObject(oldTitle);
        LayoutPopup(p);
    }
    if (!p->brush) {
        p->brush = CreateSolidBrush(kPopupShellColor);
        p->bodyBrush = CreateSolidBrush(kPopupCardColor);
        RedrawWindow(p->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    UpdateAppearance(p);
}

static Rect WorkAreaFor(Rect anchor) {
    RECT box = ToRECT(anchor);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromRect(&box, MONITOR_DEFAULTTONEAREST), &info)) return ToRect(info.rcWork);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &box, 0);
    return ToRect(box);
}

static bool HasPopupSelection(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    return win && win->showSelection && tab && tab->selectionOnPage && len(*tab->selectionOnPage) > 0;
}

static Rect ReaderRect(MainWindow* win) {
    if (!IsWindowVisible(win->hwndCanvas) || IsIconic(win->hwndFrame)) return {};
    Rect canvas = HwndMapRectToWindow(HwndClientRect(win->hwndCanvas), win->hwndCanvas, nullptr);
    if (win->sidebarOverlayShown) {
        Rect sidebar = HwndMapRectToWindow(win->sidebarOverlayRc, win->hwndFrame, nullptr).Intersect(canvas);
        if (!sidebar.IsEmpty()) {
            if (sidebar.x == canvas.x) {
                canvas.x += sidebar.dx;
            }
            canvas.dx -= sidebar.dx;
        }
    }
    HWND overlays[] = {win->hwndCaptionOverlay, win->isToolbarOverlay ? win->hwndToolbar : nullptr};
    for (HWND hwnd : overlays) {
        if (!hwnd || !IsWindowVisible(hwnd)) continue;
        Rect chrome = HwndWindowRect(hwnd).Intersect(canvas);
        if (chrome.IsEmpty()) continue;
        if (chrome.y - canvas.y < canvas.Bottom() - chrome.Bottom()) {
            int bottom = canvas.Bottom();
            canvas.y = chrome.Bottom();
            canvas.dy = bottom - canvas.y;
        } else {
            canvas.dy = chrome.y - canvas.y;
        }
    }
    return canvas;
}

static bool VisiblePopupAnchor(MainWindow* win, Rect& anchor) {
    if (!HasPopupSelection(win) || !GetSelectionScreenRect(win->CurrentTab(), anchor)) return false;
    anchor = anchor.Intersect(ReaderRect(win));
    if (anchor.IsEmpty()) return false;
    anchor = anchor.Intersect(WorkAreaFor(anchor));
    return !anchor.IsEmpty();
}

static Rect PopupWorkArea(TranslatePopup* p) {
    Rect canvas = ReaderRect(p->win);
    Rect basis = canvas.Intersect(p->anchorVisible ? p->anchor : HwndWindowRect(p->hwnd));
    return canvas.Intersect(WorkAreaFor(basis.IsEmpty() ? canvas : basis));
}

static void UpdatePopupArrow(TranslatePopup* p) {
    if (!p->anchorVisible || p->sizing || !IsWindowVisible(p->hwnd)) {
        HideTranslateArrow(p->arrow);
        return;
    }
    Rect toolbar = IsSelectionToolbarVisible(p->win) ? SelectionToolbarRect(p->win) : Rect();
    UpdateTranslateArrow(p->arrow, p->callout, PopupWorkArea(p), toolbar, p->dpi, kPopupShellColor, kPopupBorderColor);
}

static Rect ClampPopupRect(Rect popup, Rect area) {
    popup.dx = std::min(std::max(1, popup.dx), area.dx);
    popup.dy = std::min(std::max(1, popup.dy), area.dy);
    popup.x = std::clamp(popup.x, area.x, area.Right() - popup.dx);
    popup.y = std::clamp(popup.y, area.y, area.Bottom() - popup.dy);
    return popup;
}

static void PlacePopup(TranslatePopup* p) {
    if (!p->hwnd || !p->open) return;
    Rect area = PopupWorkArea(p);
    if (area.IsEmpty()) {
        if (IsWindowVisible(p->hwnd)) ShowWindow(p->hwnd, SW_HIDE);
        AvoidSelectionToolbarCover(p->win, {});
        return;
    }
    Rect current = HwndWindowRect(p->hwnd);
    Rect placed = ClampPopupRect(current, area);
    Rect toolbar = SelectionToolbarRect(p->win);
    if (!p->sizing) {
        placed.dx = p->fittedSize.dx > 0 ? p->fittedSize.dx : ScalePopup(p, p->preferredWidth);
        placed.dy = p->fittedSize.dy > 0 ? p->fittedSize.dy : ScalePopup(p, p->preferredHeight);
        placed = ClampPopupRect(placed, area);
    }
    if (p->anchorVisible && !p->sizing) {
        int minHeight = ScalePopup(p, kPopupMinHeight);
        if (p->autoSize && !toolbar.IsEmpty()) {
            int frame = std::max(0, current.dy - HwndClientRect(p->hwnd).dy);
            int chrome = frame + ScalePopup(p, kPopupHeaderHeight) + ScalePopup(p, kPopupStatusHeight) +
                         ScalePopup(p, kPopupButtonHeight);
            minHeight = std::min(minHeight, chrome + PopupLineHeight(p) + 2 * ScalePopup(p, 1));
        }
        p->callout = PlaceTranslateCallout(p->anchor, placed.Size(), area, p->position, p->dpi, toolbar, minHeight);
        placed = p->callout.popup;
    }
    bool coversToolbar = !placed.Intersect(toolbar).IsEmpty();
    if (coversToolbar) AvoidSelectionToolbarCover(p->win, placed);
    if (current != placed) {
        SetWindowPos(p->hwnd, nullptr, placed.x, placed.y, placed.dx, placed.dy, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    if (!IsWindowVisible(p->hwnd)) ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    if (!coversToolbar) AvoidSelectionToolbarCover(p->win, {});
    UpdatePopupArrow(p);
}

static TEXTMETRICW PopupTextMetrics(TranslatePopup* p) {
    HDC dc = GetDC(p->resultEdit);
    HGDIOBJ old = SelectObject(dc, p->textFont);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(p->resultEdit, dc);
    return metrics;
}

static int PopupLineHeight(TranslatePopup* p) {
    return std::max(1, (int)PopupTextMetrics(p).tmHeight);
}

static void CancelPopupFit(TranslatePopup* p) {
    if (p->hwnd) KillTimer(p->hwnd, kPopupFitTimer);
    p->fitPending = false;
}

static void FitPopup(TranslatePopup* p, PopupFit how) {
    if (!p->hwnd || !p->open || p->sizing || p->fitting) return;
    Rect work = PopupWorkArea(p);
    p->workArea = work;
    if (work.IsEmpty()) {
        PlacePopup(p);
        return;
    }
    p->fitting = true;
    Rect before = HwndWindowRect(p->hwnd);
    int width = std::min(ScalePopup(p, p->preferredWidth), work.dx);
    int height = std::min(ScalePopup(p, p->preferredHeight), work.dy);
    if (p->autoSize) {
        // Establish the wrap width before asking EDIT for its actual line count.
        if (width != before.dx) {
            p->fittedSize = {width, std::min(before.dy, work.dy)};
            PlacePopup(p);
        }
        Rect current = HwndWindowRect(p->hwnd);
        Rect body = HwndClientRect(p->resultEdit);
        int chrome = current.dy - body.dy;
        int lines = (int)SendMessageW(p->resultEdit, EM_GETLINECOUNT, 0, 0);
        i64 needed = (i64)std::max(1, lines) * PopupLineHeight(p) + chrome + ScalePopup(p, 8);
        int minimum = std::min(ScalePopup(p, kPopupMinHeight), work.dy);
        int maximum = std::min(work.dy, std::max(minimum, MulDiv(work.dy, p->maxHeight, 100)));
        height = how == PopupFit::Reset ? minimum : (int)std::min<i64>(maximum, std::max<i64>(minimum, needed));
        if (how == PopupFit::Grow) height = std::min(maximum, std::max(height, before.dy));
    }
    p->fittedSize = {width, height};
    PlacePopup(p);
    if (p->autoSize && HwndWindowRect(p->hwnd).Size() != before.Size()) p->resizeCount++;
    p->fitting = false;
}

static void QueuePopupFit(TranslatePopup* p) {
    if (!p->autoSize || p->sizing || !p->hwnd || !IsWindowVisible(p->hwnd)) return;
    if (!p->busy) {
        CancelPopupFit(p);
        FitPopup(p, PopupFit::Exact);
        return;
    }
    if (!p->fitPending) {
        p->fitPending = SetTimer(p->hwnd, kPopupFitTimer, kPopupFitInterval, nullptr) != 0;
    }
}

static void UpdateAppearance(TranslatePopup* p) {
    Str name = PopupFontName(gSettings->translatePopupFont);
    int points = PopupClamp(gSettings->translatePopupFontSize, kPopupMinFontSize, kPopupMaxFontSize);
    int width = PopupClamp(gSettings->translatePopupWidth, kPopupMinWidth, kPopupMaxWidth);
    int height = PopupClamp(gSettings->translatePopupHeight, kPopupMinHeight, kPopupMaxHeight);
    int maximum = PopupClamp(gSettings->translatePopupMaxHeight, 30, 90);
    TranslatePosition position = ParseTranslatePosition(gSettings->translatePopupPosition);
    bool bold = gSettings->translatePopupBold;
    bool fontChanged =
        !p->textFont || p->fontSize != points || p->fontDpi != p->dpi || !str::Eq(name, p->fontFace) || p->bold != bold;
    bool sizeChanged = p->autoSize != gSettings->translatePopupAutoSize || p->preferredWidth != width ||
                       p->preferredHeight != height || p->maxHeight != maximum;
    bool positionChanged = p->position != position;
    p->autoSize = gSettings->translatePopupAutoSize;
    p->preferredWidth = width;
    p->preferredHeight = height;
    p->maxHeight = maximum;
    p->position = position;
    if (fontChanged) {
        int firstLine = (int)SendMessageW(p->resultEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
        DWORD start = 0, end = 0;
        SendMessageW(p->resultEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        HFONT old = p->textFont;
        p->textFont = NewPopupFont(p->dpi, points, bold ? FW_BOLD : FW_NORMAL, name);
        p->fontSize = points;
        p->fontDpi = p->dpi;
        p->bold = bold;
        str::ReplaceWithCopy(&p->fontFace, name);
        HwndSetFont(p->resultEdit, p->textFont);
        DeleteObject(old);
        LayoutPopup(p);
        SendMessageW(p->resultEdit, EM_SETSEL, start, end);
        int now = (int)SendMessageW(p->resultEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(p->resultEdit, EM_LINESCROLL, 0, firstLine - now);
    }
    HwndSetText(p->fontSizeText, fmt("%dpt", points));
    EnableWindow(p->fontMinus, points > kPopupMinFontSize);
    EnableWindow(p->fontPlus, points < kPopupMaxFontSize);
    SendMessageW(p->autoSizeButton, BM_SETCHECK, p->autoSize ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(p->boldButton, BM_SETCHECK, p->bold ? BST_CHECKED : BST_UNCHECKED, 0);
    if (fontChanged || sizeChanged) {
        CancelPopupFit(p);
        FitPopup(p, PopupFit::Exact);
    }
    if (positionChanged) PlacePopup(p);
}

static void RefreshAppearances() {
    for (TranslatePopup* popup : gPopups) {
        if (popup->hwnd) UpdatePopupTheme(popup);
    }
}

static void SetPopupFontSize(TranslatePopup* p, int value) {
    int points = PopupClamp(value, kPopupMinFontSize, kPopupMaxFontSize);
    if (!p || points == p->fontSize) return;
    gSettings->translatePopupFontSize = points;
    ScheduleSaveSettings();
    RefreshAppearances();
}

static void SetPopupAutoSize(TranslatePopup* p, bool enabled) {
    if (!p) return;
    if (!enabled && p->autoSize) {
        Rect rect = HwndWindowRect(p->hwnd);
        gSettings->translatePopupWidth = PopupClamp(MulDiv(rect.dx, 96, p->dpi), kPopupMinWidth, kPopupMaxWidth);
        gSettings->translatePopupHeight = PopupClamp(MulDiv(rect.dy, 96, p->dpi), kPopupMinHeight, kPopupMaxHeight);
    }
    gSettings->translatePopupAutoSize = enabled;
    ScheduleSaveSettings();
    RefreshAppearances();
    if (enabled) FitPopup(p, PopupFit::Exact);
}

static void SetPopupBold(TranslatePopup* p, bool enabled) {
    if (!p || p->bold == enabled) return;
    gSettings->translatePopupBold = enabled;
    ScheduleSaveSettings();
    RefreshAppearances();
}

static void SavePopupManualSize(TranslatePopup* p) {
    Rect rect = HwndWindowRect(p->hwnd);
    if (rect.dx == p->sizeStart.dx && rect.dy == p->sizeStart.dy) {
        QueuePopupFit(p);
        PlacePopup(p);
        return;
    }
    CancelPopupFit(p);
    p->autoSize = false;
    p->preferredWidth = PopupClamp(MulDiv(rect.dx, 96, p->dpi), kPopupMinWidth, kPopupMaxWidth);
    p->preferredHeight = PopupClamp(MulDiv(rect.dy, 96, p->dpi), kPopupMinHeight, kPopupMaxHeight);
    p->fittedSize = rect.Size();
    gSettings->translatePopupAutoSize = false;
    gSettings->translatePopupWidth = p->preferredWidth;
    gSettings->translatePopupHeight = p->preferredHeight;
    SendMessageW(p->autoSizeButton, BM_SETCHECK, BST_UNCHECKED, 0);
    ScheduleSaveSettings();
    PlacePopup(p);
}

static void HidePopup(TranslatePopup* p, PopupHide how) {
    if (!p) return;
    p->anchorVisible = false;
    HideTranslateArrow(p->arrow);
    if (how == PopupHide::Ordinary && p->pinned) return;
    p->open = false;
    CancelPopupJob(p);
    ShowWindow(p->hwnd, SW_HIDE);
    AvoidSelectionToolbarCover(p->win, {});
}

static LRESULT CALLBACK EscapeChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    TranslatePopup* p = FindPopupById((u64)data);
    if (p && hwnd == p->resultEdit && msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
        int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (steps) SetPopupFontSize(p, p->fontSize + steps);
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        MSG key{};
        key.hwnd = hwnd;
        key.message = msg;
        key.wParam = wp;
        key.lParam = lp;
        if (TranslatePopupKey(key)) return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (p) HidePopup(p, PopupHide::Force);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, EscapeChildProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static HWND NewPopupControl(HWND parent, const WCHAR* cls, const WCHAR* text, DWORD style, int id = 0) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

static void CreatePopupControls(TranslatePopup* p) {
    p->title = NewPopupControl(p->hwnd, L"STATIC", L"划词翻译", SS_LEFT);
    p->fontSizeText = NewPopupControl(p->hwnd, L"STATIC", L"11pt", SS_CENTER);
    p->fontMinus = NewPopupControl(p->hwnd, L"BUTTON", L"A−", WS_TABSTOP | BS_PUSHBUTTON, ButtonFontMinus);
    p->fontPlus = NewPopupControl(p->hwnd, L"BUTTON", L"A+", WS_TABSTOP | BS_PUSHBUTTON, ButtonFontPlus);
    p->autoSizeButton =
        NewPopupControl(p->hwnd, L"BUTTON", L"自适应", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, ButtonAutoSize);
    p->boldButton = NewPopupControl(p->hwnd, L"BUTTON", L"B", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, ButtonBold);
    for (HWND control : {p->fontMinus, p->fontPlus, p->autoSizeButton, p->boldButton}) {
        SetWindowSubclass(control, EscapeChildProc, kEscapeSubclass, (DWORD_PTR)p->instance);
    }
    p->status = NewPopupControl(p->hwnd, L"STATIC", L"划词翻译", SS_LEFT);
    p->resultEdit = NewPopupControl(p->hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    SendMessageW(p->resultEdit, EM_SETLIMITTEXT, kBodyLimit, 0);
    const WCHAR* labels[] = {L"复制", L"重试", L"固定", L"设置", L"关闭"};
    for (int i = 0; i < 5; i++) {
        p->buttons[i] = NewPopupControl(p->hwnd, L"BUTTON", labels[i], WS_TABSTOP | BS_PUSHBUTTON, ButtonCopy + i);
        SetWindowSubclass(p->buttons[i], EscapeChildProc, kEscapeSubclass, (DWORD_PTR)p->instance);
    }
    SetWindowSubclass(p->resultEdit, EscapeChildProc, kEscapeSubclass, (DWORD_PTR)p->instance);
    UpdatePopupTheme(p);
    LayoutPopup(p);
}

static bool RunPopupAction(TranslatePopup* p, int action) {
    if (!p) return false;
    if (action == ButtonClose) {
        HidePopup(p, PopupHide::Force);
    } else if (action == ButtonCopy) {
        if (HasPermission(Perm::CopySelection) && CopyTextToClipboard(p->result)) {
            SetPopupStatus(p, StrL("译文已复制"));
        }
    } else if (action == ButtonRetry) {
        StartPopupJob(p);
    } else if (action == ButtonPin) {
        p->pinned = !p->pinned;
        HwndSetText(p->buttons[2], p->pinned ? StrL("已固定") : StrL("固定"));
    } else if (action == ButtonSettings) {
        OpenPopupConfig(p);
    } else if (action == ButtonFontMinus || action == ButtonFontPlus) {
        SetPopupFontSize(p, p->fontSize + (action == ButtonFontPlus ? 1 : -1));
    } else if (action == ButtonAutoSize) {
        SetPopupAutoSize(p, !p->autoSize);
    } else if (action == ButtonBold) {
        SetPopupBold(p, !p->bold);
    } else {
        return false;
    }
    return true;
}

static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = (TranslatePopup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        p = (TranslatePopup*)((CREATESTRUCTW*)lp)->lpCreateParams;
        p->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p) return DefWindowProcW(hwnd, msg, wp, lp);
    DpiScope scope(hwnd);
    switch (msg) {
        case WM_CREATE:
            CreatePopupControls(p);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SIZE:
            LayoutPopup(p);
            UpdatePopupArrow(p);
            return 0;
        case WM_MOVE:
            UpdatePopupArrow(p);
            return 0;
        case WM_SHOWWINDOW:
            if (!wp) HideTranslateArrow(p->arrow);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = (MINMAXINFO*)lp;
            Rect work = PopupWorkArea(p);
            limits->ptMinTrackSize = {std::min(ScalePopup(p, kPopupMinWidth), work.dx),
                                      std::min(ScalePopup(p, kPopupMinHeight), work.dy)};
            limits->ptMaxTrackSize = {work.dx, work.dy};
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            CancelPopupFit(p);
            p->sizing = true;
            HideTranslateArrow(p->arrow);
            p->sizeStart = HwndWindowRect(hwnd);
            return 0;
        case WM_SIZING: {
            Rect area = PopupWorkArea(p);
            if (area.IsEmpty()) return FALSE;
            RECT* proposed = (RECT*)lp;
            Rect placed = ToRect(*proposed).Intersect(area);
            if (placed.IsEmpty()) placed = ClampPopupRect(p->sizeStart, area);
            *proposed = ToRECT(placed);
            if (!placed.Intersect(SelectionToolbarRect(p->win)).IsEmpty()) AvoidSelectionToolbarCover(p->win, placed);
            return TRUE;
        }
        case WM_EXITSIZEMOVE:
            if (!p->sizing) return 0;
            p->sizing = false;
            SavePopupManualSize(p);
            return 0;
        case WM_TIMER:
            if (wp == kPopupFitTimer) {
                CancelPopupFit(p);
                FitPopup(p, PopupFit::Grow);
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            UpdatePopupTheme(p);
            FitPopup(p, PopupFit::Exact);
            PlacePopup(p);
            return 0;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
            UpdatePopupTheme(p);
            FitPopup(p, PopupFit::Exact);
            PlacePopup(p);
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN: {
            bool body = (HWND)lp == p->resultEdit;
            SetTextColor((HDC)wp, (HWND)lp == p->status ? kPopupSecondaryColor : kPopupTextColor);
            SetBkColor((HDC)wp, body ? kPopupCardColor : kPopupShellColor);
            return (LRESULT)(body ? p->bodyBrush : p->brush);
        }
        case WM_ERASEBKGND:
            PaintPopup(p, (HDC)wp);
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            PaintPopup(p, dc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED) RunPopupAction(p, LOWORD(wp));
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) HidePopup(p, PopupHide::Force);
            return 0;
        case WM_CLOSE:
            HidePopup(p, PopupHide::Force);
            return 0;
        case WM_DESTROY:
            p->open = false;
            CancelPopupJob(p);
            DeleteTranslateArrow(p->arrow);
            p->arrow = nullptr;
            p->anchorVisible = false;
            if (p->config) DestroyWindow(p->config);
            return 0;
        case WM_NCDESTROY:
            p->hwnd = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterPopupClass(const WCHAR* name, WNDPROC proc, UINT style) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.style = style;
    cls.lpfnWndProc = proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = name;
    RegisterClassExW(&cls);
}

static TranslatePopup* NewPopup(MainWindow* win) {
    RegisterPopupClass(kPopupClass, PopupWndProc, CS_DROPSHADOW);
    auto* p = new TranslatePopup();
    p->win = win;
    p->instance = ++gPopupInstance;
    p->dpi = DpiGetForHwnd(win->hwndFrame);
    VecAppend(gPopups, p);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPopupClass, L"划词翻译",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, 0, 0, ScalePopup(p, kPopupWidth),
                                ScalePopup(p, kPopupHeight), win->hwndFrame, nullptr, GetModuleHandleW(nullptr), p);
    if (!hwnd) {
        DeleteTranslatePopup(win);
        return nullptr;
    }
    p->arrow = CreateTranslateArrow(hwnd);
    return p;
}

static bool ConfigError(PopupConfig* c, Str error) {
    HwndSetText(c->note, error);
    RevealConfigControl(c, c->note);
    return false;
}

static bool ConfigTemperatureValue(PopupConfig* c, double& value) {
    Str text = HwndGetTextTemp(c->temperature);
    str::TrimWsBoth(text);
    char* end = nullptr;
    char* start = CStrTemp(text);
    value = strtod(start, &end);
    return len(text) > 0 && end == start + len(text) && value >= 0 && value <= 2;
}

static Str ConfigPromptValue(PopupConfig* c) {
    return str::ReplaceTemp(HwndGetTextTemp(c->prompt), StrL("\r\n"), StrL("\n"));
}

static Str ConfigLanguage(HWND combo, Str fallback) {
    int selected = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return fallback;
    int index = (int)SendMessageW(combo, CB_GETITEMDATA, selected, 0);
    int count = 0;
    const LlmLanguage* languages = LlmLanguages(count);
    return index >= 0 && index < count ? languages[index].code : fallback;
}

static Str ConfigThinkingValue(PopupConfig* c) {
    int index = (int)SendMessageW(c->thinking, CB_GETCURSEL, 0, 0);
    return index >= 0 && index < dimofi(kThinkingValues) ? kThinkingValues[index] : StrL("off");
}

static TranslatePosition ConfigPositionValue(PopupConfig* c) {
    int index = (int)SendMessageW(c->position, CB_GETCURSEL, 0, 0);
    return index >= 0 && index < dimofi(kPositionValues) ? kPositionValues[index] : TranslatePosition::Auto;
}

static bool ConfigIntValue(HWND child, int low, int high, int& value) {
    Str text = HwndGetTextTemp(child);
    str::TrimWsBoth(text);
    char* end = nullptr;
    char* start = CStrTemp(text);
    long parsed = strtol(start, &end, 10);
    if (len(text) == 0 || end != start + len(text) || parsed < low || parsed > high) return false;
    value = (int)parsed;
    return true;
}

static Str EffectivePopupPrompt(Str text) {
    if (str::IsEmptyOrWhiteSpace(text)) return LlmDefaultPrompt();
    return str::ReplaceTemp(text, StrL("\r\n"), StrL("\n"));
}

static bool ConfigRequestChanged(PopupConfig* c) {
    Str url = HwndGetTextTemp(c->url);
    Str model = HwndGetTextTemp(c->model);
    Str key = HwndGetTextTemp(c->key);
    str::TrimWsBoth(url);
    str::TrimWsBoth(model);
    str::TrimWsBoth(key);
    double temperature = 0;
    ConfigTemperatureValue(c, temperature);
    bool enabled = SendMessageW(c->enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool stream = SendMessageW(c->stream, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return !str::Eq(url, gSettings->translatePopupUrl) || !str::Eq(model, gSettings->translatePopupModel) ||
           !str::Eq(key, gSettings->translatePopupApiKey) ||
           (float)temperature != gSettings->translatePopupTemperature ||
           !str::Eq(ConfigPromptValue(c), EffectivePopupPrompt(gSettings->translatePopupPrompt)) ||
           !str::Eq(ConfigLanguage(c->langFrom, StrL("auto")), gSettings->translatePopupLangFrom) ||
           !str::Eq(ConfigLanguage(c->langTo, StrL("zh-CN")), gSettings->translatePopupLangTo) ||
           !str::Eq(ConfigThinkingValue(c), gSettings->translatePopupThinking) ||
           !str::Eq(c->customParams, gSettings->translatePopupCustomParams) ||
           enabled != gSettings->translatePopupEnabled || stream != gSettings->translatePopupStream;
}

static bool ConfigAppearanceChanged(PopupConfig* c) {
    int points = 0, width = 0, height = 0, maximum = 0;
    ConfigIntValue(c->fontSize, kPopupMinFontSize, kPopupMaxFontSize, points);
    ConfigIntValue(c->width, kPopupMinWidth, kPopupMaxWidth, width);
    ConfigIntValue(c->height, kPopupMinHeight, kPopupMaxHeight, height);
    ConfigIntValue(c->maxHeight, 30, 90, maximum);
    Str face = HwndGetTextTemp(c->fontFace);
    str::TrimWsBoth(face);
    return !str::Eq(face, PopupFontName(gSettings->translatePopupFont)) ||
           points != PopupClamp(gSettings->translatePopupFontSize, kPopupMinFontSize, kPopupMaxFontSize) ||
           width != PopupClamp(gSettings->translatePopupWidth, kPopupMinWidth, kPopupMaxWidth) ||
           height != PopupClamp(gSettings->translatePopupHeight, kPopupMinHeight, kPopupMaxHeight) ||
           maximum != PopupClamp(gSettings->translatePopupMaxHeight, 30, 90) ||
           (SendMessageW(c->autoSize, BM_GETCHECK, 0, 0) == BST_CHECKED) != gSettings->translatePopupAutoSize ||
           (SendMessageW(c->bold, BM_GETCHECK, 0, 0) == BST_CHECKED) != gSettings->translatePopupBold ||
           ConfigPositionValue(c) != ParseTranslatePosition(gSettings->translatePopupPosition);
}

static bool ConfigAppearanceValid(PopupConfig* c) {
    Str face = HwndGetTextTemp(c->fontFace);
    str::TrimWsBoth(face);
    if (len(face) == 0 || !str::Eq(face, PopupFontName(face))) {
        return ConfigError(c, StrL("请选择字体名称，最多 31 个字符，不能包含换行。"));
    }
    int value;
    if (!ConfigIntValue(c->fontSize, kPopupMinFontSize, kPopupMaxFontSize, value))
        return ConfigError(c, StrL("译文字号须为 8 到 32 之间的整数。"));
    if (!ConfigIntValue(c->width, kPopupMinWidth, kPopupMaxWidth, value))
        return ConfigError(c, StrL("阅读宽度须为 330 到 1600 之间的整数。"));
    if (!ConfigIntValue(c->height, kPopupMinHeight, kPopupMaxHeight, value))
        return ConfigError(c, StrL("手动高度须为 180 到 1200 之间的整数。"));
    if (!ConfigIntValue(c->maxHeight, 30, 90, value))
        return ConfigError(c, StrL("自动最大高度须为阅读区高度的 30% 到 90%。"));
    return true;
}

static void SaveConfigAppearance(PopupConfig* c) {
    Str face = HwndGetTextTemp(c->fontFace);
    str::TrimWsBoth(face);
    str::ReplaceWithCopy(&gSettings->translatePopupFont, face);
    ConfigIntValue(c->fontSize, kPopupMinFontSize, kPopupMaxFontSize, gSettings->translatePopupFontSize);
    ConfigIntValue(c->width, kPopupMinWidth, kPopupMaxWidth, gSettings->translatePopupWidth);
    ConfigIntValue(c->height, kPopupMinHeight, kPopupMaxHeight, gSettings->translatePopupHeight);
    ConfigIntValue(c->maxHeight, 30, 90, gSettings->translatePopupMaxHeight);
    gSettings->translatePopupAutoSize = SendMessageW(c->autoSize, BM_GETCHECK, 0, 0) == BST_CHECKED;
    gSettings->translatePopupBold = SendMessageW(c->bold, BM_GETCHECK, 0, 0) == BST_CHECKED;
    str::ReplaceWithCopy(&gSettings->translatePopupPosition, TranslatePositionName(ConfigPositionValue(c)));
}

static void ConfigSizeEnabled(PopupConfig* c) {
    bool automatic = SendMessageW(c->autoSize, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(c->height, !automatic);
    EnableWindow(c->maxHeight, automatic);
}

static bool ConfigCanSave(PopupConfig* c) {
    if (IsWindow(c->params)) {
        SetForegroundWindow(c->params);
        return false;
    }
    if (!ConfigAppearanceValid(c)) return false;
    Str url = HwndGetTextTemp(c->url);
    str::TrimWsBoth(url);
    TempStr model = HwndGetTextTemp(c->model);
    TempStr key = HwndGetTextTemp(c->key);
    bool enabled = SendMessageW(c->enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool appearanceOnly = !ConfigRequestChanged(c) && ConfigAppearanceChanged(c);
    if (enabled && !appearanceOnly && (str::IsEmptyOrWhiteSpace(url) || str::IsEmptyOrWhiteSpace(model))) {
        return ConfigError(c, StrL("启用自动翻译前，请填写完整 API 地址和模型。"));
    }
    if (len(url) && !str::StartsWithI(url, StrL("https://")) && !str::StartsWithI(url, StrL("http://"))) {
        return ConfigError(c, StrL("API 地址须以 https:// 或 http:// 开头。"));
    }
    if (str::Contains(key, StrL("\r")) || str::Contains(key, StrL("\n"))) {
        return ConfigError(c, StrL("密钥不能包含换行。"));
    }
    double temperature;
    if (!ConfigTemperatureValue(c, temperature)) {
        return ConfigError(c, StrL("温度须为 0 到 2 之间的数字。"));
    }
    Str error;
    if (!CheckLlmPrompt(ConfigPromptValue(c), error)) {
        ConfigError(c, error);
        str::Free(error);
        return false;
    }
    Vec<LlmJsonParam> params;
    bool valid = ReadLlmParams(c->customParams, params, error);
    FreeLlmParams(params);
    if (!valid) {
        ConfigError(c, error);
        str::Free(error);
        return false;
    }
    return true;
}

static void SavePopupConfig(PopupConfig* c) {
    if (!ConfigCanSave(c)) return;
    bool requestChanged = ConfigRequestChanged(c);
    Str urlView = HwndGetTextTemp(c->url);
    Str modelView = HwndGetTextTemp(c->model);
    Str keyView = HwndGetTextTemp(c->key);
    str::TrimWsBoth(urlView);
    str::TrimWsBoth(modelView);
    str::TrimWsBoth(keyView);
    Str url = str::Dup(urlView);
    Str model = str::Dup(modelView);
    Str key = str::Dup(keyView);
    str::ReplacePtr(&gSettings->translatePopupUrl, url);
    str::ReplacePtr(&gSettings->translatePopupModel, model);
    str::ReplacePtr(&gSettings->translatePopupApiKey, key);
    double temperature;
    ConfigTemperatureValue(c, temperature);
    gSettings->translatePopupTemperature = (float)temperature;
    str::ReplaceWithCopy(&gSettings->translatePopupPrompt, ConfigPromptValue(c));
    str::ReplaceWithCopy(&gSettings->translatePopupLangFrom, ConfigLanguage(c->langFrom, StrL("auto")));
    str::ReplaceWithCopy(&gSettings->translatePopupLangTo, ConfigLanguage(c->langTo, StrL("zh-CN")));
    str::ReplaceWithCopy(&gSettings->translatePopupThinking, ConfigThinkingValue(c));
    str::ReplaceWithCopy(&gSettings->translatePopupCustomParams, c->customParams);
    gSettings->translatePopupStream = SendMessageW(c->stream, BM_GETCHECK, 0, 0) == BST_CHECKED;
    gSettings->translatePopupEnabled = SendMessageW(c->enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SaveConfigAppearance(c);
    ScheduleSaveSettings();
    TranslatePopup* p = FindPopupById(c->instance);
    HWND hwnd = c->hwnd;
    DestroyWindow(hwnd);
    RefreshAppearances();
    if (!p) return;
    if (!gSettings->translatePopupEnabled) {
        HidePopup(p, PopupHide::Force);
        return;
    }
    if (!IsCurrentPopup(p) || !p->open) return;
    if (!requestChanged) {
        FitPopup(p, PopupFit::Exact);
        return;
    }

    if (!HasPopupSelection(p->win) || IsRectangularSelection(p->win)) return;
    bool textOnly = false;
    TempStr text = GetSelectedTextTemp(p->tab, StrL("\n"), textOnly);
    if (textOnly && str::Eq(p->source, text)) StartPopupJob(p);
}

static LRESULT CALLBACK ConfigChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* c = (PopupConfig*)GetWindowLongPtrW((HWND)data, GWLP_USERDATA);
    HWND control = hwnd;
    if (c && c->fontFace && GetParent(hwnd) == c->fontFace) control = c->fontFace;
    if (msg == WM_SETFOCUS && c && GetParent(control) == c->content) RevealConfigControl(c, control);
    if (msg == WM_MOUSEWHEEL && !(c && (control == c->prompt || control == c->langFrom || control == c->langTo ||
                                        control == c->thinking || control == c->fontFace || control == c->position)))
        return SendMessageW((HWND)data, msg, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow((HWND)data);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        HWND next = GetNextDlgTabItem((HWND)data, control, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (next) SetFocus(next);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (c && (control == c->prompt || control == c->langFrom || control == c->langTo || control == c->thinking ||
                  control == c->fontFace || control == c->position)) {
            return DefSubclassProc(hwnd, msg, wp, lp);
        }
        int button = GetDlgCtrlID(hwnd);
        if (button == ButtonSave || button == ButtonCancel || button == ConfigCustom ||
            (c && (hwnd == c->enabled || hwnd == c->stream || hwnd == c->autoSize || hwnd == c->bold)))
            SendMessageW(hwnd, BM_CLICK, 0, 0);
        else
            SendMessageW((HWND)data, WM_COMMAND, MAKEWPARAM(ButtonSave, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ConfigChildProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ConfigContentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    switch (msg) {
        case WM_ERASEBKGND: {
            auto* c = (PopupConfig*)GetWindowLongPtrW((HWND)data, GWLP_USERDATA);
            if (!c || !c->brush) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, c->brush);
            return 1;
        }
        case WM_COMMAND:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_VSCROLL:
        case WM_MOUSEWHEEL:
            return SendMessageW((HWND)data, msg, wp, lp);
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ConfigContentProc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static HWND ConfigControl(PopupConfig* c, const WCHAR* cls, const WCHAR* text, DWORD style, int x, int y, int width,
                          int height, int id = 0) {
    HWND parent = id == ButtonSave || id == ButtonCancel ? c->hwnd : c->content;
    HWND child = NewPopupControl(parent, cls, text, style, id);
    int dpi = DpiGetForHwnd(c->hwnd);
    MoveWindow(child, DpiScaleByDpi(dpi, x), DpiScaleByDpi(dpi, y), DpiScaleByDpi(dpi, width),
               DpiScaleByDpi(dpi, height), FALSE);
    HwndSetFont(child, c->font);
    SetWindowSubclass(child, ConfigChildProc, kEscapeSubclass, (DWORD_PTR)c->hwnd);
    if (id == ConfigFont) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(child, &info) && info.hwndItem) {
            SetWindowSubclass(info.hwndItem, ConfigChildProc, kEscapeSubclass, (DWORD_PTR)c->hwnd);
        }
    }
    VecAppend(c->items, ConfigItem{child, Rect(x, y, width, height), id});
    return child;
}

static void LayoutConfig(PopupConfig* c) {
    RECT client{};
    GetClientRect(c->hwnd, &client);
    int dpi = DpiGetForHwnd(c->hwnd);
    int margin = DpiScaleByDpi(dpi, 18);
    int buttonHeight = DpiScaleByDpi(dpi, 29);
    int footerY = std::max(0, (int)client.bottom - margin - buttonHeight);
    int viewHeight = std::max(1, footerY - DpiScaleByDpi(dpi, 12));
    int contentHeight = DpiScaleByDpi(dpi, kConfigContentHeight);
    MoveWindow(c->content, 0, 0, client.right, viewHeight, TRUE);
    ShowScrollBar(c->content, SB_VERT, contentHeight > viewHeight);
    RECT content{};
    GetClientRect(c->content, &content);
    c->scroll = std::max(0, std::min(c->scroll, contentHeight - viewHeight));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = contentHeight - 1;
    info.nPage = (UINT)viewHeight;
    info.nPos = c->scroll;
    SetScrollInfo(c->content, SB_VERT, &info, TRUE);
    for (ConfigItem& item : c->items) {
        Rect r = item.rect;
        int x = DpiScaleByDpi(dpi, r.x);
        int width = DpiScaleByDpi(dpi, r.dx);
        int y = DpiScaleByDpi(dpi, r.y) - c->scroll;
        bool footer = item.id == ButtonSave || item.id == ButtonCancel;
        if (footer)
            y = footerY;
        else {
            int available = std::max(1, (int)content.right - 2 * margin);
            if (r.dx >= 500) width = available;
            if (r.y == 218 || r.y == 244) {
                int gap = DpiScaleByDpi(dpi, 12);
                width = std::max(1, (available - gap) / 2);
                x = margin + (r.x > 18 ? width + gap : 0);
            }
            width = std::max(1, std::min(width, (int)content.right - margin - x));
        }
        if (item.id == ButtonSave) x = client.right - margin - width;
        if (item.id == ButtonCancel) x = client.right - margin - 2 * width - DpiScaleByDpi(dpi, 12);
        MoveWindow(item.hwnd, x, y, width, DpiScaleByDpi(dpi, r.dy), TRUE);
    }
}

static void RevealConfigControl(PopupConfig* c, HWND child) {
    RECT view{};
    GetClientRect(c->content, &view);
    Rect r = HwndWindowRect(child);
    Point origin = HwndClientToScreen(c->content, Point());
    int top = r.y - origin.y;
    int bottom = top + r.dy;
    int gap = DpiScaleByDpi(DpiGetForHwnd(c->hwnd), 6);
    int delta = top < 0 ? top - gap : bottom > view.bottom ? bottom - view.bottom + gap : 0;
    if (delta == 0) return;
    c->scroll += delta;
    LayoutConfig(c);
}

static void ScrollConfig(PopupConfig* c, UINT command) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(c->content, SB_VERT, &info);
    int step = DpiScaleByDpi(DpiGetForHwnd(c->hwnd), kConfigScrollLine);
    switch (command) {
        case SB_LINEUP:
            c->scroll -= step;
            break;
        case SB_LINEDOWN:
            c->scroll += step;
            break;
        case SB_PAGEUP:
            c->scroll -= std::max(1, (int)info.nPage - step);
            break;
        case SB_PAGEDOWN:
            c->scroll += std::max(1, (int)info.nPage - step);
            break;
        case SB_TOP:
            c->scroll = 0;
            break;
        case SB_BOTTOM:
            c->scroll = info.nMax;
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            c->scroll = info.nTrackPos;
            break;
        default:
            return;
    }
    LayoutConfig(c);
}

static void UpdateConfigTheme(PopupConfig* c) {
    int dpi = DpiGetForHwnd(c->hwnd);
    if (!c->font || c->dpi != dpi) {
        HFONT old = c->font;
        c->dpi = dpi;
        c->font = NewPopupFont(dpi, 10, FW_NORMAL);
        HwndSetFontForWindowAndItsChildren(c->hwnd, c->font);
        DeleteObject(old);
    }
    Color color = ThemeWindowBackgroundColor();
    if (!c->brush || c->background != color) {
        DeleteObject(c->brush);
        c->brush = CreateSolidBrush(color);
        c->background = color;
        DarkModeApplyToWindow(c->hwnd);
        RedrawWindow(c->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    LayoutConfig(c);
}

static void FillLanguageCombo(HWND combo, Str selected, Str fallback, int first) {
    int count = 0;
    const LlmLanguage* languages = LlmLanguages(count);
    int selectedItem = -1;
    int fallbackItem = 0;
    for (int i = first; i < count; i++) {
        int item = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)ToWStrTemp(languages[i].label).s);
        SendMessageW(combo, CB_SETITEMDATA, item, i);
        if (str::EqI(languages[i].code, selected)) selectedItem = item;
        if (str::Eq(languages[i].code, fallback)) fallbackItem = item;
    }
    SendMessageW(combo, CB_SETCURSEL, selectedItem >= 0 ? selectedItem : fallbackItem, 0);
}

static void ConfigParamsSaved(HWND owner, Str object) {
    auto* c = (PopupConfig*)GetWindowLongPtrW(owner, GWLP_USERDATA);
    if (!c) return;
    str::ReplaceWithCopy(&c->customParams, object);
    HwndSetText(c->note, StrL("自定义参数已暂存。点击本窗口的“保存”后生效。"));
}

static void ConfigParamsClosed(HWND owner) {
    auto* c = (PopupConfig*)GetWindowLongPtrW(owner, GWLP_USERDATA);
    if (c) c->params = nullptr;
}

static int CALLBACK PopupFontFamilyCb(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM data) {
    auto* names = (StrVec*)data;
    if (!lf->lfFaceName[0] || lf->lfFaceName[0] == L'@') return 1;
    Str name = ToUtf8Temp(lf->lfFaceName);
    if (!names->Contains(name)) names->Append(name);
    return 1;
}

static void FillPopupFontCombo(HWND combo) {
    StrVec names;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, PopupFontFamilyCb, (LPARAM)&names, 0);
    ReleaseDC(nullptr, dc);
    SortNoCase(&names);
    for (Str name : names) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)ToWStrTemp(name).s);
    HwndSetText(combo, PopupFontName(gSettings->translatePopupFont));
    SendMessageW(combo, CB_LIMITTEXT, LF_FACESIZE - 1, 0);
}

static void CreateConfigControls(PopupConfig* c) {
    c->dpi = DpiGetForHwnd(c->hwnd);
    c->font = NewPopupFont(c->dpi, 10, FW_NORMAL);
    c->brush = CreateSolidBrush(ThemeWindowBackgroundColor());
    c->background = ThemeWindowBackgroundColor();
    c->content = NewPopupControl(c->hwnd, L"STATIC", L"", WS_CLIPCHILDREN | WS_VSCROLL);
    SetWindowLongPtrW(c->content, GWL_EXSTYLE, GetWindowLongPtrW(c->content, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
    SetWindowSubclass(c->content, ConfigContentProc, kEscapeSubclass, (DWORD_PTR)c->hwnd);
    ConfigControl(c, L"STATIC", L"API 地址（完整 Chat Completions 或 Responses 端点）", SS_LEFT, 18, 17, 570, 24);
    c->url = ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 44, 570, 28, ConfigUrl);
    ConfigControl(c, L"STATIC", L"模型", SS_LEFT, 18, 84, 120, 24);
    c->model = ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 111, 570, 28, ConfigModel);
    ConfigControl(c, L"STATIC", L"API 密钥（本地服务可留空）", SS_LEFT, 18, 151, 570, 24);
    c->key = ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 18, 178, 570, 28,
                           ConfigKey);
    SendMessageW(c->key, EM_SETPASSWORDCHAR, 0x25cf, 0);
    ConfigControl(c, L"STATIC", L"源语言", SS_LEFT, 18, 218, 270, 24);
    ConfigControl(c, L"STATIC", L"目标语言", SS_LEFT, 306, 218, 270, 24);
    c->langFrom = ConfigControl(c, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 18, 244, 270, 260,
                                ConfigLangFrom);
    c->langTo = ConfigControl(c, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 306, 244, 270, 260,
                              ConfigLangTo);
    ConfigControl(c, L"STATIC", L"温度（0–2）", SS_LEFT, 18, 284, 570, 24);
    c->temperature =
        ConfigControl(c, L"EDIT", L"1", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 311, 570, 28, ConfigTemperature);
    ConfigControl(c, L"STATIC", L"提示词", SS_LEFT, 18, 351, 570, 24);
    c->prompt = ConfigControl(c, L"EDIT", L"",
                              WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 18,
                              378, 570, 140, ConfigPrompt);
    ConfigControl(c, L"STATIC", L"可用变量：${langFrom}、${langTo}、${sourceText}。必须包含 ${sourceText}。", SS_LEFT,
                  18, 526, 570, 43);
    c->stream = ConfigControl(c, L"BUTTON", L"Stream（流式显示）", BS_AUTOCHECKBOX | WS_TABSTOP, 18, 576, 570, 26,
                              ConfigStream);
    ConfigControl(c, L"STATIC", L"Thinking（思考）", SS_LEFT, 18, 611, 570, 24);
    c->thinking = ConfigControl(c, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 18, 638, 570, 220,
                                ConfigThinking);
    ConfigControl(c, L"STATIC", L"按 API 地址识别思考参数。未识别的中转服务请在“自定义请求参数”中设置。", SS_LEFT, 18,
                  678, 570, 43);
    ConfigControl(c, L"BUTTON", L"自定义请求参数…", BS_PUSHBUTTON | WS_TABSTOP, 18, 735, 190, 29, ConfigCustom);
    c->enabled =
        ConfigControl(c, L"BUTTON", L"划词后自动翻译", BS_AUTOCHECKBOX | WS_TABSTOP, 18, 777, 570, 26, ConfigEnabled);
    ConfigControl(c, L"STATIC", L"译文显示", SS_LEFT, 18, 817, 570, 24);
    ConfigControl(c, L"STATIC", L"译文字体（不改变界面字体）", SS_LEFT, 18, 851, 570, 24);
    c->fontFace = ConfigControl(c, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 18, 878,
                                570, 260, ConfigFont);
    ConfigControl(c, L"STATIC", L"译文字号（8–32 pt）", SS_LEFT, 18, 922, 570, 24);
    c->fontSize =
        ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 949, 570, 28, ConfigFontSize);
    c->bold = ConfigControl(c, L"BUTTON", L"译文加粗", BS_AUTOCHECKBOX | WS_TABSTOP, 18, 991, 570, 26, ConfigBold);
    c->autoSize = ConfigControl(c, L"BUTTON", L"按译文自适应高度", BS_AUTOCHECKBOX | WS_TABSTOP, 18, 1031, 570, 26,
                                ConfigAutoSize);
    ConfigControl(c, L"STATIC", L"阅读宽度 / 手动宽度（330–1600）", SS_LEFT, 18, 1071, 570, 24);
    c->width = ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 1098, 570, 28, ConfigWidth);
    ConfigControl(c, L"STATIC", L"手动高度（180–1200，仅手动模式）", SS_LEFT, 18, 1138, 570, 24);
    c->height =
        ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 1165, 570, 28, ConfigHeight);
    ConfigControl(c, L"STATIC", L"自动最大高度（占阅读区高度的 30–90%）", SS_LEFT, 18, 1205, 570, 24);
    c->maxHeight =
        ConfigControl(c, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 18, 1232, 570, 28, ConfigMaxHeight);
    ConfigControl(c, L"STATIC", L"浮窗位置（空间不足自动换边）", SS_LEFT, 18, 1272, 570, 24);
    c->position = ConfigControl(c, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 18, 1299, 570, 220,
                                ConfigPosition);
    c->note = ConfigControl(
        c, L"STATIC", L"宽高以100%缩放为基准，随系统缩放变化。拖动边框后锁定尺寸，点击“自适应”恢复。密钥保存在本机。",
        SS_LEFT, 18, 1340, 570, 65);
    ConfigControl(c, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 416, 880, 80, 29, ButtonCancel);
    ConfigControl(c, L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 508, 880, 80, 29, ButtonSave);
    HwndSetText(c->url, gSettings->translatePopupUrl);
    HwndSetText(c->model, gSettings->translatePopupModel);
    HwndSetText(c->key, gSettings->translatePopupApiKey);
    HwndSetText(c->temperature, fmt("%g", (double)gSettings->translatePopupTemperature));
    FillPopupFontCombo(c->fontFace);
    HwndSetText(c->fontSize,
                fmt("%d", PopupClamp(gSettings->translatePopupFontSize, kPopupMinFontSize, kPopupMaxFontSize)));
    HwndSetText(c->width, fmt("%d", PopupClamp(gSettings->translatePopupWidth, kPopupMinWidth, kPopupMaxWidth)));
    HwndSetText(c->height, fmt("%d", PopupClamp(gSettings->translatePopupHeight, kPopupMinHeight, kPopupMaxHeight)));
    HwndSetText(c->maxHeight, fmt("%d", PopupClamp(gSettings->translatePopupMaxHeight, 30, 90)));
    SendMessageW(c->autoSize, BM_SETCHECK, gSettings->translatePopupAutoSize ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(c->bold, BM_SETCHECK, gSettings->translatePopupBold ? BST_CHECKED : BST_UNCHECKED, 0);
    int positionItem = 0;
    TranslatePosition position = ParseTranslatePosition(gSettings->translatePopupPosition);
    for (int i = 0; i < dimofi(kPositionValues); i++) {
        SendMessageW(c->position, CB_ADDSTRING, 0, (LPARAM)kPositionLabels[i]);
        if (kPositionValues[i] == position) positionItem = i;
    }
    SendMessageW(c->position, CB_SETCURSEL, positionItem, 0);
    ConfigSizeEnabled(c);
    for (HWND control : {c->fontSize, c->width, c->height, c->maxHeight}) SendMessageW(control, EM_SETLIMITTEXT, 8, 0);
    Str prompt = str::IsEmptyOrWhiteSpace(gSettings->translatePopupPrompt) ? LlmDefaultPrompt()
                                                                           : gSettings->translatePopupPrompt;
    HwndSetText(c->prompt,
                str::ReplaceTemp(str::ReplaceTemp(prompt, StrL("\r\n"), StrL("\n")), StrL("\n"), StrL("\r\n")));
    FillLanguageCombo(c->langFrom, gSettings->translatePopupLangFrom, StrL("auto"), 0);
    FillLanguageCombo(c->langTo, gSettings->translatePopupLangTo, StrL("zh-CN"), 1);
    int thinkingItem = 1;
    for (int i = 0; i < dimofi(kThinkingValues); i++) {
        SendMessageW(c->thinking, CB_ADDSTRING, 0, (LPARAM)kThinkingLabels[i]);
        if (str::EqI(kThinkingValues[i], gSettings->translatePopupThinking)) thinkingItem = i;
    }
    SendMessageW(c->thinking, CB_SETCURSEL, thinkingItem, 0);
    SendMessageW(c->stream, BM_SETCHECK, gSettings->translatePopupStream ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(c->enabled, BM_SETCHECK, gSettings->translatePopupEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    c->customParams = str::Dup(gSettings->translatePopupCustomParams);
    SendMessageW(c->url, EM_SETLIMITTEXT, 8192, 0);
    SendMessageW(c->model, EM_SETLIMITTEXT, 1024, 0);
    SendMessageW(c->key, EM_SETLIMITTEXT, 8192, 0);
    SendMessageW(c->temperature, EM_SETLIMITTEXT, 32, 0);
    SendMessageW(c->prompt, EM_SETLIMITTEXT, 64 * 1024, 0);
    DarkModeApplyToWindow(c->hwnd);
    LayoutConfig(c);
}

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = (PopupConfig*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* p = (TranslatePopup*)((CREATESTRUCTW*)lp)->lpCreateParams;
        c = new PopupConfig();
        c->instance = p->instance;
        c->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)c);
    }
    if (!c) return DefWindowProcW(hwnd, msg, wp, lp);
    DpiScope scope(hwnd);
    switch (msg) {
        case WM_CREATE:
            CreateConfigControls(c);
            return 0;
        case WM_SIZE:
            LayoutConfig(c);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = (MINMAXINFO*)lp;
            Rect work = WorkAreaFor(HwndWindowRect(hwnd));
            limits->ptMinTrackSize = {std::min(DpiScaleByDpi(c->dpi, 400), work.dx),
                                      std::min(DpiScaleByDpi(c->dpi, 260), work.dy)};
            return 0;
        }
        case WM_VSCROLL:
            ScrollConfig(c, LOWORD(wp));
            return 0;
        case WM_MOUSEWHEEL: {
            int step = DpiScaleByDpi(DpiGetForHwnd(hwnd), kConfigScrollLine);
            c->scroll -= GET_WHEEL_DELTA_WPARAM(wp) * step / WHEEL_DELTA;
            LayoutConfig(c);
            return 0;
        }
        case WM_DPICHANGED: {
            Rect proposed = ToRect(*(RECT*)lp);
            Rect work = WorkAreaFor(proposed);
            proposed.dx = std::min(proposed.dx, work.dx);
            proposed.dy = std::min(proposed.dy, work.dy);
            proposed.x = std::max(work.x, std::min(proposed.x, work.x + work.dx - proposed.dx));
            proposed.y = std::max(work.y, std::min(proposed.y, work.y + work.dy - proposed.dy));
            SetWindowPos(hwnd, nullptr, proposed.x, proposed.y, proposed.dx, proposed.dy, SWP_NOZORDER);
            UpdateConfigTheme(c);
            return 0;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            UpdateConfigTheme(c);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ButtonSave)
                SavePopupConfig(c);
            else if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ButtonCancel)
                DestroyWindow(hwnd);
            else if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ConfigAutoSize)
                ConfigSizeEnabled(c);
            else if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == ConfigCustom) {
                if (IsWindow(c->params))
                    SetForegroundWindow(c->params);
                else
                    c->params = OpenTranslateParams(hwnd, c->customParams, ConfigParamsSaved, ConfigParamsClosed);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, ThemeWindowTextColor());
            SetBkColor((HDC)wp, ThemeWindowBackgroundColor());
            return (LRESULT)c->brush;
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, c->brush);
            return 1;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (IsWindow(c->params)) DestroyWindow(c->params);
            return 0;
        case WM_NCDESTROY: {
            TranslatePopup* p = FindPopupById(c->instance);
            if (p && p->config == hwnd) p->config = nullptr;
            DeleteObject(c->font);
            DeleteObject(c->brush);
            str::Free(c->customParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete c;
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenPopupConfig(TranslatePopup* p) {
    if (p->config && IsWindow(p->config)) {
        SetForegroundWindow(p->config);
        return;
    }
    RegisterPopupClass(kConfigClass, ConfigWndProc, 0);
    Rect area = WorkAreaFor(HwndWindowRect(p->hwnd));
    int width = std::min(ScalePopup(p, 630), area.dx);
    int height = std::min(ScalePopup(p, 795), area.dy);
    int x = area.x + (area.dx - width) / 2;
    int y = area.y + (area.dy - height) / 2;
    p->config = CreateWindowExW(WS_EX_TOOLWINDOW, kConfigClass, L"划词翻译设置",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, width,
                                height, p->win->hwndFrame, nullptr, GetModuleHandleW(nullptr), p);
    if (!p->config) return;
    ShowWindow(p->config, SW_SHOW);
    SetForegroundWindow(p->config);
    auto* c = (PopupConfig*)GetWindowLongPtrW(p->config, GWLP_USERDATA);
    if (c) SetFocus(c->url);
}

void ShowTranslatePopup(MainWindow* win) {
    if (!win || !gSettings->translatePopupEnabled || !CanTranslate() || !CanCopySource(win) || !win->showSelection ||
        IsRectangularSelection(win))
        return;
    WindowTab* tab = win->CurrentTab();
    bool textOnly = false;
    TempStr text = GetSelectedTextTemp(tab, StrL("\n"), textOnly);
    Rect anchor;
    if (!textOnly || str::IsEmptyOrWhiteSpace(text) || !VisiblePopupAnchor(win, anchor)) return;
    TranslatePopup* p = FindPopup(win);
    if (!p) p = NewPopup(win);
    if (!p) return;
    bool same = p->tab == tab && p->document == win->ctrl && str::Eq(p->source, text) && p->open;
    p->tab = tab;
    p->document = win->ctrl;
    p->anchor = anchor;
    p->anchorVisible = true;
    p->open = true;
    UpdatePopupTheme(p);
    if (same) {
        PlacePopup(p);
        return;
    }
    FitPopup(p, PopupFit::Exact);
    str::ReplaceWithCopy(&p->source, text);
    StartPopupJob(p);
}

void UpdateTranslatePopup(MainWindow* win) {
    TranslatePopup* p = FindPopup(win);
    if (!p || !p->open) return;
    if (!gSettings->translatePopupEnabled || !IsCurrentPopup(p) || !CanTranslate() || !CanCopySource(win)) {
        HidePopup(p, PopupHide::Force);
        return;
    }
    bool selecting = win->mouseAction == MouseAction::SelectingText || win->mouseAction == MouseAction::Selecting;
    if (selecting || !HasPopupSelection(win)) {
        HidePopup(p, PopupHide::Ordinary);
        if (!p->open) return;
    } else {
        Rect anchor;
        p->anchorVisible = VisiblePopupAnchor(win, anchor);
        if (p->anchorVisible) p->anchor = anchor;
    }
    UpdatePopupTheme(p);
    Rect work = PopupWorkArea(p);
    if (work.Size() != p->workArea.Size() || (!work.IsEmpty() && !IsWindowVisible(p->hwnd))) {
        FitPopup(p, PopupFit::Exact);
        return;
    }
    PlacePopup(p);
}

void HideTranslatePopup(MainWindow* win) {
    TranslatePopup* p = FindPopup(win);
    if (!p) return;
    bool selecting = win->mouseAction == MouseAction::SelectingText || win->mouseAction == MouseAction::Selecting;
    Rect anchor;
    if (!selecting && IsCurrentPopup(p) && gSettings->translatePopupEnabled && CanTranslate() && CanCopySource(win) &&
        HasPopupSelection(win) && !VisiblePopupAnchor(win, anchor)) {
        p->anchorVisible = false;
        PlacePopup(p);
        return;
    }
    HidePopup(p, PopupHide::Ordinary);
}

void DismissTranslatePopup(MainWindow* win) {
    HidePopup(FindPopup(win), PopupHide::Force);
}

void DeleteTranslatePopup(MainWindow* win) {
    TranslatePopup* p = FindPopup(win);
    if (!p) return;
    CancelPopupJob(p);
    DeleteTranslateArrow(p->arrow);
    p->arrow = nullptr;
    if (p->config) DestroyWindow(p->config);
    if (p->hwnd) DestroyWindow(p->hwnd);
    VecRemove(gPopups, p);
    DeleteObject(p->font);
    DeleteObject(p->titleFont);
    DeleteObject(p->textFont);
    str::Free(p->fontFace);
    DeleteObject(p->brush);
    DeleteObject(p->bodyBrush);
    str::Free(p->source);
    str::Free(p->result);
    str::Free(p->state);
    delete p;
}

bool IsTranslatePopupVisible(MainWindow* win) {
    TranslatePopup* p = FindPopup(win);
    return p && p->hwnd && IsWindowVisible(p->hwnd);
}

bool TranslatePopupKey(const MSG& msg) {
    if (msg.message != WM_KEYDOWN || (msg.wParam != '0' && msg.wParam != VK_NUMPAD0) ||
        !(GetKeyState(VK_CONTROL) & 0x8000)) {
        return false;
    }
    for (TranslatePopup* p : gPopups) {
        if (msg.hwnd != p->resultEdit || !IsWindowVisible(p->hwnd)) continue;
        DpiScope scope(p->hwnd);
        SetPopupFontSize(p, kPopupDefaultFontSize);
        return true;
    }
    return false;
}

bool TranslatePopupAction(MainWindow* win, Str action) {
    TranslatePopup* p = FindPopup(win);
    if (str::EqI(action, StrL("copy"))) return RunPopupAction(p, ButtonCopy);
    if (str::EqI(action, StrL("retry"))) return RunPopupAction(p, ButtonRetry);
    if (str::EqI(action, StrL("pin"))) return RunPopupAction(p, ButtonPin);
    if (str::EqI(action, StrL("settings"))) return RunPopupAction(p, ButtonSettings);
    if (str::EqI(action, StrL("close"))) return RunPopupAction(p, ButtonClose);
    if (str::EqI(action, StrL("font-minus"))) return RunPopupAction(p, ButtonFontMinus);
    if (str::EqI(action, StrL("font-plus"))) return RunPopupAction(p, ButtonFontPlus);
    if (str::EqI(action, StrL("auto-size"))) return RunPopupAction(p, ButtonAutoSize);
    if (str::EqI(action, StrL("bold"))) return RunPopupAction(p, ButtonBold);
    return false;
}

TempStr TranslatePopupStateTemp(MainWindow* win) {
    TranslatePopup* p = FindPopup(win);
    if (!p) return StrL("visible=0\nbusy=0\npinned=0\nerror=0\nrequest=0\nstatus=\"\"\ntext=\"\"\n");
    RECT rect{};
    GetWindowRect(p->hwnd, &rect);
    // Each fmt call has a 32-instruction limit, including literal spans.
    str::Builder state(GetTempArena());
    state.Append(
        fmt("visible=%d\nbusy=%d\npinned=%d\nerror=%d\nrequest=%llu\nrect=%d,%d,%d,%d\nstatus=\"%s\"\ntext=\"%s\"\n",
            IsTranslatePopupVisible(win) ? 1 : 0, p->busy ? 1 : 0, p->pinned ? 1 : 0, p->error ? 1 : 0, p->request,
            rect.left, rect.top, RectDx(rect), RectDy(rect), json::EscapeStrTemp(p->state),
            json::EscapeStrTemp(p->result)));
    state.Append(
        fmt("fontSize=%d\nfontPixels=%d\nautoSize=%d\npreferredWidth=%d\npreferredHeight=%d\ndpi=%d\nresizeCount=%llu\n"
            "bold=%d\nfontWeight=%d\n",
            p->fontSize, PopupLineHeight(p), p->autoSize ? 1 : 0, p->preferredWidth, p->preferredHeight, p->dpi,
            p->resizeCount, p->bold ? 1 : 0, (int)PopupTextMetrics(p).tmWeight));
    Rect arrow = TranslateArrowRect(p->arrow);
    Point tip = TranslateArrowTip(p->arrow);
    state.Append(
        fmt("anchorVisible=%d\nanchor=%d,%d,%d,%d\nposition=%s\nside=%s\narrowVisible=%d\n"
            "arrowRect=%d,%d,%d,%d\narrowTip=%d,%d\n",
            p->anchorVisible ? 1 : 0, p->anchor.x, p->anchor.y, p->anchor.dx, p->anchor.dy,
            TranslatePositionName(p->position), TranslatePositionName(p->callout.side),
            TranslateArrowVisible(p->arrow) ? 1 : 0, arrow.x, arrow.y, arrow.dx, arrow.dy, tip.x, tip.y));
    return state.TakeStr();
}
