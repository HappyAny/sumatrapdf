/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include <float.h>

namespace measure {

enum class MeasureTool {
    Length,
    Area,
    Radius,
    Guide,
    Calibrate,
};

constexpr double kPointsPerInch = 72;
// how near (in screen pixels) a click must be to snap to a point or hit a measurement
constexpr int kSnapPx = 8;
constexpr int kHitPx = 6;
// segments a radius arc is drawn with
constexpr int kArcSegments = 64;

inline constexpr Color kColMeasure = MkRgb(220, 50, 30);
inline constexpr Color kColGuide = MkRgb(120, 120, 120);

struct Measurement {
    MeasureTool tool = MeasureTool::Length;
    int pageNo = 0;
    PointF pts[3];
};

struct MeasurementModel {
    MeasureTool tool = MeasureTool::Length;

    Vec<Measurement> done;
    // the one being clicked: its first nPts points are set
    Measurement cur;
    int nPts = 0;
    PointF hover;
    bool haveHover = false;
    // the last two-point line drawn with Calibrate
    Measurement calib;
    bool haveCalib = false;

    // snap points of one page, computed when that page is first hovered
    int snapPageNo = 0;
    Vec<PointF> snapPts;
    PointF lastPt;

    double scale = 1;
    Str unit;
    bool snap = true;

    ~MeasurementModel() { str::Free(unit); }
};

inline int PointsNeeded(MeasureTool tool) {
    return tool == MeasureTool::Radius ? 3 : 2;
}

inline double Dist(PointF a, PointF b) {
    double dx = (double)b.x - a.x;
    double dy = (double)b.y - a.y;
    return sqrt((dx * dx) + (dy * dy));
}

// center and radius of the circle through a, b, c; false if they're on a line
inline bool Circle(PointF a, PointF b, PointF c, PointF* center, double* r) {
    double ax = a.x, ay = a.y, bx = b.x, by = b.y, cx = c.x, cy = c.y;
    double d = 2 * ((ax * (by - cy)) + (bx * (cy - ay)) + (cx * (ay - by)));
    if (fabs(d) < 1e-9) {
        return false;
    }
    double a2 = (ax * ax) + (ay * ay);
    double b2 = (bx * bx) + (by * by);
    double c2 = (cx * cx) + (cy * cy);
    double ux = ((a2 * (by - cy)) + (b2 * (cy - ay)) + (c2 * (ay - by))) / d;
    double uy = ((a2 * (cx - bx)) + (b2 * (ax - cx)) + (c2 * (bx - ax))) / d;
    *center = PointF((float)ux, (float)uy);
    *r = Dist(*center, a);
    return true;
}

// the points of m that are set, with the cursor as the next one while it's
// being clicked
inline int MeasurementPoints(const MeasurementModel* bar, const Measurement& m, bool inProgress, PointF* pts) {
    int n = PointsNeeded(m.tool);
    if (!inProgress) {
        for (int i = 0; i < n; i++) {
            pts[i] = m.pts[i];
        }
        return n;
    }
    for (int i = 0; i < bar->nPts; i++) {
        pts[i] = m.pts[i];
    }
    int k = bar->nPts;
    if (bar->haveHover && k < n) {
        pts[k++] = bar->hover;
    }
    return k;
}

//--- arithmetic in the known length field: "100-20", "2*50", "25/2", "(3+1)*2"

struct ExprParser {
    const char* s;
    bool ok = true;

    void SkipWs() {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
    }

    double Factor() {
        SkipWs();
        if (*s == '-') {
            s++;
            return -Factor();
        }
        if (*s == '(') {
            s++;
            double v = Expr();
            SkipWs();
            if (*s != ')') {
                ok = false;
                return 0;
            }
            s++;
            return v;
        }
        char* end = nullptr;
        double v = strtod(s, &end);
        if (end == s) {
            ok = false;
            return 0;
        }
        s = end;
        return v;
    }

    double Term() {
        double v = Factor();
        for (;;) {
            SkipWs();
            if (*s == '*') {
                s++;
                v *= Factor();
            } else if (*s == '/') {
                s++;
                v /= Factor();
            } else {
                return v;
            }
        }
    }

    double Expr() {
        double v = Term();
        for (;;) {
            SkipWs();
            if (*s == '+') {
                s++;
                v += Term();
            } else if (*s == '-') {
                s++;
                v -= Term();
            } else {
                return v;
            }
        }
    }
};

inline bool EvalLength(Str text, double* out) {
    ExprParser p{CStrTemp(text)};
    double v = p.Expr();
    p.SkipWs();
    if (!p.ok || *p.s != 0) {
        return false;
    }
    *out = v;
    return true;
}

inline bool SetMeasurementScale(MeasurementModel* bar, Str length, Str units) {
    if (!bar->haveCalib) {
        return false;
    }
    double dist = Dist(bar->calib.pts[0], bar->calib.pts[1]);
    str::TrimWsBoth(length);
    str::TrimWsBoth(units);
    double value = 0;
    bool parsed = len(length) > 0 && EvalLength(length, &value);
    double ratio = dist > 0 ? value / dist : 0;
    if (!parsed || !isfinite(value) || value <= 0 || !isfinite(ratio) || ratio <= 0 || ratio > FLT_MAX ||
        (float)ratio == 0 || len(units) == 0) {
        return false;
    }
    bar->scale = (float)ratio;
    str::ReplaceWithCopy(&bar->unit, units);
    return true;
}

inline TempStr MeasurementTextTemp(const MeasurementModel* bar, const Measurement& m, int n, const PointF* pts) {
    double s = bar->scale;
    Str u = bar->unit;
    // nothing to say yet about a measurement whose points are the same
    if (n < 2 || Dist(pts[0], pts[1]) <= 0) {
        return {};
    }
    switch (m.tool) {
        case MeasureTool::Length:
        case MeasureTool::Calibrate: {
            double dx = fabs((double)pts[1].x - pts[0].x) * s;
            double dy = fabs((double)pts[1].y - pts[0].y) * s;
            return fmt("%.4g %s (x %.4g, y %.4g)", Dist(pts[0], pts[1]) * s, u, dx, dy);
        }
        case MeasureTool::Area: {
            double dx = fabs((double)pts[1].x - pts[0].x) * s;
            double dy = fabs((double)pts[1].y - pts[0].y) * s;
            return fmt("%.4g \xC3\x97 %.4g = %.4g %s\xC2\xB2", dx, dy, dx * dy, u);
        }
        case MeasureTool::Radius: {
            PointF c;
            double r = 0;
            if (n < 3 || !Circle(pts[0], pts[1], pts[2], &c, &r)) {
                return {};
            }
            return fmt("R %.4g %s", r * s, u);
        }
        case MeasureTool::Guide:
            return {};
    }
    return {};
}

// pixels at the screen point pt, in page units of pageNo
inline double ScreenToPageDist(DisplayModel* dm, int pageNo, Point pt, int px) {
    PointF a = dm->CvtFromScreen(pt, pageNo);
    PointF b = dm->CvtFromScreen(Point{pt.x + px, pt.y}, pageNo);
    return Dist(a, b);
}

// pt (on the screen) as a page point, moved to the nearest line end in reach
inline PointF SnappedPoint(MeasurementModel* bar, DisplayModel* dm, int pageNo, Point pt, int reachPx) {
    PointF p = dm->CvtFromScreen(pt, pageNo);
    if (!bar->snap) {
        return p;
    }
    if (bar->snapPageNo != pageNo) {
        bar->snapPageNo = pageNo;
        EngineMupdfPagePathPoints(dm->GetEngine(), pageNo, bar->snapPts);
    }
    double reach = ScreenToPageDist(dm, pageNo, pt, reachPx);
    double best = reach * reach;
    PointF res = p;
    for (PointF q : bar->snapPts) {
        double dx = (double)q.x - p.x;
        double dy = (double)q.y - p.y;
        double d = (dx * dx) + (dy * dy);
        if (d <= best) {
            best = d;
            res = q;
        }
    }
    return res;
}

inline double DistToSegment(PointF p, PointF a, PointF b) {
    double dx = (double)b.x - a.x;
    double dy = (double)b.y - a.y;
    double l2 = (dx * dx) + (dy * dy);
    if (l2 <= 0) {
        return Dist(p, a);
    }
    double t = (((double)p.x - a.x) * dx + ((double)p.y - a.y) * dy) / l2;
    t = limitValue(t, 0.0, 1.0);
    return Dist(p, PointF((float)(a.x + t * dx), (float)(a.y + t * dy)));
}

// how far p is from what m draws
inline double DistToMeasurement(PointF p, const Measurement& m) {
    const PointF* q = m.pts;
    switch (m.tool) {
        case MeasureTool::Area: {
            PointF c1(q[1].x, q[0].y);
            PointF c2(q[0].x, q[1].y);
            double d = DistToSegment(p, q[0], c1);
            d = std::min(d, DistToSegment(p, c1, q[1]));
            d = std::min(d, DistToSegment(p, q[1], c2));
            return std::min(d, DistToSegment(p, c2, q[0]));
        }
        case MeasureTool::Radius: {
            PointF c;
            double r = 0;
            if (!Circle(q[0], q[1], q[2], &c, &r)) {
                return DBL_MAX;
            }
            return fabs(Dist(p, c) - r);
        }
        case MeasureTool::Guide: {
            // the line through the two points, not just between them
            double dx = (double)q[1].x - q[0].x;
            double dy = (double)q[1].y - q[0].y;
            double l = sqrt((dx * dx) + (dy * dy));
            if (l <= 0) {
                return Dist(p, q[0]);
            }
            return fabs((dx * ((double)q[0].y - p.y)) - (dy * ((double)q[0].x - p.x))) / l;
        }
        default:
            return DistToSegment(p, q[0], q[1]);
    }
}

// Ctrl + click: deletes the measurement nearest to pt, if one is in reach
inline bool EraseAt(MeasurementModel* bar, DisplayModel* dm, int pageNo, Point pt, int reachPx) {
    PointF p = dm->CvtFromScreen(pt, pageNo);
    double best = ScreenToPageDist(dm, pageNo, pt, reachPx);
    int found = -1;
    for (int i = 0; i < len(bar->done); i++) {
        if (bar->done[i].pageNo != pageNo) {
            continue;
        }
        double d = DistToMeasurement(p, bar->done[i]);
        if (d <= best) {
            best = d;
            found = i;
        }
    }
    if (found < 0) {
        return false;
    }
    VecRemoveAt(bar->done, found);
    return true;
}

inline void AddPoint(MeasurementModel* bar, int pageNo, PointF p) {
    bar->lastPt = p;
    if (bar->nPts == 0) {
        bar->cur = {};
        bar->cur.tool = bar->tool;
        bar->cur.pageNo = pageNo;
    }
    bar->cur.pts[bar->nPts++] = p;
    bar->hover = p;
    bar->haveHover = true;
    if (bar->nPts == PointsNeeded(bar->tool)) {
        if (bar->tool == MeasureTool::Calibrate) {
            bar->calib = bar->cur;
            bar->haveCalib = true;
        } else {
            VecAppend(bar->done, bar->cur);
        }
        bar->nPts = 0;
    }
}

template <class Painter>
void PaintOne(const MeasurementModel* bar, DisplayModel* dm, Painter& gfx, const Measurement& m, bool inProgress) {
    PointF pts[3];
    int n = MeasurementPoints(bar, m, inProgress, pts);
    int pageNo = m.pageNo;
    float w = (float)gfx.Px(2);
    auto toScreen = [&](PointF p) { return dm->CvtToScreen(pageNo, p); };
    for (int i = 0; i < n; i++) {
        gfx.DrawCross(dm, pageNo, pts[i]);
    }
    if (n < 2) {
        return;
    }
    Point a = toScreen(pts[0]);
    Point b = toScreen(pts[1]);
    TempStr text = MeasurementTextTemp(bar, m, n, pts);
    switch (m.tool) {
        case MeasureTool::Length:
        case MeasureTool::Calibrate:
            gfx.DrawLineAA(a, b, kColMeasure, w);
            gfx.DrawLabel(text, Point{(a.x + b.x) / 2, (a.y + b.y) / 2 - gfx.Px(14)});
            break;
        case MeasureTool::Area: {
            Point c1 = toScreen(PointF(pts[1].x, pts[0].y));
            Point c2 = toScreen(PointF(pts[0].x, pts[1].y));
            gfx.DrawLineAA(a, c1, kColMeasure, w);
            gfx.DrawLineAA(c1, b, kColMeasure, w);
            gfx.DrawLineAA(b, c2, kColMeasure, w);
            gfx.DrawLineAA(c2, a, kColMeasure, w);
            gfx.DrawLabel(text, Point{(a.x + b.x) / 2, (a.y + b.y) / 2});
            break;
        }
        case MeasureTool::Radius: {
            PointF c;
            double r = 0;
            if (n < 3 || !Circle(pts[0], pts[1], pts[2], &c, &r)) {
                gfx.DrawLineAA(a, b, kColMeasure, 1.0f, 128);
                break;
            }
            // the arc from the first point to the second through the third
            double a0 = atan2((double)pts[0].y - c.y, (double)pts[0].x - c.x);
            double a1 = atan2((double)pts[1].y - c.y, (double)pts[1].x - c.x);
            double a2 = atan2((double)pts[2].y - c.y, (double)pts[2].x - c.x);
            double twoPi = 2 * M_PI;
            double d1 = fmod(a1 - a0 + (2 * twoPi), twoPi);
            double d2 = fmod(a2 - a0 + (2 * twoPi), twoPi);
            double sweep = d2 < d1 ? d1 : d1 - twoPi;
            Point prev = a;
            for (int i = 1; i <= kArcSegments; i++) {
                double t = a0 + (sweep * i / kArcSegments);
                Point p = toScreen(PointF((float)(c.x + (r * cos(t))), (float)(c.y + (r * sin(t)))));
                gfx.DrawLineAA(prev, p, kColMeasure, w);
                prev = p;
            }
            Point cs = toScreen(c);
            Point mid = toScreen(pts[2]);
            gfx.DrawLineAA(cs, mid, kColMeasure, 1.0f, 128);
            gfx.DrawCross(dm, pageNo, c);
            gfx.DrawLabel(text, Point{(cs.x + mid.x) / 2, (cs.y + mid.y) / 2});
            break;
        }
        case MeasureTool::Guide: {
            // as long as the page is wide, both ways; the page clip cuts it
            RectF page = dm->GetEngine()->PageMediabox(pageNo);
            double l = Dist(pts[0], pts[1]);
            if (l <= 0) {
                break;
            }
            double ext = (page.dx + page.dy) / l;
            float ex = (float)((pts[1].x - pts[0].x) * ext);
            float ey = (float)((pts[1].y - pts[0].y) * ext);
            Point g0 = toScreen(PointF(pts[0].x - ex, pts[0].y - ey));
            Point g1 = toScreen(PointF(pts[0].x + ex, pts[0].y + ey));
            gfx.DrawLineAA(g0, g1, kColGuide, 1.0f, 200);
            break;
        }
    }
}

template <class Painter>
void PaintMeasurements(const MeasurementModel* bar, DisplayModel* dm, Painter& gfx) {
    if (!bar || !dm) {
        return;
    }
    auto paint = [&](const Measurement& m, bool inProgress) {
        if (!dm->PageVisible(m.pageNo)) {
            return;
        }
        Rect page = dm->CvtToScreen(m.pageNo, dm->GetEngine()->PageMediabox(m.pageNo));
        gfx.PushClip(page);
        PaintOne(bar, dm, gfx, m, inProgress);
        gfx.PopClip();
    };
    for (const Measurement& m : bar->done) {
        paint(m, false);
    }
    if (bar->haveCalib && bar->tool == MeasureTool::Calibrate && bar->nPts == 0) {
        paint(bar->calib, false);
    }
    if (bar->nPts > 0) {
        paint(bar->cur, true);
    }
}

inline TempStr MeasurementStateTemp(const MeasurementModel* bar) {
    const Measurement* m = nullptr;
    bool inProgress = bar->nPts > 0;
    if (inProgress) {
        m = &bar->cur;
    } else if (bar->tool == MeasureTool::Calibrate && bar->haveCalib) {
        m = &bar->calib;
    } else if (len(bar->done) > 0) {
        m = &bar->done[len(bar->done) - 1];
    }
    double lengthV = 0, area = 0, radius = 0;
    int page = 0;
    if (m) {
        PointF pts[3];
        int n = MeasurementPoints(bar, *m, inProgress, pts);
        page = m->pageNo;
        if (n >= 2) {
            lengthV = Dist(pts[0], pts[1]) * bar->scale;
            area = fabs(((double)pts[1].x - pts[0].x) * ((double)pts[1].y - pts[0].y)) * bar->scale * bar->scale;
        }
        PointF c;
        if (n >= 3 && Circle(pts[0], pts[1], pts[2], &c, &radius)) {
            radius *= bar->scale;
        }
    }
    return fmt("show=1 page=%d complete=%d count=%d scale=%.9g length=%.9g area=%.9g radius=%.9g unit=%s x=%.6g y=%.6g",
               page, inProgress ? 0 : 1, len(bar->done), bar->scale, lengthV, area, radius, bar->unit, bar->lastPt.x,
               bar->lastPt.y);
}

} // namespace measure
