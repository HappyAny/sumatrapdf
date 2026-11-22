import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, type ControlClient, withControlledSumatra } from "./control.ts";
import { IS_WIN } from "./host.ts";
import { assemblePdf, EXE, runStandalone, tmpPath } from "./util.ts";
import { findTopWindow, findVisibleChildWindow, getControlText, sendMessage, VK_RETURN, WM_CHAR } from "./winapi.ts";

const EM_SETSEL = 0x00b1;

function pdfWithPageLabels() {
  const pages = Array.from({ length: 8 }, () => "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>");
  const kids = pages.map((_, i) => `${i + 3} 0 R`).join(" ");
  return assemblePdf([
    "<< /Type /Catalog /Pages 2 0 R /PageLabels << /Nums [0 << /S /r >> 2 << /S /D /St 279 >>] >> >>",
    `<< /Type /Pages /Kids [${kids}] /Count ${pages.length} >>`,
    ...pages,
  ]);
}

async function request(client: ControlClient, cmd: ControlCommand, ...args: (string | number)[]) {
  const [code, value] = await client.request(cmd, args);
  assert.equal(code, 0, String(value));
  return String(value);
}

async function waitForPageBox(client: ControlClient) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const ui = await request(client, ControlCommand.TestUiState);
    if (ui.includes("pageBox=1")) {
      return ui;
    }
    await Bun.sleep(40);
  }
  throw new Error("The toolbar page box did not receive focus");
}

async function goToLabel(client: ControlClient, edit: number, from: string, label: string, page: number) {
  await request(client, ControlCommand.TestInvokeCommand, "CmdGoToPage");
  if (!edit) {
    assert((await waitForPageBox(client)).includes(`pageText='${from}'`));
    for (const ch of label) {
      await request(client, ControlCommand.TestInput, "char", ch.charCodeAt(0));
    }
    assert((await request(client, ControlCommand.TestUiState)).includes(`pageText='${label}'`));
    await request(client, ControlCommand.TestInput, "key", VK_RETURN, 0);
  } else {
    assert.equal(getControlText(edit), from);
    sendMessage(edit, EM_SETSEL, 0, -1);
    for (const ch of label) {
      sendMessage(edit, WM_CHAR, ch.charCodeAt(0), 0);
    }
    assert.equal(getControlText(edit), label);
    sendMessage(edit, WM_CHAR, VK_RETURN, 0);
  }
  await client.waitForRenderIdle();
  const current = await request(client, ControlCommand.TestCurrentTab);
  assert(current.endsWith(` page=${page}`), `Expected label '${label}' on physical page ${page}: ${current}`);
}

export async function testit() {
  const appdata = mkdtempSync(tmpPath("toolbar-page-labels-"));
  const file = join(appdata, "labels.pdf");
  writeFileSync(file, pdfWithPageLabels());
  writeFileSync(
    join(appdata, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nDefaultDisplayMode = single page\nDefaultZoom = fit page\nShowToolbar = true\n",
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      await client.waitForRenderIdle();
      let edit = 0;
      const legacyFrame = IS_WIN ? findTopWindow(proc.pid, "SUMATRA_PDF_FRAME") : 0;
      if (legacyFrame) {
        const toolbar = findVisibleChildWindow(legacyFrame, "SUMATRA_VIRT_TOOLBAR");
        assert(toolbar, "The toolbar should be visible");
        edit = findVisibleChildWindow(toolbar, "Edit");
        assert(edit, "The toolbar should have a page box");
      }
      await goToLabel(client, edit, "279", "281", 5);
      await goToLabel(client, edit, "281", "ii", 2);
    },
    ["-appdata", appdata, "-page", "3", file],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
