// alice-draw.js — render the ALICE chibi (head+shoulders) in 21 moods on a 2D canvas.
// Draws in a 64-unit space; the caller scales the context. window.Alice = { MOODS, draw }.
(function () {
  "use strict";

  const C = {
    SKIN: "#F4C89E", SKIN_D: "#D8AA80", SKIN_SH: "#E9B98C",
    HAIR: "#3C2C2A", HAIR_D: "#2B1F1E", HAIR_HL: "#4E3A37",
    SUIT: "#1E2129", SUIT_L: "#323742", SUIT_D: "#14161C",
    SHIRT: "#D4DCE6", TIE: "#FAD62A", TIE_D: "#D6B41C",
    INK: "#282C38", EYEW: "#FAFCFF", WHITE: "#FFFFFF",
    PINK: "#FF78A0", BLUSH: "#FCA5B4", MOUTH: "#9E4A55", TONGUE: "#F08098",
    TEAR: "#8FC8FF", SWEAT: "#BFE0FF"
  };

  const MOODS = [
    { key: "neutral",     label: "Neutral",     accent: "#3CDCEB", eyes: "open",    mouth: "flat",     blush: 0, brows: "flat" },
    { key: "happy",       label: "Happy",       accent: "#3CDC64", eyes: "happy",   mouth: "smile",    blush: 1 },
    { key: "laughing",    label: "Laughing",    accent: "#FFA51E", eyes: "happy",   mouth: "bigsmile", blush: 1 },
    { key: "funny",       label: "Funny",       accent: "#FFB020", eyes: "wink",    mouth: "grin",     blush: 1, overlay: "sparkle" },
    { key: "sad",         label: "Sad",         accent: "#508CFF", eyes: "teary",   mouth: "frown",    blush: 0, brows: "sad",  look: [0, 1.4], overlay: "tear" },
    { key: "angry",       label: "Angry",       accent: "#FF3C3C", eyes: "angry",   mouth: "teeth",    blush: 0, brows: "angry", overlay: "anger" },
    { key: "crying",      label: "Crying",      accent: "#6AA8FF", eyes: "squeeze", mouth: "openfrown",blush: 0, brows: "sad",  overlay: "tears" },
    { key: "loving",      label: "Loving",      accent: "#FF78A0", eyes: "heart",   mouth: "smile",    blush: 2, overlay: "hearts" },
    { key: "embarrassed", label: "Embarrassed", accent: "#FF8FA3", eyes: "half",    mouth: "wavy",     blush: 3, look: [-1.4, 0.4], overlay: "sweat" },
    { key: "surprised",   label: "Surprised",   accent: "#FFA51E", eyes: "wide",    mouth: "o",        blush: 0, brows: "raise", overlay: "excl" },
    { key: "shocked",     label: "Shocked",     accent: "#7FB0FF", eyes: "shock",   mouth: "bigo",     blush: 0, brows: "raise", overlay: "shock" },
    { key: "thinking",    label: "Thinking",    accent: "#3CDCEB", eyes: "open",    mouth: "sidethink",blush: 0, brows: "think", look: [-1.6, -1.2], overlay: "question" },
    { key: "winking",     label: "Winking",     accent: "#3CDC64", eyes: "wink",    mouth: "smile",    blush: 1, overlay: "sparkle" },
    { key: "cool",        label: "Cool",        accent: "#508CFF", eyes: "shades",  mouth: "smirk",    blush: 0, overlay: "sparkle" },
    { key: "relaxed",     label: "Relaxed",     accent: "#5AD7C3", eyes: "happy",   mouth: "softsmile",blush: 1 },
    { key: "delicious",   label: "Delicious",   accent: "#FFA51E", eyes: "happy",   mouth: "lick",     blush: 1, overlay: "sparkle" },
    { key: "kiss",        label: "Kiss",        accent: "#FF78A0", eyes: "wink",    mouth: "kiss",     blush: 2, overlay: "kissheart" },
    { key: "confident",   label: "Confident",   accent: "#FAD62A", eyes: "open",    mouth: "smirk",    blush: 0, brows: "raise", look: [0, -0.4], overlay: "sparkle" },
    { key: "sleepy",      label: "Sleepy",      accent: "#A078EB", eyes: "sleepy",  mouth: "o",        blush: 0, overlay: "zzz" },
    { key: "silly",       label: "Silly",       accent: "#FFB020", eyes: "cross",   mouth: "tongue",   blush: 1 },
    { key: "confused",    label: "Confused",    accent: "#A078EB", eyes: "confuse", mouth: "wavy",     blush: 0, brows: "think", tilt: -7, overlay: "question" }
  ];

  // round Harry-Potter glasses on everyone except Cool (shades) and Sleepy (eyes shut)
  MOODS.forEach((m) => { if (m.key !== "cool" && m.key !== "sleepy") m.glasses = true; });

  // ── mix two hex colors ──────────────────────────────────────────────────
  function mix(a, b, t) {
    const pa = [parseInt(a.slice(1, 3), 16), parseInt(a.slice(3, 5), 16), parseInt(a.slice(5, 7), 16)];
    const pb = [parseInt(b.slice(1, 3), 16), parseInt(b.slice(3, 5), 16), parseInt(b.slice(5, 7), 16)];
    const r = Math.round(pa[0] + (pb[0] - pa[0]) * t);
    const g = Math.round(pa[1] + (pb[1] - pa[1]) * t);
    const bl = Math.round(pa[2] + (pb[2] - pa[2]) * t);
    return "rgb(" + r + "," + g + "," + bl + ")";
  }

  function ell(ctx, x, y, rx, ry, fill) {
    ctx.beginPath(); ctx.ellipse(x, y, rx, ry, 0, 0, Math.PI * 2); ctx.fillStyle = fill; ctx.fill();
  }
  function rrect(ctx, x, y, w, h, r, fill) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath(); ctx.fillStyle = fill; ctx.fill();
  }
  function poly(ctx, pts, fill) {
    ctx.beginPath(); ctx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
    ctx.closePath(); ctx.fillStyle = fill; ctx.fill();
  }
  function stroke(ctx, w, color, fn) {
    ctx.beginPath(); ctx.lineWidth = w; ctx.strokeStyle = color;
    ctx.lineCap = "round"; ctx.lineJoin = "round"; fn(); ctx.stroke();
  }
  function heart(ctx, x, y, s, fill) {
    ctx.beginPath();
    ctx.moveTo(x, y + s * 0.75);
    ctx.bezierCurveTo(x - s * 1.1, y - s * 0.35, x - s * 0.5, y - s * 1.05, x, y - s * 0.35);
    ctx.bezierCurveTo(x + s * 0.5, y - s * 1.05, x + s * 1.1, y - s * 0.35, x, y + s * 0.75);
    ctx.closePath(); ctx.fillStyle = fill; ctx.fill();
  }
  function sparkle(ctx, x, y, s, fill) {
    ctx.beginPath();
    ctx.moveTo(x, y - s); ctx.quadraticCurveTo(x, y, x + s, y);
    ctx.quadraticCurveTo(x, y, x, y + s); ctx.quadraticCurveTo(x, y, x - s, y);
    ctx.quadraticCurveTo(x, y, x, y - s); ctx.closePath();
    ctx.fillStyle = fill; ctx.fill();
  }

  // ── eyes ──────────────────────────────────────────────────────────────
  function drawEye(ctx, x, y, style, side, lx, ly, blink, accent) {
    const openLike = style === "open" || style === "wide" || style === "teary" ||
      style === "shock" || style === "angry";
    if (blink && openLike) {
      stroke(ctx, 1.3, C.INK, () => { ctx.moveTo(x - 3.2, y); ctx.quadraticCurveTo(x, y + 1.4, x + 3.2, y); });
      return;
    }
    switch (style) {
      case "open": case "angry": {
        ell(ctx, x, y, 3.6, 4.4, C.EYEW);
        ell(ctx, x + lx, y + ly, 2.9, 3.2, C.INK);
        ell(ctx, x + lx - 1, y + ly - 1.2, 1.1, 1.3, C.WHITE);
        ell(ctx, x + lx + 1.1, y + ly + 1.2, 0.5, 0.5, "rgba(255,255,255,.7)");
        break;
      }
      case "wide": {
        ell(ctx, x, y, 4, 5, C.EYEW);
        ell(ctx, x + lx, y + ly, 2, 2.2, C.INK);
        ell(ctx, x + lx - 0.7, y + ly - 0.9, 0.8, 0.9, C.WHITE);
        break;
      }
      case "shock": {
        ell(ctx, x, y, 4, 5, C.EYEW);
        ell(ctx, x + lx, y + ly, 1.3, 1.4, C.INK);
        break;
      }
      case "teary": {
        ell(ctx, x, y, 3.7, 4.5, C.EYEW);
        ell(ctx, x + lx, y + ly - 0.4, 3, 3.3, C.INK);
        ell(ctx, x + lx - 1.1, y + ly - 1.4, 1.4, 1.5, C.WHITE);
        ell(ctx, x + lx + 1, y + ly + 1, 0.7, 0.7, "rgba(255,255,255,.8)");
        // glossy waterline
        stroke(ctx, 0.8, mix(C.TEAR, C.WHITE, 0.4), () => { ctx.moveTo(x - 2.6, y + 3.4); ctx.quadraticCurveTo(x, y + 4.2, x + 2.6, y + 3.4); });
        break;
      }
      case "happy": {
        stroke(ctx, 1.4, C.INK, () => { ctx.moveTo(x - 3.2, y + 1.4); ctx.quadraticCurveTo(x, y - 2.4, x + 3.2, y + 1.4); });
        break;
      }
      case "squeeze": {
        stroke(ctx, 1.5, C.INK, () => { ctx.moveTo(x - 3.4, y - 1.4); ctx.quadraticCurveTo(x, y + 2.2, x + 3.4, y - 1.4); });
        break;
      }
      case "half": {
        ell(ctx, x, y + 0.6, 3.6, 3.6, C.EYEW);
        ell(ctx, x + lx, y + 1.4, 2.7, 2.7, C.INK);
        ell(ctx, x + lx - 0.9, y + 0.6, 0.9, 1, C.WHITE);
        // upper lid
        ctx.save();
        ctx.beginPath(); ctx.rect(x - 4.2, y - 4, 8.4, 3.6); ctx.clip();
        ell(ctx, x, y - 3.6, 4, 4, C.SKIN); ctx.restore();
        stroke(ctx, 1.1, C.SKIN_D, () => { ctx.moveTo(x - 3.6, y + 0.2); ctx.quadraticCurveTo(x, y - 1, x + 3.6, y + 0.2); });
        break;
      }
      case "sleepy": {
        // drooping closed lids + short lashes
        stroke(ctx, 1.4, C.INK, () => { ctx.moveTo(x - 3.4, y - 0.6); ctx.quadraticCurveTo(x, y + 1.8, x + 3.4, y - 0.6); });
        stroke(ctx, 0.8, C.INK, () => { ctx.moveTo(x + 3, y + 0.2); ctx.lineTo(x + 4.2, y + 1.2); });
        break;
      }
      case "heart": {
        ell(ctx, x, y, 3.6, 4.4, C.EYEW);
        heart(ctx, x, y + 0.3, 3.2, C.PINK);
        ell(ctx, x - 1, y - 1, 0.7, 0.8, "rgba(255,255,255,.85)");
        break;
      }
      case "cross": {
        // crossed eyes — irises toward the nose
        const dir = side === "L" ? 1.4 : -1.4;
        ell(ctx, x, y, 3.6, 4.2, C.EYEW);
        ell(ctx, x + dir, y - 0.3, 2.4, 2.7, C.INK);
        ell(ctx, x + dir - 0.7, y - 1.1, 0.8, 0.9, C.WHITE);
        break;
      }
    }
  }

  function drawBrow(ctx, x, y, kind, side) {
    if (!kind || kind === "flat") return;
    const s = side === "L" ? 1 : -1;
    ctx.save();
    if (kind === "angry") stroke(ctx, 1.3, C.HAIR_D, () => { ctx.moveTo(x - 3.4 * s, y - 4.6); ctx.lineTo(x + 2.6 * s, y - 2.8); });
    else if (kind === "sad") stroke(ctx, 1.2, C.HAIR_D, () => { ctx.moveTo(x - 3 * s, y - 3); ctx.lineTo(x + 3 * s, y - 4.4); });
    else if (kind === "raise") stroke(ctx, 1.1, C.HAIR_D, () => { ctx.moveTo(x - 3, y - 5); ctx.quadraticCurveTo(x, y - 6, x + 3, y - 5); });
    else if (kind === "think") { // one raised, one flat
      if (side === "R") stroke(ctx, 1.1, C.HAIR_D, () => { ctx.moveTo(x - 3, y - 5.6); ctx.quadraticCurveTo(x, y - 6.6, x + 3, y - 5.4); });
      else stroke(ctx, 1.1, C.HAIR_D, () => { ctx.moveTo(x - 3, y - 4.4); ctx.lineTo(x + 3, y - 4.4); });
    }
    ctx.restore();
  }

  // ── mouth ────────────────────────────────────────────────────────────
  function drawMouth(ctx, x, y, style) {
    switch (style) {
      case "flat": stroke(ctx, 1, C.SKIN_D, () => { ctx.moveTo(x - 2.6, y); ctx.lineTo(x + 2.6, y); }); break;
      case "dot": ell(ctx, x, y, 1, 1.1, C.MOUTH); break;
      case "smile": case "softsmile":
        stroke(ctx, 1.2, C.MOUTH, () => { ctx.moveTo(x - 3, y - (style === "softsmile" ? 0.4 : 0)); ctx.quadraticCurveTo(x, y + (style === "softsmile" ? 2 : 3), x + 3, y - (style === "softsmile" ? 0.4 : 0)); }); break;
      case "bigsmile": {
        ctx.beginPath(); ctx.moveTo(x - 4, y - 1);
        ctx.quadraticCurveTo(x, y + 5.5, x + 4, y - 1); ctx.closePath();
        ctx.fillStyle = "#7C2E38"; ctx.fill();
        stroke(ctx, 1.1, "#F6F1E8", () => { ctx.moveTo(x - 3.4, y - 0.6); ctx.quadraticCurveTo(x, y + 0.6, x + 3.4, y - 0.6); });
        ell(ctx, x, y + 3.2, 1.7, 1.2, C.TONGUE);
        break;
      }
      case "grin": {
        ctx.beginPath(); ctx.moveTo(x - 4.2, y - 0.6);
        ctx.quadraticCurveTo(x, y + 4.2, x + 4.2, y - 0.6); ctx.closePath();
        ctx.fillStyle = "#7C2E38"; ctx.fill();
        ctx.fillStyle = "#F6F1E8"; ctx.fillRect(x - 3.4, y - 0.6, 6.8, 1.4);
        ell(ctx, x + 1.4, y + 2.4, 1.8, 1.3, C.TONGUE);
        break;
      }
      case "o": ell(ctx, x, y + 0.5, 1.6, 2, "#7C2E38"); break;
      case "bigo": {
        ell(ctx, x, y + 1, 2.6, 3.4, "#7C2E38");
        ell(ctx, x, y + 2, 1.5, 1.8, C.TONGUE);
        break;
      }
      case "frown": stroke(ctx, 1.2, C.MOUTH, () => { ctx.moveTo(x - 2.8, y + 1.6); ctx.quadraticCurveTo(x, y - 1.4, x + 2.8, y + 1.6); }); break;
      case "openfrown": {
        ctx.beginPath(); ctx.moveTo(x - 3, y + 2.4);
        ctx.quadraticCurveTo(x, y - 2.6, x + 3, y + 2.4);
        ctx.quadraticCurveTo(x, y + 3.6, x - 3, y + 2.4); ctx.closePath();
        ctx.fillStyle = "#7C2E38"; ctx.fill();
        break;
      }
      case "teeth": {
        rrect(ctx, x - 3.2, y - 1.2, 6.4, 3, 0.8, "#F6F1E8");
        stroke(ctx, 0.6, "#B9A99A", () => {
          ctx.moveTo(x - 3.2, y + 0.3); ctx.lineTo(x + 3.2, y + 0.3);
          ctx.moveTo(x - 1.4, y - 1.2); ctx.lineTo(x - 1.4, y + 1.8);
          ctx.moveTo(x + 1.4, y - 1.2); ctx.lineTo(x + 1.4, y + 1.8);
        });
        break;
      }
      case "tongue": {
        stroke(ctx, 1.2, C.MOUTH, () => { ctx.moveTo(x - 3, y - 0.4); ctx.quadraticCurveTo(x, y + 2.4, x + 3, y - 0.4); });
        ctx.beginPath(); ctx.moveTo(x - 2.2, y + 1); ctx.quadraticCurveTo(x - 2.4, y + 4.2, x + 0.2, y + 4);
        ctx.quadraticCurveTo(x + 1.6, y + 3.8, x + 1.4, y + 1); ctx.closePath();
        ctx.fillStyle = C.TONGUE; ctx.fill();
        stroke(ctx, 0.5, "#D9607E", () => { ctx.moveTo(x - 0.6, y + 1.4); ctx.lineTo(x - 0.7, y + 3.6); });
        break;
      }
      case "lick": {
        stroke(ctx, 1.2, C.MOUTH, () => { ctx.moveTo(x - 3, y); ctx.quadraticCurveTo(x, y + 2.6, x + 3, y); });
        ctx.beginPath(); ctx.moveTo(x + 0.4, y - 0.6);
        ctx.quadraticCurveTo(x + 3.4, y - 1.6, x + 2.6, y + 1.4);
        ctx.quadraticCurveTo(x + 1.6, y + 1, x + 0.4, y - 0.6); ctx.closePath();
        ctx.fillStyle = C.TONGUE; ctx.fill();
        break;
      }
      case "kiss": {
        ell(ctx, x, y + 0.3, 1.5, 1.8, C.PINK);
        ell(ctx, x, y + 0.1, 0.7, 0.9, "#E85C86");
        break;
      }
      case "smirk": stroke(ctx, 1.2, C.MOUTH, () => { ctx.moveTo(x - 2.6, y + 1.2); ctx.quadraticCurveTo(x + 1.4, y + 1.6, x + 3.2, y - 1); }); break;
      case "sidethink": stroke(ctx, 1.1, C.MOUTH, () => { ctx.moveTo(x - 2.8, y + 0.8); ctx.quadraticCurveTo(x - 0.4, y - 0.6, x + 2.4, y + 0.2); }); break;
      case "wavy": stroke(ctx, 1.1, C.MOUTH, () => { ctx.moveTo(x - 3, y); ctx.quadraticCurveTo(x - 1.4, y - 1.6, x, y); ctx.quadraticCurveTo(x + 1.4, y + 1.6, x + 3, y); }); break;
    }
  }

  // ── overlays (animated by phase p 0..1) ────────────────────────────────
  function drawOverlay(ctx, kind, accent, p, cx) {
    // head-only anchors: head center ~(32,30) r~29; eyes ~y34 at x20 / x44
    const eyeYv = 34, eyeLx = 20, eyeRx = 44;
    const tearC = mix(C.TEAR, C.WHITE, 0.2);
    switch (kind) {
      case "sparkle": {
        const a = 0.5 + 0.5 * Math.sin(p * Math.PI * 2);
        ctx.globalAlpha = 0.5 + 0.5 * a;
        sparkle(ctx, 53, 10, 2.8 + a, "#FFF6C8");
        ctx.globalAlpha = 0.4 + 0.5 * (1 - a);
        sparkle(ctx, 11, 26, 2 + (1 - a), "#FFF6C8");
        ctx.globalAlpha = 1; break;
      }
      case "tear": {
        const t = p % 1;
        ell(ctx, eyeRx, eyeYv + 4 + t * 14, 1.5, 2.2 + t, tearC);
        break;
      }
      case "tears": {
        for (let s = 0; s < 2; s++) {
          const bx = s ? eyeRx : eyeLx;
          for (let i = 0; i < 2; i++) {
            const t = (p + i * 0.5) % 1;
            ell(ctx, bx, eyeYv + 4 + t * 16, 1.6, 2.4 + t * 1.6, tearC);
          }
        }
        break;
      }
      case "hearts": {
        for (let i = 0; i < 3; i++) {
          const t = (p + i * 0.33) % 1;
          ctx.globalAlpha = 1 - t;
          heart(ctx, 47 + i * 2, 26 - t * 20, 2.6 + (1 - t) * 1.6, i % 2 ? "#FF8FB0" : C.PINK);
        }
        ctx.globalAlpha = 1; break;
      }
      case "kissheart": {
        const t = p;
        ctx.globalAlpha = 1 - t * 0.7;
        heart(ctx, 40 + t * 10, 47 - t * 12, 3.2 - t * 1.2, C.PINK);
        ctx.globalAlpha = 1; break;
      }
      case "sweat": {
        const t = (p * 1.2) % 1;
        const sx = 55, sy = 13 + t * 7;
        ctx.beginPath();
        ctx.moveTo(sx, sy - 3.4);
        ctx.quadraticCurveTo(sx + 2.4, sy, sx, sy + 2.2);
        ctx.quadraticCurveTo(sx - 2.4, sy, sx, sy - 3.4);
        ctx.closePath(); ctx.fillStyle = C.SWEAT; ctx.fill();
        break;
      }
      case "excl": {
        const a = 0.6 + 0.4 * Math.sin(p * Math.PI * 4);
        ctx.globalAlpha = a;
        rrect(ctx, 52, 4, 2.6, 8, 1.2, accent);
        ell(ctx, 53.3, 15, 1.4, 1.4, accent);
        ctx.globalAlpha = 1; break;
      }
      case "shock": {
        ctx.globalAlpha = 0.65 + 0.35 * Math.sin(p * Math.PI * 4);
        stroke(ctx, 1.1, mix(accent, C.WHITE, 0.2), () => {
          for (let i = 0; i < 7; i++) {
            const ang = Math.PI * (1.06 + i * 0.135);
            const c = Math.cos(ang), s = Math.sin(ang);
            ctx.moveTo(32 + c * 30, 30 + s * 30);
            ctx.lineTo(32 + c * 35, 30 + s * 35);
          }
        });
        ctx.globalAlpha = 1; break;
      }
      case "zzz": {
        ctx.fillStyle = mix(accent, C.WHITE, 0.3);
        for (let i = 0; i < 3; i++) {
          const t = (p + i * 0.33) % 1;
          ctx.globalAlpha = 0.4 + 0.6 * (1 - t);
          ctx.font = "bold " + (6 + i * 1.5) + "px system-ui, sans-serif";
          ctx.fillText(i === 2 ? "Z" : "z", 47 + i * 4, 18 - t * 12 - i * 3);
        }
        ctx.globalAlpha = 1; break;
      }
      case "question": {
        const a = 0.6 + 0.4 * Math.sin(p * Math.PI * 2);
        ctx.globalAlpha = a; ctx.fillStyle = accent;
        ctx.font = "bold 14px system-ui, sans-serif";
        ctx.fillText("?", 48, 17);
        ctx.globalAlpha = 1; break;
      }
    }
  }

  // ── full character ──────────────────────────────────────────────────────
  function draw(ctx, mood, t) {
    const cx = 32;
    const bob = Math.sin(t * Math.PI * 2) * 0.7;
    const blink = t > 0.85 && t < 0.965;
    const accent = mood.accent;

    ctx.save();
    if (mood.tilt) { ctx.translate(cx, 30); ctx.rotate(mood.tilt * Math.PI / 180); ctx.translate(-cx, -30); }
    ctx.translate(0, bob);

    // head-only framing: zoom about the head so it fills the frame without clipping edges
    ctx.save();
    ctx.translate(cx, 29); ctx.scale(1.45, 1.45); ctx.translate(-cx, -24);

    const headCY = 24, headR = 15;
    const glow = mix(accent, "#0A0E16", 0.15);

    // side hair locks (behind)
    rrect(ctx, cx - 18, headCY - 4, 8, 50, 4, C.HAIR);
    rrect(ctx, cx + 10, headCY - 4, 8, 50, 4, C.HAIR);
    ell(ctx, cx, headCY + 1, headR + 3, headR + 2, C.HAIR);

    // suit / shoulders
    ell(ctx, cx - 19, 55, 8, 8, C.SUIT);
    ell(ctx, cx + 19, 55, 8, 8, C.SUIT);
    rrect(ctx, cx - 22, 46, 44, 20, 9, C.SUIT);
    stroke(ctx, 0.7, C.SUIT_L, () => {
      ctx.moveTo(cx - 12, 50); ctx.lineTo(cx - 13, 66);
      ctx.moveTo(cx + 12, 50); ctx.lineTo(cx + 13, 66);
    });
    // shirt collar
    poly(ctx, [[cx - 6, 45], [cx, 49], [cx + 6, 45], [cx, 56]], C.SHIRT);
    // tie
    ell(ctx, cx, 49.5, 2.6, 2.6, glow);
    ell(ctx, cx, 49.5, 1.7, 1.7, C.TIE);
    poly(ctx, [[cx - 2.6, 51.5], [cx + 2.6, 51.5], [cx + 3.4, 63], [cx, 66], [cx - 3.4, 63]], C.TIE);
    stroke(ctx, 0.6, C.TIE_D, () => { ctx.moveTo(cx, 52); ctx.lineTo(cx, 63); });

    // head
    ell(ctx, cx, headCY, headR, headR, C.SKIN);
    // cheek shading
    ell(ctx, cx - headR + 2, headCY + 5, 3, 4, C.SKIN_SH);
    ell(ctx, cx + headR - 2, headCY + 5, 3, 4, C.SKIN_SH);

    // hair cap + bangs
    ctx.beginPath();
    ctx.moveTo(cx - headR - 0.5, headCY - 1);
    ctx.quadraticCurveTo(cx - headR - 1, headCY - headR - 3, cx, headCY - headR - 2.5);
    ctx.quadraticCurveTo(cx + headR + 1, headCY - headR - 3, cx + headR + 0.5, headCY - 1);
    // bangs underside (M-curve)
    ctx.quadraticCurveTo(cx + headR - 3, headCY + 2.5, cx + 5, headCY + 0.5);
    ctx.quadraticCurveTo(cx + 2, headCY + 4, cx, headCY + 1);
    ctx.quadraticCurveTo(cx - 2, headCY + 4, cx - 5, headCY + 0.5);
    ctx.quadraticCurveTo(cx - headR + 3, headCY + 2.5, cx - headR - 0.5, headCY - 1);
    ctx.closePath(); ctx.fillStyle = C.HAIR; ctx.fill();
    stroke(ctx, 0.8, C.HAIR_HL, () => { ctx.moveTo(cx - 6, headCY - headR + 1); ctx.quadraticCurveTo(cx, headCY - headR - 1, cx + 7, headCY - headR + 2); });

    // earpiece glow (right)
    ell(ctx, cx + headR - 1, headCY + 2, 2.4, 2.4, glow);
    ell(ctx, cx + headR - 1, headCY + 2, 1.4, 1.4, accent);
    stroke(ctx, 0.8, accent, () => { ctx.moveTo(cx + headR - 1, headCY + 4); ctx.lineTo(cx + headR - 3, headCY + 9); });

    // eyes
    const eyeY = headCY + 2, ex = 6;
    const look = mood.look || [0, 0];
    if (mood.eyes === "shades") {
      // sunglasses
      rrect(ctx, cx - 9.5, eyeY - 3, 8.5, 6, 2, "#14161C");
      rrect(ctx, cx + 1, eyeY - 3, 8.5, 6, 2, "#14161C");
      ctx.fillStyle = "#14161C"; ctx.fillRect(cx - 1.4, eyeY - 2.2, 2.8, 1.4);
      stroke(ctx, 1.1, mix(accent, C.WHITE, 0.3), () => { ctx.moveTo(cx - 8, eyeY - 1.4); ctx.lineTo(cx - 4, eyeY - 2); });
    } else if (mood.eyes === "wink") {
      drawEye(ctx, cx - ex, eyeY, "open", "L", look[0], look[1], blink, accent);
      drawEye(ctx, cx + ex, eyeY, "happy", "R", 0, 0, false, accent);
    } else if (mood.eyes === "confuse") {
      drawEye(ctx, cx - ex, eyeY, "open", "L", -0.6, -0.6, blink, accent);
      drawEye(ctx, cx + ex, eyeY, "half", "R", 0, 0, false, accent);
    } else {
      drawEye(ctx, cx - ex, eyeY, mood.eyes, "L", look[0], look[1], blink, accent);
      drawEye(ctx, cx + ex, eyeY, mood.eyes, "R", look[0], look[1], blink, accent);
    }

    if (mood.glasses && mood.eyes !== "shades") {
      // round Harry-Potter frames
      const gc = "#1B1E26";
      stroke(ctx, 1.2, gc, () => { ctx.moveTo(cx - ex, eyeY); ctx.arc(cx - ex, eyeY, 5, 0, Math.PI * 2); });
      stroke(ctx, 1.2, gc, () => { ctx.moveTo(cx + ex, eyeY); ctx.arc(cx + ex, eyeY, 5, 0, Math.PI * 2); });
      stroke(ctx, 1.2, gc, () => { ctx.moveTo(cx - 1.5, eyeY - 0.6); ctx.quadraticCurveTo(cx, eyeY - 1.6, cx + 1.5, eyeY - 0.6); }); // bridge
      stroke(ctx, 1, gc, () => { ctx.moveTo(cx - ex - 4.6, eyeY - 1); ctx.lineTo(cx - ex - 8, eyeY - 2.4); }); // temples
      stroke(ctx, 1, gc, () => { ctx.moveTo(cx + ex + 4.6, eyeY - 1); ctx.lineTo(cx + ex + 8, eyeY - 2.4); });
      // glass glint
      stroke(ctx, 0.8, "rgba(255,255,255,.5)", () => { ctx.moveTo(cx - ex - 2, eyeY - 2.4); ctx.lineTo(cx - ex + 0.5, eyeY - 3.4); });
    }

    // brows
    if (mood.brows && mood.eyes !== "shades") {
      drawBrow(ctx, cx - ex, eyeY, mood.brows, "L");
      drawBrow(ctx, cx + ex, eyeY, mood.brows, "R");
    }

    // anger vein mark
    if (mood.overlay === "anger") {
      const c = "#E23B3B";
      stroke(ctx, 0.9, c, () => {
        ctx.moveTo(cx + 8, headCY - 6); ctx.lineTo(cx + 11, headCY - 4);
        ctx.moveTo(cx + 11, headCY - 7); ctx.lineTo(cx + 8, headCY - 3.5);
        ctx.moveTo(cx + 8, headCY - 4.5); ctx.lineTo(cx + 12, headCY - 5.5);
      });
    }

    // blush
    if (mood.blush) {
      const alpha = mood.blush >= 3 ? 0.85 : mood.blush === 2 ? 0.7 : 0.5;
      const rr = mood.blush >= 3 ? 4 : 3.2;
      ctx.globalAlpha = alpha;
      ell(ctx, cx - 9, headCY + 6, rr, rr * 0.62, C.BLUSH);
      ell(ctx, cx + 9, headCY + 6, rr, rr * 0.62, C.BLUSH);
      ctx.globalAlpha = 1;
      if (mood.blush >= 3) {
        stroke(ctx, 0.5, "#E88", () => {
          for (const bx of [cx - 10, cx - 8, cx + 8, cx + 10]) { ctx.moveTo(bx, headCY + 4); ctx.lineTo(bx, headCY + 8); }
        });
      }
    }

    // mouth
    drawMouth(ctx, cx, headCY + 9, mood.mouth);

    ctx.restore(); // end head-only zoom

    // overlays (drawn in the 64 frame around the enlarged head)
    if (mood.overlay) drawOverlay(ctx, mood.overlay, accent, t, cx);

    ctx.restore();
  }

  window.Alice = { MOODS, draw, C };
})();
