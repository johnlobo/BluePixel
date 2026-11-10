#!/usr/bin/env node
// Check that the cross-runner gate rejects incomplete/corrupt evidence and accepts a different zlib encoding.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync, inflateSync } from "node:zlib";
const here = path.dirname(fileURLToPath(import.meta.url));
const source = process.argv[2];
assert(source, "usage: test_portable.mjs DIR_WITH_REPORT_AND_STATES");
const temp = mkdtempSync(path.join(os.tmpdir(), "bluepixel-portable-"));
function report(label) {
  const result = JSON.parse(readFileSync(path.join(source, "report.json")));
  result.producer = label;
  return result;
}
function write(label, data) { writeFileSync(path.join(temp, label, "report.json"), JSON.stringify(data)); }
function verify(expected, message) {
  const result = spawnSync(process.execPath, [path.join(here, "portable.mjs"), "--verify", temp, "--expect-producers", "2"], { encoding: "utf8", timeout: 30000 });
  assert.equal(result.status, expected, result.stderr);
  if (message) assert.match(result.stderr, message);
}
try {
  cpSync(source, path.join(temp, "a"), { recursive: true });
  write("a", report("a"));
  verify(1, /missing producer artifacts/);
  cpSync(source, path.join(temp, "b"), { recursive: true });
  write("b", report("b"));
  const changed = report("b");
  changed.trace[0].cpu.pc ^= 1;
  write("b", changed);
  verify(1, /trace differs/);
  const wrong = report("b");
  wrong.identity.assets.os = "wrong ROM";
  write("b", wrong);
  verify(1, /source\/ROM\/fixture identity differs/);
  write("b", report("b"));
  const checkpoint = report("b").checkpoints[0].file;
  const statePath = path.join(temp, "b", checkpoint);
  const state = readFileSync(statePath);
  const bad = Buffer.from(state);
  bad[0] ^= 1;
  writeFileSync(statePath, bad);
  verify(1, /BPST/);
  const packed = deflateSync(inflateSync(state.subarray(32)), { level: 0 });
  const header = Buffer.from(state.subarray(0, 32));
  header.writeUInt32LE(packed.length, 28);
  writeFileSync(statePath, Buffer.concat([header, packed]));
  verify(0);
  console.log("Portable gate: missing artifacts, changed trace/ROM, corrupt state rejected; recompressed state accepted");
} finally { rmSync(temp, { recursive: true, force: true }); }
