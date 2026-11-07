import createBluePixelCore from "./bluepixel.js";

const canvas = document.querySelector("#screen");
const screenWrap = document.querySelector(".screen-wrap");
const debuggerPanel = document.querySelector("#debugger");
const debuggerStatus = document.querySelector("#debugger-status");
const debuggerRegisters = document.querySelector("#debugger-registers");
const debuggerDisassembly = document.querySelector("#debugger-disassembly");
const debuggerBreakpoints = document.querySelector("#debugger-breakpoints");
const touchControls = document.querySelector("#touch-controls");
const ctx = canvas.getContext("2d", { alpha: false });
const image = ctx.createImageData(canvas.width, canvas.height);
const imageWords = new Uint32Array(image.data.buffer);
const isLittleEndian = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
const status = document.querySelector("#status");
const hint = document.querySelector("#screen-hint");
const runButton = document.querySelector("#run");
const resetButton = document.querySelector("#reset");
const soundButton = document.querySelector("#sound");
const saveSnapshotButton = document.querySelector("#save-snapshot");
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
let touchControlsEnabled = matchMedia("(pointer: coarse)").matches || navigator.maxTouchPoints > 0;
const joystickSources = new Map();
function readPreference(key, fallback) {
  try { return localStorage.getItem(`bluepixel-${key}`) ?? fallback; }
  catch { return fallback; }
}
function writePreference(key, value) {
  try { localStorage.setItem(`bluepixel-${key}`, String(value)); }
  catch { /* Preferences are optional when browser storage is unavailable. */ }
}
let displayMonitor = Number(readPreference("monitor", "0"));
if (![0, 1, 2].includes(displayMonitor)) displayMonitor = 0;
let displayWidth = Number(readPreference("screen-width", "720"));
if (![540, 720, 960].includes(displayWidth)) displayWidth = 720;
let emulationSpeed = Number(readPreference("speed", "1"));
if (![0.5, 1, 2, 5].includes(emulationSpeed)) emulationSpeed = 1;

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
  const tapeInput = document.querySelector("#tape");
  const typeText = document.querySelector("#type-text");
  return [
    { label: "BluePixel", items: [menuItem("About BluePixel CPC", () => { status.textContent = "BluePixel — Amstrad CPC 6128 emulator."; })] },
    { label: "File", items: [
      ...[0, 1].map(drive => menuBranch(`Drive ${drive === 0 ? "A" : "B"}:`, [
        menuItem("Insert Disc Image…", () => diskInputs[drive].click()),
        menuItem("Download Changed Disc…", () => saveDiskButtons[drive].click(), { disabled: saveDiskButtons[drive].disabled }),
      ])),
      menuBranch("Tape", [
        menuItem("Insert Tape…", () => tapeInput.click()),
        menuItem(core?._bp_tape_playing() ? "Stop Tape" : "Play Tape", () => setTapePlaying(!core._bp_tape_playing()), { disabled: !loadedTapeName }),
        menuItem("Rewind Tape", rewindTape, { disabled: !loadedTapeName }),
        menuItem("Remove Tape", removeTape, { disabled: !loadedTapeName }),
        menuItem(loadedTapeName ? `Loaded: ${loadedTapeName}` : "No tape loaded", () => {}, { disabled: true }),
      ]),
      menuItem("Load Snapshot…", () => snapshotInput.click()),
      menuItem("Save Snapshot…", () => saveSnapshotButton.click(), { disabled: saveSnapshotButton.disabled }),
      menuItem("Save Screenshot…", saveScreenshot),
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
        menuItem(document.fullscreenElement === screenWrap ? "Exit Full Screen" : "Full Screen", toggleFullScreen),
      ]),
      menuBranch("Display", [
        menuBranch("Monitor", [
          ...["Colour", "Green", "Greyscale"].map((name, mode) => menuItem(`${name}${displayMonitor === mode ? "  ✓" : ""}`, () => setMonitorMode(mode))),
        ]),
        menuBranch("Screen Size", [
          ...[[540, "Small"], [720, "Medium"], [960, "Large"]].map(([width, name]) => menuItem(`${name}${displayWidth === width ? "  ✓" : ""}`, () => setDisplaySize(width))),
        ]),
        menuBranch("Emulation Speed", [
          ...[[0.5, "50%"], [1, "100%"], [2, "200%"], [5, "500%"]].map(([speed, name]) => menuItem(`${name}${emulationSpeed === speed ? "  ✓" : ""}`, () => setEmulationSpeed(speed))),
        ]),
      ]),
      menuBranch("Sound", [
        menuItem(`Sound On${soundEnabled ? "  ✓" : ""}`, () => setSoundEnabled(true)),
        menuItem(`Sound Off${soundEnabled ? "" : "  ✓"}`, () => setSoundEnabled(false)),
      ]),
      menuBranch("Controls", [
        menuItem(`Touch Joystick ${touchControlsEnabled ? "On  ✓" : "Off"}`, () => setTouchControlsEnabled(!touchControlsEnabled)),
        menuItem(`Gamepad: ${navigator.getGamepads?.().some(Boolean) ? "Connected" : "Not connected"}`, () => {
          status.textContent = navigator.getGamepads?.().some(Boolean)
            ? "Gamepad connected. Use the D-pad or left stick; A/B are fire buttons."
            : "Connect a standard gamepad, then press a button to use it.";
        }),
      ]),
    ] },
    { label: "Debug", items: [
      menuItem(debuggerPanel.hidden ? "Open Debugger…" : "Hide Debugger", toggleDebugger),
      menuItem(running ? "Pause" : "Continue", () => runButton.click()),
      menuItem("Step Into", () => {
        if (debuggerPanel.hidden) toggleDebugger();
        document.querySelector("#debugger-step").click();
      }),
    ] },
    { label: "Help", items: [
      menuItem("Controls", () => { status.textContent = "Keyboard: arrows move, Ctrl/Alt fire. Gamepad: D-pad or left stick, A/B fire. Touch: use the on-screen joystick."; }),
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

menuTip.addEventListener("click", event => {
  event.stopPropagation();
  openMenu();
  canvas.focus();
});

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
let currentSnapshotName = "";
const loadedROMs = { os: null, basic: null, amsdos: null };
const currentDiskNames = ["bluepixel-a.dsk", "bluepixel-b.dsk"];
let loadedTapeName = "";
let audioContext;
let audioNode;
let audioPtr;
let pressed = new Map();
let audioConnected = false;

try {
  core = await createBluePixelCore({ locateFile: file => new URL(file, import.meta.url).href });
  if (!core._bp_init()) throw new Error("Could not allocate the CPC machine.");
  core._bp_set_monitor(displayMonitor);
  screenWrap.style.maxWidth = `${displayWidth}px`;
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
  saveSnapshotButton.disabled = !ready;
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
    currentSnapshotName = file.name;
    hint.hidden = true;
    updateReady();
    status.textContent = `${file.name} loaded.`;
  } catch (error) {
    status.textContent = `Snapshot error: ${error.message}`;
  }
});

document.querySelector("#tape").addEventListener("change", async event => {
  const file = event.currentTarget.files?.[0];
  if (!file) return;
  let dataPtr = 0;
  let namePtr = 0;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const name = new TextEncoder().encode(file.name + "\0");
    dataPtr = core._malloc(bytes.byteLength);
    namePtr = core._malloc(name.byteLength);
    if (!dataPtr || !namePtr) throw new Error("Not enough memory to load this tape.");
    core.HEAPU8.set(bytes, dataPtr);
    core.HEAPU8.set(name, namePtr);
    if (!core._bp_load_tape(dataPtr, bytes.byteLength, namePtr)) {
      throw new Error("This tape format is not supported or the file is damaged.");
    }
    loadedTapeName = file.name;
    status.textContent = `${file.name} inserted. On the CPC, type RUN"" or LOAD"" to start loading.`;
  } catch (error) {
    status.textContent = `Tape error: ${error.message}`;
  } finally {
    if (dataPtr) core._free(dataPtr);
    if (namePtr) core._free(namePtr);
    event.currentTarget.value = "";
  }
});

let debuggerMessage = "Debugger attached.";

function debugHex(value, digits = 4) {
  return `#${Number(value).toString(16).toUpperCase().padStart(digits, "0")}`;
}

function readDebugString(address, size = 160) {
  const ptr = core._malloc(size);
  if (!ptr) return "<disassembly unavailable>";
  try {
    core.HEAPU8.fill(0, ptr, ptr + size);
    const length = core._bp_debug_disassemble(address, ptr, size);
    if (!length) return "<invalid instruction>";
    let end = ptr;
    while (end < ptr + size && core.HEAPU8[end] !== 0) end++;
    return new TextDecoder().decode(core.HEAPU8.subarray(ptr, end));
  } finally {
    core._free(ptr);
  }
}

function toggleCodeBreakpoint(address) {
  core._bp_debug_toggle_code_bp(address);
  debuggerMessage = core._bp_debug_has_code_bp(address)
    ? `Breakpoint added at ${debugHex(address)}.`
    : `Breakpoint removed from ${debugHex(address)}.`;
  refreshDebugger();
}

function refreshDebugger() {
  if (debuggerPanel.hidden || !core) return;
  const registerNames = ["AF", "BC", "DE", "HL", "IX", "IY", "SP", "PC"];
  const values = registerNames.map((name, index) => [name, core._bp_debug_register(index)]);
  values.push(["IM", core._bp_debug_register(8)], ["IFF", core._bp_debug_register(9)], ["T", core._bp_debug_register(10)]);
  debuggerRegisters.replaceChildren(...values.map(([name, value]) => {
    const cell = document.createElement("div");
    cell.className = "debugger-register";
    const label = document.createElement("span"); label.textContent = name;
    const output = document.createElement("code"); output.textContent = debugHex(value, name === "T" ? 8 : name === "IM" || name === "IFF" ? 2 : 4);
    cell.append(label, output);
    return cell;
  }));

  const pc = core._bp_debug_register(7) & 0xFFFF;
  document.querySelector("#debugger-disassembly-pc").textContent = `PC ${debugHex(pc)}`;
  const rows = [];
  let address = pc;
  for (let i = 0; i < 16; i++) {
    const instructionAddress = address;
    const instruction = readDebugString(instructionAddress);
    const length = core._bp_debug_instruction_length(instructionAddress);
    const row = document.createElement("div");
    row.className = `debugger-instruction${instructionAddress === pc ? " current" : ""}`;
    const breakpoint = document.createElement("button");
    breakpoint.type = "button";
    breakpoint.className = "debugger-bp-toggle";
    breakpoint.textContent = core._bp_debug_has_code_bp(instructionAddress) ? "●" : "○";
    breakpoint.title = `${core._bp_debug_has_code_bp(instructionAddress) ? "Remove" : "Add"} breakpoint at ${debugHex(instructionAddress)}`;
    breakpoint.setAttribute("aria-label", breakpoint.title);
    breakpoint.addEventListener("click", () => toggleCodeBreakpoint(instructionAddress));
    const line = document.createElement("code");
    line.textContent = `${debugHex(instructionAddress)}  ${instruction}`;
    row.append(breakpoint, line);
    rows.push(row);
    address = (address + Math.max(1, length)) & 0xFFFF;
  }
  debuggerDisassembly.replaceChildren(...rows);

  const breakpointRows = [];
  const count = core._bp_debug_breakpoint_count();
  for (let i = 0; i < count; i++) {
    const address = core._bp_debug_breakpoint_address(i);
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = `Remove ${debugHex(address)}`;
    button.addEventListener("click", () => toggleCodeBreakpoint(address));
    breakpointRows.push(button);
  }
  debuggerBreakpoints.replaceChildren(...breakpointRows);
  const mode = running ? "Running" : "Paused";
  debuggerStatus.textContent = `${debuggerMessage} · ${mode} · PC ${debugHex(pc)}`;
}

function toggleDebugger() {
  const opening = debuggerPanel.hidden;
  debuggerPanel.hidden = !opening;
  if (opening) {
    core._bp_debug_attach();
    debuggerMessage = "Debugger ready.";
    refreshDebugger();
    debuggerPanel.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }
}

document.querySelector("#debugger-close").addEventListener("click", toggleDebugger);
document.querySelector("#debugger-resume").addEventListener("click", () => {
  core._bp_debug_resume();
  running = true;
  runButton.textContent = "Pause";
  clearHintAfterFrame = true;
  debuggerMessage = "Continuing execution.";
  if (audioContext?.state === "suspended") audioContext.resume();
  refreshDebugger();
});
document.querySelector("#debugger-pause").addEventListener("click", () => {
  core._bp_debug_pause();
  running = false;
  runButton.textContent = "Resume";
  debuggerMessage = "Execution paused.";
  drawFrame();
  refreshDebugger();
});
document.querySelector("#debugger-step").addEventListener("click", () => {
  core._bp_debug_attach();
  core._bp_debug_pause();
  running = false;
  runButton.textContent = "Resume";
  core._bp_debug_step_into();
  debuggerMessage = "Executed one instruction.";
  drawFrame();
  refreshDebugger();
});
document.querySelector("#debugger-breakpoint-form").addEventListener("submit", event => {
  event.preventDefault();
  const field = document.querySelector("#debugger-breakpoint-address");
  const raw = field.value.trim().replace(/^(#|0x)/i, "");
  if (!/^[\da-f]{1,4}$/i.test(raw)) {
    debuggerMessage = "Enter a hexadecimal address from 0000 to FFFF.";
    refreshDebugger();
    return;
  }
  core._bp_debug_attach();
  toggleCodeBreakpoint(Number.parseInt(raw, 16));
  field.value = "";
});
setInterval(refreshDebugger, 250);

function setTapePlaying(playing) {
  core._bp_tape_play(playing ? 1 : 0);
  status.textContent = playing ? "Tape playback enabled. Use RUN\"\" or LOAD\"\" on the CPC." : "Tape stopped.";
  if (menuOpen) drawFrame();
}

function rewindTape() {
  core._bp_tape_rewind();
  status.textContent = "Tape rewound.";
  if (menuOpen) drawFrame();
}

function removeTape() {
  core._bp_tape_remove();
  loadedTapeName = "";
  status.textContent = "Tape removed.";
  if (menuOpen) drawFrame();
}

function setMonitorMode(mode) {
  displayMonitor = mode;
  core._bp_set_monitor(mode);
  writePreference("monitor", mode);
  status.textContent = `Monitor set to ${["colour", "green", "greyscale"][mode]}.`;
  drawFrame();
}

function setDisplaySize(width) {
  displayWidth = Number(width);
  screenWrap.style.maxWidth = `${displayWidth}px`;
  writePreference("screen-width", displayWidth);
  status.textContent = `Screen size set to ${displayWidth}px.`;
}

function setEmulationSpeed(speed) {
  emulationSpeed = Number(speed);
  frameDebt = 0;
  writePreference("speed", emulationSpeed);
  status.textContent = `Emulation speed set to ${Math.round(emulationSpeed * 100)}%.`;
}

runButton.addEventListener("click", async () => {
  if (running) core._bp_debug_pause();
  else if (core._bp_debug_attached()) core._bp_debug_resume();
  running = !running;
  debuggerMessage = running ? "Emulation running." : "Execution paused.";
  runButton.textContent = running ? "Pause" : "Resume";
  status.textContent = running ? "Running" : "Paused";
  if (running) clearHintAfterFrame = true;
  if (running && audioContext?.state === "suspended") await audioContext.resume();
  refreshDebugger();
  canvas.focus();
});

resetButton.addEventListener("click", () => {
  core._bp_reset();
  frameDebt = 0;
  running = true;
  clearHintAfterFrame = true;
  runButton.textContent = "Pause";
  status.textContent = "Machine restarted.";
  debuggerMessage = "Machine reset.";
  refreshDebugger();
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

function saveScreenshot() {
  const source = document.createElement("canvas");
  source.width = canvas.width;
  source.height = canvas.height;
  source.getContext("2d").putImageData(image, 0, 0);

  const screenshot = document.createElement("canvas");
  screenshot.width = 720;
  screenshot.height = 540;
  const screenshotContext = screenshot.getContext("2d");
  screenshotContext.drawImage(source, 0, 0, screenshot.width, screenshot.height);
  screenshot.toBlob(blob => {
    if (!blob) {
      status.textContent = "Could not save the screenshot.";
      return;
    }
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
    link.href = url;
    link.download = `bluepixel-${timestamp}.png`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    status.textContent = "Screenshot saved.";
  }, "image/png");
}

async function toggleFullScreen() {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else await screenWrap.requestFullscreen();
  } catch (error) {
    status.textContent = `Could not toggle full screen: ${error.message}`;
  }
}

saveSnapshotButton.addEventListener("click", () => {
  const sizePtr = core._malloc(4);
  let dataPtr = 0;
  try {
    core.HEAPU32[sizePtr >> 2] = 0;
    dataPtr = core._bp_save_snapshot(sizePtr);
    const size = core.HEAPU32[sizePtr >> 2];
    if (!dataPtr || !size) throw new Error("Could not create the snapshot.");
    const bytes = core.HEAPU8.slice(dataPtr, dataPtr + size);
    const url = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
    const link = document.createElement("a");
    const baseName = currentSnapshotName.replace(/\.sna$/i, "") || "bluepixel";
    link.href = url;
    link.download = `${baseName}-bluepixel.sna`;
    link.click();
    URL.revokeObjectURL(url);
    status.textContent = "Snapshot saved.";
  } catch (error) {
    status.textContent = `Could not save snapshot: ${error.message}`;
  } finally {
    if (dataPtr) core._bp_free(dataPtr);
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

function setTouchControlsEnabled(enabled) {
  touchControlsEnabled = enabled;
  screenWrap.classList.toggle("touch-controls-enabled", enabled);
  status.textContent = enabled ? "Touch joystick enabled." : "Touch joystick disabled.";
}

setTouchControlsEnabled(touchControlsEnabled);

function setJoystickSource(source, bit, down) {
  const previous = joystickSources.get(source);
  if (previous === bit && down) return;
  if (previous !== undefined) {
    const stillHeld = [...joystickSources.entries()].some(([otherSource, otherBit]) => otherSource !== source && otherBit === previous);
    joystickSources.delete(source);
    if (!stillHeld) core._bp_joystick(previous, 0);
  }
  if (!down) return;
  const alreadyHeld = [...joystickSources.values()].includes(bit);
  joystickSources.set(source, bit);
  if (!alreadyHeld) core._bp_joystick(bit, 1);
}

touchControls.querySelectorAll("[data-joy]").forEach(button => {
  const bit = Number(button.dataset.joy);
  button.addEventListener("pointerdown", event => {
    event.preventDefault();
    button.setPointerCapture(event.pointerId);
    setJoystickSource(`touch:${event.pointerId}`, bit, true);
  });
  const release = event => setJoystickSource(`touch:${event.pointerId}`, bit, false);
  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
  button.addEventListener("lostpointercapture", release);
});

window.addEventListener("gamepadconnected", event => {
  status.textContent = `Gamepad connected: ${event.gamepad.id}.`;
});
window.addEventListener("gamepaddisconnected", event => {
  for (const source of joystickSources.keys()) {
    if (source.startsWith(`gamepad:${event.gamepad.index}:`)) setJoystickSource(source, joystickSources.get(source), false);
  }
  status.textContent = "Gamepad disconnected.";
});

function pollGamepads() {
  const gamepads = navigator.getGamepads?.() || [];
  for (const gamepad of gamepads) {
    if (!gamepad) continue;
    const prefix = `gamepad:${gamepad.index}:`;
    const directions = [
      ["up-axis", 0, (gamepad.axes[1] ?? 0) < -0.45], ["down-axis", 1, (gamepad.axes[1] ?? 0) > 0.45],
      ["left-axis", 2, (gamepad.axes[0] ?? 0) < -0.45], ["right-axis", 3, (gamepad.axes[0] ?? 0) > 0.45],
      ["up-pad", 0, Boolean(gamepad.buttons[12]?.pressed)], ["down-pad", 1, Boolean(gamepad.buttons[13]?.pressed)],
      ["left-pad", 2, Boolean(gamepad.buttons[14]?.pressed)], ["right-pad", 3, Boolean(gamepad.buttons[15]?.pressed)],
      ["fire-1", 4, Boolean(gamepad.buttons[0]?.pressed)], ["fire-2", 5, Boolean(gamepad.buttons[1]?.pressed)],
    ];
    for (const [name, bit, down] of directions) setJoystickSource(prefix + name, bit, down);
  }
}

function setKey(event, down) {
  if (event.code === "F10" || menuOpen) return;
  if (event.target instanceof HTMLElement && event.target.closest("input, textarea, select, button")) return;
  const key = keyCodes[event.code];
  const joy = joystickBits[event.code];
  if (key === undefined && joy === undefined) return;
  event.preventDefault();
  if (down && event.repeat) return;
  if (key !== undefined) core._bp_key(key, down ? 1 : 0);
  if (joy !== undefined) setJoystickSource(`keyboard:${event.code}`, joy, down);
  if (down) pressed.set(event.code, { key, joy });
  else pressed.delete(event.code);
}

window.addEventListener("keydown", event => setKey(event, true));
window.addEventListener("keyup", event => setKey(event, false));
window.addEventListener("blur", () => {
  for (const [code, item] of pressed) {
    if (item.key !== undefined) core._bp_key(item.key, 0);
    if (item.joy !== undefined) setJoystickSource(`keyboard:${code}`, item.joy, false);
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
  pollGamepads();
  if (!lastTime) lastTime = time;
  const elapsed = Math.min(time - lastTime, 100);
  lastTime = time;
  if (running) {
    frameDebt = Math.min(frameDebt + elapsed * emulationSpeed, 100);
    const frames = Math.min(Math.floor(frameDebt / 20), 5);
    for (let i = 0; i < frames; i++) {
      const stoppedByDebugger = core._bp_run_frame();
      frameDebt -= 20;
      if (stoppedByDebugger) {
        running = false;
        runButton.textContent = "Resume";
        core._bp_debug_pause();
        debuggerMessage = `Breakpoint at ${debugHex(core._bp_debug_register(7))}.`;
        status.textContent = debuggerMessage;
        refreshDebugger();
        break;
      }
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
