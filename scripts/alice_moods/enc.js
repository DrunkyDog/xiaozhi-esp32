// enc.js — animated transparent GIF89a encoder + median-cut quantizer.
// Verbatim from the ALICE Mood Studio design project, minus buildZip (files are
// written straight to disk here, so no in-browser zip step is needed).
// Exposes window.Enc = { encodeGIF }.
(function () {
  "use strict";

  // ── BitWriter (LSB-first, as GIF requires) ──────────────────────────────
  function BitWriter() { this.bytes = []; this.acc = 0; this.n = 0; }
  BitWriter.prototype.write = function (code, size) {
    this.acc |= code << this.n; this.n += size;
    while (this.n >= 8) { this.bytes.push(this.acc & 0xff); this.acc >>>= 8; this.n -= 8; }
  };
  BitWriter.prototype.flush = function () {
    if (this.n > 0) { this.bytes.push(this.acc & 0xff); this.acc = 0; this.n = 0; }
  };

  // ── GIF LZW compressor ──────────────────────────────────────────────────
  function lzw(minCode, indices) {
    const CLEAR = 1 << minCode, EOI = CLEAR + 1;
    const bw = new BitWriter();
    let codeSize, table, next;
    function reset() { table = new Map(); next = EOI + 1; codeSize = minCode + 1; }
    reset();
    bw.write(CLEAR, codeSize);
    let prefix = indices[0];
    for (let i = 1; i < indices.length; i++) {
      const s = indices[i];
      const key = prefix * 256 + s;
      if (table.has(key)) { prefix = table.get(key); continue; }
      bw.write(prefix, codeSize);
      if (next < 4096) {
        // grow BEFORE assigning the code that needs the extra bit (GIF, no early change)
        if (next === (1 << codeSize) && codeSize < 12) codeSize++;
        table.set(key, next); next++;
      } else {
        bw.write(CLEAR, codeSize); reset();
      }
      prefix = s;
    }
    bw.write(prefix, codeSize);
    bw.write(EOI, codeSize);
    bw.flush();
    return bw.bytes;
  }

  // ── median-cut quantization to <=maxColors ──────────────────────────────
  function medianCut(colorCounts, maxColors) {
    // colorCounts: Map "r,g,b" -> count
    let all = [];
    colorCounts.forEach((cnt, key) => {
      const p = key.split(",");
      all.push([+p[0], +p[1], +p[2], cnt]);
    });
    if (all.length <= maxColors) return all.map(c => [c[0], c[1], c[2]]);

    function box(list) {
      let rmn = 255, rmx = 0, gmn = 255, gmx = 0, bmn = 255, bmx = 0;
      for (const c of list) {
        if (c[0] < rmn) rmn = c[0]; if (c[0] > rmx) rmx = c[0];
        if (c[1] < gmn) gmn = c[1]; if (c[1] > gmx) gmx = c[1];
        if (c[2] < bmn) bmn = c[2]; if (c[2] > bmx) bmx = c[2];
      }
      const rr = rmx - rmn, gr = gmx - gmn, br = bmx - bmn;
      const range = Math.max(rr, gr, br);
      const axis = rr >= gr && rr >= br ? 0 : gr >= br ? 1 : 2;
      return { list, range, axis };
    }
    let boxes = [box(all)];
    while (boxes.length < maxColors) {
      // pick splittable box with largest range
      let bi = -1, best = -1;
      for (let i = 0; i < boxes.length; i++)
        if (boxes[i].list.length > 1 && boxes[i].range > best) { best = boxes[i].range; bi = i; }
      if (bi < 0) break;
      const b = boxes[bi];
      b.list.sort((x, y) => x[b.axis] - y[b.axis]);
      const mid = b.list.length >> 1;
      const l = b.list.slice(0, mid), r = b.list.slice(mid);
      boxes.splice(bi, 1, box(l), box(r));
    }
    return boxes.map(b => {
      let r = 0, g = 0, bl = 0, w = 0;
      for (const c of b.list) { r += c[0] * c[3]; g += c[1] * c[3]; bl += c[2] * c[3]; w += c[3]; }
      return [Math.round(r / w), Math.round(g / w), Math.round(bl / w)];
    });
  }

  // ── encode animated transparent GIF ─────────────────────────────────────
  // frames: [{ rgba: Uint8ClampedArray(w*h*4), delay: centiseconds }]
  function encodeGIF(width, height, frames, loop) {
    const alphaThresh = 128;
    // gather opaque colors
    const counts = new Map();
    for (const f of frames) {
      const d = f.rgba;
      for (let i = 0; i < d.length; i += 4) {
        if (d[i + 3] < alphaThresh) continue;
        const k = d[i] + "," + d[i + 1] + "," + d[i + 2];
        counts.set(k, (counts.get(k) || 0) + 1);
      }
    }
    const palColors = medianCut(counts, 255); // index 0 reserved for transparent
    // palette: 0 = transparent, 1..N = colors
    const palN = palColors.length;
    let power = 1; while ((1 << power) < palN + 1) power++; // >= palN+1 entries
    if (power < 2) power = 2;
    const tableSize = 1 << power;

    // nearest-color cache
    const cache = new Map();
    function nearest(r, g, b) {
      const k = r + "," + g + "," + b;
      let v = cache.get(k);
      if (v !== undefined) return v;
      let bi = 0, bd = Infinity;
      for (let i = 0; i < palN; i++) {
        const c = palColors[i];
        const dr = r - c[0], dg = g - c[1], db = b - c[2];
        const dist = dr * dr + dg * dg + db * db;
        if (dist < bd) { bd = dist; bi = i; }
      }
      v = bi + 1; // +1: index 0 is transparent
      cache.set(k, v);
      return v;
    }

    const out = [];
    const pushStr = s => { for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i)); };
    const u16 = n => { out.push(n & 0xff, (n >> 8) & 0xff); };

    pushStr("GIF89a");
    u16(width); u16(height);
    out.push(0x80 | (7 << 4) | (power - 1)); // packed: GCT, colorRes 8, size
    out.push(0);   // background color index (transparent)
    out.push(0);   // aspect ratio
    // global color table
    out.push(0, 0, 0); // index 0 transparent placeholder
    for (let i = 0; i < tableSize - 1; i++) {
      if (i < palN) out.push(palColors[i][0], palColors[i][1], palColors[i][2]);
      else out.push(0, 0, 0);
    }
    // loop
    if (loop) {
      out.push(0x21, 0xFF, 0x0B); pushStr("NETSCAPE2.0");
      out.push(0x03, 0x01); u16(0); out.push(0x00);
    }
    const minCode = Math.max(2, power);
    for (const f of frames) {
      // GCE
      out.push(0x21, 0xF9, 0x04, 0x09); // disposal 2 + transparent flag
      u16(f.delay || 8); out.push(0x00, 0x00);
      // image descriptor
      out.push(0x2C); u16(0); u16(0); u16(width); u16(height); out.push(0x00);
      // indices
      const d = f.rgba, n = width * height;
      const idx = new Uint8Array(n);
      for (let p = 0; p < n; p++) {
        const o = p * 4;
        idx[p] = d[o + 3] < alphaThresh ? 0 : nearest(d[o], d[o + 1], d[o + 2]);
      }
      out.push(minCode);
      const bytes = lzw(minCode, idx);
      for (let i = 0; i < bytes.length; i += 255) {
        const chunk = bytes.slice(i, i + 255);
        out.push(chunk.length);
        for (const b of chunk) out.push(b);
      }
      out.push(0x00);
    }
    out.push(0x3B);
    return new Uint8Array(out);
  }

  window.Enc = { encodeGIF };
})();
