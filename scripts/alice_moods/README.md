# ALICE mood avatar renderer

Re-exports the 21 ALICE mood avatars at any resolution, headless.

`alice-draw.js` and `enc.js` are copied verbatim from the **ALICE Mood Studio**
Claude Design project (`26be57ed-f2ce-429a-a56e-91cd709d9376`). They are the same
scripts the browser export runs, so output here is a true vector redraw at the
target size — never an upscale of an already-exported set. Keep them in sync with
the design project rather than editing them here.

## Usage

```bash
cd scripts/alice_moods && npm install          # once — installs @napi-rs/canvas
node render_alice_moods.mjs 256 ../../managed_components/78__xiaozhi-fonts/gif/alice-moods-64
```

Then rebuild; `main/CMakeLists.txt` globs that directory, so the assets partition
is regenerated automatically.

Keep `custom-assets/alice-moods-64/` in sync as the tracked source copy.

## Sizing

The assets partition is 8 MB and also holds the text font (~2.6 MB) and the
wake-word models (~2.8 MB). Measured GIF totals for all 21 moods:

| size | total | assets.bin |
|------|-------|-----------|
| 64   | 327 KB | — |
| 128  | 754 KB | 6.13 MB |
| 256  | 1.55 MB | 6.68 MB |
| 320  | ~2.2 MB | ~7.3 MB |

Growth is sub-quadratic (~`(S/128)^1.17`) because LZW collapses the long
transparent runs that get longer as the canvas grows. 320 px is the practical
ceiling before the font has to be subset.

Filenames are the emoji keys the firmware matches on, so the design's `kiss`
is written out as `kissy` to match the server's `EMOJI_MAP`.
