/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include <float.h>
#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"
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
#include "MeasurementToolbar.h"
#include "MeasurementCommon.h"

// The Measure window: pick a tool, then click points on the page. Points are
// kept in page coordinates, so zooming, scrolling or rotating between clicks
// doesn't move them, and finished measurements stay on the page, labeled,
// until cleared or the window closes.
//
//   Length      2 clicks   length, dx, dy
//   Area        2 clicks   opposite corners of a rectangle
//   Radius      3 clicks   two ends of an arc and a point on it
//   Guide line  2 clicks   a line across the whole page, for lining things up
//   Calibrate   2 clicks   a known length, then Apply with its real size
//
// Clicks snap to the ends and corners of the page's vector lines.
// Ctrl + click deletes a measurement.

using namespace measure;

struct MeasurementToolbar : WindowBase, MeasurementModel {
    MainWindow* win = nullptr;
    DropDown* mode = nullptr;
    Edit* knownLength = nullptr;
    Edit* unitEdit = nullptr;
    Checkbox* snapCheck = nullptr;
    VirtButton* apply = nullptr;
    VirtButton* clear = nullptr;
    VirtButton* resetScale = nullptr;
    VirtText* result = nullptr;
    VirtText* status = nullptr;
    PlatformFont* labelFont = nullptr;
    bool Create(MainWindow*);
    void SetMode();
    void Update();
    void Calibrate(VirtMouseEvent* = nullptr);
    void Clear(VirtMouseEvent* = nullptr);
    void ResetScale(VirtMouseEvent* = nullptr);
    void OnClose(WindowBase::CloseEvent*);
    void OnDestroy(WindowBase::DestroyEvent*);
    double DefaultScale();
    void SaveScale();
};

//--- the window

// the size of a document unit in points: PDF units are points, images are pixels
double MeasurementToolbar::DefaultScale() {
    float dpi = win->AsFixed()->GetEngine()->fileDPI;
    return dpi > 0 ? kPointsPerInch / dpi : 1;
}

void MeasurementToolbar::SaveScale() {
    FileState* fs = FileHistoryFindByPath(win->CurrentTab()->filePath);
    if (!fs) {
        return;
    }
    bool isDefault = str::Eq(unit, StrL("pt")) && scale == DefaultScale();
    fs->measurementScale = isDefault ? 0 : (float)scale;
    str::ReplaceWithCopy(&fs->measurementUnit, isDefault ? Str{} : unit);
    ScheduleSaveSettings();
}

void MeasurementToolbar::Update() {
    apply->SetIsEnabled(haveCalib && Dist(calib.pts[0], calib.pts[1]) > 0);
    PointF pts[3];
    TempStr text;
    if (nPts > 0) {
        int n = MeasurementPoints(this, cur, true, pts);
        text = MeasurementTextTemp(this, cur, n, pts);
    } else if (tool == MeasureTool::Calibrate && haveCalib) {
        text = MeasurementTextTemp(this, calib, 2, calib.pts);
    } else if (len(done) > 0) {
        const Measurement& m = done[len(done) - 1];
        int n = MeasurementPoints(this, m, false, pts);
        text = MeasurementTextTemp(this, m, n, pts);
    }
    result->SetText(len(text) > 0 ? Str(text) : Str(fmt(Tr("Units: %s").s, unit)));

    Str st;
    if (nPts == 0) {
        st = tool == MeasureTool::Calibrate && haveCalib
                 ? Tr("Enter the known length and units, then apply.")
                 : Tr("Click the start point. Ctrl+click deletes a measurement.");
    } else if (nPts == 1 && tool == MeasureTool::Radius) {
        st = Tr("Click the other end of the arc.");
    } else if (nPts == 2) {
        st = Tr("Click a point on the arc.");
    } else {
        st = Tr("Click the end point on the same page.");
    }
    status->SetText(st);
    clear->SetIsEnabled(len(done) > 0 || haveCalib);
    resetScale->SetIsEnabled(scale != DefaultScale() || !str::Eq(unit, StrL("pt")));
    HwndInvalidate(hwnd);
    if (win) {
        win->RedrawCanvas();
    }
}

void MeasurementToolbar::SetMode() {
    tool = (MeasureTool)CbGetCurrentSelection(mode);
    nPts = 0;
    haveHover = false;
    knownLength->SetIsEnabled(tool == MeasureTool::Calibrate);
    unitEdit->SetIsEnabled(tool == MeasureTool::Calibrate);
    Update();
    if (win) {
        HwndSetFocus(win->hwndFrame);
    }
}

void MeasurementToolbar::Calibrate(VirtMouseEvent*) {
    if (!win || !haveCalib) {
        return;
    }
    if (!SetMeasurementScale(this, knownLength->GetTextTemp(), unitEdit->GetTextTemp())) {
        status->SetText(Tr("Enter a positive length and a unit name."));
        HwndInvalidate(hwnd);
        return;
    }
    SaveScale();
    Update();
    HwndSetFocus(win->hwndFrame);
}

void MeasurementToolbar::Clear(VirtMouseEvent*) {
    VecReset(done);
    haveCalib = false;
    nPts = 0;
    Update();
}

void MeasurementToolbar::ResetScale(VirtMouseEvent*) {
    scale = DefaultScale();
    str::ReplaceWithCopy(&unit, StrL("pt"));
    unitEdit->SetText(unit);
    SaveScale();
    Update();
}

void MeasurementToolbar::OnClose(WindowBase::CloseEvent*) {
    CloseMeasurement(win);
}

void MeasurementToolbar::OnDestroy(WindowBase::DestroyEvent*) {
    CloseMeasurement(win);
}

bool MeasurementToolbar::Create(MainWindow* mainWin) {
    win = mainWin;
    SetFont(GetAppFont());
    CreateCustomArgs args;
    args.owner = win->hwndFrame;
    args.title = Tr("Measure");
    args.visible = false;
    args.style = WS_POPUPWINDOW | WS_CAPTION;
    args.font = GetFont();
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    closeOnEsc = true;
    onClose = MkMethod1<MeasurementToolbar, WindowBase::CloseEvent*, &MeasurementToolbar::OnClose>(this);
    onDestroy = MkMethod1<MeasurementToolbar, WindowBase::DestroyEvent*, &MeasurementToolbar::OnDestroy>(this);
    labelFont = GetAppFont();

    auto* box = new VBox();
    box->alignCross = CrossAxisAlign::Stretch;
    box->gap = DpiScale(8);
    auto* table = new Table();
    table->SetSize(3, 2);
    table->colGap = DpiScale(8);
    table->rowGap = DpiScale(6);
    const Str labels[] = {Tr("Tool:"), Tr("Known length:"), Tr("Units:")};
    for (int i = 0; i < dimofi(labels); i++) {
        table->SetCell(i, 0, NewVirtText({.s = labels[i], .font = font, .isRtl = IsUIRtl()}));
    }
    DropDown::CreateArgs ddArgs;
    ddArgs.parent = hwnd;
    ddArgs.font = font;
    ddArgs.isRtl = IsUIRtl();
    mode = new DropDown();
    mode->Create(ddArgs);
    // in MeasureTool order
    StrVec items;
    items.Append(Tr("Length"));
    items.Append(Tr("Rectangular area"));
    items.Append(Tr("Radius"));
    items.Append(Tr("Guide line"));
    items.Append(Tr("Calibrate"));
    mode->SetItems(items);
    CbSetCurrentSelection(mode, (int)tool);
    mode->onSelectionChanged = MkMethod0<MeasurementToolbar, &MeasurementToolbar::SetMode>(this);
    table->SetCell(0, 1, mode).alignH = CrossAxisAlign::Stretch;

    Edit::CreateArgs editArgs;
    editArgs.parent = hwnd;
    editArgs.font = font;
    editArgs.withBorder = true;
    editArgs.isRtl = IsUIRtl();
    editArgs.idealWidthChars = 12;
    knownLength = new Edit();
    knownLength->Create(editArgs);
    knownLength->SetText(StrL("1"));
    unitEdit = new Edit();
    unitEdit->Create(editArgs);
    table->SetCell(1, 1, knownLength).alignH = CrossAxisAlign::Stretch;
    table->SetCell(2, 1, unitEdit).alignH = CrossAxisAlign::Stretch;
    box->AddChild(table);

    snapCheck = NewDlgCheckbox(hwnd, Tr("&Snap to lines in the drawing"), font, true);
    box->AddChild(snapCheck);

    auto* buttons = new HBox();
    buttons->gap = DpiScale(8);
    apply = NewThemedButton(hwnd, Tr("Apply calibration"), font, false);
    apply->onClick = MkMethod1<MeasurementToolbar, VirtMouseEvent*, &MeasurementToolbar::Calibrate>(this);
    clear = NewThemedButton(hwnd, Tr("Clear"), font, false);
    clear->onClick = MkMethod1<MeasurementToolbar, VirtMouseEvent*, &MeasurementToolbar::Clear>(this);
    resetScale = NewThemedButton(hwnd, Tr("Reset scale"), font, false);
    resetScale->onClick = MkMethod1<MeasurementToolbar, VirtMouseEvent*, &MeasurementToolbar::ResetScale>(this);
    buttons->AddChild(apply);
    buttons->AddChild(clear);
    buttons->AddChild(resetScale);
    box->AddChild(buttons);

    result = NewVirtText({.font = font, .isRtl = IsUIRtl()});
    status = NewVirtText({.font = font, .isRtl = IsUIRtl()});
    box->AddChild(result);
    box->AddChild(status);
    layout = new Padding(box, DpiScaledInsets(8));

    scale = DefaultScale();
    str::ReplaceWithCopy(&unit, StrL("pt"));
    FileState* fs = FileHistoryFindByPath(win->CurrentTab()->filePath);
    if (fs && isfinite(fs->measurementScale) && fs->measurementScale > 0 && len(fs->measurementUnit) > 0) {
        scale = fs->measurementScale;
        str::ReplaceWithCopy(&unit, fs->measurementUnit);
    }
    unitEdit->SetText(unit);
    SetMode();
    SizeToLayout(this, layout, DpiScale(440), 0, win->hwndFrame);
    UpdateTheme();
    SetIsVisible(true);
    return true;
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
    win->DeleteToolTip();
    auto* bar = new MeasurementToolbar();
    win->measurementToolbar = bar;
    if (!bar->Create(win)) {
        win->measurementToolbar = nullptr;
        delete bar;
    }
}

void CloseMeasurement(MainWindow* win) {
    if (!win || !win->measurementToolbar) {
        return;
    }
    auto* bar = win->measurementToolbar;
    win->measurementToolbar = nullptr;
    bar->win = nullptr;
    bar->SetIsVisible(false);
    bar->ScheduleDelete();
    win->RedrawCanvas();
}

// Drops the measurement being clicked; false if there was none.
bool MeasurementCancel(MainWindow* win) {
    auto* bar = win ? win->measurementToolbar : nullptr;
    if (!bar || bar->nPts == 0) {
        return false;
    }
    bar->nPts = 0;
    bar->haveHover = false;
    bar->Update();
    return true;
}

//--- clicks on the page

bool MeasurementOnClick(MainWindow* win, Point pt, bool ctrl) {
    auto* bar = win->measurementToolbar;
    if (!bar || !win->AsFixed()) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    int pageNo = dm->GetPageNoByPoint(pt);
    if (pageNo < 1) {
        return true;
    }
    HwndSetFocus(win->hwndFrame);
    if (ctrl) {
        EraseAt(bar, dm, pageNo, pt, DpiScale(kHitPx));
        bar->Update();
        return true;
    }
    if (bar->nPts > 0 && pageNo != bar->cur.pageNo) {
        // a measurement stays on the page it started on
        return true;
    }
    bar->snap = bar->snapCheck->IsChecked();
    AddPoint(bar, pageNo, SnappedPoint(bar, dm, pageNo, pt, DpiScale(kSnapPx)));
    bar->Update();
    return true;
}

bool MeasurementOnMove(MainWindow* win, Point pt) {
    auto* bar = win->measurementToolbar;
    DisplayModel* dm = win->AsFixed();
    if (!bar || !dm || win->mouseAction != MouseAction::None) {
        return false;
    }
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    if (bar->nPts > 0 && dm->GetPageNoByPoint(pt) == bar->cur.pageNo) {
        bar->snap = bar->snapCheck->IsChecked();
        bar->hover = SnappedPoint(bar, dm, bar->cur.pageNo, pt, DpiScale(kSnapPx));
        bar->haveHover = true;
        bar->Update();
    }
    return true;
}

//--- drawing

static void DrawLabel(MeasurementToolbar* bar, Gfx* gfx, Str text, Point at) {
    if (len(text) == 0) {
        return;
    }
    Size sz = gfx->MeasureText(text, bar->labelFont);
    int pad = DpiScale(3);
    Rect r{at.x - (sz.dx / 2) - pad, at.y - (sz.dy / 2) - pad, sz.dx + (2 * pad), sz.dy + (2 * pad)};
    gfx->FillRoundedRect(r, DpiScale(3), MkRgb(255, 255, 255), kColMeasure);
    gfx->DrawText(text, r, gfxTextCenter | gfxTextVCenter, bar->labelFont, kColMeasure);
}

static void DrawCross(DisplayModel* dm, Gfx* gfx, int pageNo, PointF p) {
    Point s = dm->CvtToScreen(pageNo, p);
    int c = DpiScale(5);
    gfx->DrawLineAA({s.x - c, s.y}, {s.x + c, s.y}, kColMeasure, 1.5f);
    gfx->DrawLineAA({s.x, s.y - c}, {s.x, s.y + c}, kColMeasure, 1.5f);
}

struct WinMeasurementPainter {
    MeasurementToolbar* bar;
    Gfx* gfx;

    int Px(int value) const { return DpiScale(value); }
    void PushClip(Rect page) { gfx->PushClip(page); }
    void PopClip() { gfx->PopClip(); }
    void DrawLineAA(Point a, Point b, Color color, float width, u8 alpha = 255) {
        gfx->DrawLineAA(a, b, color, width, alpha);
    }
    void DrawLabel(Str text, Point at) { ::DrawLabel(bar, gfx, text, at); }
    void DrawCross(DisplayModel* dm, int pageNo, PointF p) { ::DrawCross(dm, gfx, pageNo, p); }
};

void PaintMeasurement(MainWindow* win, Gfx* gfx) {
    WinMeasurementPainter painter{win->measurementToolbar, gfx};
    PaintMeasurements(win->measurementToolbar, win->AsFixed(), painter);
}

//--- for tests

// state: "show=1 page=1 complete=1 count=2 scale=.. length=.. area=.. radius=.. unit=m x=.. y=.."
// length / area / radius are of the measurement being made, else the last one
TempStr TestMeasurement(MainWindow* win, Str action, int pageNo, PointF pt, Str length, Str unit) {
    auto* bar = win ? win->measurementToolbar : nullptr;
    if (!bar) {
        return StrL("show=0");
    }
    bool isClick = str::Eq(action, StrL("click"));
    bool isErase = str::Eq(action, StrL("erase"));
    DisplayModel* dm = win->AsFixed();
    if ((isClick || isErase) && dm && dm->ValidPageNo(pageNo)) {
        Point p = dm->CvtToScreen(pageNo, pt);
        if (isErase) {
            MeasurementOnClick(win, p, true);
        } else {
            LPARAM lp = MAKELPARAM(p.x, p.y);
            SendMessageW(win->hwndCanvas, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            SendMessageW(win->hwndCanvas, WM_LBUTTONUP, 0, lp);
        }
    } else if (str::Eq(action, StrL("apply"))) {
        bar->knownLength->SetText(length);
        bar->unitEdit->SetText(unit);
        bar->Calibrate();
    } else if (str::Eq(action, StrL("clear"))) {
        bar->Clear();
    } else if (str::Eq(action, StrL("reset"))) {
        bar->ResetScale();
    } else if (str::Eq(action, StrL("snap"))) {
        bar->snapCheck->SetIsChecked(pageNo != 0);
    } else if (!str::Eq(action, StrL("state"))) {
        MeasureTool tool = MeasureTool::Length;
        const char* names[] = {"length", "area", "radius", "guide", "calibrate"};
        for (int i = 0; i < dimofi(names); i++) {
            if (str::Eq(action, Str(names[i]))) {
                tool = (MeasureTool)i;
            }
        }
        CbSetCurrentSelection(bar->mode, (int)tool);
        bar->SetMode();
    }

    return MeasurementStateTemp(bar);
}
