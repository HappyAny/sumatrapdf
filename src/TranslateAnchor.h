/* License: GPL-3.0-only */

enum class TranslatePosition {
    Auto,
    Top,
    Bottom,
    Left,
    Right
};

struct TranslateCallout {
    Rect popup;
    Point target;
    TranslatePosition side = TranslatePosition::Right;
};

TranslatePosition ParseTranslatePosition(Str value);
Str TranslatePositionName(TranslatePosition value);
TranslateCallout PlaceTranslateCallout(Rect anchor, Size popupSize, Rect work, TranslatePosition preferred, int dpi,
                                       Rect toolbar = {}, int minHeight = 1);

struct TranslateArrow;
TranslateArrow* CreateTranslateArrow(HWND owner);
void UpdateTranslateArrow(TranslateArrow*, const TranslateCallout&, Rect work, Rect toolbar, int dpi, Color fill,
                          Color border);
void HideTranslateArrow(TranslateArrow*);
void DeleteTranslateArrow(TranslateArrow*);
Rect TranslateArrowRect(TranslateArrow*);
bool TranslateArrowVisible(TranslateArrow*);
Point TranslateArrowTip(TranslateArrow*);
