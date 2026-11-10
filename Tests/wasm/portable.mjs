#!/usr/bin/env node
// Export portable evidence, then restore every producer's states on each backend.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { inflateSync } from "node:zlib";
import { nativeBackend, wasmBackend } from "./backends.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const fixture = path.join(root, "Tests/fixtures/nave");
const args = process.argv.slice(2);
function option(name, fallback) {
  const index = args.indexOf(name);
  if (index < 0) return fallback;
  assert(args[index + 1] && !args[index + 1].startsWith("--"), `${name} needs a value`);
  return args[index + 1];
}
const output = option("--export"), inputs = option("--verify");
assert(output || inputs, "usage: portable.mjs --export DIR | --verify DIR [--wasm MODULE] [--label NAME] [--expect-producers N]");
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
const romDir = process.env.BLUEPIXEL_ROM_DIR || path.join(root, "web/roms");
const manifest = JSON.parse(readFileSync(path.join(romDir, "roms.json"))).machines.cpc6128;
const pins = JSON.parse(readFileSync(path.join(fixture, "roms.json"))).machines.cpc6128;
const identities = {};
const roms = {};
for (const role of ["os", "basic", "amsdos"]) {
  roms[role] = readFileSync(path.join(romDir, manifest[role].file));
  identities[role] = sha(roms[role]);
  assert.equal(identities[role], pins[role].sha256, `wrong fixture ROM: ${role}`);
}
for (const name of ["nave.dsk", "nave.noi", "fixture.json"]) identities[name] = sha(readFileSync(path.join(fixture, name)));
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: root, encoding: "utf8" }).trim();
const identity = { scenario: "nave-portable-v1", revision, assets: identities };
const wasm = option("--wasm");
process.env.BLUEPIXEL_ROM_DIR = romDir;
const backend = wasm ? await wasmBackend(wasm) : nativeBackend();
let id = 0;
async function call(method, params = {}) {
  const reply = JSON.parse(await backend.call(JSON.stringify({ jsonrpc: "2.0", id: ++id, method, params })));
  assert.equal(reply.id, id);
  assert(!reply.error, `${method}: ${JSON.stringify(reply.error)}`);
  return reply.result;
}
async function setup() {
  for (const role of Object.keys(roms)) await call("rom.load", { role, data_base64: roms[role].toString("base64") });
  await call("disc.load", { drive: 0, name: "nave.dsk", data_base64: readFileSync(path.join(fixture, "nave.dsk")).toString("base64") });
  await call("symbols.load", { data_base64: readFileSync(path.join(fixture, "nave.noi")).toString("base64") });
  await call("machine.reset");
}
function canonical(state) {
  assert.equal(state.subarray(0, 4).toString(), "BPST");
  assert.equal(state.readUInt32LE(4), 1);
  assert.equal(state.readUInt32LE(28), state.length - 32);
  const raw = inflateSync(state.subarray(32));
  assert.equal(raw.length, state.readUInt32LE(24));
  // zlib encodings can vary by version. Compare the header's machine fields and raw stream.
  return sha(Buffer.concat([state.subarray(0, 28), raw]));
}
async function observe() {
  const hash = await call("state.hash");
  // nops is a host profiling counter, deliberately excluded from canonical state.
  const { nops, ...cpu } = await call("cpu.registers");
  const screen = await call("screen.capture", { metadata: true });
  const { data_base64, ...metadata } = screen;
  return { hash, cpu, screen: { ...metadata, sha256: sha(Buffer.from(data_base64, "base64")) } };
}
function reports(dir) {
  return readdirSync(dir, { withFileTypes: true }).filter((entry) => entry.isDirectory())
    .map((entry) => ({ dir: path.join(dir, entry.name), report: JSON.parse(readFileSync(path.join(dir, entry.name, "report.json"))) }));
}
try {
  await setup();
  if (output) {
    mkdirSync(output, { recursive: true });
    const capabilities = await call("session.capabilities");
    assert.equal(capabilities.state_format, 1);
    await call("run.frames", { count: 150 });
    await call("input.text", { text: 'run"nave\n' });
    const trace = [], checkpoints = [];
    const keys = { 300: [0, 0xfd, true], 600: [0, 0xfd, false], 700: [1, 0xfe, true], 900: [1, 0xfe, false] };
    for (let frame = 0; frame < 1000; frame += 100) {
      if (keys[frame]) {
        const [row, mask, pressed] = keys[frame];
        await call("input.matrix", { row, mask, pressed });
      }
      const ran = await call("run.frames", { count: 100 });
      assert.equal(ran.ran, 100, "unexpected early stop");
      const audio = await call("audio.read");
      trace.push({ frame: frame + 100, ...await observe(), audio: { frames: audio.frames, sha256: sha(Buffer.from(audio.data_base64, "base64")) } });
      if (frame === 400 || frame === 900) {
        const state = Buffer.from((await call("state.save")).data_base64, "base64");
        const file = `frame-${frame + 100}.bpstate`;
        writeFileSync(path.join(output, file), state);
        checkpoints.push({ file, canonical: canonical(state), at: await observe() });
      }
    }
    assert.equal((await call("mem.read", { address: "game_status", length: 1 })).hex.toLowerCase(), "5a", "nave never booted");
    for (const checkpoint of checkpoints) {
      await call("state.restore", { data_base64: readFileSync(path.join(output, checkpoint.file)).toString("base64") });
      assert.deepEqual(await observe(), checkpoint.at, "local state restore changed the machine");
      await call("run.frames", { count: 100 });
      checkpoint.after100 = await observe();
    }
    const report = { identity, producer: option("--label", `${os.platform()}-${os.arch()}-${backend.name}`), host: { os: os.platform(), arch: os.arch() }, api_version: capabilities.api_version, state_format: capabilities.state_format, trace, checkpoints };
    writeFileSync(path.join(output, "report.json"), JSON.stringify(report, null, 2) + "\n");
    console.log(`Exported ${report.producer}: 10 checkpoints, video/audio digests, 2 portable states`);
  }
  if (inputs) {
    const producers = reports(inputs);
    assert.equal(producers.length, Number(option("--expect-producers", "3")), "missing producer artifacts");
    assert.equal(new Set(producers.map(({ report }) => report.producer)).size, producers.length, "duplicate producers");
    const baseline = producers[0].report;
    for (const { dir, report } of producers) {
      assert.deepEqual(report.identity, identity, "source/ROM/fixture identity differs");
      assert.equal(report.api_version, baseline.api_version);
      assert.equal(report.state_format, 1);
      assert.deepEqual(report.trace, baseline.trace, `trace differs: ${report.producer}`);
      assert.deepEqual(report.checkpoints, baseline.checkpoints, `canonical states differ: ${report.producer}`);
      for (const checkpoint of report.checkpoints) {
        const state = readFileSync(path.join(dir, checkpoint.file));
        assert.equal(canonical(state), checkpoint.canonical);
        await call("state.restore", { data_base64: state.toString("base64") });
        assert.deepEqual(await observe(), checkpoint.at, `restore differs: ${report.producer}/${checkpoint.file}`);
        await call("run.frames", { count: 100 });
        assert.deepEqual(await observe(), checkpoint.after100, `continuation differs: ${report.producer}/${checkpoint.file}`);
      }
      console.log(`Verified ${report.producer} -> ${backend.name}: exact state, screen and 100-frame continuation`);
    }
  }
} finally {
  await backend.close();
}
