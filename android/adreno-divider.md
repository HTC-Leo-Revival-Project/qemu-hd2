# The divider above the dock: an unfinished investigation

Status: **open**.  One artifact remains from the OpenGL ES backend.  This is
everything measured about it, including the things that turned out not to be
the cause, so the next attempt does not repeat them.

Renderer: `msm_adreno_gles.c`.  Reference: the software rasteriser in
`msm_adreno_draw.c`, which draws the same thing correctly.

## The symptom

A thin grey horizontal rule, three rows tall, spanning the width just above
the dock.  Guest rows **701, 702, 703** in a 480x800 framebuffer; it runs from
x=8 to x=471.

| | pixels lit out of 464 |
|---|---|
| software | 464 - solid, unbroken |
| GLES | 280-336, and it varies between frames |

Everything else in the frame matches the software renderer exactly.  A whole
image diff across two runs (minutes apart, so the clock and the wallpaper
particles differ anyway) shows differences **only** in:

* rows 11-99 - the status bar: the clock and signal icons, which differ
  because the two runs were at different times
* rows 195-351 - the clock widget's hands, likewise
* **rows 701-703 - this artifact, and nothing else**

So it is one element, not a general corruption.

## Where the gaps are

The surviving pixels are **per resolve tile, not per 8 or 16 pixels**.  The
guest renders the layer in tiles and resolves them one at a time, and from the
`resolve` trace they are:

```
resolve 0,0-128,416  off   0,0    gmemoff 0  pitch 128
resolve 0,0-128,384  off   0,416  gmemoff 0  pitch 128
resolve 0,0-128,416  off 128,0    gmemoff 0  pitch 128
resolve 0,0-128,384  off 128,416  gmemoff 0  pitch 128
resolve 0,0-128,416  off 256,0    gmemoff 0  pitch 128
resolve 0,0-128,384  off 256,416  gmemoff 0  pitch 128
resolve 0,0-96,416   off 384,0    gmemoff 0  pitch 128
resolve 0,0-96,384   off 384,416  gmemoff 0  pitch 128
```

480 = 128 + 128 + 128 + 96 across, 800 = 416 + 384 down, so eight tiles per
layer.  (The row of three-pixel marks is 8- or 16-aligned only in the small
runs at the tile edges; the dominant feature is tile-sized.)

Typical lit runs, GLES, row 702:

```
x  16..31   tile 0 (0..127)    mostly missing
x 128..383  tiles 1 and 2      complete
x 392..407  tile 3 (384..479)  mostly missing
```

**The middle tiles are always perfect; the outer ones are partial.**  That is
the shape of the whole bug, and it is the thing to explain.  Tile 3 is also
the odd one out at 96 wide against a GMEM pitch of 128, so a width/pitch
mismatch is worth keeping in mind.

The colour where it survives is **exactly right** - `(89,89,91)` on both
renderers, byte for byte.  So fragments are being **dropped**, not written
wrong: whatever draws this is producing correct pixels in some tiles and no
pixels at all in others.

## Ruled out, with the evidence

Each of these was tested and did not change the picture:

* **Blending.**  Forcing blending off for the whole backend
  (`du.rb2[1] = 0`) gave an identical frame.
* **Resolve deferral.**  A bin read-out is normally deferred to the end of the
  kick via a GMEM snapshot; disabling that (making
  `adreno_gles_gmem_snapshot()` return false, so every bin is copied out as it
  is issued) gave 328 lit pixels against 312 with deferral on - noise.
  *This test was run twice; the first time it was invalid, because the phone
  was still running an old binary.  See android/HEADLESS.md trap 1.*
* **The window scissor.**  Soft clamps per pixel with the same clip values
  GLES hands to `glScissor`, so the two agree by construction.
* **`GL_CLAMP_TO_BORDER` sampling transparent black.**  A real bug - GL ES
  fixes the border colour at `(0,0,0,0)` and
  `GL_TEXTURE_BORDER_COLOR` is desktop-only - but fixing it (clamp to edge)
  did not touch this artifact.
* **GL errors.**  None at all now: `glGetError` is clean, which it was not
  until the two desktop-only enums were removed.
* **Anything the renderer can point at.**  Zero texture failures, zero program
  failures, zero missing-host-GPU, zero dummy fetches.
* **MSAA.**  Every resolve reports `sx 1`, and only ~50 draws per interval
  have a sample grid.

## What it is not

**It is not a thin primitive.**  This is the most useful negative result.

`draw_triangle()` computes a bounding box and traces; `gl_emit()` builds the
window-space vertices the host GPU actually draws.  Both were instrumented to
log any primitive **wider than 60 px and no more than 8 rows tall**, over a
full boot's draw stream (28,210 draws), and **neither logged a single match**.

So the rule is not being drawn as a wide, thin primitive anywhere - not on the
CPU path, not on the GPU path.  Whatever produces those three rows is doing
something else: filling a whole tile and only part of it landing, or being
composed by something other than an Adreno draw.

Related: the divider's colour, `0x59595b`, **appears nowhere in the command
trace** - no clear colour, no constant.  So it is computed by a shader or
sampled from a texture, not written as a literal.

## Where to look next

In rough order of promise:

1. **Does anything other than the Adreno draw these three rows?**  On the HD2
   the final composition is the MDP blitter (`msm_mdp.c`), and the divider may
   be a layer the guest composites there rather than something it renders.
   Nothing in `qemu.log` says `msm-mdp:`, but the absence of logging is not
   evidence of absence.  A layer 464x3 blitted to y=701 would explain a
   tile-shaped pattern that no Adreno primitive matches.

2. **Whole-tile fills that partly land.**  A fast clear fills a bin with one
   draw, and there is one in this code that rewrites the colour format
   (`COLORX_4_4_4_4` becomes `COLORX_8_8_8_8`, `msm_adreno_draw.c` around
   `d->color_fmt`).  If the rule is a bin clear whose colour is blended or
   masked, the middle tiles could succeed and the edge tiles fail on format or
   mask.  Worth instrumenting the fast-clear path specifically.

3. **The 96-wide tile.**  Tile 3 resolves 96 pixels out of a GMEM row of 128.
   `resolve_copy()`'s fast path reads `(x1-x0)*bpp` bytes from
   `gmem + src->offset + y*src->pitch*bpp`; for that tile `x1-x0` is 96 while
   the stride is 128.  That is right for a straight copy, but if any step
   assumes the tile is as wide as the pitch, the last tile is where it shows -
   and it does.  The gap in tile **0** is the part that does not fit.

4. **Which draw owns those rows, directly.**  The honest answer is still not
   known.  A fragment shader could be made to record, into a second storage
   buffer, which draw last touched a given row band; dump that afterwards.  It
   is invasive but it ends the guessing, and everything above is guessing.

## Reproducing

```sh
tools/phone-boot.sh gles trace      # about four minutes to the launcher
tools/phone-shot.py divider --qmp
```

Then, to line the divider up against the software renderer:

```sh
tools/phone-boot.sh soft
tools/phone-shot.py divider_soft --qmp
```

Reading the two: rows 701-703 should be solid in `divider_soft.png` and
patchy in `divider.png`.

`ADRENO_GLES_NODEFER=1` (`phone-boot.sh gles nodec`) is the deferral
experiment above; it is worth keeping only until this is closed.
`ADRENO_GLES_BAND=1` was the geometry trace that found nothing, and its
instrumentation has been removed - re-add it in `gl_emit()` if it is needed
again, since that is the only place GPU-path geometry exists.