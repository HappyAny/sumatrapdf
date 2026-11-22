/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "gui/GpuiBridge.h"
#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "AIChatTypes.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "FileHistory.h"
#include "SumatraPDF.h"
#include "SumatraDialogs.h"
#include "Translations.h"
#include "Theme.h"
#include "AnnotPlacement.h"
#include "gui/AppShell.h"
#include "gui/DialogWidgets.h"
#include "gui/DocCanvas.h"
#include "gui/ToolWindow.h"
#include "MeasurementCommon.h"
#include "MeasurementToolbar.h"

using namespace measure;

constexpr int kMeasureClientDx = 440;
constexpr int kMeasureClientDy = 208;

struct MeasureView {
    MainWindow* win = nullptr;
    static void OnSnap(MeasureView*, gp::Ctx*, const gp::ClickEvent*);
    static void OnApply(MeasureView*, gp::Ctx*, const gp::ClickEvent*);
    static void OnClear(MeasureView*, gp::Ctx*, const gp::ClickEvent*);
    static void OnReset(MeasureView*, gp::Ctx*, const gp::ClickEvent*);
    static void OnClose(MeasureView*, gp::Ctx*, const gp::ClickEvent*);
};

struct MeasurementToolbar : MeasurementModel {
    ToolWindow* tw = nullptr;
    gp::App* app = nullptr;
    gp::Entity<MeasureView> view;
    DialogSelect mode;
    gp::InputState lengthEdit;
    gp::InputState unitEdit;
    bool invalidCalibration = false;

    ~MeasurementToolbar() {
        mode.Free();
        if (app && view.IsValid()) {
            gp::EntityDrop(app, view.id);
        }
    }
};

static double DefaultScale(MainWindow* win) {
    float dpi = win->AsFixed()->GetEngine()->fileDPI;
    return dpi > 0 ? kPointsPerInch / dpi : 1;
}

static bool IsDefaultScale(MainWindow* win, const MeasurementToolbar* bar) {
    return str::Eq(bar->unit, StrL("pt")) && bar->scale == DefaultScale(win);
}

static void SaveScale(MainWindow* win, MeasurementToolbar* bar) {
    FileState* fs = FileHistoryFindByPath(win->CurrentTab()->filePath);
    if (!fs) {
        return;
    }
    bool isDefault = IsDefaultScale(win, bar);
    fs->measurementScale = isDefault ? 0 : (float)bar->scale;
    str::ReplaceWithCopy(&fs->measurementUnit, isDefault ? Str{} : bar->unit);
    ScheduleSaveSettings();
}

static void RefreshMeasure(MainWindow* win, MeasurementToolbar* bar) {
    bar->invalidCalibration = false;
    ToolWindowInvalidate(bar->tw);
    win->RedrawCanvas();
}

static void ChangeMeasureMode(MainWindow* win, MeasurementToolbar* bar, MeasureTool tool) {
    bar->tool = tool;
    bar->nPts = 0;
    bar->haveHover = false;
    RefreshMeasure(win, bar);
    AppShellFocusFrame(win);
}

static void ApplyMeasureScale(MainWindow* win, MeasurementToolbar* bar) {
    if (!bar->haveCalib) {
        return;
    }
    if (!SetMeasurementScale(bar, InputTextTemp(&bar->lengthEdit), InputTextTemp(&bar->unitEdit))) {
        bar->invalidCalibration = true;
        ToolWindowInvalidate(bar->tw);
        return;
    }
    SaveScale(win, bar);
    RefreshMeasure(win, bar);
    AppShellFocusFrame(win);
}

static void ClearMeasurements(MainWindow* win, MeasurementToolbar* bar) {
    VecReset(bar->done);
    bar->haveCalib = false;
    bar->nPts = 0;
    RefreshMeasure(win, bar);
}

static void ResetMeasureScale(MainWindow* win, MeasurementToolbar* bar) {
    bar->scale = DefaultScale(win);
    str::ReplaceWithCopy(&bar->unit, StrL("pt"));
    gp::InputSetValue(&bar->unitEdit, ToGpui(bar->unit));
    SaveScale(win, bar);
    RefreshMeasure(win, bar);
}

void MeasureView::OnSnap(MeasureView* self, gp::Ctx* cx, const gp::ClickEvent*) {
    if (auto* bar = self->win->measurementToolbar) {
        bar->snap = !bar->snap;
        gp::Notify(cx);
    }
}

void MeasureView::OnApply(MeasureView* self, gp::Ctx* cx, const gp::ClickEvent*) {
    if (auto* bar = self->win->measurementToolbar) {
        ApplyMeasureScale(self->win, bar);
        gp::Notify(cx);
    }
}

void MeasureView::OnClear(MeasureView* self, gp::Ctx* cx, const gp::ClickEvent*) {
    if (auto* bar = self->win->measurementToolbar) {
        ClearMeasurements(self->win, bar);
        gp::Notify(cx);
    }
}

void MeasureView::OnReset(MeasureView* self, gp::Ctx* cx, const gp::ClickEvent*) {
    if (auto* bar = self->win->measurementToolbar) {
        ResetMeasureScale(self->win, bar);
        gp::Notify(cx);
    }
}

void MeasureView::OnClose(MeasureView* self, gp::Ctx*, const gp::ClickEvent*) {
    CloseMeasurement(self->win);
}

static Str MeasureStatus(const MeasurementToolbar* bar) {
    if (bar->invalidCalibration) {
        return Tr("Enter a positive length and a unit name.");
    }
    if (bar->nPts == 0) {
        return bar->tool == MeasureTool::Calibrate && bar->haveCalib
                   ? Tr("Enter the known length and units, then apply.")
                   : Tr("Click the start point. Ctrl+click deletes a measurement.");
    }
    if (bar->nPts == 1 && bar->tool == MeasureTool::Radius) {
        return Tr("Click the other end of the arc.");
    }
    if (bar->nPts == 2) {
        return Tr("Click a point on the arc.");
    }
    return Tr("Click the end point on the same page.");
}

static TempStr MeasureResultTemp(const MeasurementToolbar* bar) {
    const Measurement* m = nullptr;
    if (bar->nPts > 0) {
        m = &bar->cur;
    } else if (bar->tool == MeasureTool::Calibrate && bar->haveCalib) {
        m = &bar->calib;
    } else if (len(bar->done) > 0) {
        m = &bar->done[len(bar->done) - 1];
    }
    if (m) {
        PointF pts[3];
        int n = MeasurementPoints(bar, *m, bar->nPts > 0, pts);
        TempStr text = MeasurementTextTemp(bar, *m, n, pts);
        if (len(text) > 0) {
            return text;
        }
    }
    return fmt(Tr("Units: %s").s, bar->unit);
}

static gp::El* MeasureContent(MainWindow* win, gp::Ctx* cx) {
    MeasurementToolbar* bar = win->measurementToolbar;
    if (!bar || !win->AsFixed()) {
        return nullptr;
    }
    if (!bar->view.IsValid()) {
        bar->app = cx->app;
        bar->view = gp::EntityNewState<MeasureView>(cx->app);
        auto* view = (MeasureView*)gp::EntityGet(cx->app, bar->view.id);
        view->win = win;
        bar->mode.Init(cx->app);
        StrVec items;
        items.Append(Tr("Length"));
        items.Append(Tr("Rectangular area"));
        items.Append(Tr("Radius"));
        items.Append(Tr("Guide line"));
        items.Append(Tr("Calibrate"));
        bar->mode.SetItems(items, (int)bar->tool);
    }
    if (bar->mode.PollChanged(cx->app)) {
        ChangeMeasureMode(win, bar, (MeasureTool)bar->mode.sel);
    }
    Color bg = ThemeWindowControlBackgroundColor();
    Color fg = ThemeWindowTextColor();
    gp::El* col = gp::Div(cx->a)->FlexCol()->W(gp::kFill)->Gap(8)->Pad(8)->Bg(ToGpui(bg))->Fg(ToGpui(fg));
    gp::El* table = gp::Div(cx->a)->FlexCol()->W(gp::kFill)->Gap(6);
    col->Child(table);
    Str toolLabel = Tr("Tool:");
    Str lengthLabel = Tr("Known length:");
    Str unitLabel = Tr("Units:");
    float labelWidth = std::max(DlgWinTextDx(cx, toolLabel), DlgWinTextDx(cx, lengthLabel));
    labelWidth = std::max(labelWidth, DlgWinTextDx(cx, unitLabel));
    auto row = [&](Str label, gp::El* control) {
        gp::El* r = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->ItemsCenter()->Gap(8);
        r->Child(gp::TextEl(cx->a, GpuiDup(cx->a, label))->Font(kDlgWinFontPx)->W(labelWidth)->Shrink0());
        r->Child(gp::Div(cx->a)->Flex1()->MinW(0)->Child(control));
        table->Child(r);
    };
    row(toolLabel, bar->mode.Build(cx, StrL("measure-tool"), gp::kFill));
    bool disableEdit = bar->tool != MeasureTool::Calibrate;
    auto input = [&](gp::Str id, gp::InputState* state) {
        return gpc::Input::New(cx, id, state)
            ->W(gp::kFill)
            ->Disabled(disableEdit)
            ->IntoEl()
            ->H(kDlgWinEditDy)
            ->Font(kDlgWinFontPx);
    };
    row(lengthLabel, input(GStrL("measure-known-length"), &bar->lengthEdit));
    row(unitLabel, input(GStrL("measure-unit"), &bar->unitEdit));

    gp::Listener snap = gp::ListenTo(bar->view, &MeasureView::OnSnap);
    col->Child(DlgCheck(cx, StrL("measure-snap"), Tr("&Snap to lines in the drawing"), bar->snap, snap));
    gp::El* buttons = gp::Div(cx->a)->FlexRow()->W(gp::kFill)->ItemsCenter()->Gap(8);
    auto button = [&](gp::Str id, Str label, gp::Listener cb, bool disabled) {
        buttons->Child(gpc::Button::New(cx, id)
                           ->Outline()
                           ->WithSize(gp::UiSize::Small)
                           ->Disabled(disabled)
                           ->Label(ToGpui(label))
                           ->OnClick(cb)
                           ->IntoEl());
    };
    bool canApply = bar->haveCalib && Dist(bar->calib.pts[0], bar->calib.pts[1]) > 0;
    button(GStrL("measure-apply"), Tr("Apply calibration"), gp::ListenTo(bar->view, &MeasureView::OnApply), !canApply);
    button(GStrL("measure-clear"), Tr("Clear"), gp::ListenTo(bar->view, &MeasureView::OnClear),
           len(bar->done) == 0 && !bar->haveCalib);
    button(GStrL("measure-reset"), Tr("Reset scale"), gp::ListenTo(bar->view, &MeasureView::OnReset),
           IsDefaultScale(win, bar));
    col->Child(buttons);
    col->Child(gp::TextEl(cx->a, GpuiDup(cx->a, MeasureResultTemp(bar)))->Font(kDlgWinFontPx));
    col->Child(gp::TextEl(cx->a, GpuiDup(cx->a, MeasureStatus(bar)))->Font(kDlgWinFontPx));
    return col;
}

static void MeasureWindowClosed(MainWindow* win) {
    auto* bar = win ? win->measurementToolbar : nullptr;
    // A caption close can finish after a new toolbar opens.
    if (!bar || !bar->tw || ToolWindowIsLive(bar->tw)) {
        return;
    }
    bar->tw = nullptr;
    CloseMeasurement(win);
}

void ToggleMeasurement(MainWindow* win) {
    if (win->measurementToolbar) {
        CloseMeasurement(win);
        return;
    }
    if (!win->AsFixed()) {
        return;
    }
    CancelAnnotationPlacement(win);
    CloseSignDocumentDialog(win);
    auto* bar = new MeasurementToolbar();
    win->measurementToolbar = bar;
    bar->scale = DefaultScale(win);
    bar->unit = str::Dup(StrL("pt"));
    FileState* fs = FileHistoryFindByPath(win->CurrentTab()->filePath);
    if (fs && isfinite(fs->measurementScale) && fs->measurementScale > 0 && len(fs->measurementUnit) > 0) {
        bar->scale = fs->measurementScale;
        str::ReplaceWithCopy(&bar->unit, fs->measurementUnit);
    }
    gp::InputSetValue(&bar->lengthEdit, GStrL("1"));
    gp::InputSetValue(&bar->unitEdit, ToGpui(bar->unit));

    ToolWindowDesc desc;
    desc.name = "measure";
    desc.title = []() -> Str { return Tr("Measure"); };
    desc.build = MeasureContent;
    desc.onClosed = MeasureWindowClosed;
    if (ToolWindowsAvailable()) {
        Rect rect = ToolWindowCenteredRect(desc, win, {kMeasureClientDx, kMeasureClientDy});
        bar->tw = ToolWindowOpen(desc, win, rect);
    }
    win->RedrawCanvas();
}

void CloseMeasurement(MainWindow* win) {
    MeasurementToolbar* bar = win ? win->measurementToolbar : nullptr;
    if (!bar) {
        return;
    }
    win->measurementToolbar = nullptr;
    if (bar->tw) {
        ToolWindowClose(bar->tw);
    }
    delete bar;
    win->RedrawCanvas();
}

gp::El* BuildMeasurementFallback(MainWindow* win, gp::Ctx* cx) {
    MeasurementToolbar* bar = win->measurementToolbar;
    if (!bar || bar->tw) {
        return nullptr;
    }
    gp::El* col = MeasureContent(win, cx);
    if (!col) {
        return nullptr;
    }
    auto close = gp::ListenTo(bar->view, &MeasureView::OnClose);
    gp::El* card = DlgFrameCard(cx, col, Tr("Measure"), kMeasureClientDx, close);
    return gp::Div(cx->a)->Absolute()->Left(8)->Top(8)->Child(card);
}

bool MeasurementOnEscape(MainWindow* win) {
    MeasurementToolbar* bar = win->measurementToolbar;
    if (!bar) {
        return false;
    }
    if (bar->nPts > 0) {
        bar->nPts = 0;
        bar->haveHover = false;
        RefreshMeasure(win, bar);
    } else {
        CloseMeasurement(win);
    }
    return true;
}

static int MeasurePx(MainWindow* win, int dip) {
    return (int)roundf((float)dip / CanvasScale(win));
}

bool MeasurementOnClick(MainWindow* win, Point pt, bool ctrl) {
    auto* bar = win->measurementToolbar;
    DisplayModel* dm = win->AsFixed();
    if (!bar || !dm) {
        return false;
    }
    int pageNo = dm->GetPageNoByPoint(pt);
    if (pageNo < 1) {
        return true;
    }
    AppShellFocusFrame(win);
    if (ctrl) {
        EraseAt(bar, dm, pageNo, pt, MeasurePx(win, kHitPx));
    } else if (bar->nPts > 0 && pageNo != bar->cur.pageNo) {
        return true;
    } else {
        AddPoint(bar, pageNo, SnappedPoint(bar, dm, pageNo, pt, MeasurePx(win, kSnapPx)));
    }
    RefreshMeasure(win, bar);
    return true;
}

bool MeasurementOnMove(MainWindow* win, Point pt) {
    auto* bar = win->measurementToolbar;
    DisplayModel* dm = win->AsFixed();
    if (!bar || !dm || win->mouseAction != MouseAction::None) {
        return false;
    }
    CanvasSetCursor(win, (int)gp::CursorKind::Crosshair);
    if (bar->nPts > 0 && dm->GetPageNoByPoint(pt) == bar->cur.pageNo) {
        bar->hover = SnappedPoint(bar, dm, bar->cur.pageNo, pt, MeasurePx(win, kSnapPx));
        bar->haveHover = true;
        RefreshMeasure(win, bar);
    }
    return true;
}

struct MeasurePainter {
    gp::PaintCtx* ctx;
    float x;
    float y;
    float scale;

    int Px(int dip) const { return (int)roundf((float)dip / scale); }
    gp::Bounds Bounds(Rect r) const {
        return {x + (float)r.x * scale, y + (float)r.y * scale, (float)r.dx * scale, (float)r.dy * scale};
    }
    void PushClip(Rect page) {
        gp::Bounds b = Bounds(page);
        gp::CanvasPushClip(ctx, b.x, b.y, b.w, b.h);
    }
    void PopClip() { gp::CanvasPopClip(ctx); }
    void DrawLineAA(Point a, Point b, Color color, float width, u8 alpha = 255) {
        gp::Path* path = gp::PathNew(ctx, false);
        if (!path) {
            return;
        }
        gp::PathMoveTo(path, x + (float)a.x * scale, y + (float)a.y * scale);
        gp::PathLineTo(path, x + (float)b.x * scale, y + (float)b.y * scale);
        gp::Rgba c = ToGpui(color);
        c.a = alpha;
        gp::PathStroke(ctx, path, width * scale, c);
        gp::PathFree(path);
    }
    void DrawLabel(Str text, Point at) {
        if (len(text) == 0) {
            return;
        }
        Size sz = CanvasMeasureText(ctx, text, kDlgWinFontPx);
        int pad = Px(3);
        Point top{at.x - sz.dx / 2, at.y - sz.dy / 2};
        gp::Bounds b = Bounds(Rect{top.x - pad, top.y - pad, sz.dx + (2 * pad), sz.dy + (2 * pad)});
        float r = (float)pad * scale;
        gp::CanvasFillRound(ctx, b.x, b.y, b.w, b.h, r, ToGpui(kColWhite));
        gp::CanvasStrokeRound(ctx, b.x, b.y, b.w, b.h, r, 1, ToGpui(kColMeasure));
        CanvasDrawText(ctx, text, top, kColMeasure, kDlgWinFontPx);
    }
    void DrawCross(DisplayModel* dm, int pageNo, PointF p) {
        Point s = dm->CvtToScreen(pageNo, p);
        int c = Px(5);
        DrawLineAA({s.x - c, s.y}, {s.x + c, s.y}, kColMeasure, 1.5f);
        DrawLineAA({s.x, s.y - c}, {s.x, s.y + c}, kColMeasure, 1.5f);
    }
};

void PaintMeasurement(MainWindow* win, gp::PaintCtx* ctx, float x, float y, float scale) {
    if (!win->measurementToolbar) {
        return;
    }
    MeasurePainter painter{ctx, x, y, scale};
    PaintMeasurements(win->measurementToolbar, win->AsFixed(), painter);
}

TempStr TestMeasurement(MainWindow* win, Str action, int pageNo, PointF pt, Str length, Str unit) {
    auto* bar = win ? win->measurementToolbar : nullptr;
    if (!bar) {
        return StrL("show=0");
    }
    bool click = str::Eq(action, StrL("click"));
    bool erase = str::Eq(action, StrL("erase"));
    DisplayModel* dm = win->AsFixed();
    if ((click || erase) && dm && dm->ValidPageNo(pageNo)) {
        MeasurementOnClick(win, dm->CvtToScreen(pageNo, pt), erase);
    } else if (str::Eq(action, StrL("apply"))) {
        gp::InputSetValue(&bar->lengthEdit, ToGpui(length));
        gp::InputSetValue(&bar->unitEdit, ToGpui(unit));
        ApplyMeasureScale(win, bar);
    } else if (str::Eq(action, StrL("clear"))) {
        ClearMeasurements(win, bar);
    } else if (str::Eq(action, StrL("reset"))) {
        ResetMeasureScale(win, bar);
    } else if (str::Eq(action, StrL("snap"))) {
        bar->snap = pageNo != 0;
        ToolWindowInvalidate(bar->tw);
    } else if (!str::Eq(action, StrL("state"))) {
        MeasureTool tool = MeasureTool::Length;
        const char* names[] = {"length", "area", "radius", "guide", "calibrate"};
        for (int i = 0; i < dimofi(names); i++) {
            if (str::Eq(action, Str(names[i]))) {
                tool = (MeasureTool)i;
            }
        }
        if (bar->app) {
            bar->mode.SetSel(bar->app, (int)tool);
        }
        ChangeMeasureMode(win, bar, tool);
    }
    return MeasurementStateTemp(bar);
}
