const status = document.querySelector("#debugger-status");
const registers = document.querySelector("#registers");
const disassembly = document.querySelector("#disassembly");
const breakpoints = document.querySelector("#breakpoints");
const pcLabel = document.querySelector("#pc");
const memoryGrid = document.querySelector("#memory-grid");
const memoryAddressField = document.querySelector("#memory-address");
let memoryRendered = false;

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

function renderMemory(start = parseHexAddress(memoryAddressField.value)) {
  const api = bridge();
  if (!api) return;
  if (start === null) {
    status.textContent = "Enter a hexadecimal address from 0000 to FFFF.";
    return;
  }
  memoryAddressField.value = start.toString(16).toUpperCase().padStart(4, "0");
  let bytes;
  try { bytes = api.readMemory(start, 256); }
  catch (error) { status.textContent = `Could not read memory: ${error.message}`; return; }
  const rows = [];
  for (let rowIndex = 0; rowIndex < 16; rowIndex++) {
    const rowAddress = (start + rowIndex * 16) & 0xFFFF;
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
        renderMemory(start);
      });
      row.append(byte);
    }
    rows.push(row);
  }
  memoryGrid.replaceChildren(...rows);
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

  disassembly.replaceChildren(...state.instructions.map(instruction => {
    const row = document.createElement("div");
    row.className = `instruction${instruction.address === state.pc ? " current" : ""}`;
    const toggle = document.createElement("button");
    toggle.type = "button"; toggle.className = "bp-toggle";
    toggle.textContent = instruction.breakpoint ? "●" : "○";
    toggle.title = `${instruction.breakpoint ? "Remove" : "Add"} breakpoint at ${hex(instruction.address)}`;
    toggle.setAttribute("aria-label", toggle.title);
    toggle.addEventListener("click", () => api.toggleBreakpoint(instruction.address));
    const text = document.createElement("code"); text.textContent = `${hex(instruction.address)}  ${instruction.text}`;
    row.append(toggle, text);
    return row;
  }));

  if (!breakpoints.contains(document.activeElement)) renderBreakpoints(state.breakpoints);
  if (!memoryRendered) { renderMemory(); memoryRendered = true; }
}

document.querySelector("#continue").addEventListener("click", () => bridge()?.resume());
document.querySelector("#pause").addEventListener("click", () => bridge()?.pause());
document.querySelector("#step").addEventListener("click", () => bridge()?.stepInto());
document.querySelector("#close-window").addEventListener("click", () => window.close());
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
  renderMemory(parseHexAddress(memoryAddressField.value));
  memoryRendered = true;
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
