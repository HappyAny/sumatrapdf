/* License: GPL-3.0-only */

#include "base/Base.h"
#include "base/Win.h"
#include "gui/Dpi.h"
#include "Theme.h"
#include "DarkMode.h"
#include "LlmTranslate.h"
#include "LlmRequestOptions.h"
#include "TranslateParams.h"
#include "TranslatePopup.h"

constexpr WCHAR kParamsClass[] = L"SumatraTranslateParams";
constexpr UINT_PTR kParamsSubclass = 1;
constexpr int kParamsAdd = 3201;
constexpr int kParamsSave = 3202;
constexpr int kParamsCancel = 3203;
constexpr int kParamsRowBase = 3300;
constexpr int kParamsRowIds = 3;
constexpr int kParamsMaxRows = 64;
constexpr int kParamsRowHeight = 46;
constexpr int kParamsHeaderHeight = 110;

struct ParamRow {
    HWND name = nullptr;
    HWND value = nullptr;
    HWND remove = nullptr;
};

struct ParamsDialog {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND content = nullptr;
    HWND description = nullptr;
    HWND nameLabel = nullptr;
    HWND valueLabel = nullptr;
    HWND add = nullptr;
    HWND note = nullptr;
    HWND save = nullptr;
    HWND cancel = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    Color background = kColorUnset;
    int dpi = 96;
    int scroll = 0;
    Str initial;
    void (*onSave)(HWND, Str) = nullptr;
    void (*onClose)(HWND) = nullptr;
    Vec<ParamRow> rows;
};

struct ParamsCreate {
    ParamsDialog* dialog = nullptr;
    bool attached = false;
};

static void LayoutParams(ParamsDialog* p);
static void RevealParam(ParamsDialog* p, HWND child);

static Rect ParamsWorkArea(HWND hwnd) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info)) return ToRect(info.rcWork);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0);
    return ToRect(rect);
}

static LRESULT CALLBACK ParamChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    HWND root = (HWND)data;
    auto* p = (ParamsDialog*)GetWindowLongPtrW(root, GWLP_USERDATA);
    if (msg == WM_SETFOCUS && p && GetParent(hwnd) == p->content) RevealParam(p, hwnd);
    if (msg == WM_MOUSEWHEEL) return SendMessageW(root, msg, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        DestroyWindow(root);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        HWND next = GetNextDlgTabItem(root, hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (next) SetFocus(next);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        int control = GetDlgCtrlID(hwnd);
        bool rowButton = control >= kParamsRowBase && (control - kParamsRowBase) % kParamsRowIds == 2;
        if (control == kParamsAdd || control == kParamsSave || control == kParamsCancel || rowButton) {
            SendMessageW(hwnd, BM_CLICK, 0, 0);
        } else {
            SendMessageW(root, WM_COMMAND, MAKEWPARAM(kParamsSave, BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ParamChildProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ParamContentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    switch (msg) {
        case WM_ERASEBKGND: {
            auto* p = (ParamsDialog*)GetWindowLongPtrW((HWND)data, GWLP_USERDATA);
            if (!p || !p->brush) break;
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect((HDC)wp, &rect, p->brush);
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
            RemoveWindowSubclass(hwnd, ParamContentProc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static HWND ParamControl(ParamsDialog* p, HWND parent, const WCHAR* cls, const WCHAR* text, DWORD style, int id = 0) {
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                                 GetModuleHandleW(nullptr), nullptr);
    HwndSetFont(child, p->font);
    SetWindowSubclass(child, ParamChildProc, kParamsSubclass, (DWORD_PTR)p->hwnd);
    return child;
}

static void AddParamRow(ParamsDialog* p, Str name = {}, Str value = {}) {
    if (len(p->rows) >= kParamsMaxRows) return;
    int id = kParamsRowBase + len(p->rows) * kParamsRowIds;
    ParamRow row;
    row.name = ParamControl(p, p->content, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, id);
    row.value = ParamControl(p, p->content, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, id + 1);
    row.remove = ParamControl(p, p->content, L"BUTTON", L"删除", BS_PUSHBUTTON | WS_TABSTOP, id + 2);
    HwndSetText(row.name, name);
    HwndSetText(row.value, value);
    SendMessageW(row.name, EM_SETLIMITTEXT, 1024, 0);
    SendMessageW(row.value, EM_SETLIMITTEXT, 64 * 1024, 0);
    SendMessageW(row.name, EM_SETCUEBANNER, 0, (LPARAM)L"参数名称");
    SendMessageW(row.value, EM_SETCUEBANNER, 0, (LPARAM)L"JSON 值");
    VecAppend(p->rows, row);
    DarkModeApplyToChildControls(p->content);
    SetWindowPos(p->add, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    LayoutParams(p);
}

static void LayoutParams(ParamsDialog* p) {
    RECT client{};
    GetClientRect(p->hwnd, &client);
    int margin = DpiScaleByDpi(p->dpi, 18);
    int gap = DpiScaleByDpi(p->dpi, 12);
    int buttonHeight = DpiScaleByDpi(p->dpi, 29);
    int buttonWidth = DpiScaleByDpi(p->dpi, 80);
    int footer = std::max(0, (int)client.bottom - margin - buttonHeight);
    int noteHeight = DpiScaleByDpi(p->dpi, 50);
    int noteY = std::max(0, footer - gap - noteHeight);
    int header = DpiScaleByDpi(p->dpi, kParamsHeaderHeight);
    int viewHeight = std::max(1, noteY - gap - header);
    MoveWindow(p->content, 0, header, client.right, viewHeight, TRUE);
    int contentHeight = DpiScaleByDpi(p->dpi, 8 + len(p->rows) * kParamsRowHeight + 44);
    ShowScrollBar(p->content, SB_VERT, contentHeight > viewHeight);
    RECT content{};
    GetClientRect(p->content, &content);
    p->scroll = std::max(0, std::min(p->scroll, contentHeight - viewHeight));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = contentHeight - 1;
    info.nPage = (UINT)viewHeight;
    info.nPos = p->scroll;
    SetScrollInfo(p->content, SB_VERT, &info, TRUE);
    int removeWidth = DpiScaleByDpi(p->dpi, 58);
    int available = std::max(3, (int)content.right - 2 * margin - removeWidth - 2 * gap);
    int nameWidth = available * 2 / 5;
    int valueWidth = available - nameWidth;
    int valueX = margin + nameWidth + gap;
    MoveWindow(p->description, margin, DpiScaleByDpi(p->dpi, 14), std::max(1, (int)client.right - 2 * margin),
               DpiScaleByDpi(p->dpi, 64), TRUE);
    MoveWindow(p->nameLabel, margin, DpiScaleByDpi(p->dpi, 84), nameWidth, DpiScaleByDpi(p->dpi, 24), TRUE);
    MoveWindow(p->valueLabel, valueX, DpiScaleByDpi(p->dpi, 84), valueWidth, DpiScaleByDpi(p->dpi, 24), TRUE);
    DeferWinPosHelper positions;
    for (int i = 0; i < len(p->rows); i++) {
        const ParamRow& row = p->rows[i];
        int y = DpiScaleByDpi(p->dpi, 8 + i * kParamsRowHeight) - p->scroll;
        positions.MoveWindowNoCopyBits(row.name, Rect(margin, y, nameWidth, buttonHeight));
        positions.MoveWindowNoCopyBits(row.value, Rect(valueX, y, valueWidth, buttonHeight));
        positions.MoveWindowNoCopyBits(row.remove, Rect(valueX + valueWidth + gap, y, removeWidth, buttonHeight));
        int id = kParamsRowBase + i * kParamsRowIds;
        SetWindowLongPtrW(row.name, GWLP_ID, id);
        SetWindowLongPtrW(row.value, GWLP_ID, id + 1);
        SetWindowLongPtrW(row.remove, GWLP_ID, id + 2);
    }
    int addY = DpiScaleByDpi(p->dpi, 8 + len(p->rows) * kParamsRowHeight) - p->scroll;
    positions.MoveWindowNoCopyBits(p->add, Rect(margin, addY, DpiScaleByDpi(p->dpi, 150), buttonHeight));
    positions.End();
    EnableWindow(p->add, len(p->rows) < kParamsMaxRows);
    RepaintTranslateScrollPane(p->content);
    MoveWindow(p->note, margin, noteY, std::max(1, (int)client.right - 2 * margin), noteHeight, TRUE);
    MoveWindow(p->save, client.right - margin - buttonWidth, footer, buttonWidth, buttonHeight, TRUE);
    MoveWindow(p->cancel, client.right - margin - 2 * buttonWidth - gap, footer, buttonWidth, buttonHeight, TRUE);
}

static void RevealParam(ParamsDialog* p, HWND child) {
    RECT view{};
    GetClientRect(p->content, &view);
    Rect childRect = HwndWindowRect(child);
    Point origin = HwndClientToScreen(p->content, Point());
    int top = childRect.y - origin.y;
    int bottom = top + childRect.dy;
    int gap = DpiScaleByDpi(p->dpi, 6);
    int delta = top < 0 ? top - gap : bottom > view.bottom ? bottom - view.bottom + gap : 0;
    if (delta == 0) return;
    p->scroll += delta;
    LayoutParams(p);
}

static void ScrollParams(ParamsDialog* p, WPARAM request) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(p->content, SB_VERT, &info);
    int step = DpiScaleByDpi(p->dpi, 24);
    switch (LOWORD(request)) {
        case SB_LINEUP:
            p->scroll -= step;
            break;
        case SB_LINEDOWN:
            p->scroll += step;
            break;
        case SB_PAGEUP:
            p->scroll -= std::max(1, (int)info.nPage - step);
            break;
        case SB_PAGEDOWN:
            p->scroll += std::max(1, (int)info.nPage - step);
            break;
        case SB_TOP:
            p->scroll = 0;
            break;
        case SB_BOTTOM:
            p->scroll = info.nMax;
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            p->scroll = info.nMax <= UINT16_MAX ? HIWORD(request) : info.nTrackPos;
            break;
        default:
            return;
    }
    LayoutParams(p);
}

static void SaveParams(ParamsDialog* p) {
    Vec<LlmJsonParam> params;
    for (const ParamRow& row : p->rows) {
        Str name = HwndGetTextTemp(row.name);
        Str value = HwndGetTextTemp(row.value);
        str::TrimWsBoth(value);
        if (str::IsEmptyOrWhiteSpace(name) && len(value) == 0) continue;
        VecAppend(params, LlmJsonParam{name, value});
    }
    Str object;
    Str error;
    if (!WriteLlmParams(params, object, error)) {
        HwndSetText(p->note, error);
        str::Free(error);
        return;
    }
    if (IsWindow(p->owner) && p->onSave) p->onSave(p->owner, object);
    str::Free(object);
    DestroyWindow(p->hwnd);
}

static void RemoveParamRow(ParamsDialog* p, int index) {
    if (index < 0 || index >= len(p->rows)) return;
    ParamRow row = p->rows[index];
    DestroyWindow(row.name);
    DestroyWindow(row.value);
    DestroyWindow(row.remove);
    VecRemoveAt(p->rows, index);
    if (len(p->rows) == 0) AddParamRow(p);
    LayoutParams(p);
    SetFocus(p->rows[std::min(index, len(p->rows) - 1)].name);
}

static void UpdateParamsTheme(ParamsDialog* p) {
    int dpi = DpiGetForHwnd(p->hwnd);
    if (!p->font || p->dpi != dpi) {
        LOGFONTW font{};
        font.lfHeight = -MulDiv(10, dpi, 72);
        font.lfWeight = FW_NORMAL;
        font.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
        HFONT old = p->font;
        p->font = CreateFontIndirectW(&font);
        p->dpi = dpi;
        HwndSetFontForWindowAndItsChildren(p->hwnd, p->font);
        DeleteObject(old);
    }
    Color background = ThemeWindowBackgroundColor();
    if (!p->brush || p->background != background) {
        DeleteObject(p->brush);
        p->brush = CreateSolidBrush(background);
        p->background = background;
        DarkModeApplyToWindow(p->hwnd);
        RedrawWindow(p->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    LayoutParams(p);
}

static void CreateParamsControls(ParamsDialog* p) {
    p->content = NewTranslateScrollPane(p->hwnd);
    SetWindowSubclass(p->content, ParamContentProc, kParamsSubclass, (DWORD_PTR)p->hwnd);
    p->description =
        ParamControl(p, p->hwnd, L"STATIC",
                     L"参照服务商 API 文档填写参数。值必须是 JSON，可用对象、数组、字符串、数字、true、false 或 "
                     L"null。\n同名参数覆盖标准请求参数。",
                     SS_LEFT);
    p->nameLabel = ParamControl(p, p->hwnd, L"STATIC", L"参数名称", SS_LEFT);
    p->valueLabel = ParamControl(p, p->hwnd, L"STATIC", L"参数值（JSON）", SS_LEFT);
    p->add = ParamControl(p, p->content, L"BUTTON", L"添加参数", BS_PUSHBUTTON | WS_TABSTOP, kParamsAdd);
    p->note = ParamControl(p, p->hwnd, L"STATIC", L"仅暂存到翻译设置。主设置窗口取消时，这里的修改也会丢弃。", SS_LEFT);
    p->cancel = ParamControl(p, p->hwnd, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, kParamsCancel);
    p->save = ParamControl(p, p->hwnd, L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, kParamsSave);
    UpdateParamsTheme(p);
    Vec<LlmJsonParam> params;
    Str error;
    bool valid = ReadLlmParams(p->initial, params, error);
    if (valid) {
        for (const LlmJsonParam& param : params) AddParamRow(p, param.name, param.value);
    } else {
        HwndSetText(p->note, error);
    }
    FreeLlmParams(params);
    str::Free(error);
    if (len(p->rows) < kParamsMaxRows) AddParamRow(p);
    LayoutParams(p);
}

static LRESULT CALLBACK ParamsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = (ParamsDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* create = (ParamsCreate*)((CREATESTRUCTW*)lp)->lpCreateParams;
        p = create->dialog;
        create->attached = true;
        p->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p) return DefWindowProcW(hwnd, msg, wp, lp);
    DpiScope scope(hwnd);
    switch (msg) {
        case WM_CREATE:
            CreateParamsControls(p);
            return 0;
        case WM_SIZE:
            LayoutParams(p);
            return 0;
        case WM_VSCROLL:
            ScrollParams(p, wp);
            return 0;
        case WM_MOUSEWHEEL:
            p->scroll -= GET_WHEEL_DELTA_WPARAM(wp) * DpiScaleByDpi(p->dpi, 24) / WHEEL_DELTA;
            LayoutParams(p);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = (MINMAXINFO*)lp;
            Rect work = ParamsWorkArea(hwnd);
            limits->ptMinTrackSize = {std::min(DpiScaleByDpi(p->dpi, 400), work.dx),
                                      std::min(DpiScaleByDpi(p->dpi, 300), work.dy)};
            return 0;
        }
        case WM_DPICHANGED: {
            Rect rect = ToRect(*(RECT*)lp);
            Rect work = ParamsWorkArea(hwnd);
            rect.dx = std::min(rect.dx, work.dx);
            rect.dy = std::min(rect.dy, work.dy);
            rect.x = std::max(work.x, std::min(rect.x, work.x + work.dx - rect.dx));
            rect.y = std::max(work.y, std::min(rect.y, work.y + work.dy - rect.dy));
            SetWindowPos(hwnd, nullptr, rect.x, rect.y, rect.dx, rect.dy, SWP_NOZORDER);
            UpdateParamsTheme(p);
            return 0;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            UpdateParamsTheme(p);
            return 0;
        case WM_COMMAND: {
            if (HIWORD(wp) != BN_CLICKED) return 0;
            int control = LOWORD(wp);
            if (control == kParamsSave)
                SaveParams(p);
            else if (control == kParamsCancel)
                DestroyWindow(hwnd);
            else if (control == kParamsAdd) {
                AddParamRow(p);
                SetFocus(p->rows[len(p->rows) - 1].name);
            } else if (control >= kParamsRowBase && (control - kParamsRowBase) % kParamsRowIds == 2) {
                RemoveParamRow(p, (control - kParamsRowBase) / kParamsRowIds);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, ThemeWindowTextColor());
            SetBkColor((HDC)wp, p->background);
            return (LRESULT)p->brush;
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            FillRect((HDC)wp, &rect, p->brush);
            return 1;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (IsWindow(p->owner)) {
                if (p->onClose) p->onClose(p->owner);
                EnableWindow(p->owner, TRUE);
                SetForegroundWindow(p->owner);
            }
            DeleteObject(p->font);
            DeleteObject(p->brush);
            str::Free(p->initial);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete p;
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND OpenTranslateParams(HWND owner, Str initial, void (*onSave)(HWND, Str), void (*onClose)(HWND)) {
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = ParamsWndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kParamsClass;
    RegisterClassExW(&cls);
    auto* p = new ParamsDialog();
    p->owner = owner;
    p->initial = str::Dup(initial);
    p->onSave = onSave;
    p->onClose = onClose;
    p->dpi = DpiGetForHwnd(owner);
    Rect area = ParamsWorkArea(owner);
    int width = std::min(DpiScaleByDpi(p->dpi, 650), area.dx);
    int height = std::min(DpiScaleByDpi(p->dpi, 550), area.dy);
    ParamsCreate create{p};
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kParamsClass, L"自定义请求参数",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
                                area.x + (area.dx - width) / 2, area.y + (area.dy - height) / 2, width, height, owner,
                                nullptr, GetModuleHandleW(nullptr), &create);
    if (!hwnd) {
        if (!create.attached) {
            str::Free(p->initial);
            delete p;
        }
        return nullptr;
    }
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (len(p->rows) > 0) SetFocus(p->rows[0].name);
    return hwnd;
}
