// Headless test rig: runs the cart in wasmcart's Node host on webgl-node
// (the same native GL path the game station uses), with scripted input, and
// writes PNG screenshots of chosen frames.
//
//   node tools/shot.mjs build/red-tag-rumble.wasc out/prefix 60,300 [script.json]
//   STATION=1 node tools/shot.mjs ...    # only pad 0 reports connected, as the RetroArch core does
//   WAV=build/out.wav node tools/shot.mjs ...   # also writes the audio the cart played
//   PACE=1 node tools/shot.mjs ...       # real time, non-deterministic
//   SAVE_OUT=build/s.sav node tools/shot.mjs ...   # write the cart's save region at the end
//   PREF=1920x1080 node tools/shot.mjs ...   # ask for this frame size, as the station's core does (default: the cart's own)
//   SAVE_IN=build/s.sav node tools/shot.mjs ...    # boot with that save (off by default: the rig never
//                                                  # passes save data unless asked, so shots stay reproducible)
//   TIMING=1 node tools/shot.mjs ...     # a [rig] line every 60 frames: host ms per frame (avg and max),
//                                        # texture uploads per second (calls, KB, of which partial), GL
//                                        # programs linked so far and the wasm memory size; a last line at the end
//
// script.json entries, frame ranges inclusive:
//   { "from": 0, "to": 30, "pad": 0, "buttons": ["A"], "lx": 0, "ly": -32767, "rx": 0, "ry": 0, "lt": 0, "rt": 32767 }
//   { "from": 10, "to": 12, "keys": ["Enter", "KeyQ", "ArrowDown", "ShiftLeft"] }   keyboard (held over the range)
// sticks are -32767..32767 (y positive is down), triggers 0..32767.
import { readFileSync, writeFileSync } from 'node:fs';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const WASMCART = join(ROOT, 'third_party', 'wasmcart');
const { CartHost } = await import(pathToFileURL(join(WASMCART, 'src', 'CartHost.js')));
const { createWebGL2Context } = await import(pathToFileURL(join(WASMCART, 'node_modules', 'webgl-node', 'index.mjs')));
import { deflateSync, crc32 } from 'node:zlib';

function encodePng(rgba, w, h) {
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

const [cart, prefix, framesArg, scriptPath] = process.argv.slice(2);
if (!cart || !prefix || !framesArg) {
  console.error('usage: node tools/shot.mjs <cart.wasc> <out/prefix> <frame,frame,...> [script.json]');
  process.exit(2);
}
const shots = new Set(framesArg.split(',').map(Number));
const last = Math.max(...shots);
const script = scriptPath ? JSON.parse(readFileSync(scriptPath, 'utf8')) : [];
// wc_pad_t button bits (wasmcart.h)
const BTN = { A: 1, B: 2, X: 4, Y: 8, L: 16, R: 32, START: 64, SELECT: 128,
              UP: 256, DOWN: 512, LEFT: 1024, RIGHT: 2048, L3: 4096, R3: 8192 };
for (const s of script) for (const b of s.buttons ?? []) if (!(b in BTN)) { console.error(`unknown button ${b} in ${scriptPath}`); process.exit(2); }

const [PW, PH] = (process.env.PREF || '').split('x').map(Number);
const { gl } = createWebGL2Context(PW || 1280, PH || 720);
// TIMING=1: count the GL texture uploads and program links the cart causes, and time each host frame
const timing = !!process.env.TIMING;
const stat = { texCalls: 0, texKB: 0, subCalls: 0, links: 0, ms: 0, max: 0, n: 0 };
if (timing) {
  const wrap = (name, fn) => { const o = gl[name].bind(gl); gl[name] = (...a) => { fn(...a); return o(...a); }; };
  const bytes = (a) => { const d = a[a.length - 1]; return d && d.byteLength ? d.byteLength : 0; };
  wrap('texImage2D', (...a) => { stat.texCalls++; stat.texKB += bytes(a) / 1024; });
  wrap('texSubImage2D', (...a) => { stat.texCalls++; stat.subCalls++; stat.texKB += bytes(a) / 1024; });
  wrap('linkProgram', () => { stat.links++; });
}
const timingLine = (f) => {
  console.log(`[rig] f=${f} ms/frame avg=${(stat.ms / Math.max(1, stat.n)).toFixed(2)} max=${stat.max.toFixed(2)} tex/s=${stat.texCalls} texKB/s=${stat.texKB.toFixed(0)} partial/s=${stat.subCalls} links=${stat.links} mem=${host.memory.buffer.byteLength}`);
  stat.texCalls = 0; stat.texKB = 0; stat.subCalls = 0; stat.ms = 0; stat.max = 0; stat.n = 0;
};
const host = new CartHost();
await host.load(new Uint8Array(readFileSync(cart)), { glBackend: gl, ...(PW && PH ? { preferredWidth: PW, preferredHeight: PH } : {}), ...(process.env.SAVE_IN ? { saveData: new Uint8Array(readFileSync(process.env.SAVE_IN)) } : {}), ...(process.env.PACE ? {} : { deterministic: { seed: 7 } }) });

// STATION=1 reports only pad 0 connected, which is what the RetroArch core does
// (libretro.c writes every pad's buttons and sticks but sets connected only on
// port 0). CartHost zeroes a pad whose JS object says connected: false, so the
// rig passes every pad as connected and clears the connected byte of pads 1 to
// 3 in cart memory after the host writes them. A cart must seat players on
// button edges, never on this flag.
const station = !!process.env.STATION;
if (station) {
  const PAD_SIZE = 20, CONNECTED = 16;
  const orig = host._writePads.bind(host);
  host._writePads = (p) => { orig(p); const base = host.info.inputPtr; for (let i = 1; i < 4; i++) host._u8[base + i * PAD_SIZE + CONNECTED] = 0; };
}
const KEYC = { Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c, Minus: 0x2d, Quote: 0x34, Comma: 0x36,
  Period: 0x37, Slash: 0x38, ArrowRight: 0x4f, ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52,
  ShiftLeft: 0xe1, ShiftRight: 0xe5 };
const keyCode = (k) => KEYC[k] ?? (/^Key[A-Z]$/.test(k) ? 0x04 + k.charCodeAt(3) - 65 : /^Digit[1-9]$/.test(k) ? 0x1e + +k[5] - 1 : k === 'Digit0' ? 0x27 : null);
let heldKeys = new Set();
const audioChunks = [];
for (let f = 0; f <= last; f++) {
  const pads = [0, 1, 2, 3].map(() => ({ connected: true, buttons: 0,
    leftX: 0, leftY: 0, rightX: 0, rightY: 0, leftTrigger: 0, rightTrigger: 0 }));
  for (const s of script) {
    if (f < s.from || f > s.to) continue;
    const p = pads[s.pad ?? 0];
    for (const b of s.buttons ?? []) p.buttons |= BTN[b];
    // PRACTICE=off (15.5): the cart reads SELECT + L3 + R3 held on pad 0 in its first frames as the flag
    if (s.practice === 0) p.buttons |= BTN.SELECT | BTN.L3 | BTN.R3;
    if (s.lx !== undefined) p.leftX = s.lx;
    if (s.ly !== undefined) p.leftY = s.ly;
    if (s.rx !== undefined) p.rightX = s.rx;
    if (s.ry !== undefined) p.rightY = s.ry;
    if (s.lt !== undefined) p.leftTrigger = s.lt;
    if (s.rt !== undefined) p.rightTrigger = s.rt;
    if (s.name !== undefined) p.name = s.name;
  }
  const want = new Set();
  for (const s of script) if (s.keys && f >= s.from && f <= s.to) for (const k of s.keys) want.add(keyCode(k));
  for (const k of heldKeys) if (!want.has(k)) host.keyUp(k, 0);
  for (const k of want) if (!heldKeys.has(k)) host.keyDown(k, 0);
  heldKeys = want;
  const t0 = timing ? performance.now() : 0;
  const fr = host.runFrame(pads);
  if (timing) { const ms = performance.now() - t0; stat.ms += ms; if (ms > stat.max) stat.max = ms; stat.n++; if (f > 0 && f % 60 === 0) timingLine(f); }
  if (process.env.WAV && fr?.audio) audioChunks.push(Float32Array.from(fr.audio));
  // let the event loop breathe; PACE=1 runs in real time
  await new Promise((r) => (process.env.PACE ? setTimeout(r, 16) : setImmediate(r)));
  if (shots.has(f)) {
    const { width: W, height: H } = host.info;
    const px = new Uint8Array(W * H * 4);
    host.withRenderedFrame(() => gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, px));
    // GL rows are bottom-up
    const flipped = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) flipped.set(px.subarray((H - 1 - y) * W * 4, (H - y) * W * 4), y * W * 4);
    for (let i = 3; i < flipped.length; i += 4) flipped[i] = 255;
    writeFileSync(`${prefix}-${f}.png`, encodePng(flipped, W, H));
    console.log(`shot ${prefix}-${f}.png`);
  }
}
if (timing && stat.n) timingLine(last);
if (process.env.WAV) {
  // 16-bit stereo 48 kHz WAV of everything the cart played
  const n = audioChunks.reduce((a, c) => a + c.length, 0);
  const buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVEfmt ', 8);
  buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(2, 22); buf.writeUInt32LE(48000, 24);
  buf.writeUInt32LE(48000 * 4, 28); buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36); buf.writeUInt32LE(n * 2, 40);
  let o = 44;
  for (const c of audioChunks) for (const v of c) { buf.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(v * 32767))), o); o += 2; }
  writeFileSync(process.env.WAV, buf);
  console.log(`wav ${process.env.WAV}`);
}
if (process.env.SAVE_OUT) {
  const sd = host.getSaveData();
  if (sd) { writeFileSync(process.env.SAVE_OUT, sd); console.log(`save ${process.env.SAVE_OUT} ${sd.length} bytes`); }
}
process.exit(0);
