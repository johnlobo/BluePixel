// Find Graphics (web debugger): decoding, layout, address expressions, and the palette the bridge reads from the real core.
import assert from "node:assert/strict";
import createModule from "../../web/bluepixel-capi.js";
import { createBluePixelApi } from "../../web/capi-rpc-client.js";
import { parseAddress, penOf, layoutFor, renderGraphics, pixelInfo, lastAddress, greyPalette, byteOffset, readRange, MAX_BYTES } from "../../web/graphics-viewer.js";

// Pen bit layouts (the same as cpc.c / the macOS Find Graphics).
assert.equal(penOf(0, 0x80, 0), 1);            // pixel 0 pen bit 0 is b7
assert.equal(penOf(0, 0x08, 0), 2);            // b3
assert.equal(penOf(0, 0x20, 0), 4);            // b5
assert.equal(penOf(0, 0x02, 0), 8);            // b1
assert.equal(penOf(0, 0x40, 1), 1);            // pixel 1: b6, b2, b4, b0
assert.equal(penOf(1, 0x88, 0), 3);            // mode 1: b7 | b3<<1
assert.equal(penOf(1, 0x10, 3), 1); assert.equal(penOf(1, 0x01, 3), 2);   // pixel 3: b4 | b0<<1
assert.equal(penOf(2, 0b10100000, 0), 1); assert.equal(penOf(2, 0b10100000, 1), 0); assert.equal(penOf(2, 0b10100000, 2), 1);

// Address expressions.
const context = { symbols: new Map([["spr_player", 0x4000], ["Table", 0x8100]]), registers: { HL: 0xC123, DE: 0x0040, SP: 0xBFFE } };
assert.equal(parseAddress("C000", context), 0xC000);
assert.equal(parseAddress("#c000", context), 0xC000);
assert.equal(parseAddress("&1234", context), 0x1234);
assert.equal(parseAddress("0xFFFF", context), 0xFFFF);
assert.equal(parseAddress("spr_player", context), 0x4000);
assert.equal(parseAddress("spr_player+10", context), 0x4010);
assert.equal(parseAddress("spr_player + #10", context), 0x4010);
assert.equal(parseAddress("table", context), 0x8100);      // case-insensitive fallback
assert.equal(parseAddress("hl", context), 0xC123);
assert.equal(parseAddress("hl-3", context), 0xC120);
assert.equal(parseAddress("DE", context), 0x0040);          // a register, not the number #DE
assert.equal(parseAddress("#DE", context), 0x00DE);
assert.equal(parseAddress("0-1", context), 0xFFFF);         // wraps inside 64 KB
assert.equal(parseAddress("", context), null);
assert.equal(parseAddress("nothing", context), null);
assert.equal(parseAddress("1++2", context), null);

// Layout: stride defaults to one frame; frames wrap into columns with a 2 pixel gap.
const layout = layoutFor({ address: 0x4000, mode: 1, widthBytes: 2, height: 3, frames: 5, cols: 2 });
assert.equal(layout.stride, 6); assert.equal(layout.w, 8); assert.equal(layout.cols, 2); assert.equal(layout.rows, 3);
assert.equal(layout.width, 2 * 8 + 2); assert.equal(layout.imageHeight, 3 * 3 + 2 * 2);
assert.equal(layoutFor({ widthBytes: 999, height: 0, frames: 9999, cols: "x" }).widthBytes, 128);
assert.equal(layoutFor({ frames: 3, cols: 64 }).cols, 3);

// Rendering a two-frame mode 1 sheet from fake memory.
const memory = new Uint8Array(0x10000);
memory.set([0x88, 0x00, 0x00, 0x88], 0x4000);               // frame 0, 2 bytes wide, 2 rows
memory.set([0xFF, 0xFF], 0x4004);                           // frame 1 starts at the default stride (4)
const reads = [];
const read = (start, count) => { reads.push([start, count]); return Array.from({ length: count }, (_, i) => memory[(start + i) & 0xFFFF]); };
const palette = [0x000000, 0xFF0000, 0x00FF00, 0x0000FF, ...Array(12).fill(0x808080)];
const picture = renderGraphics({ address: 0x4000, mode: 1, widthBytes: 2, height: 2, frames: 2, cols: 2 }, read, palette);
assert.equal(picture.layout.width, 8 * 2 + 2); assert.equal(picture.layout.imageHeight, 2);
assert.equal(picture.pens[0], 3);                           // 0x88 -> pen 3 at pixel 0
assert.equal(picture.pens[1], 0); assert.equal(picture.pens[4], 0);
assert.equal(picture.pens[8], 255); assert.equal(picture.pens[9], 255);   // the gap
assert.equal(picture.pens[10], 3); assert.equal(picture.pens[17], 3);     // frame 1: 0xFF 0xFF
assert.equal(picture.pens[18 + 4], 3);                      // row 1, byte 1 of frame 0: 0x88
assert.equal(picture.pens[18 + 10], 0);                     // row 1 of frame 1 is zero
assert.equal(picture.rgba[0] >>> 0, 0xFFFF0000);            // pen 3 = 0x0000FF, stored as little-endian RGBA: R=00 G=00 B=FF
assert.equal(picture.rgba[8] >>> 0, 0xFF303030);            // gap colour

// Pointer read-out.
const here = pixelInfo(picture, 4, 1);
assert.deepEqual(here, { frame: 0, x: 4, y: 1, address: 0x4003, pen: 3 });
assert.equal(pixelInfo(picture, 8, 0), null);               // gap
assert.equal(pixelInfo(picture, 10 + 5, 1)?.address, 0x4004 + 2 + 1);
assert.equal(pixelInfo(picture, -1, 0), null);
assert.equal(lastAddress(picture.layout), 0x4007);

// Screen (8-line) layout: row y lives at (y&7)*0x800 + (y>>3)*80.
const screen = layoutFor({ address: 0xC000, mode: 1, widthBytes: 4, height: 16, interleaved: true });
assert.equal(byteOffset(screen, 0, 1), 0x800); assert.equal(byteOffset(screen, 0, 8), 80); assert.equal(byteOffset(screen, 5, 9), 0x800 + 80 + 1);
const vram = new Uint8Array(0x10000); vram[0xC800] = 0x88;     // row 1, first byte
const shot = renderGraphics({ address: 0xC000, mode: 1, widthBytes: 4, height: 16, interleaved: true },
  (start, count) => Array.from({ length: count }, (_, i) => vram[(start + i) & 0xFFFF]), palette);
assert.equal(shot.pens[1 * shot.layout.width], 3); assert.equal(shot.pens[0], 0);

// Limits and chunked reads.
assert.throws(() => renderGraphics({ widthBytes: 128, height: 512, frames: 8 }, read, palette), /Too large/);
assert.ok(128 * 512 * 8 > MAX_BYTES);
reads.length = 0; readRange(read, 0xFF00, 10000);
assert.deepEqual(reads.map(([, count]) => count), [4096, 4096, 1808]); assert.equal(reads[0][0], 0xFF00);
assert.deepEqual(greyPalette(2)[1], 0xFFFFFF); assert.equal(greyPalette(1)[3], 0xFFFFFF); assert.equal(greyPalette(0)[0], 0);

// The bridge's palette comes from the real core (screen.capture metadata).
const core = await createBluePixelApi(createModule);
const { mode, colors } = core.debugPalette();
assert.ok([0, 1, 2].includes(mode)); assert.equal(colors.length, 16);
assert.ok(colors.every(c => Number.isInteger(c) && c >= 0 && c <= 0xFFFFFF));
console.log("graphics viewer: ok");
