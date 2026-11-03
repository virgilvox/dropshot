// inline-html.mjs: fold the player shell, the wasmcart web host and the cart into ONE html file.
// usage: node inline-html.mjs <index.src.html> <host-bundle.js> <cart.wasc> <out.html>
// Placeholders in the shell: /*HOST_BUNDLE*/, __CART_B64__, __FONT_B64__.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const [src, bundle, cart, out] = process.argv.slice(2);
if (!src || !bundle || !cart || !out) {
  console.error('usage: inline-html.mjs <index.src.html> <host-bundle.js> <cart.wasc> <out.html>');
  process.exit(2);
}
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const font = fs.readFileSync(path.join(root, 'assets/fonts/Archivo.ttf')).toString('base64');
// a literal "</script" inside the bundle would end the inline script tag early
const host = fs.readFileSync(bundle, 'utf8').replace(/<\/script/gi, '<\\/script');
const cartB64 = fs.readFileSync(cart).toString('base64');

let html = fs.readFileSync(src, 'utf8');
// function replacers, so "$&" style sequences in the payloads are never interpreted
for (const [key, val] of [['/*HOST_BUNDLE*/', host], ['__CART_B64__', cartB64], ['__FONT_B64__', font]]) {
  if (!html.includes(key)) throw new Error('placeholder missing in shell: ' + key);
  html = html.replace(key, () => val);
}
fs.writeFileSync(out, html);
console.log(`wrote ${out} (${(html.length / 1048576).toFixed(2)} MB)`);
