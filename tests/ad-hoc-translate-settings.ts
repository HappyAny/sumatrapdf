// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-translate-settings.ts --no-build
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { loadPng, makeTextPdf, pollUntil, runStandalone, tmpPath, writeAppdata } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { controlById, TranslateControl } from "./translate-test-controls.ts";
import {
  captureWindowToPng,
  clientToScreen,
  enumChildWindows,
  findTopWindow,
  getClientRect,
  getControlText,
  getFocusedHwnd,
  getWindowRect,
  getWindowText,
  isWindowVisible,
  repaintWindow,
  sendMessage,
  sendText,
  setProcessDpiAware,
  setWindowPos,
} from "./winapi.ts";

const WM_KEYDOWN = 0x0100;
const VK_RETURN = 0x0d;
const VK_TAB = 0x09;
const BM_CLICK = 0x00f5;
const TIMEOUT = 8000;
const TEST_MODEL = "settings-test-model";
const TEST_KEY = "settings-test-not-a-secret";
const TRANSLATION = "设置测试译文";

type Session = Awaited<ReturnType<typeof launchControlled>>;
type ConfigForm = { hwnd: number; edits: number[]; save: number; cancel: number };

function assert(value: unknown, message: string): asserts value {
  if (!value) throw new Error(`translate-settings: ${message}`);
}

async function state(client: ControlClient, action = ""): Promise<Record<string, string>> {
  const reply = await client.request(ControlCommand.TestTranslatePopup, action ? [action] : []);
  assert(reply[0] === 0, "popup diagnostic failed");
  return Object.fromEntries(
    String(reply[1] ?? "")
      .split(/\r?\n/)
      .flatMap((line) => {
        const index = line.indexOf("=");
        return index < 0 ? [] : [[line.slice(0, index), line.slice(index + 1)]];
      }),
  );
}

async function openConfig(session: Session): Promise<ConfigForm> {
  await state(session.client, "settings");
  const hwnd = await pollUntil(
    () => findTopWindow(session.proc.pid!, "SumatraTranslateConfig"),
    (value) => value !== 0 && isWindowVisible(value),
    { timeoutMs: TIMEOUT, error: "settings window did not open" },
  );
  const edits = [TranslateControl.url, TranslateControl.model, TranslateControl.key].map((id) => controlById(hwnd, id));
  const save = controlById(hwnd, TranslateControl.save);
  const cancel = controlById(hwnd, TranslateControl.cancel);
  return { hwnd, edits, save, cancel };
}

function fillConfig(form: ConfigForm, endpoint: string): void {
  sendText(form.edits[0]!, endpoint);
  sendText(form.edits[1]!, TEST_MODEL);
  sendText(form.edits[2]!, TEST_KEY);
}

async function waitClosed(form: ConfigForm): Promise<void> {
  await pollUntil(
    () => isWindowVisible(form.hwnd),
    (value) => !value,
    {
      timeoutMs: TIMEOUT,
      error: "settings window did not close",
    },
  );
}

async function quit(session: Session): Promise<void> {
  if (session.proc.exitCode !== null) {
    session.client.close();
    return;
  }
  try {
    await session.client.quit();
    await session.proc.exited;
  } catch {
    session.client.close();
    await killAndWait(session.proc);
  }
}

function inClient(parent: number, child: number): boolean {
  const origin = clientToScreen(parent, 0, 0);
  const client = getClientRect(parent);
  const rect = getWindowRect(child);
  return (
    rect.left >= origin.x &&
    rect.top >= origin.y &&
    rect.right <= origin.x + client.right &&
    rect.bottom <= origin.y + client.bottom
  );
}

function assertButtonDrawn(parent: number, child: number, path: string): void {
  assert(isWindowVisible(child), "footer button is hidden");
  const image = loadPng(path);
  const frame = getWindowRect(parent);
  const button = getWindowRect(child);
  const width = button.right - button.left;
  const height = button.bottom - button.top;
  let darkest = 255;
  let lightest = 0;
  for (let y = Math.ceil(button.top - frame.top + height / 5); y < button.bottom - frame.top - height / 5; y++) {
    for (let x = Math.ceil(button.left - frame.left + width / 5); x < button.right - frame.left - width / 5; x++) {
      const offset = (y * image.w + x) * image.nComp;
      const value = (image.data[offset]! + image.data[offset + 1]! + image.data[offset + 2]!) / 3;
      darkest = Math.min(darkest, value);
      lightest = Math.max(lightest, value);
    }
  }
  const minimumLabelContrast = 32;
  assert(lightest - darkest >= minimumLabelContrast, `${getWindowText(child)} button label was not drawn`);
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("translate-settings.pdf");
  writeFileSync(pdf, makeTextPdf("The quick brown fox reads a book."));
  const received: { url: string; model: string; authorization: string | null }[] = [];
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const body = await request.json();
      received.push({ url: request.url, model: body.model, authorization: request.headers.get("authorization") });
      return Response.json({ choices: [{ message: { content: TRANSLATION }, finish_reason: "stop" }] });
    },
  });
  const results: { name: string; status: string; reason?: string }[] = [];
  let ordinal = 0;

  async function run(
    name: string,
    check: (session: Session, form: ConfigForm, endpoint: string, appdata: string) => Promise<void>,
    options: { saveSettings?: boolean } = {},
  ) {
    const path = `/${++ordinal}-${name}`;
    const dir = writeAppdata(
      `translate-settings-${ordinal}`,
      [
        "RestoreSession = false",
        "CheckForUpdates = false",
        "ShowToc = false",
        "SelectionToolbar = false",
        "TranslatePopupEnabled = true",
        "TranslatePopupUrl =",
        "TranslatePopupModel =",
        "TranslatePopupApiKey =",
      ].join("\n"),
    );
    const session = await launchControlled(["-appdata", dir, pdf], options);
    try {
      await session.client.waitForRenderIdle();
      await session.client.setNotificationsEnabled(false);
      const selected = await session.client.request(ControlCommand.TestRotatedTextMouseDrag, ["quick"]);
      assert(String(selected[1] ?? "").includes("selected=quick\n"), "mouse release did not select text");
      const form = await openConfig(session);
      await check(session, form, `${server.url.origin}${path}`, dir);
      results.push({ name, status: "PASS" });
      console.log(`PASS: translate-settings ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", reason: String(error) });
      console.error(`FAIL: translate-settings ${name}: ${String(error)}`);
    } finally {
      await quit(session);
      writeFileSync(tmpPath("translate-settings-results.json"), JSON.stringify(results, null, 2));
    }
  }

  try {
    await run("cancel-enter", async (session, form, endpoint) => {
      const before = await state(session.client);
      const count = received.length;
      fillConfig(form, endpoint);
      sendMessage(form.cancel, WM_KEYDOWN, VK_RETURN, 0);
      await waitClosed(form);
      const after = await state(session.client);
      assert(after.request === before.request && after.busy === "0", "Enter on Cancel applied the settings");
      assert(received.length === count, "Enter on Cancel sent a translation request");
      const reopened = await openConfig(session);
      assert(
        reopened.edits.every((hwnd) => getControlText(hwnd) === ""),
        "Cancel changed the stored settings",
      );
    });

    await run("small-client-layout", async (_session, form) => {
      const original = getWindowRect(form.hwnd);
      const client = getClientRect(form.hwnd);
      const button = getWindowRect(form.save);
      const scale = (button.bottom - button.top) / 29;
      const nonClient = original.bottom - original.top - client.bottom;
      const height = Math.round(220 * scale) + nonClient;
      assert(
        setWindowPos(form.hwnd, original.left, original.top, original.right - original.left, height),
        "resize failed",
      );
      assert(
        inClient(form.hwnd, form.save) && inClient(form.hwnd, form.cancel),
        "Save or Cancel is outside the client area",
      );
      assert(inClient(form.hwnd, form.edits[0]!), "URL field is initially outside the client area");
      sendMessage(form.edits[0]!, WM_KEYDOWN, VK_TAB, 0);
      assert(getFocusedHwnd(form.hwnd) === form.edits[1], "Tab did not focus Model");
      sendMessage(form.edits[1]!, WM_KEYDOWN, VK_TAB, 0);
      assert(getFocusedHwnd(form.hwnd) === form.edits[2], "Tab did not focus the key field");
      assert(inClient(form.hwnd, form.edits[2]!), "focused key field is outside the client area");
      const keyRect = getWindowRect(form.edits[2]!);
      assert(keyRect.bottom <= getWindowRect(form.save).top, "focused key field overlaps the footer buttons");
      sendMessage(form.save, BM_CLICK, 0, 0);
      let note = 0;
      enumChildWindows(form.hwnd, (child) => {
        if (getWindowText(child).startsWith("启用自动翻译前")) note = child;
        return true;
      });
      assert(isWindowVisible(form.hwnd) && note !== 0, "invalid settings were accepted");
      assert(inClient(form.hwnd, note), "settings validation message is outside the client area");
      assert(getWindowRect(note).bottom <= getWindowRect(form.save).top, "validation message overlaps the footer");
      const screenshot = tmpPath("translate-settings-small.png");
      await pollUntil(
        () => {
          repaintWindow(form.hwnd);
          enumChildWindows(form.hwnd, (child) => {
            repaintWindow(child);
            return true;
          });
          if (!captureWindowToPng(form.hwnd, screenshot)) return false;
          try {
            assertButtonDrawn(form.hwnd, form.save, screenshot);
            assertButtonDrawn(form.hwnd, form.cancel, screenshot);
            return true;
          } catch {
            return false;
          }
        },
        (drawn) => drawn,
        { timeoutMs: TIMEOUT, intervalMs: 30, error: "footer button labels were not drawn" },
      );
      assertButtonDrawn(form.hwnd, form.save, screenshot);
      assertButtonDrawn(form.hwnd, form.cancel, screenshot);
    });

    await run("input-enter-saves", async (session, form, endpoint) => {
      const count = received.length;
      fillConfig(form, endpoint);
      sendMessage(form.edits[1]!, WM_KEYDOWN, VK_RETURN, 0);
      await waitClosed(form);
      await pollUntil(
        () => state(session.client),
        (value) => value.busy === "0" && JSON.parse(value.text || '""') === TRANSLATION,
        {
          timeoutMs: TIMEOUT,
          error: "Enter in an input did not save and translate",
        },
      );
      const sent = received.slice(count);
      assert(sent.length === 1 && sent[0]!.url === endpoint, "saved endpoint was not used");
      assert(
        sent[0]!.model === TEST_MODEL && sent[0]!.authorization === `Bearer ${TEST_KEY}`,
        "saved model or credentials were not used",
      );
    });

    await run(
      "settings-persist-and-reload",
      async (session, form, endpoint, appdata) => {
        const expectedUrl = endpoint + "?persist=1";
        const expectedModel = 'persist-"model"\\branch';
        fillConfig(form, expectedUrl);
        sendText(form.edits[1]!, expectedModel);
        sendMessage(form.save, BM_CLICK, 0, 0);
        await waitClosed(form);
        await pollUntil(
          () => state(session.client),
          (value) => value.busy === "0" && JSON.parse(value.text || '""') === TRANSLATION,
          { timeoutMs: TIMEOUT, error: "saved configuration did not translate" },
        );

        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        const persisted = await pollUntil(
          () => {
            if (!existsSync(settingsPath)) return {};
            return Object.fromEntries(
              readFileSync(settingsPath, "utf8")
                .split(/\r?\n/)
                .flatMap((line) => {
                  const match = /^\s*(TranslatePopup\w+)\s*=\s*(.*)$/.exec(line);
                  return match ? [[match[1]!, match[2]!.trim()]] : [];
                }),
            );
          },
          (fields) =>
            fields.TranslatePopupUrl === expectedUrl &&
            fields.TranslatePopupModel === expectedModel &&
            fields.TranslatePopupApiKey === TEST_KEY &&
            (fields.TranslatePopupEnabled ?? "true") === "true",
          { timeoutMs: TIMEOUT, error: "translation fields were not persisted" },
        );
        const persistedChecks = {
          url: persisted.TranslatePopupUrl === expectedUrl,
          model: persisted.TranslatePopupModel === expectedModel,
          key: persisted.TranslatePopupApiKey === TEST_KEY,
          enabled: (persisted.TranslatePopupEnabled ?? "true") === "true",
        };
        await quit(session);

        const beforeReload = received.length;
        const restarted = await launchControlled(["-appdata", appdata, pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          const selected = await restarted.client.request(ControlCommand.TestRotatedTextMouseDrag, ["quick"]);
          assert(String(selected[1] ?? "").includes("selected=quick\n"), "reload did not select text");
          await pollUntil(
            () => state(restarted.client),
            (value) => value.busy === "0" && JSON.parse(value.text || '""') === TRANSLATION,
            { timeoutMs: TIMEOUT, error: "reloaded configuration did not translate" },
          );
          const sent = received.slice(beforeReload);
          const reloadChecks = {
            oneRequest: sent.length === 1,
            url: sent[0]?.url === expectedUrl,
            model: sent[0]?.model === expectedModel,
            key: sent[0]?.authorization === "Bearer " + TEST_KEY,
            enabled: sent.length === 1,
          };
          assert(Object.values(reloadChecks).every(Boolean), "persisted configuration did not reload correctly");
          writeFileSync(
            tmpPath("translate-settings-persistence.json"),
            JSON.stringify({ persisted: persistedChecks, reloaded: reloadChecks }, null, 2),
          );
        } finally {
          await quit(restarted);
        }
      },
      { saveSettings: true },
    );
  } finally {
    server.stop(true);
  }
  const failures = results.filter((item) => item.status === "FAIL");
  assert(failures.length === 0, `${failures.length} settings checks failed`);
}

if (import.meta.main) await runStandalone(testit);
