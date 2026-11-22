/* Copyright 2026. License: GPL-3.0-only */

struct MainWindow;
namespace gpui {
struct Ctx;
struct El;
} // namespace gpui

void ShowTranslatePopup(MainWindow* win);
void UpdateTranslatePopup(MainWindow* win);
void HideTranslatePopup(MainWindow* win);
void DismissTranslatePopup(MainWindow* win);
void DeleteTranslatePopup(MainWindow* win);
Rect TranslateToolbarScreenRect(MainWindow* win);
gpui::El* TranslateArrowBuild(MainWindow* win, gpui::Ctx* cx);
