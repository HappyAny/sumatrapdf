// SPDX-License-Identifier: GPL-3.0-only
import { ptr } from "bun:ffi";
import { enumChildWindows, getClassName, getWindowLong, sendMessage } from "./winapi.ts";

const GWL_ID = -12;
const CB_GETCOUNT = 0x0146;
const CB_GETLBTEXT = 0x0148;
const CB_GETLBTEXTLEN = 0x0149;
const CB_SETCURSEL = 0x014e;

export const TranslateControl = {
  save: 3006,
  cancel: 3007,
  url: 3101,
  model: 3102,
  key: 3103,
  temperature: 3104,
  prompt: 3105,
  stream: 3106,
  thinking: 3107,
  source: 3108,
  target: 3109,
  enabled: 3110,
  custom: 3111,
  font: 3120,
  fontSize: 3121,
  autoSize: 3122,
  width: 3123,
  height: 3124,
  maxHeight: 3125,
  bold: 3126,
  position: 3127,
  lineSpacing: 3128,
} as const;

export const ReadingControl = { smaller: 3010, larger: 3011, autoSize: 3012, bold: 3013 } as const;

export const ParamControl = { add: 3201, save: 3202, cancel: 3203 } as const;

export function defaultTranslationPrompt(source: string): string {
  return (
    "As an academic expert, translate the following text from Auto (detect the source language) to Simplified Chinese. " +
    "Preserve the meaning, paragraph breaks, formulas, variable names, numbers, and citation markers. " +
    "Output only the translation without explanations.\n\n" +
    source
  );
}

export function controlById(parent: number, id: number): number {
  let found = 0;
  enumChildWindows(parent, (hwnd) => {
    if (getWindowLong(hwnd, GWL_ID) === id) found = hwnd;
    return found === 0;
  });
  if (!found) throw new Error(`translation control ${id} is missing`);
  return found;
}

export function translationBody(parent: number): number {
  let body = 0;
  enumChildWindows(parent, (hwnd) => {
    if (/^(edit|richedit50w)$/i.test(getClassName(hwnd))) body = hwnd;
    return !body;
  });
  if (!body) throw new Error("translation text control is missing");
  return body;
}

export function comboItems(hwnd: number): string[] {
  const count = Number(sendMessage(hwnd, CB_GETCOUNT, 0, 0));
  if (count < 0 || count > 2048) throw new Error("invalid translation combo count");
  return Array.from({ length: count }, (_, index) => {
    const length = Number(sendMessage(hwnd, CB_GETLBTEXTLEN, index, 0));
    if (length < 0 || length > 2048) throw new Error("invalid translation combo text length");
    const buffer = new Uint16Array(length + 1);
    const got = Number(sendMessage(hwnd, CB_GETLBTEXT, index, ptr(buffer)));
    if (got !== length) throw new Error("could not read translation combo item");
    return String.fromCharCode(...buffer.subarray(0, got));
  });
}

export function selectCombo(hwnd: number, label: RegExp): void {
  const items = comboItems(hwnd);
  const index = items.findIndex((item) => label.test(item));
  if (index < 0) throw new Error(`translation combo has no ${label}: ${items.join(", ")}`);
  if (Number(sendMessage(hwnd, CB_SETCURSEL, index, 0)) !== index) {
    throw new Error("could not select translation combo item");
  }
}
