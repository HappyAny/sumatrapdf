/* License: GPL-3.0-only */

#include "base/Base.h"
#include "base/Win.h"
#include <dwmapi.h>
#include "TranslateAnchor.h"

constexpr int kCalloutGap = 12;
constexpr int kCalloutMinGap = 2;
constexpr int kToolbarGap = 4;
constexpr int kArrowHalfBase = 8;
constexpr int kArrowCornerInset = 8;
constexpr int kArrowMaxLength = 40;
constexpr int kArrowMaxSkew = 24;
constexpr Color kArrowColorKey = RGB(255, 0, 255);
constexpr WCHAR kArrowClass[] = L"SumatraTranslateArrow";

static int AnchorScale(int value, int dpi) {
    dpi = dpi > 0 ? std::min(dpi, 768) : 96;
    return std::max(1, MulDiv(value, dpi, 96));
}

static i64 AnchorClamp(i64 value, i64 low, i64 high) {
    return std::max(low, std::min(value, high));
}

struct AnchorBounds {
    i64 left;
    i64 top;
    i64 right;
    i64 bottom;
};

static AnchorBounds SafeWorkBounds(Rect work) {
    return {work.x, work.y, std::min<i64>(INT_MAX, (i64)work.x + std::max(0, work.dx)),
            std::min<i64>(INT_MAX, (i64)work.y + std::max(0, work.dy))};
}

static Rect ClampAnchor(Rect anchor, const AnchorBounds& work) {
    i64 left = AnchorClamp(anchor.x, work.left, work.right - 1);
    i64 top = AnchorClamp(anchor.y, work.top, work.bottom - 1);
    i64 right = AnchorClamp((i64)anchor.x + std::max(1, anchor.dx), left + 1, work.right);
    i64 bottom = AnchorClamp((i64)anchor.y + std::max(1, anchor.dy), top + 1, work.bottom);
    return Rect((int)left, (int)top, (int)(right - left), (int)(bottom - top));
}

TranslatePosition ParseTranslatePosition(Str value) {
    if (str::EqI(value, StrL("top"))) return TranslatePosition::Top;
    if (str::EqI(value, StrL("bottom"))) return TranslatePosition::Bottom;
    if (str::EqI(value, StrL("left"))) return TranslatePosition::Left;
    if (str::EqI(value, StrL("right"))) return TranslatePosition::Right;
    return TranslatePosition::Auto;
}

Str TranslatePositionName(TranslatePosition value) {
    switch (value) {
        case TranslatePosition::Top:
            return StrL("top");
        case TranslatePosition::Bottom:
            return StrL("bottom");
        case TranslatePosition::Left:
            return StrL("left");
        case TranslatePosition::Right:
            return StrL("right");
        default:
            return StrL("auto");
    }
}

static Rect CalloutCandidate(Rect anchor, Size size, const AnchorBounds& work, TranslatePosition side, int gap) {
    i64 left = anchor.x;
    i64 top = anchor.y;
    i64 right = (i64)anchor.x + anchor.dx;
    i64 bottom = (i64)anchor.y + anchor.dy;
    switch (side) {
        case TranslatePosition::Right:
            left = right + gap;
            break;
        case TranslatePosition::Left:
            left -= (i64)size.dx + gap;
            break;
        case TranslatePosition::Bottom:
            left += ((i64)anchor.dx - size.dx) / 2;
            top = bottom + gap;
            break;
        case TranslatePosition::Top:
            left += ((i64)anchor.dx - size.dx) / 2;
            top -= (i64)size.dy + gap;
            break;
        default:
            break;
    }
    left = AnchorClamp(left, work.left, work.right - size.dx);
    top = AnchorClamp(top, work.top, work.bottom - size.dy);
    return Rect((int)left, (int)top, size.dx, size.dy);
}

static bool OnCalloutSide(Rect popup, Rect anchor, TranslatePosition side, int gap) {
    switch (side) {
        case TranslatePosition::Right:
            return (i64)popup.x >= (i64)anchor.x + anchor.dx + gap;
        case TranslatePosition::Left:
            return (i64)popup.x + popup.dx + gap <= anchor.x;
        case TranslatePosition::Bottom:
            return (i64)popup.y >= (i64)anchor.y + anchor.dy + gap;
        case TranslatePosition::Top:
            return (i64)popup.y + popup.dy + gap <= anchor.y;
        default:
            return false;
    }
}

static i64 CalloutOverlap(Rect popup, Rect anchor) {
    i64 left = std::max<i64>(popup.x, anchor.x);
    i64 top = std::max<i64>(popup.y, anchor.y);
    i64 right = std::min((i64)popup.x + popup.dx, (i64)anchor.x + anchor.dx);
    i64 bottom = std::min((i64)popup.y + popup.dy, (i64)anchor.y + anchor.dy);
    return std::max<i64>(0, right - left) * std::max<i64>(0, bottom - top);
}

static int TargetCoordinate(i64 start, i64 length, i64 popupStart, i64 popupLength, int inset) {
    i64 center = start + length / 2;
    i64 low = std::max(start, popupStart + inset);
    i64 high = std::min(start + length - 1, popupStart + popupLength - inset - 1);
    return (int)(low <= high ? AnchorClamp(center, low, high) : center);
}

static Point CalloutTarget(Rect anchor, Rect popup, TranslatePosition side, int dpi) {
    int inset = AnchorScale(kArrowHalfBase + kArrowCornerInset, dpi);
    int x = TargetCoordinate(anchor.x, anchor.dx, popup.x, popup.dx, inset);
    int y = TargetCoordinate(anchor.y, anchor.dy, popup.y, popup.dy, inset);
    switch (side) {
        case TranslatePosition::Right:
            return Point(anchor.x + anchor.dx, y);
        case TranslatePosition::Left:
            return Point(anchor.x, y);
        case TranslatePosition::Bottom:
            return Point(x, anchor.y + anchor.dy);
        default:
            return Point(x, anchor.y);
    }
}

static TranslateCallout PlaceInSlot(Rect anchor, Size popupSize, const AnchorBounds& bounds,
                                    TranslatePosition preferred, int dpi) {
    TranslateCallout result;
    if (preferred != TranslatePosition::Top && preferred != TranslatePosition::Bottom &&
        preferred != TranslatePosition::Left && preferred != TranslatePosition::Right) {
        preferred = TranslatePosition::Auto;
    }
    constexpr TranslatePosition order[] = {TranslatePosition::Right, TranslatePosition::Left, TranslatePosition::Bottom,
                                           TranslatePosition::Top};
    TranslatePosition sides[5];
    int count = 0;
    if (preferred != TranslatePosition::Auto) sides[count++] = preferred;
    for (TranslatePosition side : order) {
        if (side != preferred) sides[count++] = side;
    }
    i64 leastOverlap = LLONG_MAX;
    for (int i = 0; i < count; i++) {
        TranslatePosition side = sides[i];
        Rect candidate = CalloutCandidate(anchor, popupSize, bounds, side, AnchorScale(kCalloutGap, dpi));
        if (OnCalloutSide(candidate, anchor, side, AnchorScale(kCalloutMinGap, dpi))) {
            result.popup = candidate;
            result.side = side;
            result.target = CalloutTarget(anchor, candidate, side, dpi);
            return result;
        }
        i64 overlap = CalloutOverlap(candidate, anchor);
        if (overlap < leastOverlap) {
            leastOverlap = overlap;
            result.popup = candidate;
            result.side = side;
        }
    }
    result.target = CalloutTarget(anchor, result.popup, result.side, dpi);
    return result;
}

TranslateCallout PlaceTranslateCallout(Rect anchor, Size popupSize, Rect work, TranslatePosition preferred, int dpi,
                                       Rect toolbar, int minHeight) {
    AnchorBounds bounds = SafeWorkBounds(work);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        TranslateCallout empty;
        empty.popup = Rect(work.x, work.y, 0, 0);
        empty.target = Point(work.x, work.y);
        return empty;
    }
    popupSize.dx = (int)std::min<i64>(std::max(1, popupSize.dx), bounds.right - bounds.left);
    popupSize.dy = (int)std::min<i64>(std::max(1, popupSize.dy), bounds.bottom - bounds.top);
    anchor = ClampAnchor(anchor, bounds);
    TranslateCallout normal = PlaceInSlot(anchor, popupSize, bounds, preferred, dpi);
    work =
        Rect((int)bounds.left, (int)bounds.top, (int)(bounds.right - bounds.left), (int)(bounds.bottom - bounds.top));
    toolbar = toolbar.Intersect(work);
    if (toolbar.IsEmpty()) return normal;
    int gap = AnchorScale(kToolbarGap, dpi);
    int left = toolbar.x - std::min(gap, toolbar.x - work.x);
    int top = toolbar.y - std::min(gap, toolbar.y - work.y);
    int right = toolbar.Right() + std::min(gap, work.Right() - toolbar.Right());
    int bottom = toolbar.Bottom() + std::min(gap, work.Bottom() - toolbar.Bottom());
    if (normal.popup.Intersect({left, top, right - left, bottom - top}).IsEmpty()) return normal;

    // Each slot keeps the chosen reading width and may shorten the card to leave room for the shortcuts.
    Rect slots[] = {{work.x, work.y, work.dx, top - work.y},
                    {work.x, bottom, work.dx, work.Bottom() - bottom},
                    {work.x, work.y, left - work.x, work.dy},
                    {right, work.y, work.Right() - right, work.dy}};
    TranslateCallout best;
    i64 bestOverlap = LLONG_MAX;
    i64 bestDistance = LLONG_MAX;
    for (Rect slot : slots) {
        if (slot.dx < popupSize.dx || slot.dy < std::min(popupSize.dy, minHeight) || slot.IsEmpty()) continue;
        Size size{popupSize.dx, std::min(popupSize.dy, slot.dy)};
        TranslateCallout candidate = PlaceInSlot(anchor, size, SafeWorkBounds(slot), preferred, dpi);
        i64 overlap = CalloutOverlap(candidate.popup, anchor);
        i64 distance = llabs((i64)candidate.popup.x - normal.popup.x) + llabs((i64)candidate.popup.y - normal.popup.y);
        bool better = overlap < bestOverlap;
        if (overlap == bestOverlap) {
            better = size.dy > best.popup.dy || (size.dy == best.popup.dy && distance < bestDistance);
        }
        if (better) {
            best = candidate;
            bestOverlap = overlap;
            bestDistance = distance;
        }
    }
    return best.popup.IsEmpty() ? normal : best;
}

struct TranslateArrow {
    HWND owner = nullptr;
    HWND hwnd = nullptr;
    POINT vertices[3]{};
    Rect rect;
    Point tip;
    Color fill = RGB(242, 242, 242);
    Color border = RGB(213, 213, 213);
    bool visible = false;
};

static void PaintTranslateArrow(TranslateArrow* arrow, HDC dc) {
    RECT client{};
    GetClientRect(arrow->hwnd, &client);
    int saved = SaveDC(dc);
    SetDCBrushColor(dc, kArrowColorKey);
    FillRect(dc, &client, (HBRUSH)GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, arrow->fill);
    Polygon(dc, arrow->vertices, dimofi(arrow->vertices));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, arrow->border);
    MoveToEx(dc, arrow->vertices[0].x, arrow->vertices[0].y, nullptr);
    LineTo(dc, arrow->vertices[1].x, arrow->vertices[1].y);
    MoveToEx(dc, arrow->vertices[0].x, arrow->vertices[0].y, nullptr);
    LineTo(dc, arrow->vertices[2].x, arrow->vertices[2].y);
    RestoreDC(dc, saved);
}

static LRESULT CALLBACK ArrowWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* arrow = (TranslateArrow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        arrow = (TranslateArrow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        arrow->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)arrow);
    }
    if (!arrow) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            PaintTranslateArrow(arrow, dc);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintTranslateArrow(arrow, (HDC)wp);
            return 0;
        case WM_DPICHANGED:
            return 0;
        case WM_NCDESTROY:
            arrow->hwnd = nullptr;
            arrow->visible = false;
            arrow->rect = {};
            arrow->tip = {};
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

TranslateArrow* CreateTranslateArrow(HWND owner) {
    if (!IsWindow(owner)) return nullptr;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = ArrowWndProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kArrowClass;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    auto* arrow = new TranslateArrow();
    arrow->owner = owner;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kArrowClass,
                                L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, cls.hInstance, arrow);
    if (!hwnd || !SetLayeredWindowAttributes(hwnd, kArrowColorKey, 255, LWA_COLORKEY)) {
        if (hwnd) DestroyWindow(hwnd);
        delete arrow;
        return nullptr;
    }
    return arrow;
}

void HideTranslateArrow(TranslateArrow* arrow) {
    if (!arrow) return;
    if (IsWindow(arrow->hwnd)) ShowWindow(arrow->hwnd, SW_HIDE);
    arrow->visible = false;
    arrow->rect = {};
    arrow->tip = {};
}

void DeleteTranslateArrow(TranslateArrow* arrow) {
    if (!arrow) return;
    if (IsWindow(arrow->hwnd)) DestroyWindow(arrow->hwnd);
    delete arrow;
}

static Rect ArrowFrameRect(HWND owner) {
    RECT outer{};
    if (!GetWindowRect(owner, &outer)) return {};
    RECT visible{};
    if (SUCCEEDED(DwmGetWindowAttribute(owner, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))) &&
        visible.right > visible.left && visible.bottom > visible.top) {
        return Rect(visible);
    }
    return Rect(outer);
}

static bool ArrowVertices(TranslateArrow* arrow, const TranslateCallout& callout, Rect work, Rect toolbar, int dpi) {
    Rect frame = ArrowFrameRect(arrow->owner);
    if (frame.dx <= 0 || frame.dy <= 0) return false;
    int half = AnchorScale(kArrowHalfBase, dpi);
    int corner = AnchorScale(kArrowCornerInset, dpi);
    int overlap = AnchorScale(1, dpi);
    Point target = callout.target;
    Point base;
    bool horizontal = callout.side == TranslatePosition::Right || callout.side == TranslatePosition::Left;
    if (horizontal) {
        if (frame.dy < 2 * (half + corner) + 1) return false;
        base.y = (int)AnchorClamp(target.y, (i64)frame.y + corner + half, (i64)frame.y + frame.dy - corner - half - 1);
        base.x = callout.side == TranslatePosition::Right ? frame.x + overlap : frame.x + frame.dx - overlap - 1;
        i64 length = callout.side == TranslatePosition::Right ? (i64)base.x - target.x : (i64)target.x - base.x;
        i64 skew = (i64)base.y - target.y;
        if (length < AnchorScale(kCalloutMinGap, dpi) || length > AnchorScale(kArrowMaxLength, dpi) ||
            skew > AnchorScale(kArrowMaxSkew, dpi) || skew < -AnchorScale(kArrowMaxSkew, dpi))
            return false;
        arrow->vertices[1] = {base.x, base.y - half};
        arrow->vertices[2] = {base.x, base.y + half};
    } else {
        if (callout.side != TranslatePosition::Top && callout.side != TranslatePosition::Bottom) return false;
        if (frame.dx < 2 * (half + corner) + 1) return false;
        base.x = (int)AnchorClamp(target.x, (i64)frame.x + corner + half, (i64)frame.x + frame.dx - corner - half - 1);
        base.y = callout.side == TranslatePosition::Bottom ? frame.y + overlap : frame.y + frame.dy - overlap - 1;
        i64 length = callout.side == TranslatePosition::Bottom ? (i64)base.y - target.y : (i64)target.y - base.y;
        i64 skew = (i64)base.x - target.x;
        if (length < AnchorScale(kCalloutMinGap, dpi) || length > AnchorScale(kArrowMaxLength, dpi) ||
            skew > AnchorScale(kArrowMaxSkew, dpi) || skew < -AnchorScale(kArrowMaxSkew, dpi))
            return false;
        arrow->vertices[1] = {base.x - half, base.y};
        arrow->vertices[2] = {base.x + half, base.y};
    }
    arrow->vertices[0] = {target.x, target.y};
    int left = target.x, right = target.x;
    int top = target.y, bottom = target.y;
    for (const POINT& point : arrow->vertices) {
        left = std::min(left, (int)point.x);
        right = std::max(right, (int)point.x);
        top = std::min(top, (int)point.y);
        bottom = std::max(bottom, (int)point.y);
    }
    Rect rect(left, top, right - left + 1, bottom - top + 1);
    if (rect.Intersect(work) != rect || !rect.Intersect(toolbar).IsEmpty()) return false;
    arrow->rect = rect;
    arrow->tip = target;
    for (POINT& point : arrow->vertices) {
        point.x -= left;
        point.y -= top;
    }
    return true;
}

void UpdateTranslateArrow(TranslateArrow* arrow, const TranslateCallout& callout, Rect work, Rect toolbar, int dpi,
                          Color fill, Color border) {
    if (!arrow) return;
    Rect previous = arrow->rect;
    POINT oldVertices[3];
    memcpy(oldVertices, arrow->vertices, sizeof(oldVertices));
    if (!IsWindow(arrow->hwnd) || !IsWindowVisible(arrow->owner) ||
        !ArrowVertices(arrow, callout, work, toolbar, dpi)) {
        HideTranslateArrow(arrow);
        return;
    }
    if (TranslateArrowVisible(arrow) && arrow->rect == previous && fill == arrow->fill && border == arrow->border &&
        memcmp(oldVertices, arrow->vertices, sizeof(oldVertices)) == 0)
        return;
    arrow->fill = fill;
    arrow->border = border;
    Rect rect = arrow->rect;
    SetWindowPos(arrow->hwnd, nullptr, rect.x, rect.y, rect.dx, rect.dy,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    InvalidateRect(arrow->hwnd, nullptr, FALSE);
    UpdateWindow(arrow->hwnd);
    arrow->visible = IsWindowVisible(arrow->hwnd) != FALSE;
}

Rect TranslateArrowRect(TranslateArrow* arrow) {
    return TranslateArrowVisible(arrow) ? HwndWindowRect(arrow->hwnd) : Rect();
}

bool TranslateArrowVisible(TranslateArrow* arrow) {
    return arrow && arrow->visible && IsWindow(arrow->hwnd) && IsWindowVisible(arrow->hwnd) &&
           IsWindowVisible(arrow->owner);
}

Point TranslateArrowTip(TranslateArrow* arrow) {
    return TranslateArrowVisible(arrow) ? arrow->tip : Point();
}
