// web/persist.js: which files are kept, how they go back into a newer disc, and when the watcher
// stores them.
//   node Tests/wasm/test_persist.mjs
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { AmsdosDisc } from "../../web/amsdos.js";
import { applySavedFiles, changedFiles, createDiscWatcher, discKey, memoryFileStore } from "../../web/persist.js";

const nave = new Uint8Array(readFileSync(new URL("../fixtures/nave/nave.dsk", import.meta.url)));
const bytes = (...values) => Uint8Array.from(values);
const withFile = (image, name, data) => { const d = new AmsdosDisc(image); d.write(name, data); return d.image; };

// --- discKey: cache-busting queries share one entry ---
assert.equal(discKey("https://x.example/gamez/catdoku.dsk?v=123"), "/gamez/catdoku.dsk");
assert.equal(discKey("https://x.example/gamez/catdoku.dsk"), "/gamez/catdoku.dsk");

// --- changedFiles: new and modified only ---
{
  const original = [{ user: 0, name: "A.BIN", data: bytes(1, 2) }, { user: 0, name: "B.BIN", data: bytes(3) }];
  const current = [
    { user: 0, name: "A.BIN", data: bytes(1, 2) },        // same
    { user: 0, name: "B.BIN", data: bytes(4) },           // changed
    { user: 0, name: "SAVE.DAT", data: bytes(5) },        // new
    { user: 1, name: "A.BIN", data: bytes(1, 2) },        // same name, other user: new
  ];
  assert.deepEqual(changedFiles(original, current).map(f => `${f.user}:${f.name}`), ["0:B.BIN", "0:SAVE.DAT", "1:A.BIN"]);
}

// --- applySavedFiles: into the published disc, problems reported ---
{
  const save = { user: 0, name: "CATDOKU.SAV", data: new Uint8Array(1024).fill(7) };
  const result = applySavedFiles(nave, [save]);
  assert.deepEqual(result.applied, ["CATDOKU.SAV"]);
  assert.deepEqual(result.errors, []);
  const back = new AmsdosDisc(result.image).files().find(f => f.name === "CATDOKU.SAV");
  assert.deepEqual(back.data, save.data);

  const none = applySavedFiles(nave, []);
  assert.equal(none.image, nave, "nothing saved: the disc is untouched");

  const tooBig = applySavedFiles(nave, [{ user: 0, name: "HUGE.DAT", data: new Uint8Array(300 * 1024) }, save]);
  assert.deepEqual(tooBig.applied, ["CATDOKU.SAV"], "a file that does not fit is skipped, the rest still go in");
  assert.equal(tooBig.errors.length, 1);

  const notDsk = applySavedFiles(new Uint8Array(100), [save]);
  assert.equal(notDsk.applied.length, 0);
  assert.match(notDsk.errors[0], /not a DSK/);
}

// --- watcher: stores after the image settles, only the changed files ---
{
  const store = memoryFileStore();
  const messages = [];
  let current = nave;
  const originalFiles = new AmsdosDisc(nave).files();
  const watcher = createDiscWatcher({ key: "/gamez/nave.dsk", originalFiles, store, exportDisc: () => current, report: m => messages.push(m) });
  watcher.start(nave);

  assert.equal(await watcher.poll(), false, "no change");
  current = withFile(nave, "SAVE.DAT", bytes(1, 2, 3));
  assert.equal(await watcher.poll(), false, "changed this poll: maybe still writing");
  assert.equal(await watcher.poll(), true, "same image twice: stored");
  const kept = await store.get("/gamez/nave.dsk");
  assert.deepEqual(kept.map(f => f.name), ["SAVE.DAT"]);
  assert.deepEqual(kept[0].data.subarray(0, 3), bytes(1, 2, 3));
  assert.equal(await watcher.poll(), false, "already stored");
  assert.match(messages.at(-1), /SAVE\.DAT/);

  current = withFile(current, "SAVE.DAT", bytes(9));
  await watcher.poll();
  assert.equal(await watcher.poll(), true);
  assert.equal((await store.get("/gamez/nave.dsk"))[0].data[0], 9, "the newer contents replace the old");

  // Next visit: a newer published disc gets the saved file back.
  const newer = withFile(nave, "EXTRA.BIN", bytes(4));
  const result = applySavedFiles(newer, await store.get("/gamez/nave.dsk"));
  const names = new AmsdosDisc(result.image).files().map(f => f.name);
  assert(names.includes("SAVE.DAT") && names.includes("EXTRA.BIN"));

  await store.delete("/gamez/nave.dsk");
  assert.deepEqual(await store.get("/gamez/nave.dsk"), []);
}
console.log("test_persist: ok");
