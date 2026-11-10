// Use the browser-compatible WebSocket API, including token authentication via subprotocols.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = fileURLToPath(new URL("../..", import.meta.url));
const directory = mkdtempSync("/tmp/bp-websocket-");
const tokenFile = path.join(directory, "token");
const child = spawn(path.join(root, "build/bluepixel-headless"), ["--websocket", "0", "--token-file", tokenFile], {
  env: { ...process.env, BLUEPIXEL_ROM_DIR: path.join(root, "web/roms") },
  stdio: ["ignore", "pipe", "inherit"],
});
const lines = createInterface({ input: child.stdout });
const exited = new Promise(resolve => child.once("exit", code => resolve(code)));
function deadline(promise) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error("WebSocket test timed out")), 10000); })])
    .finally(() => clearTimeout(timer));
}
let ws;
try {
  const ready = await deadline(new Promise((resolve, reject) => {
    lines.once("line", line => resolve(JSON.parse(line)));
    child.once("error", reject);
    child.once("exit", () => reject(new Error("server exited before ready")));
  }));
  assert(ready.server_ready);
  const token = readFileSync(tokenFile, "utf8").trim();
  ws = new WebSocket(`ws://127.0.0.1:${ready.websocket_port}/rpc`, ["bluepixel.v1", `bluepixel.token.${token}`]);
  await deadline(new Promise((resolve, reject) => {
    ws.addEventListener("open", resolve, { once: true });
    ws.addEventListener("error", () => reject(new Error("WebSocket upgrade failed")), { once: true });
  }));
  assert.equal(ws.protocol, "bluepixel.v1");
  const response = deadline(new Promise(resolve => ws.addEventListener("message", event => resolve(JSON.parse(event.data)), { once: true })));
  ws.send(JSON.stringify({ jsonrpc: "2.0", id: 42, method: "session.capabilities", params: {} }));
  const reply = await response;
  assert.equal(reply.id, 42);
  assert.equal(reply.result.api_version, "1.4");
  assert.deepEqual(reply.result.transports, ["websocket"]);
  const closed = deadline(new Promise(resolve => ws.addEventListener("close", resolve, { once: true })));
  ws.close(1000);
  assert.equal((await closed).code, 1000);
} finally {
  ws?.close();
  child.kill("SIGTERM");
  assert.equal(await deadline(exited), 0);
  lines.close();
  rmSync(directory, { recursive: true });
}
console.log("standard WebSocket client: PASS (token subprotocol, RPC, capabilities, clean close)");
