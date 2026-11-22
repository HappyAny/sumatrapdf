// The toolbar wraps its buttons onto a second row when the window is too
// narrow for one, instead of cutting off the buttons on the right. The Edit
// PDF row goes below the wrapped rows.
//
// Run: bun tests/toolbar-wrap.ts [--no-build]

import { mkdtempSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { type ControlClient, ControlCommand, withControlledSumatra } from "./control.ts";
import { IS_WIN } from "./host.ts";
import { cmdId, EXE, ROOT, runStandalone, tmpPath } from "./util.ts";
import { captureWindowToPng, findChildWindow, findTopWindow, sleep } from "./winapi.ts";

// wide enough for one row, and too narrow for it
const kWideDx = 1400;
const kNarrowDx = 420;

async function toolbarDy(client: ControlClient): Promise<number> {
  const raw = String((await client.request(ControlCommand.TestLayout, []))[1] ?? "");
  const m = /item name=toolbar visible=1 rect=-?\d+,-?\d+,\d+,(\d+)/.exec(raw);
  if (!m) {
    throw new Error(`toolbar-wrap: no toolbar in the layout: ${raw}`);
  }
  return +m[1]!;
}

// toolbar height with and without the Edit PDF row
async function measure(dx: number, png: string): Promise<{ dy: number; editDy: number; lastRowOffset: number }> {
  const pdf = join(ROOT, "ext", "a-zlib", "zlib.3.pdf");
  const appdata = mkdtempSync(tmpPath("toolbar-wrap-"));
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nShowToolbar = true\n");
  return withControlledSumatra(
    EXE,
    async (client, proc) => {
      await client.waitForRenderIdle();
      await sleep(200);
      const dy = await toolbarDy(client);
      const [, buttons] = await client.request(ControlCommand.TestToolbarButtons);
      const buttonTop = (name: string) => {
        const cmd = cmdId(name);
        const m = new RegExp(`^idx=\\d+ cmd=${cmd} hidden=0 rect=-?\\d+,(-?\\d+),-?\\d+,-?\\d+`, "m").exec(
          String(buttons),
        );
        if (!m) {
          throw new Error(`toolbar-wrap: no visible ${name} button`);
        }
        return Number(m[1]);
      };
      const lastRowOffset = buttonTop("CmdToggleEditPDF") - buttonTop("CmdOpenFile");
      if (IS_WIN) {
        const legacy = findTopWindow(proc.pid, "SUMATRA_PDF_FRAME");
        const hwnd = legacy
          ? findChildWindow(legacy, "SUMATRA_VIRT_TOOLBAR")
          : findTopWindow(proc.pid, "GpuiSystemMonitor");
        if (hwnd) {
          captureWindowToPng(hwnd, tmpPath(png));
        }
      }
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleEditPDF"]);
      await client.waitForRenderIdle();
      await sleep(200);
      const editDy = await toolbarDy(client);
      return { dy, editDy, lastRowOffset };
    },
    ["-appdata", appdata, "-window-pos", `${dx}x700@0x0`, pdf],
  );
}

export async function testit(): Promise<void> {
  const wide = await measure(kWideDx, "toolbar-wrap-wide.png");
  const narrow = await measure(kNarrowDx, "toolbar-wrap-narrow.png");
  console.log(`  toolbar height: ${JSON.stringify(wide)} at ${kWideDx}px, ${JSON.stringify(narrow)} at ${kNarrowDx}px`);
  const row = wide.dy;
  if (narrow.dy < row * 2) {
    throw new Error(`toolbar-wrap: a narrow window should wrap the toolbar onto more rows (${row} -> ${narrow.dy})`);
  }
  if (wide.lastRowOffset !== 0 || narrow.lastRowOffset !== narrow.dy - row) {
    throw new Error(`toolbar-wrap: Edit PDF button row positions differ from toolbar height`);
  }
  if (wide.editDy !== row * 2 || narrow.editDy !== narrow.dy + row) {
    throw new Error(
      `toolbar-wrap: the Edit PDF row should add one row: wide ${JSON.stringify(wide)}, narrow ${JSON.stringify(narrow)}`,
    );
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
