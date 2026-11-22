/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
namespace gpui {
struct Ctx;
struct El;
struct PaintCtx;
} // namespace gpui

void ToggleMeasurement(MainWindow*);
void CloseMeasurement(MainWindow*);
bool MeasurementOnEscape(MainWindow*);
bool MeasurementOnClick(MainWindow*, Point, bool ctrl);
bool MeasurementOnMove(MainWindow*, Point);
void PaintMeasurement(MainWindow*, gpui::PaintCtx*, float x, float y, float scale);
gpui::El* BuildMeasurementFallback(MainWindow*, gpui::Ctx*);
TempStr TestMeasurement(MainWindow*, Str action, int pageNo, PointF, Str length, Str unit);
