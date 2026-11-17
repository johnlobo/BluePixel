// Start-up from URL parameters: ?disk=…&sym=…&run=…&warp=…
// Pure logic, no DOM and no core: app.js supplies the I/O, Tests/wasm/test_autoload.mjs supplies fakes.

export const MAX_WARP_FRAMES = 3000;   // 60 s of emulated time, run without drawing
export const BOOT_FRAMES = 150;        // same boot wait as Tests/wasm/test_web_client.mjs before typing
const MAX_RUN_LENGTH = 200;

// Resolve a resource parameter against the page. Only same-origin http(s) URLs are accepted, so a
// shared link cannot make the page fetch from another site.
function resolveResource(name, value, pageUrl, errors) {
  if (value === null || value === "") return null;
  let url;
  try { url = new URL(value, pageUrl); }
  catch { errors.push(`${name}: invalid URL`); return null; }
  if (url.origin !== pageUrl.origin || !/^https?:$/.test(url.protocol)) {
    errors.push(`${name}: only files from ${pageUrl.origin} are accepted`);
    return null;
  }
  return url;
}

export function parseLaunchParams(search, pageHref) {
  const pageUrl = new URL(pageHref);
  const query = new URLSearchParams(search);
  const errors = [];
  const disk = resolveResource("disk", query.get("disk"), pageUrl, errors);
  const sym = resolveResource("sym", query.get("sym"), pageUrl, errors);

  let run = query.get("run");
  if (run !== null) {
    run = run.replace(/[\r\n]+$/, "");
    if (/[\x00-\x1F\x7F]/.test(run)) { errors.push("run: control characters are not allowed"); run = null; }
    else if (run.length > MAX_RUN_LENGTH) { errors.push(`run: longer than ${MAX_RUN_LENGTH} characters`); run = null; }
    else if (run === "") run = null;
  }

  let warp = 0;
  const warpText = query.get("warp");
  if (warpText !== null && warpText !== "") {
    const value = Number(warpText);
    if (!Number.isInteger(value) || value < 0) errors.push("warp: must be a whole number of frames");
    else warp = Math.min(value, MAX_WARP_FRAMES);
  }

  const active = Boolean(disk || sym || run || warp);
  return { active, disk, sym, run, warp, errors };
}

function fileName(url) {
  const last = url.pathname.split("/").pop();
  try { return decodeURIComponent(last) || "disc.dsk"; } catch { return last || "disc.dsk"; }
}

// io: fetchBytes(url) -> Uint8Array, insertDisk(bytes, name) -> boolean, loadSymbols(bytes) -> count,
//     runFrames(count), typeText(text), report(message).
// Order: disc, symbols, boot frames, typed command, rest of the warp. A failing resource is reported
// and skipped; the machine keeps running without it.
export async function runAutoload(params, io) {
  const summary = { disk: false, symbols: 0, typed: false, frames: 0, errors: [...params.errors] };
  for (const message of params.errors) io.report(message);

  if (params.disk) {
    try {
      const bytes = await io.fetchBytes(params.disk);
      if (!io.insertDisk(bytes, fileName(params.disk))) throw new Error("not a supported DSK image");
      summary.disk = true;
    } catch (error) { summary.errors.push(`disk: ${error.message}`); io.report(`disk: ${error.message}`); }
  }

  if (params.sym) {
    try {
      const bytes = await io.fetchBytes(params.sym);
      summary.symbols = io.loadSymbols(bytes);
      if (!summary.symbols) throw new Error("no symbols found");
    } catch (error) { summary.errors.push(`sym: ${error.message}`); io.report(`sym: ${error.message}`); }
  }

  let remaining = params.warp;
  if (params.run) {
    const boot = Math.max(BOOT_FRAMES - summary.frames, 0);
    io.runFrames(boot); summary.frames += boot;
    remaining = Math.max(remaining - boot, 0);
    io.typeText(params.run + "\n");
    summary.typed = true;
  }
  if (remaining > 0) { io.runFrames(remaining); summary.frames += remaining; }
  return summary;
}
