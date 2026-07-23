// Headless re-export of the ALICE Mood Studio avatars at an arbitrary size.
//
// This runs the design project's own alice-draw.js and enc.js unmodified, so the
// output is the same vector redraw the browser export produces — not an upscale
// of the shipped 128px set. Only the host differs: @napi-rs/canvas instead of a
// DOM canvas, and files written to disk instead of a zip download.
//
// Usage: node render.mjs <size> <outDir>

import { createCanvas, GlobalFonts } from '@napi-rs/canvas';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const SIZE = parseInt(process.argv[2] ?? '256', 10);
const OUT = process.argv[3] ?? join(here, 'out');
const FRAMES = 14;

// The zzz / question overlays draw text with `system-ui`. Register a real face
// so those two moods do not silently lose their glyph in a headless process.
for (const f of ['/System/Library/Fonts/Helvetica.ttc',
                 '/System/Library/Fonts/Supplemental/Arial.ttf']) {
  try { GlobalFonts.registerFromPath(f, 'system-ui'); break; } catch { /* try next */ }
}

// Both design scripts are IIFEs that publish onto `window`.
globalThis.window = globalThis;
for (const f of ['alice-draw.js', 'enc.js']) {
  new Function(readFileSync(join(here, f), 'utf8'))();
}

const { MOODS, draw } = window.Alice;
const { encodeGIF } = window.Enc;

// The firmware keys emoji off the packed filename and the server's EMOJI_MAP
// says "kissy" where the design says "kiss".
const deviceName = (key) => (key === 'kiss' ? 'kissy' : key);

mkdirSync(OUT, { recursive: true });

let total = 0;
for (const mood of MOODS) {
  const canvas = createCanvas(SIZE, SIZE);
  const ctx = canvas.getContext('2d');
  const sc = SIZE / 64;
  const frames = [];
  for (let f = 0; f < FRAMES; f++) {
    ctx.setTransform(sc, 0, 0, sc, 0, 0);
    ctx.clearRect(0, 0, 64, 64);
    draw(ctx, mood, f / FRAMES);
    frames.push({ rgba: ctx.getImageData(0, 0, SIZE, SIZE).data, delay: 8 });
  }
  const bytes = encodeGIF(SIZE, SIZE, frames, true);
  const name = deviceName(mood.key) + '.gif';
  writeFileSync(join(OUT, name), bytes);
  total += bytes.length;
  console.log(`${name.padEnd(16)} ${SIZE}x${SIZE}  ${(bytes.length / 1024).toFixed(1)} KB`);
}
console.log(`\n${MOODS.length} gifs  ${(total / 1048576).toFixed(2)} MB total`);
