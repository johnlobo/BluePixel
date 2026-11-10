// Browser adapter for the JSON-RPC CPCApi WebAssembly entry points.
// Binary payloads cross the API as base64; the legacy app-facing helpers below only
// keep the existing UI code small while every machine operation goes through RPC.
export async function createBluePixelApi(createModule) {
  const wasm = await createModule({ locateFile: file => new URL(file, import.meta.url).href });
  const session = wasm._bpapi_create();
  if (!session) throw new Error("Could not allocate the CPC machine.");
  let requestId = 0;
  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder();
  function rpc(method, params = {}) {
    const request = JSON.stringify({ jsonrpc: "2.0", id: ++requestId, method, params });
    const size = wasm.lengthBytesUTF8(request) + 1;
    const input = wasm._malloc(size);
    if (!input) throw new Error("Not enough memory for a CPCApi request.");
    wasm.stringToUTF8(request, input, size);
    const output = wasm._bpapi_dispatch(session, input);
    wasm._free(input);
    if (!output) throw new Error("CPCApi returned no response.");
    const raw = wasm.UTF8ToString(output);
    wasm._free(output);
    const response = JSON.parse(raw);
    if (response.error) throw new Error(response.error.message || "CPCApi request failed.");
    return response.result;
  }
  function fromHeap(ptr, size) { return wasm.HEAPU8.slice(ptr, ptr + size); }
  function fromStringPtr(ptr) {
    let end = ptr;
    while (wasm.HEAPU8[end]) end++;
    return textDecoder.decode(wasm.HEAPU8.subarray(ptr, end));
  }
  function toBase64(bytes) {
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(binary);
  }
  function fromBase64(value) {
    const binary = atob(value);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }
  function copyOut(bytes, sizePtr) {
    const ptr = wasm._malloc(Math.max(1, bytes.length));
    if (!ptr) return 0;
    wasm.HEAPU8.set(bytes, ptr);
    if (sizePtr) wasm.HEAPU32[sizePtr >> 2] = bytes.length;
    return ptr;
  }
  const facade = Object.create(wasm);
  Object.assign(facade, {
    initialize: () => true,
    destroy: () => wasm._bpapi_destroy(session),
    loadRom(slot, ptr, size) {
      const role = ({ [-1]: "os", 0: "basic", 7: "amsdos" })[slot];
      if (!role) return false;
      return size ? rpc("rom.load", { role, data_base64: toBase64(fromHeap(ptr, size)) }) : rpc("rom.unload", { role });
    },
    loadDisc(drive, ptr, size) { return rpc("disc.load", { drive, name: `bluepixel-${drive ? "b" : "a"}.dsk`, data_base64: toBase64(fromHeap(ptr, size)) }); },
    discName(drive, ptr, size) { const name = rpc("disc.info", { drive }).name; wasm.stringToUTF8(name, ptr, size); return name.length; },
    exportDisc(drive, sizePtr) { return copyOut(fromBase64(rpc("disc.export", { drive }).data_base64), sizePtr); },
    releaseDiscExport() {},
    loadSnapshot(ptr, size) { return rpc("snapshot.load", { data_base64: toBase64(fromHeap(ptr, size)) }); },
    saveSnapshot(sizePtr) { return copyOut(fromBase64(rpc("snapshot.save").data_base64), sizePtr); },
    freeBuffer(ptr) { wasm._free(ptr); },
    loadTape(ptr, size, namePtr) { return rpc("tape.load", { name: fromStringPtr(namePtr), data_base64: toBase64(fromHeap(ptr, size)) }); },
    tapePlaying() { return rpc("tape.status").playing ? 1 : 0; },
    tapeLoaded() { return rpc("tape.status").loaded ? 1 : 0; },
    tapeName(ptr, size) { const name = rpc("tape.status").name; wasm.stringToUTF8(name, ptr, size); return name.length; },
    tapePlay(play) { return rpc("tape.play", { play: Boolean(play) }); },
    tapeRewind() { return rpc("tape.rewind"); },
    tapeRemove() { return rpc("tape.remove"); },
    runFrame() { return rpc("run.frames", { count: 1 }).break; },
    reset() { return rpc("machine.reset"); },
    setMonitor(monitor) { return rpc("display.set_monitor", { monitor }); },
    typeText(ptr) { return rpc("input.text", { text: fromStringPtr(ptr) }); },
    key(key, down) { return rpc("input.key", { key, down: Boolean(down) }); },
    joystick(bit, down) { return rpc("input.joystick", { bit, down: Boolean(down) }); },
    renderFrame() {
      const rendered = rpc("screen.render");
      const rgba = fromBase64(rendered.data_base64);
      const ptr = wasm._malloc(rgba.length);
      if (!ptr) return 0;
      // app.js reads the frame as uint32 ARGB and converts it into ImageData.
      const words = new Uint32Array(wasm.HEAPU8.buffer, ptr, rgba.length / 4);
      for (let i = 0; i < words.length; i++) words[i] = ((rgba[i * 4 + 3] << 24) | (rgba[i * 4] << 16) | (rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2]) >>> 0;
      framePtrs.push(ptr);
      while (framePtrs.length > 2) wasm._free(framePtrs.shift());
      return ptr;
    },
    readAudio(ptr, frames) {
      const result = rpc("audio.read", { frames: Math.min(frames, 8192) });
      const samples = fromBase64(result.data_base64);
      wasm.HEAPU8.set(samples, ptr);
      return result.frames;
    },
  });
  const framePtrs = [];
  const reg = () => rpc("cpu.registers");
  const bpTypes = ["code", "mem_read", "mem_write", "io", "change"];
  const bpList = () => rpc("debug.breakpoints.list").breakpoints;
  Object.assign(facade, {
    debugAttach: () => rpc("debug.attach"),
    debugAttached: () => rpc("debug.status").attached,
    debugResume: () => rpc("debug.resume"), debugPause: () => rpc("debug.pause"),
    debugStepInto: () => rpc("debug.step_into"), debugStepOut: () => rpc("debug.step_out"),
    debugRegister(index) { const r = reg(); return [r.af, r.bc, r.de, r.hl, r.ix, r.iy, r.sp, r.pc, r.im, (r.iff1 ? 1 : 0) | (r.iff2 ? 2 : 0), r.nops * 4][index] ?? 0; },
    debugDisassemble(address, ptr, size) { const text = rpc("debug.disassemble", { address: address & 0xffff }).instructions[0]?.text || ""; wasm.stringToUTF8(text, ptr, size); return text.length; },
    debugInstructionLength(address) { return rpc("debug.disassemble", { address: address & 0xffff }).instructions[0]?.length || 1; },
    debugReadMemory(address, ptr, size) {
      const start = address & 0xffff;
      const firstSize = Math.min(size, 0x10000 - start);
      const firstHex = rpc("debug.mem.read", { address: start, length: firstSize }).hex;
      const bytes = firstHex.match(/../g)?.map(v => parseInt(v, 16)) || [];
      if (firstSize < size) {
        const restHex = rpc("debug.mem.read", { address: 0, length: size - firstSize }).hex;
        bytes.push(...(restHex.match(/../g)?.map(v => parseInt(v, 16)) || []));
      }
      wasm.HEAPU8.set(bytes, ptr); return bytes.length;
    },
    debugWriteMemory(address, value) { return rpc("debug.mem.write", { address: address & 0xffff, value: value & 255 }); },
    debugToggleCodeBreakpoint(address) { return rpc("debug.breakpoints.toggle_code", { address: address & 0xffff }).set; },
    debugHasCodeBreakpoint(address) { return bpList().some(bp => bp.type === "code" && bp.start === (address & 0xffff)); },
    debugBreakpointCount: () => bpList().length,
    debugBreakpointId(index) { return bpList()[index]?.id ?? -1; },
    debugBreakpointType(id) { return Math.max(0, bpTypes.indexOf(bpList().find(bp => bp.id === id)?.type)); },
    debugBreakpointStart(id) { return bpList().find(bp => bp.id === id)?.start ?? 0; },
    debugBreakpointEnd(id) { return bpList().find(bp => bp.id === id)?.end ?? 0; },
    debugBreakpointPassCount(id) { return bpList().find(bp => bp.id === id)?.pass_count ?? 0; },
    debugBreakpointCondition(id, ptr, size) { const value = bpList().find(bp => bp.id === id)?.condition || ""; wasm.stringToUTF8(value, ptr, size); return value.length; },
    debugRemoveBreakpoint(id) { return rpc("debug.breakpoints.remove", { id }); },
    debugAddMemoryBreakpoint(start, end, write) { return rpc("debug.breakpoints.add_memory", { start, end, access: write ? "write" : "read" }).id; },
    debugSetBreakpointCondition(id, ptr, passCount) { const result = rpc("debug.breakpoints.set_condition", { id, condition: fromStringPtr(ptr), pass_count: passCount }); facade.lastBreakpointError = result.error; return result.set; },
    debugLastError() { facade.lastErrorPtr ??= wasm._malloc(256); if (facade.lastErrorPtr) wasm.stringToUTF8(facade.lastBreakpointError || "", facade.lastErrorPtr, 256); return facade.lastErrorPtr || 0; },
    debugSymbolSet(ptr, value) { return rpc("debug.symbols.set", { name: fromStringPtr(ptr), value }); },
    debugSymbolClear() { return rpc("debug.symbols.clear"); },
    debugSymbolCount: () => rpc("debug.symbols.list").symbols.length,
    debugSymbolValue(index) { return rpc("debug.symbols.list").symbols[index]?.value ?? -1; },
    debugSymbolName(index, ptr, size) { const name = rpc("debug.symbols.list").symbols[index]?.name || ""; wasm.stringToUTF8(name, ptr, size); return name.length; },
    debugResetTstates() { return rpc("run.reset_tstates"); },
    debugTstates(ptr, size) { const value = String(reg().nops * 4); wasm.stringToUTF8(value, ptr, size); return value.length; },
    debugMarkData(start, end) { return rpc("debug.data.mark", { start, end }); },
    debugClearData(address) { return rpc("debug.data.clear", { address }); },
  });
  return facade;
}
