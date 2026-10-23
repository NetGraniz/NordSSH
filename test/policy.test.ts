import test from "node:test";
import assert from "node:assert/strict";
import {
  needsPasteConfirmation,
  remoteJoin,
  boundedFontSize,
} from "../src/policy.ts";
test("paste warns on newlines and terminal control characters", () => {
  for (const text of ["ls\npwd", "ls\r", "\x1b[200~echo", "\x7f"])
    assert.equal(needsPasteConfirmation(text), true);
  assert.equal(needsPasteConfirmation("echo Привет"), false);
});
test("remote names cannot escape the displayed folder", () => {
  assert.equal(remoteJoin("/home/test/", "hello.txt"), "/home/test/hello.txt");
  for (const name of ["..", ".", "../x", "x/y", "x\\y", "\0"])
    assert.throws(() => remoteJoin("/home", name));
});
test("font size is bounded", () => {
  assert.equal(boundedFontSize(NaN), 14);
  assert.equal(boundedFontSize(99), 24);
  assert.equal(boundedFontSize(-1), 11);
});
