// URL start-up (web/autoload.js): parameter parsing, ordering with fakes, and a real run against the
// WASM core through the same adapter the browser uses (nave fixture: disc, symbols, run"nave).
//   node Tests/wasm/test_autoload.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { parseLaunchParams, runAutoload, BOOT_FRAMES, MAX_WARP_FRAMES } from "../../web/autoload.js";
import createModule from "../../web/bluepixel-capi.js";
import { createBluePixelApi } from "../../web/capi-rpc-client.js";

const PAGE = "https://cpc.example/bluepixel/index.html";

// --- parseLaunchParams ---
{
  const p = parseLaunchParams("", PAGE);
  assert.equal(p.active, false);
  assert.deepEqual(p.errors, []);
}
{
  const p = parseLaunchParams("?disk=../gamez/catdoku.dsk&sym=../gamez/catdoku.noi&run=run%22catdoku&warp=500", PAGE);
  assert.equal(p.active, true);
  assert.equal(p.disk.href, "https://cpc.example/gamez/catdoku.dsk");
  assert.equal(p.sym.href, "https://cpc.example/gamez/catdoku.noi");
  assert.equal(p.run, 'run"catdoku');
  assert.equal(p.warp, 500);
  assert.deepEqual(p.errors, []);
}
{
  const p = parseLaunchParams("?disk=https://evil.example/x.dsk&sym=//evil.example/x.noi", PAGE);
  assert.equal(p.disk, null, "another origin must be refused");
  assert.equal(p.sym, null, "a protocol-relative URL to another origin must be refused");
  assert.equal(p.errors.length, 2);
}
{
  const p = parseLaunchParams("?disk=javascript:alert(1)", PAGE);
  assert.equal(p.disk, null);
  assert.equal(p.errors.length, 1);
}
{
  assert.equal(parseLaunchParams("?warp=999999", PAGE).warp, MAX_WARP_FRAMES);
  const bad = parseLaunchParams("?warp=-3", PAGE);
  assert.equal(bad.warp, 0);
  assert.equal(bad.errors.length, 1);
  assert.equal(parseLaunchParams("?warp=1.5", PAGE).errors.length, 1);
}
{
  assert.equal(parseLaunchParams("?run=run%22x%0A", PAGE).run, 'run"x', "a trailing newline is dropped, one is added on typing");
  const ctl = parseLaunchParams("?run=a%07b", PAGE);
  assert.equal(ctl.run, null);
  assert.equal(ctl.errors.length, 1);
  assert.equal(parseLaunchParams(`?run=${"x".repeat(201)}`, PAGE).errors.length, 1);
}

// --- runAutoload ordering with fakes ---
function fakeIo(files) {
  const log = [];
  return {
    log,
    async fetchBytes(url) {
      log.push(`fetch ${url.pathname}`);
      if (!(url.pathname in files)) throw new Error(`${url.pathname}: HTTP 404`);
      return files[url.pathname];
    },
    insertDisk(bytes, name) { log.push(`disk ${name}`); return bytes.length > 0; },
    loadSymbols(bytes) { log.push("symbols"); return bytes.length; },
    runFrames(count) { log.push(`frames ${count}`); },
    typeText(text) { log.push(`type ${JSON.stringify(text)}`); },
    report(message) { log.push(`report ${message}`); },
  };
}
{
  const io = fakeIo({ "/gamez/g.dsk": new Uint8Array(4), "/gamez/g.noi": new Uint8Array(7) });
  const summary = await runAutoload(parseLaunchParams("?disk=../gamez/g.dsk&sym=../gamez/g.noi&run=run%22g&warp=1000", PAGE), io);
  assert.deepEqual(io.log, [
    "fetch /gamez/g.dsk", "disk g.dsk", "fetch /gamez/g.noi", "symbols",
    `frames ${BOOT_FRAMES}`, 'type "run\\"g\\n"', `frames ${1000 - BOOT_FRAMES}`,
  ]);
  assert.deepEqual(summary, { disk: true, symbols: 7, typed: true, frames: 1000, errors: [] });
}
{
  // A missing symbol file is reported; the disc still loads and the command is still typed.
  const io = fakeIo({ "/gamez/g.dsk": new Uint8Array(4) });
  const summary = await runAutoload(parseLaunchParams("?disk=../gamez/g.dsk&sym=../gamez/missing.noi&run=run%22g", PAGE), io);
  assert.equal(summary.disk, true);
  assert.equal(summary.typed, true);
  assert.equal(summary.symbols, 0);
  assert.equal(summary.errors.length, 1);
  assert.match(summary.errors[0], /^sym: .*404/);
}
{
  // A rejected disc does not stop the rest; warp without run just runs frames.
  const io = fakeIo({ "/gamez/bad.dsk": new Uint8Array(0) });
  const summary = await runAutoload(parseLaunchParams("?disk=../gamez/bad.dsk&warp=40", PAGE), io);
  assert.equal(summary.disk, false);
  assert.match(summary.errors[0], /not a supported DSK image/);
  assert.deepEqual(io.log.slice(-1), ["frames 40"]);
}

// --- real WASM core, browser adapter ---
const core = await createBluePixelApi(createModule);
assert(core.initialize());
const file = path => new Uint8Array(readFileSync(new URL(path, import.meta.url)));
function transfer(bytes, action) {
  const ptr = core._malloc(Math.max(bytes.length, 1));
  try { core.HEAPU8.set(bytes, ptr); return action(ptr, bytes.length); }
  finally { core._free(ptr); }
}
function memory(address) {
  const ptr = core._malloc(1);
  try { core.debugReadMemory(address, ptr, 1); return core.HEAPU8[ptr]; }
  finally { core._free(ptr); }
}
try {
  for (const [slot, name] of [[-1, "OS6128.ROM"], [0, "BASIC1-1.ROM"], [7, "AMSDOS.ROM"]]) {
    assert(transfer(file(`../../web/roms/${name}`), (ptr, size) => core.loadRom(slot, ptr, size)));
  }
  core.reset();
  const served = { "/gamez/nave.dsk": "../fixtures/nave/nave.dsk", "/gamez/nave.noi": "../fixtures/nave/nave.noi" };
  const summary = await runAutoload(parseLaunchParams("?disk=../gamez/nave.dsk&sym=../gamez/nave.noi&run=run%22nave&warp=600", PAGE), {
    async fetchBytes(url) { return file(served[url.pathname]); },
    insertDisk: (bytes, name) => transfer(bytes, (ptr, size) => core.loadDisc(0, ptr, size, name)),
    loadSymbols: bytes => transfer(bytes, (ptr, size) => core.loadSymbols(ptr, size)),
    runFrames(count) { for (let i = 0; i < count; i++) core.runFrame(); },
    typeText: text => transfer(new TextEncoder().encode(text + "\0"), ptr => core.typeText(ptr)),
    report(message) { throw new Error(`unexpected report: ${message}`); },
  });
  assert.equal(summary.disk, true);
  assert(summary.symbols > 0, "nave.noi must load symbols");
  const names = new Map(Array.from({ length: core.debugSymbolCount() }, (_, i) => {
    const ptr = core._malloc(48);
    try { core.debugSymbolName(i, ptr, 48); return [core.UTF8ToString(ptr).toLowerCase(), core.debugSymbolValue(i)]; }
    finally { core._free(ptr); }
  }));
  assert.equal(names.get("fwk_project_init"), 0x200, "symbol value from nave.noi");
  assert(!names.has("s__code"), "linker section starts are skipped");
  for (let frames = 0; frames < 3000 && memory(0x1ffc) !== 0x5a; frames += 10) for (let i = 0; i < 10; i++) core.runFrame();
  assert.equal(memory(0x1ffc), 0x5a, "the game must boot from the URL start-up sequence");
} finally {
  core.destroy?.();
}
console.log("test_autoload: ok");
