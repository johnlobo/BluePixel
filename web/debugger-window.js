const status = document.querySelector("#debugger-status");
const registers = document.querySelector("#registers");
const disassembly = document.querySelector("#disassembly");
const breakpoints = document.querySelector("#breakpoints");
const pcLabel = document.querySelector("#pc");

function bridge() {
  if (!window.opener || window.opener.closed) return null;
  try { return window.opener.bluePixelDebugger || null; }
  catch { return null; }
}

function hex(value, digits = 4) {
  return `#${Number(value).toString(16).toUpperCase().padStart(digits, "0")}`;
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

  breakpoints.replaceChildren(...state.breakpoints.map(address => {
    const button = document.createElement("button");
    button.type = "button"; button.textContent = `Remove ${hex(address)}`;
    button.addEventListener("click", () => api.removeBreakpoint(address));
    return button;
  }));
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

window.addEventListener("beforeunload", () => bridge()?.windowClosed());
render();
setInterval(render, 250);
