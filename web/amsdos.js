// AMSDOS files inside a DSK image (DATA format only): list, read, write, delete.
// Pure logic on a Uint8Array, no DOM and no core. Tests: Tests/wasm/test_amsdos.mjs.
//
// DATA format: 40 tracks of 9 sectors (IDs 0xC1..0xC9) of 512 bytes, 1 KB blocks, directory in
// blocks 0 and 1 (64 entries of 32 bytes, track 0 sectors 0xC1..0xC4), 180 blocks in all.
// Sectors are found by ID, so interleaved images (C1 C6 C2 C7 ...) work.

const SECTOR_SIZE = 512;
const FIRST_SECTOR = 0xC1;
const SECTORS_PER_TRACK = 9;
const BLOCK_SIZE = 1024;
const DIR_ENTRIES = 64;
const DIR_BLOCKS = 2;
const TOTAL_BLOCKS = 180;
const RECORD = 128;
const RECORDS_PER_EXTENT = 128;      // 16 blocks of 1 KB
const BLOCKS_PER_EXTENT = 16;
const FREE = 0xE5;

export class AmsdosError extends Error {}

function ascii(bytes, start, length) {
  let text = "";
  for (let i = 0; i < length; i++) text += String.fromCharCode(bytes[start + i]);
  return text;
}

// Map every sector of side 0 to its offset in the image: "track:id" -> offset.
function sectorTable(image) {
  const header = ascii(image, 0, 8);
  const extended = header === "EXTENDED";
  if (!extended && header !== "MV - CPC") throw new AmsdosError("not a DSK image");
  const tracks = image[0x30], sides = image[0x31];
  const fixedSize = image[0x32] | (image[0x33] << 8);
  const table = new Map();
  let offset = 0x100;
  for (let t = 0; t < tracks * sides; t++) {
    const size = extended ? image[0x34 + t] * 256 : fixedSize;
    if (!size) continue;   // unformatted track (extended images only)
    if (offset + size > image.length || ascii(image, offset, 10) !== "Track-Info") throw new AmsdosError("truncated DSK image");
    const track = image[offset + 0x10], side = image[offset + 0x11];
    const count = image[offset + 0x15];
    let data = offset + 0x100;
    for (let s = 0; s < count; s++) {
      const info = offset + 0x18 + s * 8;
      const id = image[info + 2], n = image[info + 3];
      const length = extended ? (image[info + 6] | (image[info + 7] << 8)) : 128 << n;
      if (side === 0 && length === SECTOR_SIZE) table.set(`${track}:${id}`, data);
      data += length;
    }
    offset += size;
  }
  return table;
}

// One AMSDOS disc view over a copy of the image bytes.
export class AmsdosDisc {
  constructor(image) {
    this.image = new Uint8Array(image);
    this.sectors = sectorTable(this.image);
    for (let i = 0; i < 4; i++) {
      if (!this.sectors.has(`0:${FIRST_SECTOR + i}`)) throw new AmsdosError("not a DATA format disc");
    }
  }

  // Offset of 512 bytes of logical sector n (0 = track 0 sector 0xC1).
  sectorOffset(n) {
    const key = `${Math.floor(n / SECTORS_PER_TRACK)}:${FIRST_SECTOR + (n % SECTORS_PER_TRACK)}`;
    const offset = this.sectors.get(key);
    if (offset === undefined) throw new AmsdosError(`sector ${key} missing`);
    return offset;
  }

  readBlock(block) {
    const out = new Uint8Array(BLOCK_SIZE);
    out.set(this.image.subarray(this.sectorOffset(block * 2), this.sectorOffset(block * 2) + SECTOR_SIZE), 0);
    out.set(this.image.subarray(this.sectorOffset(block * 2 + 1), this.sectorOffset(block * 2 + 1) + SECTOR_SIZE), SECTOR_SIZE);
    return out;
  }

  writeBlock(block, data) {
    for (let half = 0; half < 2; half++) {
      const chunk = new Uint8Array(SECTOR_SIZE);
      chunk.set(data.subarray(half * SECTOR_SIZE, (half + 1) * SECTOR_SIZE));
      this.image.set(chunk, this.sectorOffset(block * 2 + half));
    }
  }

  // Directory entry i: offset in the image.
  entryOffset(i) {
    const byte = i * 32;
    return this.sectorOffset(Math.floor(byte / SECTOR_SIZE)) + (byte % SECTOR_SIZE);
  }

  entries() {
    const list = [];
    for (let i = 0; i < DIR_ENTRIES; i++) {
      const o = this.entryOffset(i);
      const user = this.image[o];
      if (user === FREE || user > 15) continue;
      let name = "";
      for (let k = 1; k <= 11; k++) {
        const c = this.image[o + k] & 0x7F;
        if (k === 9) name += ".";
        if (c !== 0x20) name += String.fromCharCode(c);
      }
      if (name.endsWith(".")) name = name.slice(0, -1);
      const extent = (this.image[o + 12] & 0x1F) | (this.image[o + 14] << 5);
      const blocks = [];
      for (let k = 16; k < 32; k++) if (this.image[o + k]) blocks.push(this.image[o + k]);
      list.push({ index: i, user, name, extent, records: this.image[o + 15], blocks });
    }
    return list;
  }

  // Files as { user, name, data }, data rounded up to 128-byte records as stored.
  files() {
    const byKey = new Map();
    for (const entry of this.entries()) {
      const key = `${entry.user}:${entry.name}`;
      if (!byKey.has(key)) byKey.set(key, { user: entry.user, name: entry.name, extents: [] });
      byKey.get(key).extents.push(entry);
    }
    const out = [];
    for (const file of byKey.values()) {
      file.extents.sort((a, b) => a.extent - b.extent);
      const chunks = [];
      let size = 0;
      for (const extent of file.extents) {
        const bytes = extent.records * RECORD;
        let left = bytes;
        for (const block of extent.blocks) {
          if (left <= 0) break;
          const data = this.readBlock(block).subarray(0, Math.min(BLOCK_SIZE, left));
          chunks.push(data);
          left -= data.length;
          size += data.length;
        }
      }
      const data = new Uint8Array(size);
      let at = 0;
      for (const chunk of chunks) { data.set(chunk, at); at += chunk.length; }
      out.push({ user: file.user, name: file.name, data });
    }
    return out;
  }

  remove(name, user = 0) {
    const wanted = normaliseName(name);
    for (const entry of this.entries()) {
      if (entry.user === user && entry.name === wanted) this.image[this.entryOffset(entry.index)] = FREE;
    }
  }

  // Replaces (or creates) a file. Throws AmsdosError when the disc has no room; the image is
  // then left unchanged.
  write(name, data, user = 0) {
    const wanted = normaliseName(name);
    const field = nameField(wanted);
    const backup = this.image.slice();
    try {
      this.remove(wanted, user);
      const used = new Set();
      for (const entry of this.entries()) for (const block of entry.blocks) used.add(block);
      const free = [];
      for (let b = DIR_BLOCKS; b < TOTAL_BLOCKS; b++) if (!used.has(b)) free.push(b);
      const freeEntries = [];
      for (let i = 0; i < DIR_ENTRIES; i++) if (this.image[this.entryOffset(i)] === FREE) freeEntries.push(i);

      const records = Math.ceil(data.length / RECORD);
      const blocks = Math.ceil(records * RECORD / BLOCK_SIZE);
      const extents = Math.max(1, Math.ceil(records / RECORDS_PER_EXTENT));
      if (blocks > free.length) throw new AmsdosError(`no room on the disc for ${wanted}`);
      if (extents > freeEntries.length) throw new AmsdosError(`directory full, cannot write ${wanted}`);

      const padded = new Uint8Array(blocks * BLOCK_SIZE).fill(0x1A);   // CP/M end-of-file filler
      padded.set(data);
      for (let k = 0; k < blocks; k++) this.writeBlock(free[k], padded.subarray(k * BLOCK_SIZE, (k + 1) * BLOCK_SIZE));

      for (let x = 0; x < extents; x++) {
        const o = this.entryOffset(freeEntries[x]);
        const entry = new Uint8Array(32);
        entry[0] = user;
        entry.set(field, 1);
        entry[12] = x & 0x1F;
        entry[14] = x >> 5;
        entry[15] = Math.min(RECORDS_PER_EXTENT, records - x * RECORDS_PER_EXTENT);
        for (let k = 0; k < BLOCKS_PER_EXTENT; k++) {
          const index = x * BLOCKS_PER_EXTENT + k;
          if (index < blocks) entry[16 + k] = free[index];
        }
        this.image.set(entry, o);
      }
    } catch (error) {
      this.image = backup;
      throw error;
    }
  }
}

// "catdoku.sav" -> "CATDOKU.SAV"; 8.3, printable ASCII only.
export function normaliseName(name) {
  const upper = String(name).toUpperCase();
  const [base, ext = "", ...rest] = upper.split(".");
  if (rest.length || !base || base.length > 8 || ext.length > 3 || /[^\x21-\x7E]/.test(base + ext)) {
    throw new AmsdosError(`invalid AMSDOS file name: ${name}`);
  }
  return ext ? `${base}.${ext}` : base;
}

function nameField(name) {
  const [base, ext = ""] = name.split(".");
  const field = new Uint8Array(11).fill(0x20);
  for (let i = 0; i < base.length; i++) field[i] = base.charCodeAt(i);
  for (let i = 0; i < ext.length; i++) field[8 + i] = ext.charCodeAt(i);
  return field;
}
