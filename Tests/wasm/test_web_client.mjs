// Exercise the actual web adapter against WASM, including its buffer ownership.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import createModule from "../../web/bluepixel-capi.js";
import { createBluePixelApi } from "../../web/capi-rpc-client.js";

const allocations = new Set();
const core = await createBluePixelApi(async options => {
  const module = await createModule(options);
  const malloc = module._malloc, free = module._free;
  module._malloc = size => { const ptr = malloc(size); if (ptr) allocations.add(ptr); return ptr; };
  module._free = ptr => { allocations.delete(ptr); free(ptr); };
  return module;
});
const file = path => new Uint8Array(readFileSync(new URL(path, import.meta.url)));
function transfer(bytes, action) {
  const ptr = core._malloc(bytes.length);
  try { core.HEAPU8.set(bytes, ptr); return action(ptr, bytes.length); }
  finally { core._free(ptr); }
}
const text = (value, action) => transfer(new TextEncoder().encode(value + "\0"), action);
function memory(address, count = 1) {
  const ptr = core._malloc(count);
  try { core.debugReadMemory(address, ptr, count); return Array.from(core.HEAPU8.subarray(ptr, ptr + count)); }
  finally { core._free(ptr); }
}
function string(action, size = 256) {
  const ptr = core._malloc(size);
  try { action(ptr, size); return core.UTF8ToString(ptr); }
  finally { core._free(ptr); }
}
const run = count => { for (let i = 0; i < count; i++) core.runFrame(); };

try {
  for (const [slot, name] of [[-1, "OS6128.ROM"], [0, "BASIC1-1.ROM"], [7, "AMSDOS.ROM"]]) {
    assert(transfer(file(`../../web/roms/${name}`), (ptr, size) => core.loadRom(slot, ptr, size)));
  }
  const disc = file("../fixtures/nave/nave.dsk");
  for (const drive of [0, 1]) {
    assert(transfer(disc, (ptr, size) => core.loadDisc(drive, ptr, size, "nave.dsk")));
    assert.equal(string((ptr, size) => core.discName(drive, ptr, size)), "nave.dsk");
  }
  const sizePtr = core._malloc(4);
  try {
    const baseline = allocations.size;
    for (let i = 0; i < 20; i++) {
      const ptr = core.exportDisc(i % 2, sizePtr);
      assert(ptr && core.HEAPU32[sizePtr >> 2] > 0);
      assert.equal(new TextDecoder().decode(core.HEAPU8.subarray(ptr, ptr + 8)), "MV - CPC");
      core.releaseDiscExport();
      assert.equal(allocations.size, baseline, "disc export must release its buffer");
    }
  } finally { core._free(sizePtr); }

  core.reset();
  run(150);
  text('run"nave\n', ptr => core.typeText(ptr));
  for (let frames = 0; frames < 3000 && memory(0x1ffc)[0] !== 0x5a; frames += 10) run(10);
  assert.equal(memory(0x1ffc)[0], 0x5a, "the game must boot through the same adapter used by the browser");
  const x = memory(0x2051)[0];
  core.key(1, true); run(40); core.key(1, false);
  assert(memory(0x2051)[0] > x, "keyboard matrix input must move the player");
  core.joystick(4, true); run(1); core.joystick(4, false);

  const audio = core._malloc(8192 * 8);
  try {
    const frames = core.readAudio(audio, 8192);
    assert(frames > 0);
    assert(Array.from(core.HEAPF32.subarray(audio >> 2, (audio >> 2) + frames * 2)).every(Number.isFinite));
  } finally { core._free(audio); }
  for (let i = 0; i < 4; i++) {
    const frame = core.renderFrame();
    assert(frame);
    const pixels = core.HEAPU32.subarray(frame >> 2, (frame >> 2) + 768 * 540);
    assert(pixels.some(pixel => (pixel & 0xffffff) !== 0), "the rendered game must contain visible pixels");
  }

  const snapshotSize = core._malloc(4);
  let snapshot = 0;
  try {
    const pc = core.debugRegister(7), value = memory(0x8000)[0];
    snapshot = core.saveSnapshot(snapshotSize);
    const length = core.HEAPU32[snapshotSize >> 2];
    assert(snapshot && length > 256);
    core.debugWriteMemory(0x8000, value ^ 255); run(3);
    assert(core.loadSnapshot(snapshot, length));
    assert.equal(core.debugRegister(7), pc);
    assert.equal(memory(0x8000)[0], value);
  } finally { if (snapshot) core.freeBuffer(snapshot); core._free(snapshotSize); }

  core.debugAttach(); core.debugPause();
  const pc = core.debugRegister(7);
  assert(string((ptr, size) => core.debugDisassemble(pc, ptr, size)).length > 0);
  assert(core.debugInstructionLength(pc) > 0);
  text("web_test", ptr => core.debugSymbolSet(ptr, 0x8000));
  assert(core.debugSymbolCount() > 0);
  assert.equal(string((ptr, size) => core.debugSymbolName(0, ptr, size)), "web_test");
  assert.equal(core.debugSymbolValue(0), 0x8000);
  core.debugSymbolClear();
  core.debugToggleCodeBreakpoint(pc);
  assert(core.debugHasCodeBreakpoint(pc));
  const bp = core.debugBreakpointId(0);
  assert(text("A=#42", ptr => core.debugSetBreakpointCondition(bp, ptr, 2)));
  assert.equal(string((ptr, size) => core.debugBreakpointCondition(bp, ptr, size)), "A=#42");
  assert.equal(core.debugBreakpointPassCount(bp), 2);
  assert.equal(text("x".repeat(160), ptr => core.debugSetBreakpointCondition(bp, ptr, 0)), false);
  assert(core.UTF8ToString(core.debugLastError()).length > 0);
  core.debugRemoveBreakpoint(bp);
  assert(!core.debugHasCodeBreakpoint(pc));
  const watch = core.debugAddMemoryBreakpoint(0x8000, 0x8001, true);
  assert.equal(core.debugBreakpointType(watch), 2);
  core.debugRemoveBreakpoint(watch);
  core.debugMarkData(0x8000, 0x800f); core.debugClearData(0x8000);
  assert.equal(memory(0xffff, 2).length, 2, "debugger reads must wrap at 64 KiB");
  core.debugResetTstates(); core.debugStepInto();
  assert(Number(string((ptr, size) => core.debugTstates(ptr, size))) > 0);
  core.debugResume(); run(2);

  const tape = new Uint8Array([90, 88, 84, 97, 112, 101, 33, 26, 1, 32, 32, 232, 3]);
  text("test.cdt", name => transfer(tape, (ptr, size) => core.loadTape(ptr, size, name)));
  assert(core.tapeLoaded());
  core.tapePlay(true); assert(core.tapePlaying());
  core.tapePlay(false); core.tapeRewind(); core.tapeRemove();
  assert(!core.tapeLoaded());
  core.loadRom(7, 0, 0);
} finally {
  core.destroy(); core.destroy();
}
assert.equal(allocations.size, 0, "all adapter-owned and UI buffers must be freed");
console.log("web adapter: PASS (ROMs, drives A/B, game boot, keyboard/joystick, render/audio, SNA, debugger, tape, buffer ownership)");
