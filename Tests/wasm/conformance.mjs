#!/usr/bin/env node
// Native vs WASM conformance: the same JSON-RPC request sequence goes to two CPCApi backends and the responses that
// define machine state (state.hash, cpu.registers) are compared every 100 frames over 1000 frames of the nave fixture.
//
//   node Tests/wasm/conformance.mjs --wasm web/bluepixel-capi.js     native binary vs the WASM build (needs Emscripten)
//   node Tests/wasm/conformance.mjs --self-test                      native vs a second native process: checks the harness
//
// Needs BLUEPIXEL_ROM_DIR (the ROMs pinned by Tests/fixtures/nave/roms.json) and build/bluepixel-headless.
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { createInterface } from "node:readline";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const fixture = path.join(root, "Tests", "fixtures", "nave");
const romDir = process.env.BLUEPIXEL_ROM_DIR;
if (!romDir) { console.error("BLUEPIXEL_ROM_DIR is not set"); process.exit(2); }
const args = process.argv.slice(2);
const wasmIndex = args.indexOf("--wasm");
const selfTest = args.includes("--self-test");
if (!selfTest && wasmIndex < 0) { console.error("usage: conformance.mjs --wasm <bluepixel-capi.js> | --self-test"); process.exit(2); }

const FRAMES = 1000, STEP = 100;
const b64 = (file) => readFileSync(file).toString("base64");

function nativeBackend() {
  const child = spawn(path.join(root, "build", "bluepixel-headless"), [], { stdio: ["pipe", "pipe", "inherit"], env: process.env });
  const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
  return {
    name: "native",
    async call(request) { child.stdin.write(request + "\n"); const { value } = await lines.next(); return value; },
    async close() { child.stdin.end(); await new Promise((resolve) => child.on("close", resolve)); },
  };
}

async function wasmBackend(modulePath) {
  const { default: create } = await import(pathToFileURL(path.resolve(modulePath)).href);
  const core = await create();
  const session = core._bpapi_create();
  if (!session) throw new Error("bpapi_create failed");
  return {
    name: "wasm",
    async call(request) {
      const size = core.lengthBytesUTF8(request) + 1;
      const ptr = core._malloc(size);
      core.stringToUTF8(request, ptr, size);
      const out = core._bpapi_dispatch(session, ptr);
      core._free(ptr);
      const reply = core.UTF8ToString(out);
      core._free(out);
      return reply;
    },
    async close() { core._bpapi_destroy(session); },
  };
}

let nextId = 0;
const request = (method, params = {}) => JSON.stringify({ jsonrpc: "2.0", id: ++nextId, method, params });

// The ROM directory's roms.json names the files; both backends receive them explicitly, byte for byte. (The nave
// fixture's own roms.json pins which ROMs make its reference hash valid; this harness compares two backends with each
// other, so any ROM set works, and disposable ROMs are enough for the CI self-test.)
const manifest = JSON.parse(readFileSync(path.join(romDir, "roms.json"), "utf8")).machines.cpc6128;
const setup = [
  ...["os", "basic", "amsdos"].map((role) => request("rom.load", { role, data_base64: b64(path.join(romDir, manifest[role].file)) })),
  request("disc.load", { drive: 0, data_base64: b64(path.join(fixture, "nave.dsk")), name: "nave.dsk" }),
  request("symbols.load", { data_base64: b64(path.join(fixture, "nave.noi")) }),
  request("machine.reset"),
  request("run.frames", { count: 150 }),                           // firmware boot
  request("input.text", { text: 'run"nave\n' }),
];

const backends = [nativeBackend(), selfTest ? nativeBackend() : await wasmBackend(args[wasmIndex + 1])];
backends[1].name = selfTest ? "native#2" : "wasm";

async function both(req) {
  const [a, b] = await Promise.all(backends.map((x) => x.call(req)));
  return [JSON.parse(a), JSON.parse(b)];
}

let compared = 0, equal = 0;
const failures = [];
function compare(label, a, b) {
  compared++;
  const left = JSON.stringify(a.result ?? a.error), right = JSON.stringify(b.result ?? b.error);
  if (left === right) equal++;
  else failures.push(`${label}: ${backends[0].name}=${left.slice(0, 160)} ${backends[1].name}=${right.slice(0, 160)}`);
}

for (const req of setup) {
  const [a, b] = await both(req);
  if (a.error || b.error) { console.error("setup failed:", req.slice(0, 80), JSON.stringify(a.error), JSON.stringify(b.error)); process.exit(1); }
}
// from here on, run the same inputs: some key presses so input handling is part of what is compared
const script = { 300: [0, 0xfd, true], 600: [0, 0xfd, false], 700: [1, 0xfe, true], 900: [1, 0xfe, false] };
for (let frame = 0; frame < FRAMES; frame += STEP) {
  for (let i = 0; i < STEP; i += 100) {
    const key = script[frame + i];
    if (key) { const [a, b] = await both(request("input.matrix", { row: key[0], mask: key[1], pressed: key[2] })); compare(`input @${frame}`, a, b); }
  }
  const [r1, r2] = await both(request("run.frames", { count: STEP }));
  compare(`run.frames @${frame + STEP}`, r1, r2);
  const [h1, h2] = await both(request("state.hash"));
  compare(`state.hash @${frame + STEP}`, h1, h2);
  const [c1, c2] = await both(request("cpu.registers"));
  compare(`cpu.registers @${frame + STEP}`, c1, c2);
  console.log(`frame ${frame + STEP}: ${h1.result.hash} ${h1.result.hash === h2.result.hash ? "==" : "!="} ${h2.result.hash}`);
}
const [s1, s2] = await both(request("state.save"));
compare("state.save bytes", s1, s2);
for (const b of backends) await b.close();

console.log(`conformance ${backends[0].name} vs ${backends[1].name}: ${equal}/${compared} comparisons equal over ${FRAMES} frames (hash every ${STEP})`);
if (failures.length) { console.error(failures.join("\n")); process.exit(1); }
