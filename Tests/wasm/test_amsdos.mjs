// web/amsdos.js: files inside a DATA format DSK. Unit checks on the nave fixture, then a cross
// check with the real AMSDOS ROM in the WASM core: a file the CPC saves is read back here, and a
// file written here is read by the CPC.
//   node Tests/wasm/test_amsdos.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { AmsdosDisc, AmsdosError, normaliseName } from "../../web/amsdos.js";
import createModule from "../../web/bluepixel-capi.js";
import { createBluePixelApi } from "../../web/capi-rpc-client.js";

const file = path => new Uint8Array(readFileSync(new URL(path, import.meta.url)));
const nave = file("../fixtures/nave/nave.dsk");
const pattern = (length, seed) => Uint8Array.from({ length }, (_, i) => (i * 7 + seed) & 0xFF);

// --- reading: the fixture's files are AMSDOS binaries with a valid header checksum ---
{
  const files = new AmsdosDisc(nave).files();
  assert(files.length > 0, "nave.dsk has files");
  const bin = files.find(f => f.name === "NAVE.BIN");
  assert(bin, `NAVE.BIN expected, got ${files.map(f => f.name)}`);
  let sum = 0;
  for (let i = 0; i < 67; i++) sum += bin.data[i];
  assert.equal(sum & 0xFFFF, bin.data[67] | (bin.data[68] << 8), "AMSDOS header checksum");
  const length = bin.data[64] | (bin.data[65] << 8);
  assert(bin.data.length >= 128 + length, "file holds its declared length");
}

// --- write / read back, replace, several extents ---
{
  const disc = new AmsdosDisc(nave);
  const before = disc.files().length;
  disc.write("test.dat", pattern(3000, 1));
  let saved = disc.files().find(f => f.name === "TEST.DAT");
  assert.equal(saved.data.length, 3072, "rounded up to 128-byte records");
  assert.deepEqual(saved.data.subarray(0, 3000), pattern(3000, 1));
  assert.equal(disc.files().length, before + 1);

  disc.write("TEST.DAT", pattern(200, 2));   // replace: one file, new contents
  const tests = disc.files().filter(f => f.name === "TEST.DAT");
  assert.equal(tests.length, 1);
  assert.deepEqual(tests[0].data.subarray(0, 200), pattern(200, 2));

  disc.write("BIG", pattern(40000, 3));      // 3 extents of 16 KB
  saved = new AmsdosDisc(disc.image).files().find(f => f.name === "BIG");
  assert.deepEqual(saved.data.subarray(0, 40000), pattern(40000, 3));

  disc.remove("big");
  assert(!disc.files().some(f => f.name === "BIG"));
  assert.deepEqual(new AmsdosDisc(nave).image, nave, "the source image is never modified");
}

// --- errors leave the image as it was ---
{
  const disc = new AmsdosDisc(nave);
  const copy = disc.image.slice();
  assert.throws(() => disc.write("HUGE.DAT", new Uint8Array(200 * 1024)), AmsdosError);
  assert.deepEqual(disc.image, copy);
  for (const bad of ["TOOLONGNAME.X", "A.ABCD", "", "A.B.C", "SP CE"]) assert.throws(() => normaliseName(bad), AmsdosError, bad);
  assert.equal(normaliseName("catdoku.sav"), "CATDOKU.SAV");
  assert.throws(() => new AmsdosDisc(new Uint8Array(512)), AmsdosError);
}

// --- real AMSDOS in the WASM core ---
const core = await createBluePixelApi(createModule);
assert(core.initialize());
function transfer(bytes, action) {
  const ptr = core._malloc(Math.max(bytes.length, 1));
  try { core.HEAPU8.set(bytes, ptr); return action(ptr, bytes.length); }
  finally { core._free(ptr); }
}
function memory(address, length) {
  const ptr = core._malloc(length);
  try { core.debugReadMemory(address, ptr, length); return core.HEAPU8.slice(ptr, ptr + length); }
  finally { core._free(ptr); }
}
function exportDisc() {
  const sizePtr = core._malloc(4);
  try {
    const ptr = core.exportDisc(0, sizePtr);
    return core.HEAPU8.slice(ptr, ptr + core.HEAPU32[sizePtr >> 2]);
  } finally { core.releaseDiscExport(); core._free(sizePtr); }
}
const frames = n => { for (let i = 0; i < n; i++) core.runFrame(); };
const type = text => { transfer(new TextEncoder().encode(text + "\n\0"), ptr => core.typeText(ptr)); frames(400); };   // typing runs at keyboard speed
try {
  for (const [slot, name] of [[-1, "OS6128.ROM"], [0, "BASIC1-1.ROM"], [7, "AMSDOS.ROM"]]) {
    assert(transfer(file(`../../web/roms/${name}`), (ptr, size) => core.loadRom(slot, ptr, size)));
  }
  // A text file written here, read by BASIC.
  const disc = new AmsdosDisc(nave);
  disc.write("INJ.TXT", new TextEncoder().encode("HELLO123\r\n"));
  core.reset();
  assert(transfer(disc.image, (ptr, size) => core.loadDisc(0, ptr, size, "t.dsk")));
  frames(150);
  type('OPENIN"INJ.TXT":INPUT#9,A$:CLOSEIN');
  type("POKE &4000,LEN(A$):FOR I=1 TO LEN(A$):POKE &4000+I,ASC(MID$(A$,I,1)):NEXT");
  frames(200);
  const got = memory(0x4000, 9);
  assert.equal(new TextDecoder().decode(got.subarray(1, 1 + got[0])), "HELLO123", "the CPC reads the injected file");

  // A binary file saved by the CPC, read here.
  type("FOR I=0 TO 511:POKE &5000+I,(I*3) AND 255:NEXT");
  frames(200);
  type('SAVE"CPCW",B,&5000,512');
  frames(400);
  const saved = new AmsdosDisc(exportDisc()).files().find(f => f.name === "CPCW.BIN");   // AMSDOS adds .BIN to binary files
  assert(saved, `the CPC file is in the directory: ${new AmsdosDisc(exportDisc()).files().map(f => f.name)}`);
  assert.deepEqual(saved.data.subarray(128, 640), Uint8Array.from({ length: 512 }, (_, i) => (i * 3) & 0xFF), "after the 128-byte header");
  assert(new AmsdosDisc(exportDisc()).files().some(f => f.name === "INJ.TXT"), "the injected file is still there");
} finally {
  core.destroy?.();
}
console.log("test_amsdos: ok");
