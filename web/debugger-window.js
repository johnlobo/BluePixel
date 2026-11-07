const status = document.querySelector("#debugger-status");
const registers = document.querySelector("#registers");
const disassembly = document.querySelector("#disassembly");
const breakpoints = document.querySelector("#breakpoints");
const pcLabel = document.querySelector("#pc");
const memoryGrid = document.querySelector("#memory-grid");
const memoryAddressField = document.querySelector("#memory-address");
const stack = document.querySelector("#stack");
const disassemblyAddressField = document.querySelector("#disassembly-address");
let disassemblyStart = null;
let followPC = true;
let memoryRendered = false;
const MEMORY_BYTES = 0x10000;
const MEMORY_ROW_HEIGHT = 20;
const DISASSEMBLY_ROW_HEIGHT = 16;
const VIRTUAL_ROWS = 48;
let disassemblyRenderStart = -1;
let memoryRenderStart = -1;
let disassemblyScrollQueued = false;
let memoryScrollQueued = false;

function bridge() {
  if (!window.opener || window.opener.closed) return null;
  try { return window.opener.bluePixelDebugger || null; }
  catch { return null; }
}

function hex(value, digits = 4) {
  return `#${Number(value).toString(16).toUpperCase().padStart(digits, "0")}`;
}

function parseHexAddress(value) {
  const raw = value.trim().replace(/^(#|0x)/i, "");
  return /^[\da-f]{1,4}$/i.test(raw) ? Number.parseInt(raw, 16) : null;
}

function renderMemory(start = null, resetScroll = false) {
  const api = bridge();
  if (!api) return;
  if (start !== null) {
    if (start < 0 || start > 0xFFFF) {
      status.textContent = "Enter a hexadecimal address from 0000 to FFFF.";
      return;
    }
  }
  const firstRow = Math.max(0, Math.min(0xFFF, Math.floor(start !== null && resetScroll ? start / 16 : memoryGrid.scrollTop / MEMORY_ROW_HEIGHT)));
  if (firstRow === memoryRenderStart && !resetScroll) return;
  memoryRenderStart = firstRow;
  const startAddress = firstRow * 16;
  memoryAddressField.value = startAddress.toString(16).toUpperCase().padStart(4, "0");
  const rowCount = Math.min(VIRTUAL_ROWS, 0x1000 - firstRow);
  let bytes;
  try { bytes = api.readMemory(startAddress, rowCount * 16); }
  catch (error) { status.textContent = `Could not read memory: ${error.message}`; return; }
  const rows = [];
  const top = document.createElement("div"); top.className = "virtual-spacer";
  top.style.height = `${firstRow * MEMORY_ROW_HEIGHT}px`;
  rows.push(top);
  for (let rowIndex = 0; rowIndex < rowCount; rowIndex++) {
    const rowAddress = startAddress + rowIndex * 16;
    const row = document.createElement("div"); row.className = "memory-row";
    const label = document.createElement("code"); label.className = "memory-row-label"; label.textContent = hex(rowAddress);
    row.append(label);
    for (let column = 0; column < 16; column++) {
      const address = (rowAddress + column) & 0xFFFF;
      const byte = document.createElement("input");
      byte.className = "memory-byte"; byte.value = bytes[rowIndex * 16 + column].toString(16).toUpperCase().padStart(2, "0");
      byte.maxLength = 2; byte.inputMode = "text"; byte.setAttribute("aria-label", `Memory ${hex(address)}`);
      byte.addEventListener("keydown", event => { if (event.key === "Enter") { event.preventDefault(); byte.blur(); } });
      byte.addEventListener("change", () => {
        const raw = byte.value.trim();
        if (!/^[\da-f]{2}$/i.test(raw)) {
          status.textContent = "Enter exactly two hexadecimal digits for a memory byte.";
          byte.value = bytes[rowIndex * 16 + column].toString(16).toUpperCase().padStart(2, "0");
          return;
        }
        bridge()?.writeMemory(address, Number.parseInt(raw, 16));
        memoryRenderStart = -1; renderMemory();
      });
      row.append(byte);
    }
    rows.push(row);
  }
  const bottom = document.createElement("div"); bottom.className = "virtual-spacer";
  bottom.style.height = `${(0x1000 - firstRow - rowCount) * MEMORY_ROW_HEIGHT}px`;
  rows.push(bottom);
  memoryGrid.replaceChildren(...rows);
  if (resetScroll && start !== null) memoryGrid.scrollTop = firstRow * MEMORY_ROW_HEIGHT;
}

function renderBreakpoints(items) {
  const api = bridge();
  if (!api) return;
  breakpoints.replaceChildren(...items.map(bp => {
    const row = document.createElement("div"); row.className = "breakpoint-row";
    const heading = document.createElement("div"); heading.className = "breakpoint-heading";
    const range = bp.start === bp.end ? hex(bp.start) : `${hex(bp.start)}–${hex(bp.end)}`;
    const description = document.createElement("span"); description.textContent = `${bp.type} ${range}`;
    const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "Remove";
    remove.addEventListener("click", () => {
      api.removeBreakpoint(bp.id);
      renderBreakpoints(api.getState().breakpoints);
    });
    heading.append(description, remove);

    const condition = document.createElement("form"); condition.className = "bp-condition";
    const expression = document.createElement("input"); expression.className = "condition-expression";
    expression.value = bp.condition; expression.maxLength = 150; expression.placeholder = "condition (optional)";
    expression.setAttribute("aria-label", `Condition for ${bp.type} breakpoint ${range}`);
    const pass = document.createElement("input"); pass.className = "condition-pass"; pass.type = "number";
    pass.min = "0"; pass.step = "1"; pass.value = String(bp.passCount); pass.title = "Pass count; 0 stops on every match";
    pass.setAttribute("aria-label", "Pass count");
    const apply = document.createElement("button"); apply.type = "submit"; apply.textContent = "Apply";
    condition.append(expression, pass, apply);
    condition.addEventListener("submit", event => {
      event.preventDefault();
      const passCount = Number.parseInt(pass.value, 10);
      if (!Number.isInteger(passCount) || passCount < 0) {
        status.textContent = "Pass count must be zero or a positive integer.";
        return;
      }
      const result = api.setBreakpointCondition(bp.id, expression.value.trim(), passCount);
      status.textContent = result.ok ? "Breakpoint condition applied." : `Condition error: ${result.error}`;
      renderBreakpoints(api.getState().breakpoints);
    });
    row.append(heading, condition);
    return row;
  }));
}

function renderDisassembly(start, pc, resetScroll = false) {
  const api = bridge(); if (!api) return;
  const targetAddress = start & 0xFFFF;
  const firstAddress = Math.max(0, Math.min(0xFFFF, Math.floor(resetScroll ? targetAddress : disassembly.scrollTop / DISASSEMBLY_ROW_HEIGHT)));
  disassemblyStart = firstAddress;
  if (firstAddress === disassemblyRenderStart && !resetScroll) return;
  disassemblyRenderStart = firstAddress;
  disassemblyAddressField.value = firstAddress.toString(16).toUpperCase().padStart(4, "0");
  const rowCount = Math.min(VIRTUAL_ROWS, 0x10000 - firstAddress);
  const instructions = api.getInstructions(firstAddress, rowCount, true);
  const rows = instructions.map(instruction => {
    const row = document.createElement("div");
    row.dataset.address = String(instruction.address);
    row.className = `instruction${instruction.address === pc ? " current" : ""}`;
    const toggle = document.createElement("button");
    toggle.type = "button"; toggle.className = "bp-toggle";
    toggle.textContent = instruction.breakpoint ? "●" : "○";
    toggle.title = `${instruction.breakpoint ? "Remove" : "Add"} breakpoint at ${hex(instruction.address)}`;
    toggle.setAttribute("aria-label", toggle.title);
    toggle.addEventListener("click", () => { api.toggleBreakpoint(instruction.address); disassemblyRenderStart = -1; renderDisassembly(disassemblyStart, pc); });
    const address = document.createElement("code"); address.className = "instruction-address"; address.textContent = hex(instruction.address);
    const bytes = document.createElement("code"); bytes.className = "instruction-bytes";
    bytes.textContent = instruction.bytes.map(value => value.toString(16).toUpperCase().padStart(2, "0")).join(" ");
    const text = document.createElement("code"); text.className = "instruction-asm"; text.textContent = instruction.text;
    row.append(toggle, address, bytes, text); return row;
  });
  const top = document.createElement("div"); top.className = "virtual-spacer";
  top.style.height = `${firstAddress * DISASSEMBLY_ROW_HEIGHT}px`;
  const bottom = document.createElement("div"); bottom.className = "virtual-spacer";
  bottom.style.height = `${(0x10000 - firstAddress - rowCount) * DISASSEMBLY_ROW_HEIGHT}px`;
  disassembly.replaceChildren(top, ...rows, bottom);
  if (resetScroll) disassembly.scrollTop = targetAddress * DISASSEMBLY_ROW_HEIGHT;
}

function render() {
  const api = bridge();
  if (!api) {
    status.textContent = "The emulator window is no longer available.";
    return;
  }
  let state;
  try { state = api.getState(); }
  catch { status.textContent = "The emulator is reloading. Reopen the debugger with F7."; return; }

  status.textContent = `${state.message} · ${state.running ? "Running" : "Paused"}`;
  pcLabel.textContent = `PC ${hex(state.pc)}`;
  const pcInView = state.pc >= disassemblyStart && state.pc < disassemblyStart + VIRTUAL_ROWS;
  if (disassemblyStart === null || (followPC && !pcInView)) {
    renderDisassembly(state.pc, state.pc, true);
  } else {
    for (const row of disassembly.children) if (row.dataset.address) row.classList.toggle("current", Number(row.dataset.address) === state.pc);
  }
  disassembly.dataset.pc = String(state.pc);
  const items = Object.entries(state.registers);
  registers.replaceChildren(...items.map(([name, value]) => {
    const cell = document.createElement("div");
    cell.className = "register";
    const label = document.createElement("span"); label.textContent = name;
    const output = document.createElement("code");
    output.textContent = hex(value, name === "T" ? 8 : name === "IM" || name === "IFF" ? 2 : 4);
    cell.append(label, output);
    return cell;
  }));

  const sp = Number(state.registers.SP) & 0xFFFF;
  const stackBytes = api.readMemory(sp, 64);
  stack.replaceChildren(...Array.from({ length: 32 }, (_, index) => {
    const address = (sp + index * 2) & 0xFFFF;
    const value = stackBytes[index * 2] | (stackBytes[index * 2 + 1] << 8);
    const row = document.createElement("div"); row.className = `stack-row${index === 0 ? " stack-top" : ""}`;
    const addr = document.createElement("code"); addr.textContent = hex(address);
    const word = document.createElement("code"); word.textContent = hex(value);
    row.append(addr, word); return row;
  }));

  if (!breakpoints.contains(document.activeElement)) renderBreakpoints(state.breakpoints);
  if (!memoryRendered) { renderMemory(); memoryRendered = true; }
}

document.querySelector("#continue").addEventListener("click", () => bridge()?.resume());
document.querySelector("#pause").addEventListener("click", () => bridge()?.pause());
document.querySelector("#step").addEventListener("click", () => bridge()?.stepInto());
document.querySelector("#step-out").addEventListener("click", () => bridge()?.stepOut());
document.querySelector("#close-window").addEventListener("click", () => window.close());
const symbolInput = document.querySelector("#symbol-file");
document.querySelector("#load-symbols").addEventListener("click", () => symbolInput.click());
document.querySelector("#clear-symbols").addEventListener("click", () => {
  bridge()?.clearSymbols(); document.querySelector("#symbol-status").textContent = "Symbols cleared";
});
symbolInput.addEventListener("change", async () => {
  const file = symbolInput.files?.[0]; if (!file) return;
  const api = bridge(); if (!api) return;
  api.clearSymbols();
  let loaded = 0;
  const parseValue = token => {
    let value = token;
    if (value.startsWith("%")) return /^[01]+$/.test(value.slice(1)) ? parseInt(value.slice(1), 2) : null;
    if (/^[#&$]/.test(value)) return /^[\da-f]+$/i.test(value.slice(1)) ? parseInt(value.slice(1), 16) : null;
    if (/^0x[\da-f]+$/i.test(value)) return parseInt(value.slice(2), 16);
    if (/^[\da-f]+h$/i.test(value)) return parseInt(value.slice(0, -1), 16);
    if (/^\d+$/.test(value)) return parseInt(value, 10);
    return null;
  };
  for (const rawLine of (await file.text()).split(/\r?\n/)) {
    const line = rawLine.trim(); if (!line || /^[;*]/.test(line)) continue;
    const tokens = line.split(/[\s=]+/).filter(Boolean);
    if (tokens.length < 2) continue;
    let name = "", address = null;
    if (/^DEF$/i.test(tokens[0]) && tokens.length >= 3) { name = tokens[1]; address = parseValue(tokens[2]); }
    else if (/^al$/i.test(tokens[0]) && tokens.length >= 3) { name = tokens[2].replace(/^\./, ""); address = parseValue(tokens[1]); }
    else if (/^[\da-f]{8}$/i.test(tokens[0])) { address = parseInt(tokens[0], 16); name = tokens[1]; }
    else {
      name = tokens[0].replace(/:$/, "");
      for (const token of tokens.slice(1)) { if (/^equ$/i.test(token) || /^bytes$/i.test(token)) continue; address = parseValue(token); if (address !== null) break; }
    }
    if (name && address !== null && address >= 0 && address <= 0xFFFF && !/^(l__|s__|\.__\.)/.test(name) && api.setSymbol(name, address)) loaded++;
  }
  document.querySelector("#symbol-status").textContent = `${loaded} symbols loaded`;
  symbolInput.value = ""; render();
});
document.querySelector("#breakpoint-form").addEventListener("submit", event => {
  event.preventDefault();
  const input = document.querySelector("#breakpoint-address");
  const raw = input.value.trim().replace(/^(#|0x)/i, "");
  if (!/^[\da-f]{1,4}$/i.test(raw)) {
    status.textContent = "Enter a hexadecimal address from 0000 to FFFF.";
    return;
  }
  bridge()?.addBreakpoint(Number.parseInt(raw, 16));
  input.value = "";
  render();
});
document.querySelector("#memory-form").addEventListener("submit", event => {
  event.preventDefault();
  const address = parseHexAddress(memoryAddressField.value);
  if (address === null) { status.textContent = "Enter a hexadecimal address from 0000 to FFFF."; return; }
  memoryRenderStart = -1; renderMemory(address, true);
  memoryRendered = true;
});
document.querySelector("#disassembly-form").addEventListener("submit", event => {
  event.preventDefault();
  const address = parseHexAddress(disassemblyAddressField.value);
  if (address === null) { status.textContent = "Enter a hexadecimal address from 0000 to FFFF."; return; }
  followPC = false; document.querySelector("#follow-pc").classList.remove("active");
  disassemblyRenderStart = -1; renderDisassembly(address, Number(bridge()?.getState().pc) & 0xFFFF, true);
});
document.querySelector("#disassembly-prev").addEventListener("click", () => {
  followPC = false; document.querySelector("#follow-pc").classList.remove("active");
  disassemblyRenderStart = -1; renderDisassembly(Math.max(0, disassemblyStart - 0x100), Number(bridge()?.getState().pc) & 0xFFFF, true);
});
document.querySelector("#disassembly-next").addEventListener("click", () => {
  followPC = false; document.querySelector("#follow-pc").classList.remove("active");
  disassemblyRenderStart = -1; renderDisassembly(Math.min(0xFFFF, disassemblyStart + 0x100), Number(bridge()?.getState().pc) & 0xFFFF, true);
});
document.querySelector("#follow-pc").addEventListener("click", () => {
  followPC = true; document.querySelector("#follow-pc").classList.add("active");
  const pc = Number(bridge()?.getState().pc) & 0xFFFF; disassemblyRenderStart = -1; renderDisassembly(pc, pc, true);
});
const stopFollowingPC = () => {
  if (!followPC) return;
  followPC = false;
  document.querySelector("#follow-pc").classList.remove("active");
};
disassembly.addEventListener("wheel", stopFollowingPC, { passive: true });
disassembly.addEventListener("pointerdown", stopFollowingPC);
disassembly.addEventListener("keydown", event => {
  if (["ArrowDown", "ArrowUp", "PageDown", "PageUp", "Home", "End", " "].includes(event.key)) stopFollowingPC();
});

disassembly.addEventListener("scroll", () => {
  if (disassemblyScrollQueued) return;
  disassemblyScrollQueued = true;
  requestAnimationFrame(() => { disassemblyScrollQueued = false; renderDisassembly(disassemblyStart ?? 0, Number(bridge()?.getState().pc) & 0xFFFF); });
});
memoryGrid.addEventListener("scroll", () => {
  if (memoryScrollQueued) return;
  memoryScrollQueued = true;
  requestAnimationFrame(() => { memoryScrollQueued = false; renderMemory(); });
});
document.querySelector("#memory-breakpoint-form").addEventListener("submit", event => {
  event.preventDefault();
  const start = parseHexAddress(document.querySelector("#memory-bp-start").value);
  const end = parseHexAddress(document.querySelector("#memory-bp-end").value);
  if (start === null || end === null || start > end) {
    status.textContent = "Enter a valid memory range from 0000 to FFFF, with start ≤ end.";
    return;
  }
  const write = document.querySelector("#memory-bp-access").value === "write";
  const id = bridge()?.addMemoryBreakpoint(start, end, write);
  if (id >= 0) {
    document.querySelector("#memory-bp-start").value = "";
    document.querySelector("#memory-bp-end").value = "";
    renderBreakpoints(bridge().getState().breakpoints);
  } else status.textContent = "Could not add memory breakpoint; the breakpoint limit may be reached.";
});

window.addEventListener("beforeunload", () => bridge()?.windowClosed());
render();
setInterval(render, 250);
