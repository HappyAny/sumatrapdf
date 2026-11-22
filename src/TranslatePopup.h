/* Copyright 2026. License: GPL-3.0-only */

struct MainWindow;

HWND NewTranslateScrollPane(HWND parent);
void RepaintTranslateScrollPane(HWND pane);
void ShowTranslatePopup(MainWindow* win);
void UpdateTranslatePopup(MainWindow* win);
void HideTranslatePopup(MainWindow* win);
void DismissTranslatePopup(MainWindow* win);
void DeleteTranslatePopup(MainWindow* win);
bool IsTranslatePopupVisible(MainWindow* win);
bool TranslatePopupKey(const MSG& msg);
bool TranslatePopupAction(MainWindow* win, Str action);
TempStr TranslatePopupStateTemp(MainWindow* win);
