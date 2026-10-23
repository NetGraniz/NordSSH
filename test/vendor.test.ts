import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
test("GLib backport uses a mutable FFI output pointer", () => {
  const source = readFileSync("vendor/glib-0.18.5/src/variant_iter.rs", "utf8");
  const body = source.slice(source.indexOf("fn impl_get("));
  assert.match(body, /let mut p: \*mut libc::c_char/);
  assert.match(body, /s as \*const u8 as \*const _,\s+&mut p,/);
});
