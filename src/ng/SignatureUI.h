/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

namespace gpui {
struct Ctx;
struct El;
} // namespace gpui

struct MainWindow;
struct WindowTab;
struct PdfSigEntry;
enum class SigVerdict;

void CheckTabSignatures(WindowTab*);
bool TabHasSignatures(WindowTab*);
gpui::El* SignatureIcon(gpui::Ctx*, SigVerdict, float);
void ShowSignatureDialog(MainWindow*, const PdfSigEntry*);
TempStr SignatureUiStateTemp(MainWindow*, Str action);
