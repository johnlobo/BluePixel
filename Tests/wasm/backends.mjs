// Shared CPCApi transports for conformance tests; calls are serialized by the caller.
import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");

export function nativeBackend() {
  const child = spawn(process.env.BLUEPIXEL_HEADLESS_BINARY || path.join(root, "build", "bluepixel-headless"), [], { stdio: ["pipe", "pipe", "inherit"], env: process.env });
  const exited = new Promise((resolve, reject) => { child.once("error", reject); child.once("close", resolve); });
  const lines = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
  return {
    name: "native",
    async call(request) { child.stdin.write(request + "\n"); const { value, done } = await lines.next(); if (done) throw new Error("headless exited before replying"); return value; },
    async close() { child.stdin.end(); await exited; },
  };
}

export async function wasmBackend(modulePath) {
  const { default: create } = await import(pathToFileURL(path.resolve(modulePath)).href);
  const core = await create();
  const session = core._bpapi_create();
  if (!session) throw new Error("bpapi_create failed");
  return {
    name: "wasm",
    async call(request) {
      const size = core.lengthBytesUTF8(request) + 1;
      const ptr = core._malloc(size);
      core.stringToUTF8(request, ptr, size);
      const out = core._bpapi_dispatch(session, ptr);
      core._free(ptr);
      const reply = core.UTF8ToString(out);
      core._free(out);
      return reply;
    },
    async close() { core._bpapi_destroy(session); },
  };
}

