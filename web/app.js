import createBluePixelCore from "./bluepixel.js";

const canvas = document.querySelector("#screen");
const ctx = canvas.getContext("2d", { alpha: false });
const image = ctx.createImageData(canvas.width, canvas.height);
const status = document.querySelector("#status");
const hint = document.querySelector("#screen-hint");
const runButton = document.querySelector("#run");
const resetButton = document.querySelector("#reset");
const soundButton = document.querySelector("#sound");
const saveDiskButton = document.querySelector("#save-disc");
const typeButton = document.querySelector("#type");

let core;
let running = false;
let clearHintAfterFrame = false;
let lastTime = 0;
let frameDebt = 0;
let hasOS = false;
let hasBasic = false;
let hasSnapshot = false;
const loadedROMs = { os: null, basic: null, amsdos: null };
let currentDiskName = "bluepixel.dsk";
let audioContext;
let audioNode;
let audioPtr;
let pressed = new Map();

try {
  core = await createBluePixelCore({ locateFile: file => new URL(file, import.meta.url).href });
  if (!core._bp_init()) throw new Error("Could not allocate the CPC machine.");
  status.textContent = "Loading the CPC 6128 ROM set…";
} catch (error) {
  status.textContent = `Could not start WebAssembly: ${error.message}`;
  throw error;
}

function transfer(bytes, action) {
  const ptr = core._malloc(bytes.byteLength);
  if (!ptr) throw new Error("Not enough memory to load this file.");
  try {
    core.HEAPU8.set(bytes, ptr);
    return action(ptr, bytes.byteLength);
  } finally {
    core._free(ptr);
  }
}

function updateReady(message) {
  const ready = (hasOS && hasBasic) || hasSnapshot;
  runButton.disabled = !ready;
  resetButton.disabled = !ready;
  soundButton.disabled = !ready;
  typeButton.disabled = !ready;
  if (ready) {
    status.textContent = "Ready. Click the screen and use your keyboard.";
  } else if (message) {
    hint.hidden = false;
    status.textContent = message;
  }
}

async function installROM(select, slot, key, bytes, name) {
  const ok = bytes
    ? transfer(bytes, (ptr, size) => core._bp_load_rom(slot, ptr, size))
    : core._bp_load_rom(slot, 0, 0);
  if (!ok) throw new Error("The ROM could not be loaded.");
  if (key === "os") hasOS = Boolean(bytes);
  if (key === "basic") hasBasic = Boolean(bytes);
  loadedROMs[key] = select.value;
  if ((key === "os" || key === "basic") && hasOS && hasBasic) core._bp_reset();
  status.textContent = bytes ? `${name} loaded.` : "Disk ROM removed.";
  updateReady();
}

async function loadROMFromFolder(select, slot, key) {
  const name = select.value;
  if (!name && key === "amsdos") {
    try { await installROM(select, slot, key, null, ""); }
    catch (error) { status.textContent = `ROM error: ${error.message}`; }
    return true;
  }
  if (!name) return false;
  try {
    const url = new URL(`./roms/${encodeURIComponent(name)}`, import.meta.url);
    const response = await fetch(url);
    if (!response.ok) throw new Error(`${name} was not found in the bundled ROM folder.`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    await installROM(select, slot, key, bytes, name);
    return true;
  } catch (error) {
    status.textContent = `ROM error: ${error.message}`;
    if (loadedROMs[key] !== null) select.value = loadedROMs[key];
    return false;
  }
}

const osROM = document.querySelector("#os-rom");
const basicROM = document.querySelector("#basic-rom");
const amsdosROM = document.querySelector("#amsdos-rom");
osROM.addEventListener("change", () => loadROMFromFolder(osROM, -1, "os"));
basicROM.addEventListener("change", () => loadROMFromFolder(basicROM, 0, "basic"));
amsdosROM.addEventListener("change", () => loadROMFromFolder(amsdosROM, 7, "amsdos"));

async function loadDefaultROMs() {
  const results = await Promise.all([
    loadROMFromFolder(osROM, -1, "os"),
    loadROMFromFolder(basicROM, 0, "basic"),
    loadROMFromFolder(amsdosROM, 7, "amsdos"),
  ]);
  if (!hasOS || !hasBasic) {
    const missing = [];
    if (!hasOS) missing.push("OS");
    if (!hasBasic) missing.push("BASIC");
    updateReady(`${missing.join(" and ")} ROM not found in the bundled ROM folder. Choose another ROM from its menu.`);
  } else {
    status.textContent = results[2]
      ? "CPC 6128 ROM set loaded. Ready."
      : "CPC 6128 is ready; its optional disk ROM was not found. Choose one from the menu or select a file.";
  }
}

loadDefaultROMs();

document.querySelector("#disk").addEventListener("change", async event => {
  const file = event.currentTarget.files?.[0];
  if (!file) return;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (!transfer(bytes, (ptr, size) => core._bp_load_disc(ptr, size))) {
      throw new Error("This is not a supported DSK image.");
    }
    currentDiskName = file.name;
    saveDiskButton.disabled = false;
    status.textContent = `${file.name} inserted in drive A.`;
  } catch (error) {
    status.textContent = `Disk error: ${error.message}`;
  }
});

document.querySelector("#snapshot").addEventListener("change", async event => {
  const file = event.currentTarget.files?.[0];
  if (!file) return;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    if (!transfer(bytes, (ptr, size) => core._bp_load_snapshot(ptr, size))) {
      throw new Error("This is not a supported SNA snapshot.");
    }
    hasSnapshot = true;
    hint.hidden = true;
    updateReady();
    status.textContent = `${file.name} loaded.`;
  } catch (error) {
    status.textContent = `Snapshot error: ${error.message}`;
  }
});

runButton.addEventListener("click", async () => {
  running = !running;
  runButton.textContent = running ? "Pause" : "Resume";
  status.textContent = running ? "Running" : "Paused";
  if (running) clearHintAfterFrame = true;
  if (running && audioContext?.state === "suspended") await audioContext.resume();
  canvas.focus();
});

resetButton.addEventListener("click", () => {
  core._bp_reset();
  frameDebt = 0;
  running = true;
  clearHintAfterFrame = true;
  runButton.textContent = "Pause";
  status.textContent = "Machine restarted.";
  canvas.focus();
});

typeButton.addEventListener("click", () => {
  const input = document.querySelector("#type-text");
  if (!input.value) return;
  const text = new TextEncoder().encode(input.value + "\0");
  transfer(text, ptr => core._bp_type(ptr));
  input.value = "";
  canvas.focus();
});

saveDiskButton.addEventListener("click", () => {
  const sizePtr = core._malloc(4);
  let dataPtr = 0;
  try {
    core.HEAPU32[sizePtr >> 2] = 0;
    dataPtr = core._bp_export_disc(sizePtr);
    const size = core.HEAPU32[sizePtr >> 2];
    if (!dataPtr || !size) throw new Error("No disk is inserted.");
    const bytes = core.HEAPU8.slice(dataPtr, dataPtr + size);
    const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = currentDiskName.replace(/\.dsk$/i, "") + "-bluepixel.dsk";
    link.click();
    URL.revokeObjectURL(url);
    status.textContent = "Disk image downloaded.";
  } catch (error) {
    status.textContent = `Could not save disk: ${error.message}`;
  } finally {
    core._bp_release_disc_export();
    core._free(sizePtr);
  }
});

const keyCodes = {
  KeyA: 69, KeyB: 54, KeyC: 62, KeyD: 61, KeyE: 58, KeyF: 53, KeyG: 52,
  KeyH: 44, KeyI: 35, KeyJ: 45, KeyK: 37, KeyL: 36, KeyM: 38, KeyN: 46,
  KeyO: 34, KeyP: 27, KeyQ: 67, KeyR: 50, KeyS: 60, KeyT: 51, KeyU: 42,
  KeyV: 55, KeyW: 59, KeyX: 63, KeyY: 43, KeyZ: 71,
  Digit0: 32, Digit1: 64, Digit2: 65, Digit3: 57, Digit4: 56,
  Digit5: 49, Digit6: 48, Digit7: 41, Digit8: 40, Digit9: 33,
  ArrowUp: 0, ArrowRight: 1, ArrowDown: 2, ArrowLeft: 8,
  Enter: 18, NumpadEnter: 6, Space: 47, Backspace: 79, Delete: 16,
  Escape: 66, Tab: 68, ShiftLeft: 21, ShiftRight: 21,
  ControlLeft: 23, ControlRight: 23,
  Minus: 25, Equal: 26, BracketLeft: 17, BracketRight: 19,
  Semicolon: 28, Quote: 29, Backslash: 22, Comma: 39, Period: 31, Slash: 30,
  F1: 13, F2: 14, F3: 5, F4: 20, F5: 12, F6: 4, F7: 10, F8: 11, F9: 3, F10: 15,
};
const joystickBits = { ArrowUp: 0, ArrowDown: 1, ArrowLeft: 2, ArrowRight: 3, ControlLeft: 4, ControlRight: 4, AltLeft: 5, AltRight: 5 };

function setKey(event, down) {
  if (event.target instanceof HTMLElement && event.target.closest("input, textarea, select, button")) return;
  const key = keyCodes[event.code];
  const joy = joystickBits[event.code];
  if (key === undefined && joy === undefined) return;
  event.preventDefault();
  if (down && event.repeat) return;
  if (key !== undefined) core._bp_key(key, down ? 1 : 0);
  if (joy !== undefined) core._bp_joystick(joy, down ? 1 : 0);
  if (down) pressed.set(event.code, { key, joy });
  else pressed.delete(event.code);
}

window.addEventListener("keydown", event => setKey(event, true));
window.addEventListener("keyup", event => setKey(event, false));
window.addEventListener("blur", () => {
  for (const [code, item] of pressed) {
    if (item.key !== undefined) core._bp_key(item.key, 0);
    if (item.joy !== undefined) core._bp_joystick(item.joy, 0);
    pressed.delete(code);
  }
});

soundButton.addEventListener("click", async () => {
  try {
    audioContext ??= new AudioContext({ sampleRate: 44100 });
    audioPtr ??= core._malloc(1024 * 2 * Float32Array.BYTES_PER_ELEMENT);
    audioNode ??= audioContext.createScriptProcessor(1024, 0, 2);
    audioNode.onaudioprocess = event => {
      const frames = event.outputBuffer.length;
      const offset = audioPtr >> 2;
      core.HEAPF32.fill(0, offset, offset + frames * 2);
      core._bp_read_audio(audioPtr, frames);
      const interleaved = core.HEAPF32.subarray(offset, offset + frames * 2);
      const left = event.outputBuffer.getChannelData(0);
      const right = event.outputBuffer.getChannelData(1);
      for (let i = 0; i < frames; i++) {
        left[i] = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
      }
    };
    audioNode.connect(audioContext.destination);
    await audioContext.resume();
    soundButton.textContent = "Sound enabled";
    soundButton.disabled = true;
    status.textContent = "Audio is enabled.";
  } catch (error) {
    status.textContent = `Audio error: ${error.message}`;
  }
});

function drawFrame() {
  const ptr = core._bp_frame();
  if (!ptr) return;
  const pixels = core.HEAPU32.subarray(ptr >> 2, (ptr >> 2) + canvas.width * canvas.height);
  for (let i = 0; i < pixels.length; i++) {
    const pixel = pixels[i];
    const j = i * 4;
    image.data[j] = (pixel >>> 16) & 255;
    image.data[j + 1] = (pixel >>> 8) & 255;
    image.data[j + 2] = pixel & 255;
    image.data[j + 3] = 255;
  }
  ctx.putImageData(image, 0, 0);
}

function animate(time) {
  if (!lastTime) lastTime = time;
  const elapsed = Math.min(time - lastTime, 100);
  lastTime = time;
  if (running) {
    frameDebt += elapsed;
    let count = 0;
    while (frameDebt >= 20 && count++ < 4) {
      core._bp_run_frame();
      frameDebt -= 20;
    }
    drawFrame();
    if (clearHintAfterFrame) {
      hint.hidden = true;
      clearHintAfterFrame = false;
    }
  }
  requestAnimationFrame(animate);
}
requestAnimationFrame(animate);
