// Saved files for discs loaded by URL (?disk=…): what a program writes to the disc is kept in the
// browser (IndexedDB) and put back into the published disc on the next visit.
// Only the files that differ from the published disc are kept, not the whole image, so a new
// version of the game still loads and keeps its save files. Pure logic plus a small IndexedDB
// wrapper; app.js supplies the core. Tests: Tests/wasm/test_persist.mjs.

import { AmsdosDisc, AmsdosError } from "./amsdos.js";

const DB_NAME = "bluepixel";
const DB_STORE = "disc-files";

const fileKey = file => `${file.user}:${file.name}`;

function sameBytes(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// Files of `current` that are new or different from `original` (both from AmsdosDisc.files()).
export function changedFiles(original, current) {
  const before = new Map(original.map(file => [fileKey(file), file.data]));
  return current.filter(file => {
    const old = before.get(fileKey(file));
    return !old || !sameBytes(old, file.data);
  });
}

// Storage key of a disc: its URL path without query, so ?v=… cache busters share one entry.
export function discKey(url) {
  return new URL(url).pathname;
}

// Writes saved files into a disc image. Returns { image, applied, errors }; a file that cannot be
// written (no room, bad name, not a DATA disc) is reported and skipped.
export function applySavedFiles(image, saved) {
  const result = { image, applied: [], errors: [] };
  if (!saved?.length) return result;
  let disc;
  try { disc = new AmsdosDisc(image); }
  catch (error) { result.errors.push(error.message); return result; }
  for (const file of saved) {
    try { disc.write(file.name, file.data, file.user); result.applied.push(file.name); }
    catch (error) {
      if (!(error instanceof AmsdosError)) throw error;
      result.errors.push(error.message);
    }
  }
  result.image = disc.image;
  return result;
}

// Simple 32-bit FNV-1a, to notice that the exported disc changed without keeping copies.
export function hashBytes(bytes) {
  let h = 0x811C9DC5;
  for (let i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193); }
  return h >>> 0;
}

// Watches one drive. poll() is called on a timer: it exports the disc, and when the image has
// changed and then stayed the same for one more poll (the program finished writing), stores the
// files that differ from the published disc.
//   io: exportDisc() -> Uint8Array | null, store: { put(key, files) }, report(message)
export function createDiscWatcher({ key, originalFiles, exportDisc, store, report = () => {} }) {
  let lastHash = null;      // image at the previous poll
  let savedHash = null;     // image whose files were last stored (or the start-up image)
  let busy = false;
  return {
    start(image) { lastHash = savedHash = hashBytes(image); },
    async poll() {
      if (busy) return false;
      busy = true;
      try {
        const image = exportDisc();
        if (!image) return false;
        const hash = hashBytes(image);
        const stable = hash === lastHash;
        lastHash = hash;
        if (!stable || hash === savedHash) return false;
        let files;
        try { files = changedFiles(originalFiles, new AmsdosDisc(image).files()); }
        catch (error) {
          if (!(error instanceof AmsdosError)) throw error;
          report(`saved files: ${error.message}`);
          savedHash = hash;
          return false;
        }
        await store.put(key, files);
        savedHash = hash;
        report(files.length ? `saved ${files.map(file => file.name).join(", ")} in this browser` : "no saved files");
        return true;
      } finally { busy = false; }
    },
  };
}

// Same interface as openFileStore, in memory (tests, and browsers without IndexedDB).
export function memoryFileStore() {
  const data = new Map();
  return {
    async get(key) { return data.get(key) ?? []; },
    async put(key, files) { if (files.length) data.set(key, files); else data.delete(key); },
    async delete(key) { data.delete(key); },
  };
}

// IndexedDB store: key = disc path, value = [{ user, name, data: Uint8Array }].
export function openFileStore(indexedDB = globalThis.indexedDB) {
  if (!indexedDB) return memoryFileStore();
  const opened = new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(DB_STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  const run = (mode, action) => opened.then(db => new Promise((resolve, reject) => {
    const tx = db.transaction(DB_STORE, mode);
    const request = action(tx.objectStore(DB_STORE));
    tx.oncomplete = () => resolve(request.result);
    tx.onerror = () => reject(tx.error);
  }));
  return {
    async get(key) { return (await run("readonly", store => store.get(key))) ?? []; },
    put(key, files) { return run("readwrite", store => files.length ? store.put(files, key) : store.delete(key)); },
    delete(key) { return run("readwrite", store => store.delete(key)); },
  };
}
