// Find Graphics for the web debugger: memory shown as CPC graphics (mode 0/1/2, linear or screen layout),
// as one bitmap or a grid of frames. Pure logic (no DOM, no core) so Tests/wasm/test_graphics_viewer.mjs can run it.

export const MAX_BYTES = 262144;        // bytes read from the machine for one picture
export const MAX_PIXELS = 16000000;
export const GAP = 2;                   // pixels between frames
export const PIXELS_PER_BYTE = [2, 4, 8];
const READ_CHUNK = 4096;                // largest block the debugger bridge reads at once
const REGISTERS = ["AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC"];

function hexValue(text) {
  const m = /^(?:#|&|\$|0x)([\da-f]{1,4})$/i.exec(text);
  return m ? parseInt(m[1], 16) : null;
}

/** An address as typed in the field: hex (`C000`, `#C000`, `&C000`, `0xC000`), a symbol, a register (`hl`), and `+`/`-` of those
 *  (`spr_player+16`). A bare name is tried as a symbol, then a register, then hex, so `#DE` is the number and `DE` the register. */
export function parseAddress(text, { symbols = new Map(), registers = {} } = {}) {
  const source = String(text ?? "").replace(/\s+/g, "");
  if (!source) return null;
  const terms = source.match(/[+-]?[^+-]+/g);
  if (!terms || terms.join("") !== source) return null;
  let total = 0;
  for (const [index, term] of terms.entries()) {
    const sign = term[0] === "-" ? -1 : 1;
    const body = /^[+-]/.test(term) ? term.slice(1) : term;
    if (!body || (index > 0 && !/^[+-]/.test(term))) return null;
    let value = hexValue(body);
    if (value === null && symbols.has(body)) value = symbols.get(body);
    if (value === null) { const name = REGISTERS.find(r => r === body.toUpperCase()); if (name && registers[name] !== undefined) value = registers[name]; }
    if (value === null && /^[\da-f]{1,4}$/i.test(body)) value = parseInt(body, 16);
    if (value === null) { const lower = body.toLowerCase(); for (const [name, address] of symbols) if (name.toLowerCase() === lower) { value = address; break; } }
    if (value === null) return null;
    total += sign * value;
  }
  return ((total % 0x10000) + 0x10000) % 0x10000;
}

function clamp(value, low, high, fallback) {
  const n = Number.isFinite(Number(value)) && String(value).trim() !== "" ? Math.trunc(Number(value)) : fallback;
  return Math.max(low, Math.min(high, n));
}

/** Normalised picture layout from raw field values. */
export function layoutFor(params) {
  const mode = clamp(params.mode, 0, 2, 1);
  const widthBytes = clamp(params.widthBytes, 1, 128, 16);
  const height = clamp(params.height, 1, 512, 64);
  const frames = clamp(params.frames, 1, 256, 1);
  const cols = Math.min(frames, clamp(params.cols, 1, 64, 4));
  const ppb = PIXELS_PER_BYTE[mode];
  const stride = Number(params.stride) > 0 ? Math.trunc(Number(params.stride)) : widthBytes * height;
  const w = widthBytes * ppb, rows = Math.ceil(frames / cols);
  return {
    address: (Number(params.address) || 0) & 0xFFFF, mode, widthBytes, height, frames, cols, rows, ppb, stride, w,
    interleaved: Boolean(params.interleaved), gap: GAP,
    width: cols * w + (cols - 1) * GAP, imageHeight: rows * height + (rows - 1) * GAP,
  };
}

/** Pen (0-15) of pixel `p` of byte `b`, with the CPC's bit layout for each screen mode. */
export function penOf(mode, b, p) {
  if (mode === 0) {
    const bits = p === 0 ? [7, 3, 5, 1] : [6, 2, 4, 0];
    return bits.reduce((pen, bit, k) => pen | (((b >> bit) & 1) << k), 0);
  }
  if (mode === 1) return ((b >> (7 - p)) & 1) | (((b >> (3 - p)) & 1) << 1);
  return (b >> (7 - p)) & 1;
}

/** Byte offset of (x, y) inside a frame, `x` in pixels. */
export function byteOffset(layout, x, y) {
  const bx = Math.floor(x / layout.ppb);
  return layout.interleaved ? (y & 7) * 0x800 + (y >> 3) * 80 + bx : y * layout.widthBytes + bx;
}

export function greyPalette(mode) {
  const top = [15, 3, 1][mode];
  return Array.from({ length: 16 }, (_, pen) => { const g = Math.min(255, Math.round(pen * 255 / top)); return (g << 16) | (g << 8) | g; });
}

/** Reads `count` bytes starting at `start`, in blocks the bridge accepts. */
export function readRange(read, start, count) {
  const out = new Array(count);
  for (let done = 0; done < count; done += READ_CHUNK) {
    const part = read((start + done) & 0xFFFF, Math.min(READ_CHUNK, count - done));
    for (let i = 0; i < part.length; i++) out[done + i] = part[i];
  }
  return out;
}

/** Decodes the picture. `read(start, count)` returns an array of bytes; `palette` is 16 colours as 0xRRGGBB.
 *  Result: layout, `rgba` (Uint32Array, little-endian RGBA for ImageData) and `pens` (255 in the gaps). */
export function renderGraphics(params, read, palette) {
  const layout = layoutFor(params);
  const lastRow = layout.height - 1;
  const frameBytes = layout.interleaved ? 7 * 0x800 + (lastRow >> 3) * 80 + layout.widthBytes : layout.widthBytes * layout.height;
  if (layout.frames * frameBytes > MAX_BYTES || layout.width * layout.imageHeight > MAX_PIXELS) {
    throw new Error("Too large: reduce frames, width or height");
  }
  const rgba = new Uint32Array(layout.width * layout.imageHeight).fill(0xFF303030);
  const pens = new Uint8Array(layout.width * layout.imageHeight).fill(255);
  const colours = palette.map(rgb => (0xFF000000 | ((rgb & 255) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 255)) >>> 0);
  for (let f = 0; f < layout.frames; f++) {
    const ox = (f % layout.cols) * (layout.w + GAP), oy = Math.floor(f / layout.cols) * (layout.height + GAP);
    const bytes = readRange(read, layout.address + f * layout.stride, frameBytes);
    for (let y = 0; y < layout.height; y++) {
      const rowStart = layout.interleaved ? (y & 7) * 0x800 + (y >> 3) * 80 : y * layout.widthBytes;
      for (let bx = 0; bx < layout.widthBytes; bx++) {
        const b = bytes[rowStart + bx] ?? 0;
        for (let p = 0; p < layout.ppb; p++) {
          const pen = penOf(layout.mode, b, p), i = (oy + y) * layout.width + ox + bx * layout.ppb + p;
          rgba[i] = colours[pen]; pens[i] = pen;
        }
      }
    }
  }
  return { layout, rgba, pens };
}

/** What is under image pixel (ix, iy): frame, position, address and pen; null over a gap or outside. */
export function pixelInfo(picture, ix, iy) {
  const { layout, pens } = picture;
  ix = Math.floor(ix); iy = Math.floor(iy);
  if (ix < 0 || iy < 0 || ix >= layout.width || iy >= layout.imageHeight) return null;
  const column = Math.floor(ix / (layout.w + GAP)), row = Math.floor(iy / (layout.height + GAP));
  const x = ix % (layout.w + GAP), y = iy % (layout.height + GAP), frame = row * layout.cols + column;
  if (column >= layout.cols || frame >= layout.frames || x >= layout.w || y >= layout.height) return null;
  const address = (layout.address + frame * layout.stride + byteOffset(layout, x, y)) & 0xFFFF;
  return { frame, x, y, address, pen: pens[iy * layout.width + ix] };
}

/** Last address covered by the picture, for "Mark as data". */
export function lastAddress(layout) {
  const lastRow = layout.height - 1;
  const frameBytes = layout.interleaved ? 7 * 0x800 + (lastRow >> 3) * 80 + layout.widthBytes : layout.widthBytes * layout.height;
  return (layout.address + (layout.frames - 1) * layout.stride + frameBytes - 1) & 0xFFFF;
}
