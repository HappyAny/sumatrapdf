/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "gui/GpuiBridge.h"
#include "VirtKeys.h"
#include "base/UITask.h"
#include "base/GuessFileType.h"
#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/PlatformFont.h"
#include "Settings.h"
#include "DisplayMode.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "AIChatTypes.h"
#include "WindowTab.h"
#include "Theme.h"
#include "Menu.h"
#include "SumatraPDF.h"
#include "SumatraConfig.h"
#include "EutlTrust.h"
#include "SvgIcons.h"
#include "Translations.h"
#include "shared/PdfToolsCommon.h"
#include "gui/AppShell.h"
#include "gui/DialogWidgets.h"
#include "gui/Sidebar.h"
#include "gui/ToolWindow.h"
#if OS_WIN
#include <wincrypt.h>
#endif
#include "SignatureUI.h"

constexpr int kSigDialogDx = 620;
constexpr int kSigDialogDy = 460;
constexpr float kSigStatusFontPx = 16;
constexpr float kSigStatusIconPx = 24;
constexpr float kSigStatusGapPx = 10;
constexpr float kSigHeadPad = 12;
constexpr float kSigContentGap = 6;
constexpr float kSigEditMargin = 3;
constexpr float kSigFooterDy = 48;
constexpr float kEditLineRems = 1.25f;

bool TabHasSignatures(WindowTab* tab) {
    return tab && tab->signatures && len(tab->signatures->entries) > 0;
}

#if OS_WIN
struct SigStatusJob {
    WindowTab* tab = nullptr;
    EngineBase* engine = nullptr;
    bool needsEutl = false;
    StrVec trustedCerts;
    PdfSigResult* result = nullptr;
};

static void OnSigStatusDone(SigStatusJob* job) {
    defer {
        job->engine->Release();
        delete job;
    };
    WindowTab* tab = job->tab;
    if (!IsWindowTabValid(tab) || tab->GetEngine() != job->engine) {
        delete job->result;
        return;
    }
    tab->signaturesPending = false;
    delete tab->signatures;
    tab->signatures = job->result;
    MainWindow* win = tab->win;
    if (IsCurrentTab(win, tab)) {
        SidebarResetSignatureRows(win);
        SetTocVisibility(win, win->InPresentation() ? tab->showTocPresentation : tab->showToc);
        RebuildMenuBar(win);
        AppShellInvalidate(win);
    }
}

static void SigStatusThread(SigStatusJob* job) {
    if (job->needsEutl && EutlNeedsUpdate()) {
        EutlUpdate(nullptr);
    }
    Vec<Str> trusted;
    for (int i = 0; i < len(job->trustedCerts); i++) {
        VecAppend(trusted, job->trustedCerts.At(i));
    }
    job->result = EngineMupdfCheckSignatures(job->engine, &trusted);
    uitask::Post(MkFunc0<SigStatusJob>(OnSigStatusDone, job), "SigStatusDone");
}
#endif

void CheckTabSignatures(WindowTab* tab) {
#if OS_WIN
    if (!tab) {
        return;
    }
    delete tab->signatures;
    tab->signatures = nullptr;
    tab->signaturesPending = false;
    EngineBase* engine = tab->GetEngine();
    if (gPluginMode || !engine) {
        return;
    }
    bool isPdf = EngineMupdfIsPdf(engine);
    bool signedDoc = isPdf ? EngineMupdfCountSignedSignatures(engine) > 0 : EngineMupdfHasOfdSignatures(engine);
    if (!signedDoc) {
        return;
    }
    EutlRegisterLookup();
    auto* job = new SigStatusJob;
    job->tab = tab;
    job->engine = engine;
    job->needsEutl = isPdf;
    if (gSettings->ofdTrustedCertificates) {
        for (Str path : *gSettings->ofdTrustedCertificates) {
            job->trustedCerts.Append(path);
        }
    }
    tab->signaturesPending = true;
    engine->AddRef();
    RunAsync(MkFunc0<SigStatusJob>(SigStatusThread, job), StrL("SigStatus"));
#else
    (void)tab;
#endif
}

static Color VerdictColor(SigVerdict verdict) {
    if (verdict == SigVerdict::Invalid) {
        return MkRgb(0xc6, 0x28, 0x28);
    }
    if (verdict == SigVerdict::Unknown) {
        return MkRgb(0xb5, 0x80, 0x00);
    }
    return MkRgb(0x1a, 0x7f, 0x37);
}

gp::El* SignatureIcon(gp::Ctx* cx, SigVerdict verdict, float dx) {
    const char* svg = gIconSigValid;
    if (verdict == SigVerdict::Invalid) {
        svg = gIconSigInvalid;
    } else if (verdict == SigVerdict::Unknown) {
        svg = gIconSigProblem;
    }
    return gpc::Icon::Empty(cx)->Data(ToGpui(Str(svg)))->Size(dx)->Color(ToGpui(VerdictColor(verdict)))->IntoEl();
}

static TempStr SigDetailsText(const PdfSigEntry* entry) {
    str::Builder out;
    out.Append(Tr("Validity Summary"));
    out.AppendChar('\n');
    StrVec lines;
    Split(&lines, entry->validity, StrL("\n"), true);
    for (Str line : lines) {
        str::TrimWsBoth(line);
        out.Append(line);
        out.AppendChar('\n');
    }
    out.AppendChar('\n');
    out.Append(Tr("Signer Info"));
    out.AppendChar('\n');
    StrVec all;
    Split(&all, entry->details, StrL("\n"), true);
    for (int i = 1; i < len(all); i++) {
        Str line = all[i];
        if (str::StartsWith(line, StrL("  Status:")) || str::Contains(entry->validity, line)) {
            continue;
        }
        str::TrimWsBoth(line);
        if (str::EndsWith(line, StrL(":"))) {
            continue;
        }
        out.Append(line);
        out.AppendChar('\n');
    }
    return ToStrTemp(out);
}

struct SigDialog {
    ToolWindow* window = nullptr;
    gp::InputState* text = nullptr;
    Str status;
    Str cert;
    SigVerdict verdict = SigVerdict::Unknown;
    float lineDy = 18;
};
static SigDialog gSigDialog;

static void ClearSigDialog() {
    delete gSigDialog.text;
    str::Free(gSigDialog.status);
    str::Free(gSigDialog.cert);
    gSigDialog = {};
}

static void CloseSigDialog() {
    ToolWindow* window = gSigDialog.window;
    if (window) {
        if (gp::Window* host = ToolWindowGpui(window)) {
            gp::InputBlur(gSigDialog.text, host->app, host);
        }
        ToolWindowClose(window);
    }
    ClearSigDialog();
}

struct SigDialogView {
    static void OnClose(SigDialogView*, gp::Ctx*, const gp::ClickEvent*) { CloseSigDialog(); }
#if OS_WIN
    static void OnCert(SigDialogView*, gp::Ctx* cx, const gp::ClickEvent*) {
        Str der = gSigDialog.cert;
        PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)der.s, (DWORD)len(der));
        if (!cert) {
            return;
        }
        using Fn = BOOL(WINAPI*)(DWORD, const void*, HWND, LPCWSTR, DWORD, void*);
        HMODULE dll = LoadLibraryW(L"cryptui.dll");
        if (dll) {
            Fn view = (Fn)GetProcAddress(dll, "CryptUIDlgViewContext");
            if (view) {
                view(CERT_STORE_CERTIFICATE_CONTEXT, cert, (HWND)gp::PlatWindowHandle(cx->win), L"Certificate", 0,
                     nullptr);
            }
            FreeLibrary(dll);
        }
        CertFreeCertificateContext(cert);
    }
#endif
};

static gp::El* BuildSigDialog(MainWindow*, gp::Ctx* cx) {
    if (!gSigDialog.text) {
        return nullptr;
    }
    static gp::Entity<SigDialogView> view;
    if (!view.IsValid()) {
        view = gp::EntityNewState<SigDialogView>(cx->app);
    }
    float rem = gSigDialog.lineDy / kEditLineRems;
    gp::WindowSetRemSize(cx->win, rem);
    float remScale = 16.f / rem;
    float buttonFont = (float)kToolDlgTextFontPx * remScale;
    Color bg = ThemeWindowControlBackgroundColor();
    Color fg = ThemeWindowTextColor();
    gp::El* head = gp::Div(cx->a)
                       ->FlexRow()
                       ->W(gp::kFill)
                       ->Shrink0()
                       ->ItemsCenter()
                       ->PadX(kToolDlgPadDx)
                       ->PadT(kSigHeadPad)
                       ->Gap(kSigStatusGapPx)
                       ->Child(SignatureIcon(cx, gSigDialog.verdict, kSigStatusIconPx))
                       ->Child(gp::TextEl(cx->a, GpuiDup(cx->a, gSigDialog.status))
                                   ->Font(kSigStatusFontPx * remScale)
                                   ->Fg(ToGpui(VerdictColor(gSigDialog.verdict))));
    const gp::Theme& th = gp::ThemeNow(cx->app);
    gp::InputEditorStyle textStyle;
    textStyle.fontSize = buttonFont;
    textStyle.fontFamily = gp::FontFamilyIntern(GStrL("Segoe UI"));
    textStyle.foreground = ToGpui(fg);
    textStyle.caret = th.caret;
    textStyle.selection = gp::RgbaOpacity(th.selection, 0.4f);
    float headDy = kSigHeadPad + std::max(kSigStatusIconPx, gSigDialog.lineDy);
    float available = gp::WindowSize(cx->win).dipH - headDy - kSigFooterDy - 4 * kSigContentGap;
    gp::InputState* state = gSigDialog.text;
    state->viewH = std::max(available, gSigDialog.lineDy);
    gp::El* edit = gp::InputBase::New(cx, GStrL("signature-details"), true, gp::AccessibilityRole::MultilineTextInput)
                       ->BindInput(state)
                       ->AriaValue(gp::InputValue(state))
                       ->W(gp::kFill)
                       ->H(state->viewH)
                       ->Shrink0()
                       ->PadX(kSigEditMargin)
                       ->ClipY()
                       ->ScrollY(state->scrollY)
                       ->ScrollFromPath()
                       ->Bg(ToGpui(bg))
                       ->Child(gp::Textarea::New(cx, state, textStyle));
    gp::El* content =
        gp::Div(cx->a)->W(gp::kFill)->Flex1()->MinH(0)->PadX(kToolDlgPadDx)->PadY(kSigContentGap)->Child(edit);
    gp::El* cert = nullptr;
#if OS_WIN
    if (len(gSigDialog.cert) > 0) {
        cert = gpc::Button::New(cx, GStrL("signature-cert"))
                   ->Label(ToGpui(Tr("Show Signer's Certificate...")))
                   ->Outline()
                   ->OnClick(gp::ListenTo(view, &SigDialogView::OnCert))
                   ->IntoEl();
    }
#endif
    gp::El* foot = DlgFooter(cx, Tr("Close"), gp::ListenTo(view, &SigDialogView::OnClose), {}, cert);
    return gp::Div(cx->a)
        ->FlexCol()
        ->SizeFull()
        ->Bg(ToGpui(bg))
        ->Gap(kSigContentGap)
        ->Child(head)
        ->Child(content)
        ->Child(foot);
}

void ShowSignatureDialog(MainWindow* win, const PdfSigEntry* entry) {
    if (!win || !entry || !ToolWindowsAvailable()) {
        return;
    }
    CloseSigDialog();
    gSigDialog.text = new gp::InputState;
    gSigDialog.text->focus = gp::FocusHandleNew(win->gpuiWin->app);
    gSigDialog.text->kind = gp::InputKind::Textarea;
    gSigDialog.text->mode.kind = gp::LayoutModeKind::PlainText;
    gSigDialog.text->readonly = true;
    TempStr text = SigDetailsText(entry);
    gp::InputSetValue(gSigDialog.text, ToGpui(text));
    int dpi = std::max(96, AppShellWindowDpi(win));
    PlatformFont* font = GetUserGuiFont(StrL("Segoe UI"), MulDiv(kToolDlgTextFontPx, dpi, 96));
    gSigDialog.lineDy = (float)std::max(1, PlatformFontLineHeight(font)) * 96.f / (float)dpi;
    Size measured = PlatformFontMeasureText(font, text);
    int dx = std::max(kSigDialogDx, MulDiv(measured.dx, 96, dpi) + 2 * (kToolDlgPadDx + (int)kSigEditMargin));
    gSigDialog.status = str::Dup(entry->status);
    gSigDialog.cert = str::Dup(entry->certDer);
    gSigDialog.verdict = entry->verdict;
    ToolWindowDesc desc;
    desc.name = "signature-properties";
    desc.title = +[] { return Tr("Signature Properties"); };
    desc.build = BuildSigDialog;
    desc.onKey =
        +[](MainWindow*, gp::Ctx* cx, const gp::KeyEvent* ev) { return ToolWindowDialogKey(cx, ev, CloseSigDialog); };
    desc.onClosed = +[](MainWindow*) {
        // A caption close can finish after a new dialog opens.
        if (gSigDialog.window && !ToolWindowIsLive(gSigDialog.window)) {
            ClearSigDialog();
        }
    };
    gSigDialog.window = ToolWindowOpen(desc, win, ToolWindowCenteredRect(desc, win, {dx, kSigDialogDy}));
    if (!gSigDialog.window) {
        ClearSigDialog();
    }
}

TempStr SignatureUiStateTemp(MainWindow* win, Str action) {
    WindowTab* tab = win->CurrentTab();
    PdfSigResult* signatures = tab ? tab->signatures : nullptr;
    if (str::StartsWith(action, StrL("row "))) {
        SidebarSelectSignature(win, ParseInt(Str(action.s + 4, len(action) - 4)));
    } else if (str::StartsWith(action, StrL("open ")) && signatures) {
        int row = ParseInt(Str(action.s + 5, len(action) - 5));
        if (row >= 0 && row < len(signatures->entries)) {
            ShowSignatureDialog(win, signatures->entries[row]);
        }
    }
    int count = signatures ? len(signatures->entries) : 0;
    bool shown = SidebarContentVisible(win, SidebarContent::Signatures);
    str::Builder out;
    out.Append(fmt("rows=%d\npanel=%d\nlistFocused=%d\npage=%d\ndialog=%d\n", count, shown ? 1 : 0,
                   shown && SidebarPanelHasFocus(win, SidebarSlot::Top) ? 1 : 0,
                   win->ctrl ? win->ctrl->CurrentPageNo() : 0, gSigDialog.window ? 1 : 0));
    if (gSigDialog.text) {
        out.Append(FromGpui(gp::InputValue(gSigDialog.text)));
    }
    return ToStrTemp(out);
}
