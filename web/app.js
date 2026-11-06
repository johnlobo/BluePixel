import createBluePixelCore from "./bluepixel.js";

const canvas = document.querySelector("#screen");
const ctx = canvas.getContext("2d", { alpha: false });
const image = ctx.createImageData(canvas.width, canvas.height);
const imageWords = new Uint32Array(image.data.buffer);
const isLittleEndian = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
const status = document.querySelector("#status");
const hint = document.querySelector("#screen-hint");
const runButton = document.querySelector("#run");
const resetButton = document.querySelector("#reset");
const soundButton = document.querySelector("#sound");
const saveDiskButtons = [document.querySelector("#save-disc-a"), document.querySelector("#save-disc-b")];
const typeButton = document.querySelector("#type");
let menuOpen = false;
let activeMenu = 1;
let menuStack = [];
let menuLayouts = [];
let menuBarLayouts = [];
let typeEntry = false;
let typeBuffer = "";
let soundEnabled = true;
let menuTipTimer;
const menuTip = document.querySelector("#menu-tip");

function closeMenu() {
  menuOpen = false;
  typeEntry = false;
  hint.hidden = running || hasSnapshot;
  drawFrame();
}

function showMenuTip() {
  menuTip.hidden = false;
  clearTimeout(menuTipTimer);
  menuTipTimer = setTimeout(() => { menuTip.hidden = true; }, 2600);
}

function menuItem(label, action, options = {}) {
  return { label, action, disabled: Boolean(options.disabled), stayOpen: Boolean(options.stayOpen) };
}

function menuBranch(label, items) {
  return { label, items };
}

function romItems(select) {
  return [...select.options].map(option => menuItem(
    `${option.textContent}${option.value === select.value ? "  ✓" : ""}`,
    () => {
      select.value = option.value;
      select.dispatchEvent(new Event("change", { bubbles: true }));
    },
  ));
}

function menuGroups() {
  const diskInputs = [document.querySelector("#disk-a"), document.querySelector("#disk-b")];
  const snapshotInput = document.querySelector("#snapshot");
  const typeText = document.querySelector("#type-text");
  return [
    { label: "BluePixel", items: [menuItem("About BluePixel CPC", () => { status.textContent = "BluePixel — Amstrad CPC 6128 emulator."; })] },
    { label: "File", items: [
      ...[0, 1].map(drive => menuBranch(`Drive ${drive === 0 ? "A" : "B"}:`, [
        menuItem("Insert Disc Image…", () => diskInputs[drive].click()),
        menuItem("Download Changed Disc…", () => saveDiskButtons[drive].click(), { disabled: saveDiskButtons[drive].disabled }),
      ])),
      menuItem("Load Snapshot…", () => snapshotInput.click()),
      menuItem("Auto Type…", () => { typeEntry = true; typeBuffer = typeText.value || ""; }, { stayOpen: true }),
    ] },
    { label: "Settings", items: [
      menuBranch("Memory", [
        menuBranch("Operating System ROM", romItems(document.querySelector("#os-rom"))),
        menuBranch("BASIC ROM", romItems(document.querySelector("#basic-rom"))),
        menuBranch("Disk ROM", romItems(document.querySelector("#amsdos-rom"))),
      ]),
      menuBranch("Machine", [
        menuItem(running ? "Pause" : "Resume", () => runButton.click()),
        menuItem("Reset", () => resetButton.click()),
      ]),
      menuBranch("Sound", [
        menuItem(`Sound On${soundEnabled ? "  ✓" : ""}`, () => setSoundEnabled(true)),
        menuItem(`Sound Off${soundEnabled ? "" : "  ✓"}`, () => setSoundEnabled(false)),
      ]),
    ] },
    { label: "Help", items: [
      menuItem("Keyboard Controls", () => { status.textContent = "Click the screen for keyboard focus. Arrows also control the joystick; Ctrl/Alt fire."; }),
    ] },
  ];
}

function setActiveMenu(index) {
  const groups = menuGroups();
  activeMenu = (index + groups.length) % groups.length;
  menuStack = [{ title: groups[activeMenu].label, items: groups[activeMenu].items, selected: 0 }];
}

function openMenu() {
  menuOpen = true;
  setActiveMenu(activeMenu);
  hint.hidden = true;
  clearTimeout(menuTipTimer);
  menuTip.hidden = true;
  drawFrame();
}

function activateMenuEntry(entry) {
  if (!entry || entry.disabled) return;
  if (entry.items) {
    menuStack.push({ title: entry.label, items: entry.items, selected: 0 });
    return;
  }
  entry.action?.();
  if (!entry.stayOpen) closeMenu();
}

function handleMenuKey(event) {
  if (!menuOpen) return false;
  if (typeEntry) {
    if (event.code === "Escape") { typeEntry = false; typeBuffer = ""; }
    else if (event.code === "Enter") {
      if (typeBuffer) {
        const bytes = new TextEncoder().encode(typeBuffer + "\0");
        transfer(bytes, ptr => core._bp_type(ptr));
      }
      typeEntry = false;
      typeBuffer = "";
    } else if (event.code === "Backspace") typeBuffer = typeBuffer.slice(0, -1);
    else if (event.key.length === 1 && !event.ctrlKey && !event.metaKey) typeBuffer += event.key;
    return true;
  }
  const current = menuStack.at(-1);
  if (event.code === "Escape") {
    if (menuStack.length > 1) menuStack.pop();
    else closeMenu();
  } else if (menuStack.length > 1 && event.code === "ArrowLeft") menuStack.pop();
  else if (menuStack.length === 1 && event.code === "ArrowLeft") setActiveMenu(activeMenu - 1);
  else if (menuStack.length === 1 && event.code === "ArrowRight") setActiveMenu(activeMenu + 1);
  else if (event.code === "ArrowDown") current.selected = (current.selected + 1) % current.items.length;
  else if (event.code === "ArrowUp") current.selected = (current.selected + current.items.length - 1) % current.items.length;
  else if (event.code === "ArrowRight" || event.code === "Enter") activateMenuEntry(current.items[current.selected]);
  return true;
}

window.addEventListener("keydown", event => {
  if (soundEnabled) resumeAudio();
  if (event.code === "F10") {
    event.preventDefault();
    if (menuOpen) closeMenu(); else openMenu();
    canvas.focus();
    return;
  }
  if (handleMenuKey(event)) {
    event.preventDefault();
    event.stopImmediatePropagation();
    drawFrame();
  }
});

canvas.addEventListener("click", event => {
  if (!menuOpen) {
    showMenuTip();
    if (!running) openMenu();
    canvas.focus();
    return;
  }
  const rect = canvas.getBoundingClientRect();
  const x = (event.clientX - rect.left) * canvas.width / rect.width;
  const y = (event.clientY - rect.top) * canvas.height / rect.height;
  const barItem = menuBarLayouts.find(item => x >= item.x && x <= item.x + item.width && y >= 8 && y <= 36);
  if (barItem) {
    setActiveMenu(barItem.index);
    drawFrame();
    return;
  }
  const layout = menuLayouts[0];
  const frame = menuStack.at(-1);
  if (layout && x >= layout.x && x <= layout.x + layout.width && y >= layout.rowTop) {
    const index = Math.floor((y - layout.rowTop) / layout.rowHeight);
    if (index >= 0 && index < frame.items.length) {
      frame.selected = index;
      activateMenuEntry(frame.items[index]);
      drawFrame();
      return;
    }
  }
  closeMenu();
});
let core;
let running = false;
let clearHintAfterFrame = false;
let lastTime = 0;
let frameDebt = 0;
let hasOS = false;
let hasBasic = false;
let hasSnapshot = false;
const loadedROMs = { os: null, basic: null, amsdos: null };
const currentDiskNames = ["bluepixel-a.dsk", "bluepixel-b.dsk"];
let audioContext;
let audioNode;
let audioPtr;
let pressed = new Map();
let audioConnected = false;

try {
  core = await createBluePixelCore({ locateFile: file => new URL(file, import.meta.url).href });
  if (!core._bp_init()) throw new Error("Could not allocate the CPC machine.");
  try { initializeAudioGraph(); }
  catch (error) { soundEnabled = false; soundButton.textContent = "Sound Off"; console.warn(error.message); }
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
    running = true;
    runButton.textContent = "Pause";
    clearHintAfterFrame = true;
    status.textContent = results[2]
      ? "CPC 6128 ROM set loaded. Running."
      : "CPC 6128 is running; optional disk ROM was not found.";
  }
}

loadDefaultROMs();

[0, 1].forEach(drive => {
  const input = document.querySelector(drive === 0 ? "#disk-a" : "#disk-b");
  input.addEventListener("change", async event => {
    const file = event.currentTarget.files?.[0];
    if (!file) return;
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (!transfer(bytes, (ptr, size) => core._bp_load_disc(drive, ptr, size))) {
        throw new Error("This is not a supported DSK image.");
      }
      currentDiskNames[drive] = file.name;
      saveDiskButtons[drive].disabled = false;
      status.textContent = `${file.name} inserted in drive ${drive === 0 ? "A" : "B"}.`;
    } catch (error) {
      status.textContent = `Disk error: ${error.message}`;
    }
  });
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

function downloadDisc(drive) {
  const sizePtr = core._malloc(4);
  let dataPtr = 0;
  try {
    core.HEAPU32[sizePtr >> 2] = 0;
    dataPtr = core._bp_export_disc(drive, sizePtr);
    const size = core.HEAPU32[sizePtr >> 2];
    if (!dataPtr || !size) throw new Error("No disk is inserted.");
    const bytes = core.HEAPU8.slice(dataPtr, dataPtr + size);
    const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = currentDiskNames[drive].replace(/\.dsk$/i, "") + "-bluepixel.dsk";
    link.click();
    URL.revokeObjectURL(url);
    status.textContent = `Drive ${drive === 0 ? "A" : "B"} disk image downloaded.`;
  } catch (error) {
    status.textContent = `Could not save disk: ${error.message}`;
  } finally {
    core._bp_release_disc_export();
    core._free(sizePtr);
  }
}

saveDiskButtons.forEach((button, drive) => button.addEventListener("click", () => downloadDisc(drive)));

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
  if (event.code === "F10" || menuOpen) return;
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

function initializeAudioGraph() {
  if (audioConnected) return;
  if (!window.AudioContext) throw new Error("Web Audio is not available in this browser.");
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
  audioConnected = true;
  soundButton.textContent = soundEnabled ? "Sound On" : "Sound Off";
}

async function resumeAudio() {
  if (!soundEnabled) return;
  try {
    initializeAudioGraph();
    if (audioContext.state === "suspended") await audioContext.resume();
  } catch (error) {
    soundEnabled = false;
    soundButton.textContent = "Sound Off";
    status.textContent = `Audio error: ${error.message}`;
  }
}

async function setSoundEnabled(enabled) {
  soundEnabled = enabled;
  soundButton.textContent = enabled ? "Sound On" : "Sound Off";
  if (enabled) await resumeAudio();
  else if (audioContext?.state === "running") await audioContext.suspend();
  status.textContent = soundEnabled ? "Sound is on." : "Sound is off.";
  if (menuOpen) drawFrame();
}

soundButton.addEventListener("click", () => setSoundEnabled(!soundEnabled));
window.addEventListener("pointerdown", () => resumeAudio(), { capture: true });

function drawFrame() {
  if (!core) return;
  const ptr = core._bp_frame();
  if (!ptr) return;
  const pixels = core.HEAPU32.subarray(ptr >> 2, (ptr >> 2) + canvas.width * canvas.height);
  for (let i = 0; i < pixels.length; i++) {
    const pixel = pixels[i];
    imageWords[i] = isLittleEndian
      ? (0xFF000000 | ((pixel & 255) << 16) | (pixel & 0xFF00) | ((pixel >>> 16) & 255))
      : (pixel | 0xFF000000);
  }
  ctx.putImageData(image, 0, 0);
  if (menuOpen) drawAppMenu();
}

function drawAppMenu() {
  const groups = menuGroups();
  menuBarLayouts = [];
  menuLayouts = [];
  ctx.save();
  ctx.font = "bold 15px monospace";
  ctx.textBaseline = "middle";
  const barY = 8, barH = 28;
  let barX = 8;
  ctx.fillStyle = "#e7e7e7";
  ctx.fillRect(barX, barY, 500, barH);
  for (let i = 0; i < groups.length; i++) {
    const width = Math.ceil(ctx.measureText(groups[i].label).width) + 22;
    const selected = i === activeMenu;
    menuBarLayouts.push({ index: i, x: barX, width });
    ctx.fillStyle = selected ? "#111" : "#e7e7e7";
    ctx.fillRect(barX, barY, width, barH);
    ctx.fillStyle = selected ? "#fff" : "#111";
    ctx.fillText(groups[i].label, barX + 11, barY + barH / 2);
    barX += width;
  }

  const frame = menuStack.at(-1);
  if (frame) {
    const x = 8, y = 39, width = 430, rowTop = 68, rowHeight = 27;
    const height = 35 + frame.items.length * rowHeight + 24;
    ctx.fillStyle = "#111";
    ctx.fillRect(x - 2, y - 2, width + 4, height + 4);
    ctx.fillStyle = "#d2d2d2";
    ctx.fillRect(x, y, width, height);
    ctx.fillStyle = "#c5c5c5";
    ctx.fillRect(x + 2, y + 2, width - 4, 25);
    ctx.fillStyle = "#111";
    ctx.font = "bold 14px monospace";
    ctx.fillText(menuStack.map(part => part.title).join("  ›  "), x + 10, y + 14);
    ctx.font = "14px monospace";
    for (let i = 0; i < frame.items.length; i++) {
      const item = frame.items[i];
      const rowY = rowTop + i * rowHeight;
      const selected = i === frame.selected;
      ctx.fillStyle = selected ? "#111" : "#fff";
      ctx.fillRect(x + 3, rowY, width - 6, rowHeight);
      ctx.fillStyle = item.disabled ? "#888" : selected ? "#fff" : "#111";
      const suffix = item.items ? "  ›" : "";
      ctx.fillText(`${item.label}${suffix}`, x + 12, rowY + rowHeight / 2, width - 42);
    }
    const footerY = rowTop + frame.items.length * rowHeight;
    ctx.fillStyle = "#d2d2d2";
    ctx.fillRect(x + 3, footerY, width - 6, 22);
    ctx.fillStyle = "#222";
    ctx.font = "12px monospace";
    const footer = typeEntry ? "Type command, then Enter to send · Esc cancels" : `${status.textContent} · F10 closes`;
    ctx.fillText(footer, x + 10, footerY + 11, width - 20);
    menuLayouts.push({ x, width, rowTop, rowHeight, itemCount: frame.items.length });
  }
  ctx.restore();
}
function animate(time) {
  if (!lastTime) lastTime = time;
  const elapsed = Math.min(time - lastTime, 100);
  lastTime = time;
  if (running) {
    frameDebt = Math.min(frameDebt + elapsed, 100);
    const frames = Math.min(Math.floor(frameDebt / 20), 5);
    for (let i = 0; i < frames; i++) {
      core._bp_run_frame();
      frameDebt -= 20;
    }
    if (frames > 0) {
      drawFrame();
      if (clearHintAfterFrame) {
        hint.hidden = true;
        clearHintAfterFrame = false;
      }
    }
  }
  requestAnimationFrame(animate);
}
requestAnimationFrame(animate);
