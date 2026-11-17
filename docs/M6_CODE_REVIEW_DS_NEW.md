# M6 code review (new pass) — WindowServer and display correctness

Reviewed commit: `587de1a` ("RedNaviMTL: IOSurface textures always re-read, new scanout ring copies start from the shown picture, file log for system daemons").
Review date: 2026-11-09.

This is a fresh review of the *current* tree, not a re-run of `docs/M6_CODE_REVIEW_DS.md` (which reviewed
`fa30c28`). The old review is used only as a checklist; every finding below was re-verified against the code
that is in the tree now. Findings are ordered by how directly they explain the reported symptoms, not by file.

Scope of the review:

- `RedNaviMTL/compute.m` (Metal device, textures, command buffers, render/compute/blit encoders, direct scanout)
- `RedNaviMTL/rn_vk.c`, `RedNaviMTL/rn_vk.h` (Vulkan/RADV backend)
- `RedNaviMTL/plugin.m` (plug-in installation and process gating)
- `RedNaviFB/kern_fb.cpp` (IOFramebuffer, attributes, VBL, diagnostics)
- `RedNaviAccel2/kern_accel2.cpp` (display pipe, transactions, scanout copy)
- `RedNavi/kern_probe.cpp` (GPU user client, scanout surfaces, page flip, direct-scanout flag)
- `mesa-bridge/src/ac_apple_iokit.c` (scanout mapping and present helpers)

Reported symptoms, as given:

1. an intermittent black band at the top of the screen;
2. minimise/maximise animations lag and leave pixel trails;
3. random pixels are left on the screen;
4. lighter grey rectangles appear in the top bar;
5. general glitches;
6. some icons disappear;
7. some text is drawn wider than it should be;
8. green/orange gradient checkerboards appear in window corners.

Severity legend: **S1** breaks the picture or loses frames, **S2** produces wrong pixels or stalls, **S3**
performance/robustness, **S4** diagnostics or latent bug.

## Symptom → most likely causes (short map)

| Symptom | Findings that explain it |
| --- | --- |
| Black band at the top | 1, 2, 3, 11, 13, 17, 21, 31, 33, 36, 39, 45, 48 |
| Animation lag / trails | 2, 3, 4, 8, 12, 15, 16, 20, 24, 25, 26, 27, 31, 32, 33, 35, 37, 38, 39, 41, 46, 47, 50 |
| Random pixels left on screen | 1, 3, 4, 8, 9, 10, 18, 20, 21, 27, 29, 39, 42, 43, 44, 45, 48 |
| Lighter grey rectangles in the top bar | 4, 8, 9, 19, 21, 22, 29, 33, 43, 46 |
| Icons disappearing / text wider | 5, 6, 7, 14, 20, 23, 28, 30, 32, 35, 40, 42, 44, 45 |
| Green/orange gradient checkerboards | 7, 18, 20, 21, 23, 28, 29, 39, 42, 44, 45 |

The rest of this document explains each finding, why it matters, and what to change. Each entry says what was
double-checked; where a claim depends on an assumption (e.g. an undocumented Apple structure), it says so.

---

## 1. The direct-scanout host image has no `TRANSFER_SRC`/`TRANSFER_DST` usage, so every copy to or from it is invalid (S1)

**Where:** `RedNaviMTL/rn_vk.c:483-486` (`rn_vk_image_from_host`), `RedNaviMTL/rn_vk.c:1171-1184`
(`rn_vk_image_copy`), `RedNaviMTL/compute.m:1317-1320`, `RedNaviMTL/compute.m:1342-1343`,
`RedNaviMTL/compute.m:1686-1689`.

**What the code does.** `rn_vk_image_from_host` creates the linear image over the scanout surface with:

```c
.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (im->color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0) | (im->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)
```

There is no `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` and no `VK_IMAGE_USAGE_TRANSFER_DST_BIT`. The same image is then
used as the source in `rn_vk_image_copy(vk, shown, img)` (the seed of a new display-VRAM copy,
`compute.m:1342-1343`) and as the destination in `rn_vk_image_copy(vk, scanSrc, scan)`
(`compute.m:1686-1689`, the copy that puts the finished frame on the screen). `rn_vk_image_copy` records
`vkCmdCopyImage` between the two images (`rn_vk.c:1183-1184`), and Vulkan requires both images to have been
created with the corresponding transfer usage bit.

**Why it produces the symptoms.** This is undefined behaviour. Depending on the driver and the validation
state it either fails the submit (the caller sees `rn_vk_image_copy != 0` and the frame is never presented),
or the copy is dropped/misbehaves. A dropped seed copy leaves the new per-IOSurface VRAM image with whatever
was in VRAM (often black or the previous ring frame), which is exactly the “black band at the top”, “random
pixels” and “trails” pattern when CoreDisplay draws only the damaged part of the next ring surface. A dropped
final copy leaves the screen showing the previous frame, or a partially copied one.

**Fix.** Add `VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT` to the host image when the
device supports them (the linear format feature query at `rn_vk.c:479-480` should check
`VK_FORMAT_FEATURE_TRANSFER_SRC_BIT`/`DST_BIT` too). If a format does not support them, do not use
`rn_vk_image_copy` on the host image: copy through a staging buffer instead. Enable
`VK_LAYER_KHRONOS_validation` in a debug build and treat this as an error, not a warning.

**Double-check.** Verified that `rn_vk_image_from_host` is the only creator of the scanout host image, that
both call sites use it through `rn_vk_image_copy`, and that `rn_vk_image_copy` really records
`vkCmdCopyImage` (not a buffer copy). The usage word is exactly the one quoted above; no later code adds bits
to `im->img` (usage is fixed at creation).

## 2. The end-of-frame scanout copy hard-codes 1920×1080, while the scanout image is cached with the first size seen (S1)

**Where:** `RedNaviMTL/compute.m:1309-1325` (`rn_scanout_image`), `RedNaviMTL/compute.m:1686-1689`
(end-of-command-buffer copy), `RedNaviMTL/compute.m:1442` (pass redirection).

**What the code does.** `rn_scanout_image` records the scanout geometry once, inside `dispatch_once`:

```c
static rn_vk_image *img; static dispatch_once_t once; static size_t sw, sh;
...
if (r) { sw = bw; sh = bh; ... }
...
return img && w == sw && h == sh ? img : NULL;
```

The pass redirection calls it with the target size (`rn_scanout_image(vk, rt->w, rt->h)`,
`compute.m:1442`), but the final copy calls it with literal constants:

```c
rn_vk_image *scan = rn_scanout_image(rn_vk_shared(), 1920, 1080);
if (scan && rn_vk_image_copy(rn_vk_shared(), scanSrc, scan) == 0) rn_frame_done();
```

(`compute.m:1687-1688`). On a 1920×1080 mode both calls agree. On any other mode the first call returns the
image (if the mode is 1920×1080-sized) but the final call returns `NULL`, or vice versa. `scanSrc` has already
been set by `runRenderPass`; the copy is silently skipped, and `rn_frame_done()` is never called.

**Why it produces the symptoms.** On a non-1080p mode, or after a mode change, the frame is rendered into the
per-IOSurface VRAM copy (`rn_display_vram`) but never copied to the scanout. If `gpuDirect` was already set
by an earlier successful frame (finding 3), the display pipe stops copying the IOSurface, so the screen
freezes on the old picture or shows the old frame under newly damaged regions: black band, trails, random
pixels. On a mode change the hard-coded size is wrong even if the scanout image itself was created at the new
size.

**Fix.** Do not hard-code the mode. Store the scanout geometry in one place and use it for both calls, or
derive it from `scanSrc->w`/`scanSrc->h` and the image that `rn_scanout_image` returned. Invalidate the cache
in `setDisplayMode` (see finding 17). If the final copy cannot be done, clear the direct flag / fall back
instead of silently dropping the frame (finding 3).

**Double-check.** Confirmed the literal `1920, 1080` at `compute.m:1687`; confirmed `rn_scanout_image`
returns `NULL` for any size different from the one captured in `dispatch_once` (`compute.m:1324`); confirmed
`scanSrc` is set before the final copy (`compute.m:1518-1524`). This is not a stale comment: the code path is
live in `commitBody`.

## 3. `gpuDirect` is latched on the first direct frame and never cleared when direct scanout stops working (S1)

**Where:** `RedNavi/kern_probe.cpp:2270-2275` (setting), `RedNavi/kern_probe.cpp:2074-2077` (clearing only on
client close), `RedNaviAccel2/kern_accel2.cpp:586-595` (the display pipe trusts it).

**What the code does.** A successful direct frame sends `RN_PRESENT_FRAME_DONE` through
`rn_frame_done()`; the probe user client sets:

```cpp
if (in->surface == RN_PRESENT_FRAME_DONE) { gpuOwner->gpuDirect = 1; setDirect = true; ... }
```

The only place that clears it is `clientClose()` (`kern_probe.cpp:2076`). `performTransaction` then does:

```cpp
if (... callPlatformFunction("rednavi-gpu-frames", ...) == kIOReturnSuccess && direct) {
    if (say) rnNote("the plug-in puts the frames into the scanout itself: not copied");
    return 0;
}
```

(`kern_accel2.cpp:591-594`). There is no heartbeat, timestamp, or “direct failed” message. Once one frame was
presented this way, the CPU fallback is disabled for the lifetime of the WindowServer connection.

**Why it produces the symptoms.** Direct scanout has several failure modes that are all silent: the scanout
host image copy is invalid (finding 1), the final copy is skipped because of the hard-coded size (finding 2),
the per-IOSurface image cannot be allocated, the scanout surface is not available after a mode change
(finding 17), or the copy fence times out. After any of them, `gpuDirect` is still 1, so
`performTransaction` keeps returning success without copying anything. The display shows a stale frame; damaged
regions are never refreshed. This is the most plausible explanation for “intermittent black band” plus
“pixels remain at random”: a frame or a region fails once, and from then on nothing is copied.

**Fix.** Make the flag a lease, not a latch:

- the plug-in sends `RN_PRESENT_FRAME_DONE` on every successfully presented frame;
- the probe stores a timestamp/counter; `performTransaction` treats direct as valid only if a frame arrived
  within, say, two refresh periods, otherwise it copies the surface;
- add a `RN_PRESENT_DIRECT_OFF` (or reuse a flag) that the plug-in sends when it disables/abandons direct
  scanout, and clear `gpuDirect` on mode change and on `rn_scanout_image`/`rn_vk_image_copy` failure;
- on `clientClose`, clear it as today.

**Double-check.** Verified the set path (`kern_probe.cpp:2272-2274`), the trust path
(`kern_accel2.cpp:587-594`), and that the only clears are the connection close and the initial state. Searched
for other writers of `gpuDirect`: only `clientClose` clears it. So the latch is real, not reset elsewhere.

## 4. Direct passes write a separate VRAM image, but `syncToSurface` writes the texture's own (stale) staging back to the IOSurface (S1)

**Where:** `RedNaviMTL/compute.m:1438-1445` (target substitution), `RedNaviMTL/compute.m:1518-1525`
(`scanSrc`), `RedNaviMTL/compute.m:333-350` (`syncToSurface`), `RedNaviMTL/compute.m:1677`
(`surfWritten` write-back).

**What the code does.** For a pass into a CoreDisplay scanout surface, `runRenderPass` replaces the target
image with the per-IOSurface card copy:

```c
tg[i] = (rn_vk_target){rp->tex[i]->img, ...};
if (rt->displayScan && rp->tex[i]->pf == rt->pf && rn_scanout_image(vk, rt->w, rt->h)) {
    rn_vk_image *vr = rn_display_vram(vk, rt);
    if (vr) { tg[i].image = vr; direct = YES; }
}
```

The pass writes `vr`, not `rp->tex[i]->img`. After the pass, `commitBody` writes every stored surface back:

```c
if (st != MTLCommandBufferStatusError) for (RedNaviMTLTexture *t in surfWritten) [t syncToSurface];
```

and `syncToSurface` copies from `rn_vk_image_staging(img)` — the *texture's* image, not `vr` — into the
IOSurface. For a zero-copy texture `syncToSurface` returns immediately (`if (!surf || zeroCopy ...) return;`),
so the IOSurface memory is never written at all in the direct path. For a copied texture the staging contains
whatever was last uploaded, not the frame that was just rendered into `vr`.

**Why it produces the symptoms.** Any consumer of the IOSurface sees the old content: WindowServer's damage
tracking, another process, a screenshot, or `performTransaction` after `gpuDirect` is cleared. The screen may
look right for a few frames while direct works, then show stale regions, trails, or a black band when the
direct path stops. It also means the “IOSurface texture always re-read” fix from `587de1a` re-reads the old
surface into the image, not the direct result.

**Fix.** After a direct pass, make the IOSurface agree with the card copy: either (a) render directly into the
zero-copy image when the surface is imported, and use `vr` only as a shadow; or (b) copy `vr` back into
`img`/the zero-copy image at the end of the command buffer before releasing it, then update the surface (or
mark it zero-copy and skip the CPU copy). Do not call `syncToSurface` for a texture whose pixels were written
to a different image; track which image is authoritative.

**Double-check.** Confirmed the target substitution at `compute.m:1440-1445`, the write-back set at
`compute.m:1590` (`note(rp->tex[i], rp->store[i])`) and `compute.m:1677`, and that `syncToSurface` reads
`img` (`compute.m:338`). Also confirmed the zero-copy early return (`compute.m:334`). This is a real
authority mismatch, not just a missed optimisation.

## 5. Texture views do not inherit buffer/client-storage/plane fields, and `note()` skips them (S1)

**Where:** `RedNaviMTL/compute.m:381-401` (`viewWithPixelFormat:swizzle:`), `RedNaviMTL/compute.m:1582-1586`
(`note` in `commitBody`).

**What the code does.** A view is built with a new `RedNaviMTLTexture` but only these fields are copied:

```c
t->img = v; t->dev = dev; t->pf = newPf; t->w = w; t->h = h; t->d = d; t->ttype = ttype; t->alen = alen;
t->texel = texel; t->usage_ = usage_; t->parent = parent ? parent : self;
t->zeroCopy = ...; t->displayScan = ...; t->surf = ...; t->swz = ...;
```

`srcBuf`, `srcOff`, `srcBpr`, `srcRaw`, `srcBase`, `srcLen`, `srcDealloc` and `surfPlane` are not copied.
`note()` in `commitBody` drops any texture that has none of `surf`, `srcBuf`, `srcRaw`:

```c
if (!t || (!t->surf && !t->srcBuf && !t->srcRaw)) return;
```

So a view of a buffer-backed/client-memory texture is never added to `surfUsed`, and `syncFromBuffer` never
runs for it. A view of a planar IOSurface has `surfPlane == 0` even when the parent shows plane 1, so
`syncFromSurface`/`syncToSurface` copy the wrong plane.

**Why it produces the symptoms.** Client-storage textures are exactly how CoreAnimation and several UI
clients pack small images (glyph atlases, icon bitmaps) into pooled `MTLBuffer`s. If such a texture is viewed
with another pixel format (very common for alpha-only/greyscale glyph data), the view's GPU image is never
refreshed from the buffer: it keeps the zeroes from creation or the previous frame. Icons disappear, glyphs
are missing or stretched/wrong width. For planar surfaces, the wrong plane is sampled, which gives the
green/orange gradients and colour-fringed corners.

**Fix.** Copy all storage fields in `viewWithPixelFormat:swizzle:` (retain `srcBuf`, copy `srcDealloc`, copy
`surfPlane`), and make `note()` resolve the root/parent before deciding whether a texture needs a sync. A view
shares the root's host copy; syncing the root once is enough.

**Double-check.** Verified the field list in the view constructor and the exact `note()` condition. Also
checked that `viewWithPixelFormat:` is used by `newTextureViewWithPixelFormat:` and its variants
(`compute.m:403-411`), so normal Metal API calls reach it. No other code copies the missing fields later.

## 6. The implementation supports 16 bindings, but the device advertises 31 buffers and 128 textures (S1)

**Where:** `RedNaviMTL/compute.m:581-585` (`RNDispatch` arrays), `compute.m:699-703` (`RNDraw` arrays),
`compute.m:783-787`/`1132-1136` (encoder arrays), `compute.m:915-920`, `937-946` (`setBuffer`/`setTexture`
cap at 16), `compute.m:544-570` (`rn_bind_args` tests `m < 16`), `compute.m:2388-2391` (limits table),
`rn_vk.c:735`, `rn_vk.c:816-818` (vertex buffers/attributes cap at 16).

**What the code does.** Every Metal-side binding array is 16 entries, and every setter silently ignores an
index ≥ 16:

```c
- (void)setBuffer:... atIndex:(NSUInteger)index { if (index >= 16) return; ... }
- (void)setTexture:... atIndex:(NSUInteger)index { if (index < 16) texs[index] = ...; }
```

`rn_bind_args` then reports “no buffer bound at index N” or “no texture bound” for those slots. At the same
time the device answers, in the limits table, `maxVertexBuffers = 31`, `maxFragmentBuffers = 31`,
`maxComputeBuffers = 31`, `maxFragmentTextures = 128`, `maxComputeTextures = 128`,
`maxVertexAttributes = 31`. The vertex path is worse: `rn_vk_render_create` rejects `nvlayout > 16` and
`binding >= 16`, and `vlbind` is 16 entries (`rn_vk.c:735`, `816-821`).

**Why it produces the symptoms.** Metal clients size their binding indices from the advertised limits. A
pipeline with a buffer at index 16–30 is legal by the device's own answer; here the binding is dropped, the
shader reads a stale/zero descriptor, or the whole command buffer fails with “no buffer bound”. That directly
explains missing icons, missing text runs, and geometry that is drawn with the wrong vertex data (“text wider”,
misplaced UI). A whole failed command buffer explains frames that never update (trails).

**Fix.** Either raise the internal arrays to the advertised maxima (31 buffers/attributes, 128 textures is
large but can be a sparse map) and validate indices against them, or lower the advertised limits to 16 so
clients adapt. The former is preferable for WindowServer, which may use high indices in its compositor
pipelines. Add a one-time log when an index is rejected.

**Double-check.** Counted the arrays at the three encoder classes and `RNDispatch`/`RNDraw`; checked the two
`rn_bind_args` branches; checked the limits table values. There is no other path that stores indices ≥ 16
(e.g. `setTextures:withRange:` calls the same per-index setter). So the mismatch is real.

## 7. IOSurface-backed textures are accepted with the wrong pixel format or a too-small row pitch (S1/S2)

**Where:** `RedNaviMTL/compute.m:290-317` (`initWithDevice:descriptor:iosurface:plane:`),
`compute.m:319-332` (`syncFromSurface`), `compute.m:333-350` (`syncToSurface`).

**What the code does.** Creation validates size and bytes-per-element only:

```c
const size_t sw = ... IOSurfaceGetWidthOfPlane(...), sh = ...;
const size_t sb = ... IOSurfaceGetBytesPerElementOfPlane(...);
if (w > sw || h > sh || texel != sb) { ... return nil; }
```

It never compares `IOSurfaceGetPixelFormat(io)` with `desc.pixelFormat`, and never checks
`IOSurfaceGetBytesPerRowOfPlane(io) >= w * texel`. The sync loops then do:

```c
for (NSUInteger y = 0; y < h; y++) memcpy(dst + y * w * texel, src + y * pitch, w * texel);
```

**Why it produces the symptoms.** `BGRA8Unorm` and `RGBA8Unorm` both have 4 bytes per element, so a BGRA
surface can be bound to an RGBA descriptor (or the reverse). The shader then reads red and blue swapped;
combined with gradients and blur passes this is exactly the green/orange checkerboard pattern in window
corners. If the surface's row pitch is smaller than `w * texel` (or the surface is planar with different
geometry), the row copy reads across row boundaries: adjacent rows bleed into each other, producing repeating
gradient/checker patterns and “text wider” artefacts. The zero-copy path is protected by
`rn_vk_image_from_host`'s row-pitch check (it falls back to copies), but the copied path has no such guard.

**Fix.** Validate `IOSurfaceGetPixelFormat` against the Metal format (accept only the explicit pairs, e.g.
`'BGRA'` ↔ `BGRA8Unorm`, `'RGBA'` ↔ `RGBA8Unorm`, plus the sRGB variants); validate
`bpr >= w*texel` and `bpr % texel == 0`; validate plane geometry. If they do not match, either convert
explicitly or refuse the texture with a clear log. Do not silently bind a mismatched format.

**Double-check.** Verified the creation checks and both sync loops; confirmed there is no format compare
anywhere in the IOSurface path; confirmed `IOSurfaceGetBytesPerElement` is the only pitch-related quantity
used. The zero-copy fallback check is real, but it only protects the `rn_vk_image_from_host` branch.

## 8. Surface synchronization copies the whole surface per texture, ignores damage, and shares mutable staging (S2/S3)

**Where:** `RedNaviMTL/compute.m:319-332`, `333-350`; `compute.m:1581-1603`; `compute.m:1677`.

**What the code does.** Every IOSurface-backed texture in a command buffer is synchronized with nested loops
over the whole width and height (1920×1080 or larger), once before the command buffer and once after. The
only lock is `IOSurfaceLock`; the staging buffer is the image's single host copy, shared by all views of the
same root (`rn_vk.c:456-457` copies the `stage` pointer). The loop ignores the transaction's dirty rectangle,
the texture's `storeAction` region, and any per-surface damage information. Two textures over the same
IOSurface (different views, different planes, or a root plus a view) each copy the full surface in and out.

**Why it produces the symptoms.** This is both correctness and performance:

- A later texture's `syncFromSurface` overwrites the staging that an earlier texture's `replaceRegion` or
  upload had just written; a later `syncToSurface` writes that stale staging back to the surface. The result
  is lost updates and torn frames — random pixels and trails.
- A dirty-rectangle compositor update is turned into a full-surface copy. At 60 Hz with several surfaces this
  is tens of megabytes per frame of CPU work; the compositor thread spends its budget copying instead of
  drawing, which is the lag. It also widens the window in which the CPU copy and the display engine run
  concurrently.
- Because the copy is CPU-side and the surface may be written by the GPU/another process, the frame is not
  atomic: the display pipe can copy a half-updated surface (black band at the top, tearing).

**Fix.** Track damage per surface (the display transaction already carries dirty bits/options; see finding 11)
and copy only that region. Serialize per root image, not per texture object. Do not sync a view separately
from its root. Prefer a GPU copy into the card image (or zero-copy) over CPU row loops. At minimum, add a
generation/seed check and a per-root mutex; `surfSeed` is currently only written, never used to skip work
(`compute.m:251`, `330`, `349`).

**Double-check.** Verified the loops and the shared `stage` pointer in `rn_vk_image_view`. Confirmed
`surfSeed` is assigned in both sync functions but never read anywhere else (grep for `surfSeed`). Confirmed
`commitBody` iterates `surfUsed` as an ordered set of texture objects, so a root and its view are distinct
entries.

## 9. `replaceRegion:`/`getBytes:` bypass command-buffer ordering, lose uploads to `syncFromBuffer`, and do not validate `bytesPerRow` (S1/S2)

**Where:** `RedNaviMTL/compute.m:437-461`; `compute.m:351-357` (`syncFromBuffer`); `compute.m:1603`.

**What the code does.** `replaceRegion:` writes directly into the image's staging buffer, marks the image
host-written, and immediately calls `syncToSurface`. `getBytes:` calls `syncFromSurface` and reads the
staging. The next `commitBody` calls, for every used texture:

```c
for (RedNaviMTLTexture *t in surfUsed) { [t syncFromSurface]; [t syncFromBuffer]; }
```

For a texture with `srcBuf`/`srcRaw` (client storage), `syncFromBuffer` overwrites the staging from the
buffer/client memory, so the `replaceRegion:` data is lost before any GPU work. For an IOSurface texture,
`syncFromSurface` overwrites the staging from the surface, so `replaceRegion:` is lost unless the immediate
`syncToSurface` happened to race correctly. The loops also use `bytesPerRow` without checking
`bpr >= region.size.width * texel` (overread) and use `bytesPerImage` without checking it against the region
(overread); `level`/`slice` are simply rejected with a log instead of returning an error.

**Why it produces the symptoms.** UI clients use `replaceRegion:` for small updates (glyph atlas tiles, icon
bitmaps, cursor shapes). Losing those updates gives missing icons, blank glyphs, and stale text; an overread
gives random pixels and gradient garbage in the affected tile. Because `getBytes:` can read the surface
instead of the GPU result, clients that read back a rendered texture get stale data.

**Fix.** Make the upload path explicit: `replaceRegion:` should mark a pending CPU upload for the image and
`commitBody` should apply it after `syncFromBuffer` (or skip `syncFromBuffer` for that texture this frame).
Validate `bpr`/`bpi` before touching memory. Implement level/slice for the formats that have them, or return a
proper error so the caller can fall back. Do not mix CPU writes to staging with a later full-surface sync
without a generation counter.

**Double-check.** Verified the order of operations in `commitBody` (`syncFromSurface` then `syncFromBuffer`,
line 1603), the `replaceRegion:` body, and the `syncFromBuffer` overwrite. Confirmed `bytesPerRow` is used
directly in the `memcpy` without a lower-bound check. `level`/`slice` are checked and logged, but the method
returns `void`, so the caller cannot tell.

## 10. `images_in`/`images_out` change image state while recording, so a failed submit poisons the image (S1/S2)

**Where:** `RedNaviMTL/rn_vk.c:582-600` (`images_in`), `603-614` (`images_out`), `615-641` (readback),
`643-685` (`dispatch_regions`), `926-1099` (`rn_vk_render_pass`), `869-907` (`clear_targets`).

**What the code does.** `images_in` clears `r->dirty` and sets `r->initialized = 1` while the command buffer
is being recorded, before `vkQueueSubmit` and before the fence wait. `images_out` sets `r->gpudirty = 0`
while recording. If anything later fails — `AllocateCommandBuffers`, `BeginCommandBuffer`,
`CreateFence`, `QueueSubmit`, or the 8 s fence timeout — the function returns an error, but the image state
has already been changed as if the transfer had happened.

**Why it produces the symptoms.** A transient failure (out of memory, a lost Vulkan device, a timeout during
a mode change) marks the host copy as “already uploaded” and the GPU copy as “already read back”. Every
subsequent frame then skips the upload/readback and samples stale or zero memory. From the user's point of
view a region stops updating: icons disappear, trails remain, random pixels persist, and only a process
restart (which recreates the textures) clears it. This matches the “intermittent” character of the symptoms.

**Fix.** Record state changes and apply them only after the fence confirms completion; on failure, restore
the old values or mark the image invalid (forcing a full re-upload). A small per-submission transaction
(`dirty_before`, `initialized_before`) is enough. `images_out` should also only clear `gpudirty` after the
copy is known to have executed.

**Double-check.** Verified that both helpers mutate the root (`im->base ? im->base : im`) during recording,
that every caller has failure paths after calling them, and that no caller rolls the flags back. The same
pattern exists in the compute, render-pass and clear paths, so this is systemic.

## 11. `performTransaction` ignores the dirty bits and options, drops the resource-only case, and reports success when it copies nothing (S1/S2)

**Where:** `RedNaviAccel2/kern_accel2.cpp:569-603` (`validateTransaction`, `isTransactionComplete`, the head of
`performTransaction`).

**What the code does.** `validateTransaction` returns 0 unconditionally and `isTransactionComplete` returns `true`
unconditionally, so the family considers every transaction valid and complete the moment it is handed over. In
`performTransaction`, `getTransactionDirtyBits()` and `getTransactionOptions()` are read only for the first three
transactions (inside an `if (n <= 3)` logging block, line 577); the values never influence what is copied. The
plane lookup asks for both `getPlaneIOSurface(0, 0)` and `getPlaneResource(0, 0)`, but if the surface is null the
function returns 0 (“success”) and never looks at the resource (line 584). If the surface format, width, height or
pitch do not match the scanout exactly (BGRA and same geometry, lines 599-603), it logs and again returns 0.

**Why it produces the symptoms.** A transaction is the display refresh. The dirty bits say which plane/region
changed; ignoring them means every transaction is treated as “copy the whole 1920×1080 surface”, while the
resource-only transactions (no IOSurface, just an `IOAccelResource2`) are silently dropped. WindowServer sees
`isTransactionComplete == true` and `kIOReturnSuccess`, assumes the frame is on screen, and reuses the surface or
frees the resource. The next transaction therefore composites on top of content that was never displayed: stale
regions, the “random pixels left on the screen” and the trailing during animation. The format/size mismatch case is
worse: the frame is dropped but the caller is told it was shown, so the picture freezes on the previous frame until
something else forces a full redraw. Since the mismatch test is exact (BGRA, equal width/height), one unusual
surface (e.g. a scaled animation frame, a 10-bit format, or a different pitch) makes the drop intermittent, which is
the character of the reported glitches.

**Fix.** Make `validateTransaction` reject transactions this pipe cannot serve (wrong format/size) with a real error
instead of returning success; make `isTransactionComplete` reflect whether the copy actually happened. Honour the
dirty bits (skip the copy when nothing changed; copy only the damaged rows when the API exposes them). Implement the
resource-only path (copy from the resource’s memory descriptor) or return `kIOReturnUnsupported`. Log the options
once, not only for the first three transactions.

**Double-check.** Verified that the dirty bits/options are used only inside the `n <= 3` block, that `resP` is
logged but never used, and that every failure path returns 0. Confirmed the 0x42475241 (‘BGRA’) exact test and the
`sp < sw * 4` pitch check.

## 12. Every transaction re-maps the surface and copies ~8 MB on the CPU while holding a global lock; the descriptor is never released (S2)

**Where:** `RedNaviAccel2/kern_accel2.cpp:604-629` (`performTransaction`), `558-568` (`scanoutCPU`).

**What the code does.** For each transaction the function calls `surf->getMemoryDescriptor()` (line 604) and never
releases the returned descriptor. It then calls `md->prepare()`, maps it read-only (`md->map(kIOMapReadOnly)`),
copies `width * 4` bytes per row for all `height` rows with `memcpy` into the write-combined scanout mapping
(lines 609-614), and calls `md->complete()`/`src->release()`. The whole thing runs under `gXferLock`. The scanout
mapping itself is cached by `scanoutCPU`, but the surface-side mapping is created and torn down every frame. The
only ordering primitive after the copy is `__sync_synchronize()` (line 615).

**Why it produces the symptoms.** A 1920×1080 BGRA frame is 8.3 MB. At 60 Hz that is ~500 MB/s of CPU `memcpy`
plus two page-table walks and an `IOMemoryDescriptor` map/unmap per frame, all serialised by one mutex shared with
the display pipe. During a minimise/maximise animation WindowServer asks for a transaction on every display link
tick; the copy cannot keep up, so frames arrive late and the animation stutters. If the copy is still running when
the surface is reused, the tail of the frame is a mix of old and new content — exactly the trails and random pixels.
The descriptor returned by `getMemoryDescriptor()` is not released: if it is a +1 reference (the usual IOKit
convention for `get*` methods that create a descriptor over a surface) this leaks one descriptor per refresh, and
after minutes of animation the kernel is under memory pressure. `kIOMapReadOnly` on a WC source is also the wrong
cache mode for a bulk read: reads from WC memory are uncached-but-combined for writes only, so every source byte is
a separate PCIe read unless the CPU combines them.

**Fix.** Keep one long-lived map of the scanout and one of the surface (invalidate it on `initFramebufferResource`
and on `destroyFramebufferResource`), or better, do the copy on the GPU (the bridge already has `rn_bridge_present`
and the Metal plug-in can blit). Copy only the dirty region. Use `kIOMapInhibitCache` or a DMA engine for the
source. Use `OSSynchronizeIO()`/`IODelay` after the WC stores, not a compiler barrier, because the display engine can
read the memory before the WC buffers drain. Release the descriptor if it is owned.

**Double-check.** Verified the per-frame `prepare`/`map`/`release`/`complete` sequence and that `md` is never
released. Verified the mutex scope and the single `__sync_synchronize()` (which on x86 is `mfence`; the WC flush
primitive is `sfence`, and `OSSynchronizeIO` is the IOKit wrapper). The 8.3 MB/frame figure is arithmetic from the
default 1920×1080 mode.

## 13. The scanout-memory ring, the retained framebuffer and the scanout map are never released (S3/S4)

**Where:** `RedNaviAccel2/kern_accel2.cpp:292-294`, `522-549` (`initFramebufferResource`,
`destroyFramebufferResource`), `45`/`908` (`gFb`), `555-568` (`gScanMap`).

**What the code does.** `initFramebufferResource` allocates a `RedNaviScanoutMemory` with `OSTypeAlloc` and stores
its pointer in `gScanout[gScanoutNext]`, a 64-entry ring. When the ring wraps, the slot is overwritten without
releasing the object. `destroyFramebufferResource` only logs and forwards to the family; it does not remove the
memory from the ring. `gFb` is `retain()`ed in `createDisplayPipe` and never released. `gScanMap` is held for the
process lifetime and there is no `stop()`/`free()` override in `RedNaviAccel2` that releases it. `isScanoutMap`
reads words `2..34` of an arbitrary object to find the pointer, i.e. it reads up to 280 bytes from a map object
whose size may be smaller.

**Why it matters.** The comment itself says the display set-up rebuilds the pipe “35 times in 10 minutes”. Every
rebuild leaks one `RedNaviScanoutMemory`; over a session that is thousands of objects, each potentially with a
size field and a resource reference, so the kernel slowly loses memory and the registry fills with stale
resources. The stale pointers in `gScanout` also make `isScanoutMap` match an *old* memory object if a new object
happens to be allocated at the same address, misclassifying a non-scanout map. Reading beyond the object in
`isScanoutMap` is a latent out-of-bounds read. These do not directly paint wrong pixels, but they make the pipe
unreliable after a while (a rebuild that no longer becomes active, transactions that fail), which shows up as a
frozen or partially updated screen.

**Fix.** Store the memory in an `OSSet`/`OSDictionary` keyed by the resource; remove and release it in
`destroyFramebufferResource`; release `gFb` and `gScanMap` in a proper `stop()`. Do not scan raw object words: keep
the association from map to memory when the map is created.

**Double-check.** Verified there is no `RedNaviAccel2::stop`/`free` that touches `gScanMap`/`gFb`, that the ring
overwrites without release, and that `destroyFramebufferResource` does not touch `gScanout`.

## 14. Diagnostics allocate and rewrite registry properties on every attribute call, and the attribute table is racy (S3)

**Where:** `RedNaviFB/kern_fb.cpp:67-73` (`note`), `75-106` (`publishDiag`), `304-315` (`record`), and every
`setAttribute`/`getAttribute`/`...ForConnection` (lines 398-470).

**What the code does.** Every attribute entry point first calls `note()`, which calls `publishDiag()`. `publishDiag`
reads 16 pixels through an uncached framebuffer mapping (`kIOMapInhibitCache`, line 288), allocates six `OSData`
objects, an `OSNumber`, an `OSDictionary`, and calls `setProperty("fb-diag", d)`. Every path also calls `record()`,
which linearly scans up to 48 entries, allocates an `OSData` of the whole 48-entry table, and calls
`setProperty("attr-table", d)`. `note()` writes `attrLog`/`attrRaw`/`attrPos` without a lock, and `record()` reserves
a slot with `OSIncrementAtomic` but fills `kind`/`sel` afterwards, so a concurrent publication can serialize a
partially initialised entry. `publishDiag` reads `attrRaw`/`attrLog` while another thread writes them.

**Why it produces the symptoms.** `setProperty` takes the registry and I/O Kit locks and wakes registry observers.
WindowServer calls framebuffer attributes on its composition path (aperture, connection enable, colour mode, …), so
this work is *inside* the frame. On a machine where the display path is already marginal, the extra locking and the
16 uncached reads every call add latency and jitter, and the registry churn can block the WindowServer thread long
enough to miss a vblank — the “animations lag” and “glitches”. The torn attribute table is less visible but makes the
diagnostics wrong exactly when they are needed. Since these properties are for debugging, the cost is paid by the
product for no user benefit.

**Fix.** Gate `publishDiag`/`record` behind a boot arg or a rate limit (e.g. at most once per second, on the
diagnostic timer only). Build the property from a snapshot taken under the same lock that writes it, or use a
per-thread buffer. Do not read framebuffer pixels on attribute calls; sample them on the 1 s timer. Never allocate
inside the attribute fast path.

**Double-check.** Verified that all four attribute entry points call `note()` before doing any work, that
`note()` calls `publishDiag()`, and that `record()` is called on every path including the override returns.
Confirmed `diagMap` is mapped `kIOMapInhibitCache`.

## 15. Vertical blank support is off unless a boot arg is set, has only four slots, and is not tied to the mode’s timing (S2/S3)

**Where:** `RedNaviFB/kern_fb.cpp:523-546` (`enableController`), `549-587` (`registerForInterruptType`,
`getVBLTime`, `unregisterInterrupt`, `setInterruptState`), `686-708` (`setDisplayMode`).

**What the code does.** The VBL timer is created only when `fbExp() & 16` is set (line 523); otherwise
`registerForInterruptType` forwards to `IOFramebuffer`, whose base implementation does not know this hardware, and
`getVBLTime` forwards to the base as well. When enabled, there are exactly four registration slots (lines 558-567);
the fifth registration returns `kIOReturnNoResources`. The timer is free-running: it computes a period from
`fbRefresh` and wakes at `vblNext + period`, but it is not synchronised to the display engine’s actual vertical
blank, and `setDisplayMode` updates `fbRefresh` without recomputing `vblNext`/`vblPeriod`. `vblLast`/`vblPeriod`
are written from the timer thread and read from `getVBLTime` with no lock.

**Why it produces the symptoms.** WindowServer’s display link paces on `getVBLTime` and on the VBL interrupt. With
the default (bit 4 off), there is no real VBL: the display link falls back to a timer or to the base’s answer, so
presentations are not aligned to the display’s refresh. The result is tearing and the impression that animations
“lag” because the compositor is pacing at the wrong time. When the mode changes, the timer keeps the old period
until the next tick, so the first frames after a mode change are paced wrong (visible as a jump or a black band on
the first frame). Only four slots means the family’s own handler and several clients compete; if more than four are
registered, the later ones silently get no VBL, and one of them may be the one WindowServer depends on. The
unsynchronised `vblLast`/`vblPeriod` can return a stale period or a time in the past.

**Fix.** Make VBL a first-class feature (on by default once the hardware path is trusted); drive it from the
display engine’s vblank interrupt or a timer phase-locked to the mode; recompute `vblNext`/`vblPeriod` in
`setDisplayMode`; use an `IOLock` or atomics for the published time/period; support more than four registrations
(or chain to the family’s handler as the reference driver does).

**Double-check.** Verified the bit-4 guard on both the timer creation and every VBL method, the four-slot array and
the `kIOReturnNoResources` return, and that `setDisplayMode` only assigns `fbRefresh`. Confirmed the timer callback
copies the registration array but not `vblLast`/`vblPeriod` under a lock.

## 16. A mode change does not invalidate the direct-scanout image cache, the bridge’s `display_info`, or the pipe’s scanout resource (S1/S2)

**Where:** `RedNaviFB/kern_fb.cpp:686-708` (`setDisplayMode`), `RedNaviMTL/compute.m:1309-1326`
(`rn_scanout_image`), `mesa-bridge/src/ac_apple_iokit.c:376` and `757-767` (`display_info`), and
`RedNaviAccel2/kern_accel2.cpp:522-544` (`initFramebufferResource`).

**What the code does.** `setDisplayMode` reprograms the probe and updates `fbWidth`/`fbHeight`/`fbRefresh`/
`fbPitch`, then remaps only `diagMap`. It does not tell the Metal plug-in or the Vulkan bridge that the geometry
changed. `rn_scanout_image` caches the scanout image and its `sw`/`sh` with `dispatch_once`, so after a mode change
it keeps returning the old image and, for any new size, returns NULL forever (`w == sw && h == sh` fails). The
bridge sets `display_info` once when the device is created (`ac_apple_iokit.c:376`) and never updates it, so
`rn_bridge_scanout_info` keeps reporting the old width/height/pitch/surface count. `initFramebufferResource` builds
the pipe’s `RedNaviScanoutMemory` from the pitch/height that were current when the pipe was created.

**Why it produces the symptoms.** After any resolution or refresh change, direct scanout silently stops working
(the geometry no longer matches), and the code falls back to the CPU copy path, which is slow and, per finding 12,
can drop frames. Meanwhile the swapchain’s surface capabilities are built from the stale `display_info`, so
WindowServer allocates surfaces of the wrong size for the new mode; the display engine scans out the new mode’s
geometry while the compositor writes the old one, which produces the black band at the top, the wrong pitch
(streaked image) and, when the sizes disagree, the “random” content at the edges. If the probe’s scanout physical
offset is the same but the pitch changed, the CPU copy path writes rows at the old pitch into the new framebuffer.

**Fix.** On `setDisplayMode`, publish the new geometry to the plug-in and the bridge (a platform function or a
property the bridge re-reads), reset `rn_scanout_image` (replace the `dispatch_once` cache with a checked
generation counter), refresh `display_info`, and recreate or update the pipe’s scanout resource. Do not derive
persistent caches from a one-shot `dispatch_once` when the value is mode-dependent.

**Double-check.** Verified `setDisplayMode` touches only `diagMap`, that `rn_scanout_image` is `dispatch_once` and
returns NULL on a size mismatch, that `display_info` is assigned only in device creation, and that
`initFramebufferResource` reads the provider’s pitch/height once.

## 17. Zero-copy host images are imported without host coherence and without flushing or invalidating, so the GPU can sample stale surface memory (S1/S2)

**Where:** `RedNaviMTL/rn_vk.c:469-514` (`rn_vk_image_from_host`, especially the memory-type search at
`496-500`), `582-600` (`images_in`), `RedNaviMTL/compute.m:305-315` (zero-copy texture creation).

**What the code does.** The host-pointer import picks the first memory type that is `HOST_VISIBLE`
(`rn_vk.c:496-500`); it does not require `HOST_COHERENT`. The IOSurface’s pixels are written by the CPU, by
WindowServer and by other processes, and read by the GPU as a sampled image or a render target. There is no
`vkFlushMappedMemoryRanges` before the GPU reads and no `vkInvalidateMappedMemoryRanges` after the GPU writes. When
`images_in` first uses a host image it transitions it from `PREINITIALIZED` to `GENERAL` with
`srcAccessMask = 0` (`rn_vk.c:589`), i.e. it does not declare the host write that produced the contents. After that
first use, `dirty` is never set for a zero-copy surface: `syncFromSurface` returns early for `zeroCopy`
(`compute.m:320`), so no later host write is ever made visible.

**Why it produces the symptoms.** On non-coherent memory the GPU may read what the cache holds, not what the CPU
just wrote: recently changed regions of a surface sample old pixels, which is the “random pixels left on the
screen” and the stale icons. The missing host-write dependency means the transition itself is undefined; a driver
that reorders the upload past the shader read can sample uninitialised memory, which on RADV with a tiled surface
is the green/orange checkerboard pattern in window corners (the compiler’s tiled swizzle interpreted as colour).
Because the failure depends on cache state and timing, it is intermittent.

**Fix.** Require `HOST_COHERENT` for host imports, or call `vkFlushMappedMemoryRanges`/`vkInvalidateMappedMemoryRanges`
(or the external-memory equivalents) around every use. In `images_in`, use `VK_ACCESS_HOST_WRITE_BIT` and
`VK_PIPELINE_STAGE_HOST_BIT` as the source for a `PREINITIALIZED`/host-written image, and synchronise subsequent
CPU writes with a real barrier. Treat the IOSurface’s ownership as: CPU writes → flush → GPU; GPU writes → barrier →
invalidate → CPU.

**Double-check.** Verified the memory-type predicate does not include `HOST_COHERENT`, that no flush/invalidate
entry point is called anywhere in `rn_vk.c`, and that `syncFromSurface` early-returns for `zeroCopy` before
`rn_vk_image_host_wrote`. Confirmed the `srcAccessMask = 0` in the first-use barrier.

## 18. `rn_scanout_image` rejects any pitch that is not exactly `width * 4`, disabling direct scanout on most non-multiple-of-64 widths (S2)

**Where:** `RedNaviMTL/compute.m:1309-1326` (`rn_scanout_image`), `RedNaviFB/kern_fb.cpp:50`
(`pitchBytes`), `RedNavi/kern_probe.cpp:1327`/`1088` (pitch published as `rn_pitch_pixels(w) * 4`).

**What the code does.** The scanout pitch published to the bridge is `rn_pitch_pixels(width) * 4`, and
`rn_pitch_pixels` rounds the width up to a multiple of 64 pixels. `rn_scanout_image` accepts the surface only if
`bp == bw * 4` (line 1318). For any mode whose width is not a multiple of 64 — 1366, 1440, 1512, 1680, 2560×1600
on some timings, 3440, many scaled modes — the real pitch is larger than `bw * 4`, so the check fails, the function
logs “scanout surface 0 not available” and returns NULL. Direct scanout is then permanently disabled for that mode.

**Why it produces the symptoms.** With direct scanout off, every CoreDisplay pass renders into system-memory
surfaces and is copied back (the very path the direct path was added to avoid: the comment cites 3-5 ms per
compositor pass). Animations lag and trail, and the extra copies are where the stale/partial frames come from. This
explains why the symptoms are worse on some displays/modes than others and why they appear after a mode change
(see finding 16).

**Fix.** Accept `bp >= bw * 4` and pass the real pitch to the image (the host import already takes the pitch; the
linear image’s `rowPitch` must equal it, which is the correct check). If RADV cannot make a linear image with a
padded pitch, fall back to staging in a pitch-sized buffer rather than disabling direct scanout entirely.

**Double-check.** Verified the exact `bp != bw * 4` test, that the probe publishes `selPitch * 4` with
`selPitch = rn_pitch_pixels(hactive)`, and that `pitchBytes` in the framebuffer rounds to 64 pixels. For 1920 and
2560 the check passes; for 1366 (rounded to 1408) it fails.

## 19. The “finished frame” decision is a fragment-function-name heuristic, and only one scanout source per command buffer is kept (S1/S2)

**Where:** `RedNaviMTL/compute.m:1516-1523` (`runRenderPass`), `892-893` (`scanSrc`/`directPasses`),
`1686-1690` (`commitBody`).

**What the code does.** A direct pass is considered a finished frame unless the *first* draw’s fragment function
name starts with `"ColorFill"` (`[d0->pso->names[1] hasPrefix:@"ColorFill"]`). When it is considered finished, the
single field `scanSrc` is overwritten with the pass’s display-VRAM image, and `commitBody` copies that one image to
the scanout at the end. `directPasses` is incremented but never read.

**Why it produces the symptoms.** The name comes from the translated AIR function; it is not a contract. A real
frame whose first draw happens to be a fill (or whose fill is inlined/renamed) is misclassified as a clear and not
presented, and a clear-only pass with a different name is presented as a frame — both produce stale or half-drawn
frames during animation. More importantly, a command buffer that renders to more than one display surface (a
double-buffered ring, or an overlay drawn in a second pass) keeps only the *last* `scanSrc`; the earlier finished
surface is never copied to the scanout, so the screen shows the wrong surface until the next frame catches up,
which is exactly the “black band”/trail pattern on alternating frames.

**Fix.** Decide “finished frame” from the pass’s target being a display surface and from its store action and
viewport coverage, not from a function name; keep a list of `(surface, pass)` pairs per command buffer and copy
each to the scanout (or present each, if the pipe supports per-surface flips). Remove the unused `directPasses` or
use it to validate the decision.

**Double-check.** Verified the heuristic uses only `draws[0]` and the fragment name prefix, that `scanSrc` is a
single pointer overwritten on each qualifying pass, and that `directPasses` has no reader. Confirmed the final copy
uses only `scanSrc`.

## 20. The per-IOSurface card copy is seeded from the scanout (not the surface), keyed without format or plane, and evicted without reference tracking (S1/S2)

**Where:** `RedNaviMTL/compute.m:1328-1348` (`rn_display_vram_get`), `1350-1354`
(`rn_display_vram_peek`), `1439-1445`/`1522` (use as render target and scanout source).

**What the code does.** `rn_display_vram_get` caches one device-local image per IOSurface in a dictionary keyed by
`(IOSurfaceID << 32) | (w << 16) | h` (line 1333); the key ignores the pixel format and the plane. A newly created
image is “seeded” by `rn_vk_image_copy(vk, shown, img)` from `rn_scanout_image(vk, w, h)` — the front scanout — and
not from the IOSurface’s own contents (lines 1342-1343). The cache holds at most 16 entries; when it overflows it
destroys the oldest image (`rn_vk_image_destroy`) and removes it from the dictionary (line 1339), with no check
that the image is still referenced by an in-flight command buffer. Access is serialised by `@synchronized` on the
class, a process-wide lock.

**Why it produces the symptoms.** CoreDisplay allocates a ring of scanout surfaces and draws only the damaged
region into the next one, relying on the previous contents. Starting a new ring surface from the *current front
buffer* is a guess: if the surface was last used two or more frames ago, or if the front buffer is mid-flip, the
seed is wrong. The unseeded regions are the “random pixels” and the “green/orange checkerboards” (uninitialised
VRAM interpreted as BGRA). Ignoring the format in the key means two textures over the same surface with different
formats (e.g. sRGB vs UNORM views, or a 10-bit variant) share one image and one is interpreted with the other’s
format: colours and gradients are wrong. Ignoring the plane means the luma and chroma planes of a biplanar surface
share one image. Destroying an entry after 16 while a command buffer still holds `scanSrc` is a use-after-free from
the GPU’s point of view: the image’s memory can be reused before the copy runs, which corrupts the frame. The
`@synchronized` class lock also serialises every direct-scanout lookup across all threads, adding jitter.

**Fix.** Seed a new ring image from the IOSurface’s actual contents (the zero-copy or copy path already has them),
and treat an unseeded image as “clear to black” rather than “whatever VRAM holds”. Include the pixel format and
plane in the key. Keep a reference count or a generation/epoch per image and defer destruction until the command
buffer that uses it completes. Keep the cache large enough for the ring (the probe publishes `fb_surfaces`), and
avoid a global lock on the composition path.

**Double-check.** Verified the key expression omits format/plane, that the seed comes from `rn_scanout_image`, that
eviction calls `rn_vk_image_destroy` unconditionally, and that `rn_vk_image_create3d` zeroes the staging buffer and
marks `dirty`, so an unseeded optimal-tiling image starts black after the first upload but a mid-frame eviction can
still reuse it. Confirmed `rn_display_vram_peek` uses the same cache and returns a raw pointer outside any lifetime
guarantee.

## 21. `rn_vk_image_copy` requires bit-identical formats and leaves the destination's CPU-visible state stale (S1)

**Where:** `RedNaviMTL/rn_vk.c:1166-1200` (`rn_vk_image_copy`), `rn_vk.c:582-600` (`images_in`),
`RedNaviMTL/compute.m:1686-1689` (scanout copy at the end of `commitBody`), `compute.m:1342-1343`
(seed of a new direct-scanout image), `rn_vk.c:517-520` (`rn_vk_image_staging`).

**What the code does.** The copy is refused unless width, height and the *Vulkan* format are identical:

```c
if (!src || !dst || src->w != dst->w || src->h != dst->h || src->fmt != dst->fmt) return 6;
```

It then calls `images_in(vk, cb, both, 2)`, which for the destination may transition it from UNDEFINED and, if
`dirty`, uploads the destination's staging buffer *before* the copy. After `vkCmdCopyImage` the function does not
touch `dst->dirty`, `dst->initialized` or `dst->gpudirty`. Because `images_in` has already cleared `dirty` and set
`initialized`, a later `rn_vk_image_staging(dst)` sees `gpudirty == 0` and returns the staging buffer as it was
*before* the copy (usually zeroes). The copy is only correct for a host-memory destination, where staging is the
destination itself.

**Why it produces the symptoms.** `commitBody` copies the finished frame from `scanSrc` (a device-local image whose
format is `rn_vk_format_of(rt->pf)`) into the single scanout image, which is always created as
`MTLFormatBGRA8Unorm` (`compute.m:1317-1321`). If CoreDisplay's last pass renders into a
`BGRA8Unorm_sRGB`, `RGBA8Unorm_sRGB`, `RGB10A2Unorm` or `BGR10A2Unorm` surface, the formats differ, the copy
returns 6, `rn_frame_done()` is never called, and the display pipe keeps scanning out the previous contents. That is
the intermittent black band at the top (the part of the panel that was never updated), the pixel trails after
minimise/maximise (the panel is one or more frames behind the compositor), and the stale lighter/darker rectangles.
The same mismatch makes `rn_display_vram_get`'s seeding copy fail, leaving a brand-new ring image at its zeroed
contents: the black/green/orange checkerboards. The silent success of `commitBody` (it never checks the copy's
result before proceeding to present) hides all of this.

**Fix.** Copy (or blit, for format conversion) through a format-compatible path: create the scanout/display image in
the surface's format, or use `vkCmdBlitImage` between formats in the same compatibility class, and use a
`vkCmdCopyImage` only when the formats are identical. Make the copy update the destination state explicitly
(`gpudirty = 1` when a host read will be needed, or copy device→staging inside the same command buffer), and return
the failure to `commitBody` so the command buffer is marked failed and `rn_frame_done()` is not skipped silently.
Also stop requiring exact width/height in the seed path: handle the ring surface and the scanout being different
sizes by a scaled blit or by not seeding at all.

**Double-check.** Verified the exact equality test at `rn_vk.c:1169`, that `images_in` clears `dirty` and sets
`initialized` at `rn_vk.c:588-599`, that no `gpudirty` assignment follows `CmdCopyImage`, and that the scanout
image's format is hard-coded to BGRA8 at `compute.m:1317`. Verified that `commitBody` ignores the copy's return
value except as the condition for `rn_frame_done()`, so the command buffer still reports `Completed`.

## 22. Sampler capabilities are advertised but comparison, anisotropy, reduction and mirror-clamp are not implemented (S2)

**Where:** `RedNaviMTL/compute.m:2364-2367` (capability table), `RedNaviMTL/rn_vk.h:49-56`
(`rn_vk_sampler_desc`), `RedNaviMTL/rn_vk.c:230-252` (`sampler_info`), `compute.m:482-489`
(`rn_address_of`), `compute.m:493-515` (`rn_new_sampler_state`).

**What the code does.** The device answers `supportsSamplerCompareFunction`, `supportsSamplerReductionMode`,
`supportsMirrorClampToEdgeSamplerMode` and `supportsShaderMinLODClamp` with YES. The descriptor that reaches
Vulkan has only min/mag/mip filters, three address modes, border colour, unnormalised coordinates and LOD clamps.
`sampler_info` never sets `compareEnable`, `compareOp`, `anisotropyEnable`, `maxAnisotropy` or `reductionMode`, so
Vulkan's defaults apply: no comparison, 1× anisotropy, weighted-average reduction. `MTLSamplerDescriptor`'s
`compareFunction`, `maxAnisotropy`, `reductionMode` and `supportArgumentBuffers` are never read.
`MTLSamplerAddressModeMirrorClampToEdge` is mapped to `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE`, which is not the
same operation: Metal mirrors the edge texel, clamp repeats it.

**Why it produces the symptoms.** CoreAnimation, CoreImage and application shaders choose code paths from these
flags. A comparison sampler (shadow/PCF, depth-of-field, some blur kernels) silently becomes a plain sampler that
returns the sampled value instead of the comparison result: shadows and masks become uniformly dark or bright,
which is one source of the flat grey rectangles in the menu bar. Anisotropic filtering requested for scaled text,
icons and gradients is dropped to 1×, producing the shimmering and the “text drawn wider” look when a glyph atlas is
sampled at a non-integer scale. Mirror-clamp used for repeating decorative backgrounds and tiled layers samples the
wrong edge row/column. Reduction-mode samplers (min/max filters used for downsampling and for some blur passes)
return an average instead of a min/max, changing the brightness of blurred regions.

**Fix.** Extend `rn_vk_sampler_desc` with compare op, anisotropy enable/max, reduction mode and a distinct
mirror-clamp address enum; fill `VkSamplerCreateInfo` accordingly (`compareEnable/compareOp`,
`anisotropyEnable/maxAnisotropy`, `reductionMode`, `VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE`). Read the
corresponding `MTLSamplerDescriptor` properties in `rn_new_sampler_state`. Until then, answer NO for the features
that are not implemented so clients do not select those paths.

**Double-check.** Confirmed the flags are unconditional YES at `compute.m:2365-2367`, that the descriptor struct has
no compare/anisotropy/reduction fields (`rn_vk.h:49-56`), that `sampler_info` sets none of them (`rn_vk.c:230-252`),
and that `MirrorClampToEdge` shares the `ClampToEdge` case at `compute.m:482`. Confirmed `rn_new_sampler_state`
copies only filters, address, pixel coordinates, border and LOD (`compute.m:493-510`).

## 23. The capability table advertises features whose implementations are nil or ignored (S2)

**Where:** `RedNaviMTL/compute.m:2357-2367` (capabilities), `compute.m:417-420` (shared/remote textures),
`compute.m:1185` (visibility result), `compute.m:1157-1161` (vertex buffers), `compute.m:2387-2391` (limits),
`rn_vk.c:816-821` (16 vertex layouts/attributes), `compute.m:404-411` (views).

**What the code does.** The device says YES to `supportsSharedTextureHandles`, `supportsCountingOcclusionQuery`,
`supportsDynamicAttributeStride`, `supportsTextureOutOfBoundsReads`, `supportsRelaxedTextureViewRequirements` and
`supportsLinearTextureFromSharedBuffer`. In reality:

- `newSharedTextureHandle` logs and returns nil; `newRemoteTextureViewForDevice:` returns nil;
  `remoteStorageTexture` returns nil (`compute.m:417-420`). A texture cannot be shared with another process or
  device at all.
- `setVisibilityResultMode:` logs and ignores any non-disabled mode (`compute.m:1185`); no render-pass descriptor
  visibility buffer is ever read or written.
- There is no `setVertexBuffer:offset:attributeStride:atIndex:` implementation; layouts take the static stride from
  the vertex descriptor (`compute.m:1157`, `rn_vk.c:816-821`).
- Texture reads outside the image are not clamped or zeroed by the plug-in; Metal promises zero, Vulkan leaves the
  result undefined unless robust image access happens to be enabled (RADV does not enable it for this usage).
- “Relaxed texture view requirements” is false: a view must keep the texel size and be the whole of a 2D single-level
  texture (`rn_vk.c:449-450`, `compute.m:404-411`).

The limits table also advertises 31 vertex buffers/attributes and 128 fragment/compute textures while every binding
array in the plug-in is 16 entries (`compute.m:581-585`, `699-703`, `783-787`, `1132-1136`; `rn_vk.c:816-821`
rejects more than 16).

**Why it produces the symptoms.** A client that believes shared handles work creates an IOSurface/remote-texture
path and then draws a nil texture: the layer is missing entirely (disappearing icons), or the previous contents
remain (pixel trails). Occlusion queries silently return whatever was in the visibility buffer, so clients that cull
by query result draw the wrong subset of geometry, producing missing or doubled content and the coloured corner
checkerboards when a buffer is interpreted as pixels. A wrong dynamic stride misaddresses every vertex; the result
is warped geometry or garbage triangles. Out-of-bounds reads returning VRAM instead of zero explain isolated random
pixels. Advertising 31 attributes while only 16 exist makes the *pipeline creation* fail late (or the last 16
attributes be dropped), so some UI shaders render with the wrong vertex layout — texts wider, icons offset.

**Fix.** Either implement these features or make the capability queries answer NO, and make the limit answers match
the arrays actually used (16 unless the arrays are enlarged). Add the `attributeStride` setter and thread the stride
through `rn_vk_vbuf`/pipeline creation. For out-of-bounds reads, either enable robust image access on the device or
clamp in the translator. Fail pipeline creation explicitly when a descriptor needs more than the supported count.

**Double-check.** Verified each capability string and its YES at `compute.m:2364-2367`, the nil returns at
`417-420`, the ignore at `1185`, the missing dynamic-stride setter (only `setVertexBuffer:offset:atIndex:` exists at
`1157`), the 16-entry arrays, and the `nvlayout > 16 || nvattr > 16` rejection in `rn_vk.c:816-817`. Verified the
published limits at `2387-2391`.

## 24. Completion status, completion handlers, scanout copy and present happen in the wrong order (S1)

**Where:** `RedNaviMTL/compute.m:1680-1691` (`commitBody` tail), `compute.m:1390` (`enqueue`),
`compute.m:1393` (`waitUntilScheduled`), `compute.m:1396-1405` (`addScheduledHandler:`/`addCompletedHandler:`),
`compute.m:1692-1696` (`waitUntilCompleted`).

**What the code does.** At the end of `commitBody`:

```c
if (st != MTLCommandBufferStatusError) st = MTLCommandBufferStatusCompleted;
... run completed handlers ...
if (scanSrc && st != MTLCommandBufferStatusError) { copy into scanout; rn_frame_done(); }
for (id<MTLDrawable> dr in presents) [dr present];
dispatch_semaphore_signal(done);
```

So the buffer is declared `Completed` and the completion handlers run *before* the pixels reach the scanout and
before the drawable is presented. `addCompletedHandler:` added from another thread during that window sees
`st == Completed` and invokes the block immediately (`compute.m:1400-1405`). `waitUntilCompleted` then waits with
`DISPATCH_TIME_NOW`, i.e. it does not wait at all; `waitUntilScheduled` is empty; `enqueue` only changes the status
to `Enqueued` without submitting anything.

**Why it produces the symptoms.** This is the frame-pacing and lifetime hole behind the trails and the black band.
The completion handler is where CoreAnimation releases or reuses the drawable's texture and where WindowServer
signals that the frame has been consumed. Running it before the scanout copy lets the next frame overwrite the
IOSurface (and `scanSrc`) while the copy is still pending; the panel then shows a mixture of two frames, i.e. the
leading edge of the top band and the ghost pixels left behind animations. Because `waitUntilCompleted` returns
immediately, any client on another thread that calls it to read back or to recycle a surface observes half-written
memory. Announcing `Completed` even when the scanout copy fails means no frame is presented at all, yet the client
believes it was, which also matches the “sometimes nothing updates” behaviour.

**Fix.** Order the tail as: perform the scanout copy and presents, then run the completed handlers, then set
`Completed`, then signal the completion semaphore. If the copy or present fails, set the buffer to `Error` (or at
least do not report `Completed`). Implement `waitUntilScheduled` with its own semaphore signalled when the commit
starts, implement `enqueue` as the actual submission point (or document it as a no-op only if commit remains
synchronous), and make `waitUntilCompleted` block on the semaphore without `DISPATCH_TIME_NOW`. Do not run
completion handlers early when they are added after the status change — they should run only after the tail above.

**Double-check.** Verified the order at `1680-1691` (status first, handlers next, copy next, present last), the
`DISPATCH_TIME_NOW` at `1695`, the empty `waitUntilScheduled` at `1393`, and the immediate-invocation branches in
both handler setters at `1396-1405`. Confirmed the drawable's present is the last operation in the commit.

## 25. Event waits busy-poll for up to 10 s while holding the single commit lock; listeners spawn polling threads (S1/S2)

**Where:** `RedNaviMTL/compute.m:846-870` (`waitFor:`, `notifyListener:`, `shm_open`),
`compute.m:1653-1660` (the wait inside `commitBody`), `compute.m:1563-1569` (the commit lock).

**What the code does.** `commit` takes one process-wide `NSRecursiveLock` around the whole synchronous
`commitBody`. Inside it, an `encodeWaitForEvent:` record calls `[event waitFor:value timeoutMS:10000]`, which loops
`usleep(200)` until the shared value reaches the target or ten seconds pass. The event is a 4 KB `shm_open` region
created with mode `0600`; `notifyListener:` starts a detached global-queue thread per notification that polls for up
to 60 seconds before invoking the block.

**Why it produces the symptoms.** CoreAnimation legitimately encodes waits for events that a *different* command
buffer signals, often on another thread. That producer's `commit` needs the same lock, which the waiting thread is
holding: the wait can never be satisfied, so every such frame stalls for ten seconds and then fails. This is the
intermittent long freeze followed by a stale/black frame. Even when the producer is in the same command buffer, the
200 µs polling adds jitter to every composition pass. `notifyListener:` leaks a thread per call for up to a minute;
a compositor registering listeners per frame accumulates hundreds of threads, burning CPU and descheduling the
render thread — the lag and the irregular trails during minimise/maximise. `shm_open` with mode 0600 also means an
event created by a user process cannot be opened by WindowServer (different uid), so the shared-event path is not
actually cross-process at all; the wait times out and the frame is dropped.

**Fix.** Do not wait while holding the commit lock: either submit the wait to the GPU (VkSemaphore timeline or a
VkFence per event value) or release the lock, wait on a condition variable, and re-acquire. Replace the polling
loops with `pthread_cond_t`/dispatch sources or a timeline semaphore. Create the shared region with permissions
that allow the peer process (or pass a file descriptor through the handle) and validate the shared handle instead of
silently timing out. `notifyListener:` should register a dispatch source on the shared value, not allocate a
thread.

**Double-check.** Confirmed the lock scope (`compute.m:1563-1569`), that `commitBody` calls `waitFor:` at
`1655-1660`, the 10 s timeout, the `usleep(200)` loop at `849-856`, the 60 s polling thread at `858-864`, and the
`0600` mode at `838`. Also confirmed that `notifyListener:` copies the block and never cancels the polling thread
when the listener is deallocated.

## 26. The lazy render-pipeline cache is written from two threads without synchronisation (S1)

**Where:** `RedNaviMTL/compute.m:659-668` (`byKey[5 * 6]`), `compute.m:677-691`
(`renderForPrimitive:cull:clockwise:`), `compute.m:1485` (called while drawing), `compute.m:2087` (called while
creating a pipeline state).

**What the code does.** Each `RedNaviMTLRenderPipelineState` owns a 30-entry cache of `rn_vk_render *` indexed by
primitive type, cull mode and winding. `renderForPrimitive:` tests `byKey[key]`, and if absent calls
`rn_vk_render_create` and stores the result, with no lock. It is called in two different contexts: from
`runRenderPass` while a command buffer is being committed (under the commit lock), and from
`newRenderPipelineStateWithDescriptor:` while the pipeline object is being built, which is not under any lock.
`dealloc` destroys every entry without coordinating with either context.

**Why it produces the symptoms.** Two threads can both see a null entry and create two Vulkan pipelines; one is
leaked, and the loser's draws use a pipeline that is never destroyed, which is only a leak. The dangerous case is a
torn or freed pointer: thread A reads the pointer and is preempted, thread B replaces/frees it (e.g. the state object
is released on another thread), and A binds freed Vulkan state. That is a driver crash or a corrupted draw, visible
as a burst of garbage/random pixels and a frame that never completes (black band). Because the cache is also
consulted for every draw, an inconsistent entry can bind the wrong blend/cull state, which is one way the coloured
corner checkerboards appear (triangles drawn with the wrong topology/culling). The pattern is timing-dependent,
which matches “every now and then”.

**Fix.** Protect the cache with a mutex (or build all 30 variants eagerly in the initialiser, which is cheap compared
with the compositor's lifetime), publish entries with an atomic pointer, and never destroy the cache while a commit
that references it can still be in flight. Reference-count `rn_vk_render` objects per command buffer if pipeline
states can be released concurrently with composition.

**Double-check.** Verified the unsynchronised null check/store at `677-691`, that `dealloc` iterates and destroys
without a lock at `668`, that `renderForPrimitive:` is called from `runRenderPass` at `1485`, and that
`newRenderPipelineStateWithDescriptor:` calls it at `2087` outside `commit`'s lock. Confirmed there is no
`@synchronized` or mutex around `byKey`.

## 27. The single Vulkan queue, command pool and render command buffer are used outside the commit lock (S1)

**Where:** `RedNaviMTL/rn_vk.c:25-39` (one `queue`, `pool`, `rcb`, `rfence` for the process),
`RedNaviMTL/rn_vk.c:517-520` (`rn_vk_image_staging` → `image_readback_now`), `rn_vk.c:625-646`
(`image_readback_now`), `rn_vk.c:1166-1200` (`rn_vk_image_copy`), `RedNaviMTL/compute.m:1563-1569`
(the only serialisation), `compute.m:1342-1343` (copy called from the display cache under a different lock).

**What the code does.** The plug-in keeps exactly one `VkQueue`, one `VkCommandPool` and one reusable render command
buffer per process. Only `commitBody` is serialised, by the recursive commit lock. But:

- `rn_vk_image_staging` calls `image_readback_now` whenever the image is `gpudirty`, and it is called from
  `getBytes:`, `syncFromSurface`, `syncToSurface`, the trace dump, the blit CPU paths and the PNG dump — at arbitrary
  times and threads, outside the commit lock. `image_readback_now` allocates a command buffer from `vk->pool`,
  submits to `vk->queue` and waits on its own fence.
- `rn_display_vram_get` calls `rn_vk_image_copy` under `@synchronized([RedNaviMTLTexture class])`, which is not the
  commit lock. `rn_vk_image_copy` likewise allocates from `vk->pool` and submits to `vk->queue`.
- `g_readback_vk` (`rn_vk.c:515`) is a plain global written by whichever render pass ran last and read by any thread
  that asks for staging.

Vulkan requires external synchronisation for a `VkQueue` and a `VkCommandPool`; using them from two threads is
undefined behaviour even when each submission has its own fence.

**Why it produces the symptoms.** A readback or a seed copy that overlaps a compositor commit can reset the same
command pool, submit two command buffers concurrently, or reuse the render command buffer while it is pending. The
driver may return `VK_ERROR_DEVICE_LOST`, mix the two submissions, or write the readback into the image while the
render pass is still using it. On screen this is exactly the family of symptoms: a frame that never reaches the
scanout (black band), random pixels from a torn readback, and checkerboards when an image is read while it is being
written. The `@synchronized` class lock makes it worse by serialising every direct-scanout lookup while still not
serialising against `commit`.

**Fix.** Introduce one mutex that guards every submission (`queue`, `pool`, command-buffer allocation, fence waits),
and take it in `commitBody`, `image_readback_now`, `rn_vk_image_copy`, `dispatch_regions`/`rn_vk_dispatch` and
`clear_targets`. Alternatively give each thread its own command pool and use a queue mutex only around
`QueueSubmit`, with fences owned by the submission. Remove the global `g_readback_vk`: the image should carry the
device it belongs to, or the call should receive it explicitly. Never read back an image that an in-flight command
buffer may still write; use a per-image fence or a deferred-destruction queue.

**Double-check.** Verified the single queue/pool/rcb/rfence in `struct rn_vk` at `25-39`, the lock scope in
`commit` at `1563-1569`, that `image_readback_now` allocates/submits/waits at `625-646` without any lock, that
`rn_vk_image_staging` can call it from any thread at `517-520`, and that `rn_display_vram_get` uses
`@synchronized` rather than the commit lock at `1331`. Confirmed `g_readback_vk` is not atomic and is set from the
render path.

## 28. Depth and stencil are not implemented, yet depth/stencil states and attachments are accepted (S2)

**Where:** `RedNaviMTL/compute.m:205-232` (`rn_vk_format_of`), `compute.m:718-730`
(`RedNaviMTLDepthStencilState`), `compute.m:1182-1193` (state setters), `compute.m:1425-1433`
(`renderCommandEncoderWithDescriptor:`), `compute.m:2040-2090` (render pipeline creation).

**What the code does.** `rn_vk_format_of` maps no depth or stencil format. A `MTLTextureDescriptor` for
`Depth32Float`, `Depth16Unorm`, `Depth32Float_Stencil8` or `Stencil8` therefore fails texture creation with “no
Vulkan format for it”, so the attachment cannot even be allocated. `renderCommandEncoderWithDescriptor:` returns
nil when there is no colour attachment (“depth-only passes are not implemented”), and logs and ignores depth and
stencil attachments when there is one. `setDepthStencilState:` is routed to `ignored:`, `setDepthBias:` is an empty
method, `setStencilReferenceValue:` is empty and `setDepthClipMode:` ignores clamp. Pipeline creation validates only
colour attachments and `rasterSampleCount`.

**Why it produces the symptoms.** WindowServer and CoreAnimation use stencil for window clipping, rounded-corner
masks, backdrop effects and some layer flattening; applications use depth for 3D content and for stencil-based
effects. Ignoring the stencil state means a mask pass does not clip anything: content paints outside the rounded
rectangle or over a neighbouring window, leaving the coloured checkerboards in the corners (the untouched corners
of a layer whose mask was supposed to clear them). A depth-tested scene draws in submission order instead of
front-to-back, so hidden surfaces win and visible ones disappear — “certain icons disappear” when a later, hidden
icon overwrites a visible one. A depth-only encoder returning nil makes the whole command buffer silently skip that
pass, so a mask or shadow layer is never rendered. The `depthClipMode` clamp being ignored also changes geometry at
the near plane for transformed layers.

**Fix.** Add the depth/stencil formats (with format-feature checks) and create depth images with
`VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`; extend `rn_vk_target` and the render pass with a depth/stencil
attachment and map `MTLDepthStencilDescriptor` (`depthCompareFunction`, `depthWriteEnabled`, stencil front/back
ops) to `VkPipelineDepthStencilStateCreateInfo`. Populate the depth attachment in
`renderCommandEncoderWithDescriptor:` instead of returning nil for depth-only passes. Implement
`setDepthBias`, `setStencilReferenceValue` and `setDepthClipMode` (or, if some are genuinely unsupported, fail the
pipeline at creation rather than silently drawing without them). Stencil-based clipping is the priority for
WindowServer correctness.

**Double-check.** Verified the absence of any depth/stencil case in `rn_vk_format_of` (`compute.m:205-232`), the
nil return for a pass with no colour attachment and the ignore log for depth/stencil attachments
(`1425-1433`), the `ignored:` routing at `1190`, the empty bias/reference methods at `1191-1193`, and that pipeline
creation only inspects colour attachments at `2040-2090`. Confirmed the depth-stencil state object stores nothing
but the device and label (`720-730`).

## 29. Clear values are not converted for sRGB views, 10-bit layouts or integer attachments (S2)

**Where:** `RedNaviMTL/rn_vk.c:859-867` (`clear_value`), `rn_vk.c:888-893` (`clear_targets`),
`rn_vk.c:1020-1028` (clears inside `rn_vk_render_pass`).

**What the code does.** `clear_value` copies the four floats and, when the view and the base image disagree about
BGRA-vs-RGBA ordering, swaps components 0 and 2. Its own comment says that sRGB views over UNORM images (or the
reverse) are not corrected. It also always writes `out->float32`, which is the wrong union member for integer
colour formats (`VK_FORMAT_R32_UINT`, `RGBA8_UINT`, etc.): Vulkan requires `uint32`/`int32` values there.
10-bit formats are not handled beyond the R/B swap, even though `BGR10A2` and `RGB10A2` do not order components the
same way as 8-bit BGRA.

**Why it produces the symptoms.** `vkCmdClearColorImage` converts the clear value using the *image's* format, not the
view's. If CoreDisplay or CoreAnimation clears through an `sRGB` view over a UNORM image (or the reverse), the clear
is encoded with one transfer function and sampled with the other: a mid-grey clear becomes visibly lighter or
darker. That is the most direct explanation of the lighter grey rectangles in the menu bar and of clears that do not
match the layer's background. For 10-bit surfaces the channel order is wrong, so a clear that should be neutral grey
comes out tinted, and alpha bits can be set incorrectly. Integer clears writing float bits produce essentially
random colours, visible as the green/orange checkerboards when an integer mask/atlas is cleared and then sampled.
The same values are used both in the no-draw path (`clear_targets`) and in the draw path, so both are affected.

**Fix.** Convert the clear value per destination format: linear→sRGB encoding for sRGB formats (and the reverse when
the view is UNORM over an sRGB base), the correct channel order/packing for 10-bit and BGR variants, and the
`uint32`/`int32` union member for integer formats. The cleanest solution is to perform the clear with a view that
matches the base image's format (or with the render pass's `loadOp = CLEAR`, which uses the attachment view and
therefore the format the pipeline expects), so no manual conversion is needed. Cover the case where the base and the
view have different transfer functions by clearing through a temporary base-format view and letting the sampler/view
do the conversion.

**Double-check.** Verified that `clear_value` only swaps R/B and explicitly documents the missing sRGB correction at
`859-867`, that it unconditionally writes `float32` at `861`, that `BGR10A2`/`RGB10A2` map to distinct Vulkan
formats without any conversion at `210-211`, and that both clear sites call it (`890`, `1024`). Confirmed render-pass
attachments use `loadOp = LOAD` and the clears are separate `vkCmdClearColorImage` calls (`795`, `1020-1028`), so
the image format really is used for conversion.

## 30. Texture creation and views reject arrays, cubes, MSAA, mipmaps and many pixel formats (S2)

**Where:** `RedNaviMTL/compute.m:269-280` (texture creation restrictions), `compute.m:205-232`
(format table), `compute.m:404-411` (view restrictions), `compute.m:424-426` (reported level/array counts),
`rn_vk.c:449-450` (view texel-size restriction), `rn_vk.c:816-821` (16 attributes/layouts).

**What the code does.** The constructor accepts only plain 2D, 3D and 1D/1D-array textures; it rejects 2D arrays,
cube/cube arrays, multisample textures and any array length other than 1 (except 1D arrays). A request for more than
one mip level only logs and the texture reports `mipmapLevelCount == 1`. Views are limited to the whole of a 2D
texture, level 0, slice 0, and must keep the same texel size. The format table omits many Metal formats:
`R8Snorm`, `RG8Snorm`, `RGBA8Snorm`, `RGB9E5Float`, `RG11B10Float`, `RGB565`, `BGR5A1`, `A1BGR5`, the 16-bit
signed-normalised formats, `RGB32Float`, the 16-bit integer formats, and every depth/stencil format. `usage_` is
stored but not used to validate creation.

**Why it produces the symptoms.** CoreAnimation and WindowServer use 2D arrays for glyph atlases, cursor images and
layered image stacks; a rejected array texture becomes a nil texture, so the glyph/cursor is not drawn (icons and
text disappearing) or the fallback path draws per-glyph quads with different metrics — the “some texts are wider”
effect. Cube maps and cube arrays are used by some window effects and by application content; they fail outright.
MSAA is used by WindowServer for some backdrop/quality paths; a multisample texture is rejected, and the resolve is
logged and ignored (`compute.m:1432`), leaving aliased edges. Mipmaps being silently truncated means a sampler that
asks for LOD > 0 gets the level-0 image magnified/minified with no mip chain, producing shimmer and the wide-text
look at small scales. Missing formats (notably 16-bit and snorm formats used for image buffers, and `RGB565` for
small icons) make `newTextureWithDescriptor:` return nil, which is the direct cause of missing icons and of layers
that draw as transparent holes. 1D-array gamma tables are special-cased as a 2D image with one row per slice, but a
2D-array view of them is impossible because views reject arrays.

**Fix.** Create array/cube textures as `VK_IMAGE_TYPE_2D` with `arrayLayers = arrayLength` and the matching
`VK_IMAGE_VIEW_TYPE_2D_ARRAY`/`CUBE`; implement mip levels with `mipLevels` and per-level staging upload; implement
MSAA with `samples` and a resolve attachment (or answer `rasterSampleCount` unsupported at pipeline creation, but do
not silently drop the resolve); extend `rn_vk_format_of` with the missing formats (checking
`optimalTilingFeatures`); allow views over a subrange and with a different `textureType` where Vulkan permits it.
Return nil at creation time only for genuinely unsupported combinations, and log the exact format/type so the client
can fall back deliberately instead of drawing a hole.

**Double-check.** Verified the rejection conditions at `compute.m:272-273`, the mipmap log and the reported count at
`276`/`424`, the view restrictions at `404-411`, the texel-size equality in `rn_vk_image_view` at `449-450`, and the
format table at `205-232` (no depth/stencil, no snorm, no RGB565/RGB9E5/RG11B10/16-bit integer). Confirmed the
multisample-resolve log at `1432` and that `usage_` is only stored, not validated.

## 31. The finished frame is copied into the live scanout surface without a flip or vertical-blank wait (S1)

**Where:** `RedNaviMTL/compute.m:1309-1326` (`rn_scanout_image`, always surface 0), `compute.m:1686-1690`
(the end-of-commit copy and `rn_frame_done`), `compute.m:1355-1360` (`rn_frame_done`),
`RedNavi/kern_probe.cpp:2272-2274` (`RN_PRESENT_FRAME_DONE` only sets `gpuDirect`), `RedNaviAccel2/kern_accel2.cpp:586-595`
(the pipe stops copying when that bit is set).

**What the code does.** `rn_scanout_image` maps display surface **0** only (`rn_bridge_scanout(0, ...)`) and caches
it. At the end of every command buffer `commitBody` does a full-frame `vkCmdCopyImage` from the per-surface VRAM copy
into that host-memory image, waits for the copy, and calls `rn_frame_done()`. `rn_frame_done` does not flip a page:
it calls `rn_bridge_present(0xFFFF0001, 0)`, which the kext interprets as “a desktop frame was rendered into the
scanout, do not copy” and only sets `gpuDirect = 1`. The display pipe continues scanning surface 0 while the copy is
overwriting it. The copy is also unconditional about which surface CoreDisplay actually rendered into: ring surfaces
1 and 2 are copied to surface 0.

**Why it produces the symptoms.** The display controller reads the same memory that the copy writes, with no vblank
synchronisation and no double buffering. The rows at the top of the panel are scanned before the copy reaches them
and show the previous frame, while the lower rows show the new one — the black band at the top and the upper/lower
tear line. During animations the mismatch moves every frame, which is perceived as trails. A slow or partial copy
leaves a horizontal band of uninitialised/old pixels; a copy that happens while the pipe is in the middle of a frame
produces the random pixels. Because surface 0 is hard-coded, any time CoreDisplay uses a different ring surface the
pipe never sees the new content at all. This is the strongest single explanation for the “banda nera in alto” and for
trails that persist after an animation ends.

**Fix.** Render into an off-screen surface and flip it with `rn_bridge_present(surface, wait)` at the next vertical
blank, or at minimum wait for vblank and copy into the surface that is not currently being scanned. Pass the real
surface index (from the IOSurface/transaction) to `rn_frame_done`, and only copy when that surface is the one the pipe
will scan. Double-buffer at least two of the ring surfaces and make `rn_scanout_image` a function of the surface, not
a single cached surface 0.

**Double-check.** Verified the `rn_bridge_scanout(0, ...)` call at `compute.m:1313`, the unconditional copy and
`rn_frame_done()` at `1686-1689`, that `rn_frame_done` only calls the magic present at `1355-1360`, and that
`RN_PRESENT_FRAME_DONE` only sets `gpuDirect` without calling `present()`/page flip at `kern_probe.cpp:2272-2274`.
Confirmed the pipe then skips its own copy at `kern_accel2.cpp:586-595`.

## 32. Blits are CPU memcpys over the staging buffer, ignore `bytesPerImage`/slices/3D and bypass the direct-scanout image (S2)

**Where:** `RedNaviMTL/compute.m:1060-1070` (`RNBlit` fields), `compute.m:980-1045` (`rn_run_blit`),
`compute.m:1075-1115` (encoder methods), `compute.m:1108-1115` (`generateMipmapsForTexture`,
`synchronizeResource`, `synchronizeTexture` no-ops).

**What the code does.** Every blit is executed at commit as a `memcpy` on `rn_vk_image_staging(t->img)`. The
`RNBlit` record stores only `sbpr`/`dbpr`; `sourceBytesPerImage` and `destinationBytesPerImage` are accepted and
dropped. `rn_run_blit` rejects `origin.z != 0`, `size.depth > 1`, and any slice/level other than the special 1D-array
row trick; texture→texture requires equal texel sizes and does no format conversion. `synchronizeResource`,
`synchronizeTexture`, `generateMipmapsForTexture`, `optimizeContentsForGPUAccess` are empty. For a display-scanout
texture the live pixels are in `rn_display_vram`, not in `t->img`, so a blit that reads such a texture reads the
stale staging copy, and a blit that writes it is overwritten by the next direct pass.

**Why it produces the symptoms.** CoreAnimation and WindowServer use blits for the small “client storage” uploads
(glyph atlases, cursor images, snapshot tiles) and for readbacks. Ignoring `bytesPerImage` corrupts the layout of
3D/array uploads, so tiles are shifted or repeated — visible as text drawn wider, duplicated glyphs, and random
pixels left behind after a window moves. The lack of format conversion makes a blit between an sRGB and a UNORM
texture copy raw values, changing colours of moved content. Because the copy is a CPU loop over a full image, each
blit stalls the composition thread; on a 4K surface this is milliseconds per blit, contributing to the animation lag.
On a display-scanout texture the stale source also produces trails/ghosting consistent with the earlier direct-scanout
findings.

**Fix.** Implement blits as GPU `vkCmdCopyBufferToImage`/`vkCmdCopyImage`/`vkCmdCopyImageToBuffer` commands recorded
into the same command buffer as the surrounding passes, with `bytesPerImage`, slices, levels and 3D extents honoured.
Implement format conversion with `vkCmdBlitImage` for compatible formats, and keep the CPU path only as a fallback for
tiny buffers. For display-scanout textures, use the `rn_display_vram` image as the source/destination (and mark state
accordingly) or flush the direct image back to staging before the blit.

**Double-check.** Verified `RNBlit` has no `bpi` member and the setters do not store it (`compute.m:1060-1070`,
`1075-1095`), that `rn_run_blit` uses only `bpr` and rejects `z`/depth (`984-1035`), that texture→texture requires
equal texel size and uses `memmove` (`1018-1032`), and that the three synchronisation/mipmap methods are empty
(`1108-1115`). Confirmed the blit uses `t->img` staging and never `rn_display_vram`.

## 33. A failed command buffer still runs its completion handlers and presents its drawables (S2)

**Where:** `RedNaviMTL/compute.m:1578-1580` (`if (!vk) [self fail:...]` without return), `compute.m:1636-1691`
(the record loop, the tail after a failure), `compute.m:1690` (presents are unconditional).

**What the code does.** `commitBody` calls `[self fail:@"RADV is not available"]` when `rn_vk_shared()` is null but
continues executing. The record loop breaks on the first error, leaving later passes and dispatches unrecorded. The
tail then still runs:

```c
if (st != MTLCommandBufferStatusError) st = MTLCommandBufferStatusCompleted;
... run completed handlers unconditionally ...
if (scanSrc && st != MTLCommandBufferStatusError) { copy ... }
for (id<MTLDrawable> dr in presents) [dr present];
```

So an errored buffer invokes its completion handlers (even though the frame is incomplete) and still presents every
drawable added with `presentDrawable:`. Earlier passes may already have committed their surfaces; later ones are
missing.

**Why it produces the symptoms.** Presenting a partially executed frame is exactly the “half of the screen updated,
the other half stale” pattern: the passes before the error wrote into their targets, the ones after it did not, and the
drawable is presented anyway. The black band at the top, the leftover pixels and the grey rectangles can all be the
visible result of a buffer that failed in the middle (for example because a sampler, a format or an event wait
failed) but was reported as presented. Completion handlers also release/recycle the drawable, so the client starts the
next frame on top of the partial one, compounding the trails.

**Fix.** Make `fail:` or the caller abort the tail: do not present when `st == Error`, do not run completion handlers
as if the frame were complete, and return immediately after the `!vk` failure (or guard every later use of `vk`).
Either roll back the surfaces already written (re-present the previous frame) or keep the previous scanout image
intact so the panel does not switch to a partial frame. Log the failing record and the reason once per buffer.

**Double-check.** Verified the missing `return` after `[self fail:@"RADV is not available"]` at `1578-1580`, that the
loop breaks on error at `1638`, that handlers run unconditionally at `1681-1683`, and that the present loop at `1690`
is outside any status check. Confirmed the scanout copy is guarded but the present is not.

## 34. The Vulkan device is opened once and never reopened; device loss or a failed first open is permanent, and destructors assume a non-null device (S2)

**Where:** `RedNaviMTL/compute.m:107-121` (`rn_vk_shared` with `dispatch_once`), `compute.m:124`
(`rn_compute_usable`), `RedNaviMTL/rn_vk.c:521-535` (`rn_vk_image_destroy`), `rn_vk.c:218-231`
(`rn_vk_buffer_destroy`), `compute.m:287`/`474`/`526`/`668` (destructors).

**What the code does.** `rn_vk_shared` opens RADV exactly once inside a `dispatch_once`; the result, including
`NULL`, is frozen for the life of the process. There is no path that closes and reopens the device after a
`VK_ERROR_DEVICE_LOST`, a driver reset or a failed initial open (for example the kext or the GPU not being ready when
WindowServer first asks for a Metal device). Every subsequent texture, buffer, pipeline and sampler then fails at
creation, but the device object still advertises `supportsFamily:`/`supportsFeatureSet:` YES and `isHeadless == NO`,
so WindowServer keeps selecting it. Destructors call `rn_vk_image_destroy(rn_vk_shared(), img)`,
`rn_vk_buffer_destroy(rn_vk_shared(), b)`, `rn_vk_sampler_destroy(...)`, `rn_vk_compute_destroy(...)` and
`rn_vk_render_destroy(...)` without checking the pointer; those functions dereference `vk->DestroyImageView`,
`vk->DestroyBuffer`, etc. Objects that outlive the device (or are torn down after a failed open) crash.

**Why it produces the symptoms.** A transient failure at startup produces a permanently black or frozen desktop
until logout, with WindowServer repeatedly creating nil textures (missing icons, no window content). A device loss
during a session produces a burst of failed command buffers; because the tails still present (finding 33), the screen
shows the last completed frame with partial updates — the black band, the trails, the grey rectangles. A crash in a
destructor during teardown explains the sporadic “WindowServer restarted” that resets the whole desktop.

**Fix.** Detect `VK_ERROR_DEVICE_LOST`/`VK_ERROR_OUT_OF_DEVICE_MEMORY` in the submission helpers and reopen the
device (destroy and recreate the `rn_vk`, re-create the images/buffers that survive, or fail all clients cleanly and
let WindowServer fall back). Do not cache a failed open forever: retry on the next device request with a backoff.
Make every destructor tolerate a null `rn_vk` (return after freeing the wrapper) and make `rn_vk_close` part of a
controlled teardown that first drains in-flight work.

**Double-check.** Verified the `dispatch_once` latch at `107-121`, that no call to a close/reopen exists, that
`rn_compute_usable` simply tests the cached pointer at `124`, and that the destroy functions dereference `vk`
unconditionally at `rn_vk.c:521-535` and `218-231`. Confirmed the destructors pass `rn_vk_shared()` directly at
`compute.m:287`, `474`, `526`, `668`.

## 35. Presentation timing, presented handlers and GPU timestamps are ignored (S2)

**Where:** `RedNaviMTL/compute.m:1406-1408` (`presentDrawable:` variants), `compute.m:1379-1380`
(`GPUStartTime`/`GPUEndTime`), `compute.m:1396-1405` (handler registration), `compute.m:1690` (`[dr present]`),
`compute.m:738-744` (protocol list; no `addPresentedHandler:` implementation).

**What the code does.** All three `presentDrawable:` forms collapse to “append the drawable and call `present` at the
end of the synchronous commit”. `presentationTime` and `afterMinimumDuration:` are discarded, `GPUStartTime` and
`GPUEndTime` return 0, and `addPresentedHandler:` is not implemented (the generic stub answers zero and never calls
the block). The present itself happens after the buffer has already been marked `Completed` and after its completion
handlers have run (finding 24).

**Why it produces the symptoms.** CoreAnimation uses presentation timestamps and presented handlers to decide when a
drawable is free and when the next frame should be submitted. Ignoring them means the client reuses a drawable as
soon as its completion handler runs — before `present` — and then draws the next animation step into a surface the
display is still showing. That is precisely how minimise/maximise leaves trails and how frames are dropped or shown
twice. `afterMinimumDuration:` is the API used to cap the frame rate for animated layers; dropping it makes
animations run at whatever rate the synchronous commit achieves, which is both slow and uneven. A presented handler
that never fires can leave a client waiting forever for a frame that will never be acknowledged.

**Fix.** Store the requested presentation time/minimum duration with each drawable, schedule the present at the
appropriate display-link callback, and call the presented handlers with the actual presentation time after the flip.
Report real GPU start/end timestamps (around the submission and fence wait) so clients can pace. Keep the drawable
retained until after the present and only then run the completion/recycling path.

**Double-check.** Verified all three present variants call the same method and ignore their arguments
(`1406-1408`), that `GPUStartTime`/`GPUEndTime` return 0 (`1379-1380`), that `addPresentedHandler:` does not exist in
the class and would be answered by the generic stub (`738-744`), and that `present` is the last statement of
`commitBody` (`1690`), after the status change.

## 36. `RN_PRESENT_FRAME_DONE` is a global, surface-less flag with per-connection ownership (S1)

**Where:** `RedNavi/kern_probe.cpp:2270-2284` (the magic present), `kern_probe.cpp:2076` (clear on close),
`kern_probe.cpp:434` (`gpuDirect` definition), `RedNaviAccel2/kern_accel2.cpp:586-595` (the pipe reads it),
`RedNaviMTL/compute.m:1355-1360` (`rn_frame_done`).

**What the code does.** Any client that calls `rn_bridge_present(0xFFFF0001, 0)` sets the probe-global
`gpuDirect = 1`. `performTransaction` reads the `rednavi-gpu-frames` property (the same global) and, if non-zero,
skips copying the IOSurface into the scanout for **every** transaction and every client. The flag carries no surface
index, no connection identity and no frame sequence. It is cleared only when the connection that set it closes
(`setDirect`), so two clients race: the second client's direct frames keep the flag set after the first closes, or the
first client's close clears it while the second is still rendering directly.

**Why it produces the symptoms.** If any application (not just WindowServer) uses the Metal plug-in and takes the
direct path, the display pipe stops copying for the whole GPU. Transactions from other clients then show stale
surfaces; when the direct client exits, the bit is cleared and the pipe starts copying surfaces that were never
updated in direct mode. The desktop can freeze while a game or a Metal app is running, then jump when it quits, and
the reverse (WindowServer direct, another client clearing) leaves the panel on an old frame — the black band/trails.
Because there is no surface id, a frame rendered into ring surface 1 can suppress the copy for a transaction that
targets surface 0.

**Fix.** Replace the global bit with per-surface/per-frame state: `RN_PRESENT_FRAME_DONE` should carry the surface
index and a sequence number, and `performTransaction` should skip only the copy for that surface/frame. Tie the state
to the connection (`setDirect` per client) with a reference count, clear it on disconnect and on timeout, and fall
back to the copy path when the direct client has not produced a frame for the current vblank.

**Double-check.** Verified the global assignment at `kern_probe.cpp:2272-2274`, the single clear at `2076`, that the
property returns the global at `1419`, and that `kern_accel2` reads the property once and returns 0 for *all*
transactions (`586-595`). Confirmed `rn_frame_done` sends only the magic value, no surface.

## 37. Texture views and non-exact-format sampler paths bypass the direct-scanout image and read stale staging (S1)

**Where:** `RedNaviMTL/compute.m:1350-1354` (`rn_display_vram_peek`), `compute.m:566` (use in `rn_bind_args`),
`compute.m:1440-1445` (redirect condition in `runRenderPass`), `compute.m:333-338` (`syncFromSurface`),
`compute.m:394-411` (view construction).

**What the code does.** The direct-scanout render pass writes into a per-IOSurface device image
(`rn_display_vram`). When a draw samples a scanout texture, `rn_bind_args` replaces the binding with that image only
if `rn_display_vram_peek` returns non-null; the helper requires `t->pf == rt->pf`. A texture view created with
`newTextureViewWithPixelFormat:` has a different `pf` (for example an `sRGB` view over a `BGRA8Unorm` base, or an
`RGBA8Unorm` view over `BGRA8`), so the peek returns NULL and the sampler is bound to `t->img`, whose staging buffer
was last filled by `syncFromSurface` from the IOSurface — not by the direct pass. The same condition
(`rp->tex[i]->pf == rt->pf`) also prevents a *view used as a render target* from being redirected to the display
image, so that pass writes into staging instead.

**Why it produces the symptoms.** Sampling the previous frame through a view is a normal thing for CoreDisplay and
CoreAnimation to do (sRGB views for colour management, swizzled views for masks). Those samples now return the
IOSurface contents from before the direct pass, i.e. one or more frames old, or zeroes when the surface is newly
allocated. The visible result is ghosting/trails during animations and wrong colours in composed regions — the same
pixels are correct when sampled directly and stale when sampled through a view. A pass that renders through a view
never reaches the display image, so its output is invisible until a later full copy, leaving rectangles and bands.

**Fix.** Make the direct-scanout image the canonical storage for an IOSurface/plane, independent of the view's
format, and create a format view of it for any compatible pixel format (the texel size is the same for sRGB/UNORM
pairs). Base the redirect and the peek on the parent surface/plane, not on `pf` equality. When a real format
conversion is needed, do an explicit conversion pass rather than falling back to staging.

**Double-check.** Verified the `pf` equality check in the peek at `1350-1354`, its use at `566`, the identical check
in the render redirect at `1442`, and that `syncFromSurface` writes `t->img` staging from the IOSurface
(`333-338`). Confirmed views keep the same image and texel size but a different `pf` (`394-401`,
`rn_vk.c:449-450`), so the peek fails while the image is valid.

## 38. Every pass, dispatch and copy is submitted and waited for individually, on the caller's thread (S2)

**Where:** `RedNaviMTL/rn_vk.c:1080-1098` (`rn_vk_render_pass` submit + `WaitForFences`), `rn_vk.c:668-690`
(`rn_vk_dispatch` submit + wait), `rn_vk.c:625-646` (`image_readback_now`), `rn_vk.c:1166-1200`
(`rn_vk_image_copy`), `RedNaviMTL/compute.m:1563-1569` (`commit` runs `commitBody` synchronously under one lock).

**What the code does.** Each render pass allocates a command buffer, records it, submits it to the single queue and
blocks in `WaitForFences(..., 8 s)`. Each compute dispatch does the same. Each readback and each image copy does the
same. `commit` itself runs `commitBody` on the calling thread and the whole thing is serialised by a process-wide
recursive lock. There is no batching of the records of one command buffer into one Vulkan submission and no overlap
of CPU encoding with GPU execution.

**Why it produces the symptoms.** A composition frame with several passes and dispatches pays the full submit/wait
round trip for each one, plus the fence wait at the end of each pass, on WindowServer's composition thread. The CPU
cannot prepare the next frame while the GPU draws the current one, so a frame that should take one GPU frame time
takes many, and the lock prevents any other thread from making progress. This is the direct cause of the “animations
lag” symptom, and the latency makes every partial-update bug above more visible (a stale surface stays on screen
longer). It also amplifies the event-wait deadlock (finding 25): the lock is held for the entire multi-pass sequence.

**Fix.** Record all passes, dispatches and blits of a command buffer into one (or a small ring of) Vulkan command
buffer(s), submit once at the end, and wait only when the client actually asks for completion (`waitUntilCompleted`,
a readback, or a surface hand-off). Use timeline semaphores/fences for cross-command-buffer dependencies instead of
blocking inside the commit. Double-buffer the scanout and the descriptor/command-buffer state so the CPU can encode
frame N+1 while the GPU executes frame N. Keep the lock only around queue submission and shared-state mutation, not
around the GPU wait.

**Double-check.** Verified the per-pass `QueueSubmit` + `WaitForFences` at `rn_vk.c:1084-1098`, the same in
`rn_vk_dispatch` at `668-690`, the same in `image_readback_now` and `rn_vk_image_copy`, and that `commit` calls
`commitBody` synchronously while holding the lock (`compute.m:1563-1569`). Confirmed there is no batching of
`records` into a single submission.

## 39. The display pipe's CPU copy and the plug-in's GPU writes can touch the same scanout surface concurrently (S1/S2)

**Where:** `RedNaviAccel2/kern_accel2.cpp:577-645` (`performTransaction`, `gXferLock`, `scanoutCPU`, the
`gpuDirect` check), `kern_accel2.cpp:586-595` (property read), `RedNavi/kern_probe.cpp:434` (`gpuDirect`),
`RedNaviMTL/compute.m:1686-1689` (end-of-frame copy), `mesa-bridge/src/ac_apple_iokit.c:726-753` (`rn_bridge_scanout`).

**What the code does.** When `gpuDirect` is 0, `performTransaction` locks `gXferLock`, maps the IOSurface read-only,
maps the scanout memory write-combined and `memcpy`s the whole surface, then issues `__sync_synchronize()` and
unlocks. The plug-in, on its own thread, may simultaneously be writing the same IOSurface (through `syncToSurface` or
a direct pass into `rn_display_vram`), copying the finished frame into the scanout image, or reading the surface for a
texture upload. `gXferLock` is private to the accelerator and is never taken by the plug-in. `gpuDirect` is read once
at the start of the transaction and can change immediately afterwards; the flag can also be cleared by another
connection (finding 36).

**Why it produces the symptoms.** The transaction can copy a surface while the plug-in is halfway through updating
it, producing a frame that mixes old and new pixels — exactly the random pixels and the band at the top. If the
transaction's copy happens after the plug-in's end-of-frame copy but before the next vblank, it overwrites the new
frame with the old IOSurface (the surface is not where the direct image writes), so the panel jumps back one frame:
the trails. `__sync_synchronize()` only orders the CPU's own stores for the write-combined mapping; it does not wait
for the GPU writes that produced the frame, nor for another core, so it does not make the copy safe against the
plug-in. The write-combined mapping itself is not coherent with the GPU's view of the same memory.

**Fix.** Introduce a single ownership protocol for each scanout surface: the plug-in publishes “frame N ready for
surface S” with a fence/sequence, the accelerator waits for that fence and copies only when it owns the surface, and
the plug-in never writes a surface that the pipe is currently copying. Disable the accelerator copy for a surface
while direct mode is active for that surface (not globally), and use a coherent mapping (or an explicit GPU→CPU
flush/invalidate) for any CPU access. Replace the volatile `gpuDirect` check with a locked/per-surface state machine.

**Double-check.** Verified the copy loop and `gXferLock` at `kern_accel2.cpp:600-640`, the single early `gpuDirect`
read at `586-595`, that the plug-in's end-of-frame copy (`compute.m:1686-1689`) takes no such lock, and that
`rn_bridge_scanout` maps the surface independently in `ac_apple_iokit.c:726-753`. Confirmed `__sync_synchronize()` is
only a compiler/CPU barrier, not a GPU fence.

## 40. 1D-array textures are stored as a 2D image with the array length as height, but slice addressing is inconsistent (S3)

**Where:** `RedNaviMTL/compute.m:269-280` (creation: `rows = is1d ? alen : desc.height`), `compute.m:419-426`
(`textureType`/`arrayLength`/`height`), `compute.m:437-446` (`replaceRegion` rejects `slice != 0`),
`compute.m:1075-1095` (blit maps a slice to `dorg.y`), `compute.m:450-455` (`getBytes` rejects `slice != 0`).

**What the code does.** A `MTLTextureType1DArray` is allocated as a 2D image whose height is the array length
(`rows = alen`), and `textureType` still reports `1DArray` with `arrayLength = alen`. `height` reports the row count,
i.e. the array length. `replaceRegion:`/`getBytes:` reject any non-zero `slice`, so a client cannot upload or read a
slice through those APIs, but the blit encoder maps `destinationSlice` to a row offset (`dorg.y += slice`) and
`sourceSlice` similarly, and only for 1D arrays. CoreDisplay's gamma tables are exactly R32Float 1D arrays (the
comment at `compute.m:240` says so).

**Why it produces the symptoms.** A client that treats the texture as a 1D array uses `replaceRegion:...slice:` or
`getBytes:...slice:` and gets nothing (the call is silently ignored), so the gamma table is never uploaded and the
display uses the previous/default values — a washed-out or wrong-colour desktop. A client that uses the row-based
form works, but a client that uses the blit slice form works only by accident of the row mapping. The mismatch also
makes `height` and `arrayLength` both report the array length, which confuses clients computing row pitches or
mip sizes, and the texture cannot be viewed as a 2D array (views reject arrays). The result is wrong colour
management and, for image atlases stored as 1D arrays, missing or shifted rows — another source of “texts wider” and
missing tiles.

**Fix.** Allocate 1D arrays as a real `VK_IMAGE_TYPE_1D` with `arrayLayers = alen` and a `1D_ARRAY` view, or keep the
2D packing but implement `slice` consistently in `replaceRegion`/`getBytes` (slice → row offset) and report `height`
as 1 for a 1D array. Make the blit and view paths agree on the addressing. Add a test that uploads a two-slice
R32Float 1D array through both the region API and a blit and reads it back.

**Double-check.** Verified `rows = is1d ? alen : desc.height` at `275`, that `textureType`/`arrayLength` report
1D-array semantics at `419-426`, that `replaceRegion` and `getBytes` reject `slice` at `437-446`/`450-455`, and that
the blit adds the slice to `y` at `1084-1095`. Confirmed the CoreDisplay gamma-table comment at `240`.

## 41. The small-buffer cache keeps mapped Vulkan memory for the life of the process and is never drained (S3)

**Where:** `RedNaviMTL/rn_vk.c:172-176` (`RN_BUF_CACHE_*`, `g_buf_cache`), `rn_vk.c:178-213` (`rn_vk_buffer_create`), `rn_vk.c:215-231`
(`rn_vk_buffer_destroy`), `rn_vk.c:156-164` (`rn_vk_close`), `RedNaviMTL/compute.m:163` (buffer dealloc path).

**What the code does.** Every buffer up to 64 KiB that is destroyed while still mapped is pushed into a
process-global `g_buf_cache[1024]` array and returned, instead of being freed
(`rn_vk_buffer_destroy`). Nothing ever removes an entry from that array: `rn_vk_buffer_create` pops one on an exact
size-class hit, but a miss leaves it there, and a cache-full event simply frees the new buffer. `rn_vk_close`
destroys the command pool and the `VkDevice` with the cache still populated, so the cached `VkBuffer` and
`VkDeviceMemory` handles are destroyed implicitly by `vkDestroyDevice` while `g_buf_cache` still holds pointers to
them. The buffers also stay permanently mapped (`VK_MEMORY_PROPERTY_HOST_VISIBLE|HOST_COHERENT`): the cache is the
only owner of those mappings, and each entry retains the kernel buffer object and GPU address the comment at
`rn_vk.c:168-171` says are expensive to recreate.

**Why it produces the symptoms.** The cache is bounded by count, not by bytes: 1024 entries of up to 64 KiB is up
to 64 MiB of permanently mapped, never-reclaimed device memory, on top of the live pools. WindowServer creates
thousands of small buffers per second (`setVertexBytes`/`setFragmentBytes` make one per draw). Under the memory
pressure that follows, the compositor and the GPU driver start to fail allocations, and a failed `vkAllocateMemory`
shows up as a dropped frame or a missing surface — the minimisation lag and the random tiles. Worse, after a device
loss or a future device reopen (finding 34) `g_buf_cache` still hands out buffers created on the destroyed device:
`rn_vk_buffer_create` returns the cached pointer without checking which `rn_vk` it belongs to, so the next draw binds
a `VkBuffer` that no longer exists. The cache also keeps the *contents* of freed buffers alive, so a buffer reused
after a client freed it is only safe because `rn_vk_buffer_create` zeroes it; no other state is reset.

**Fix.** Drain the cache in `rn_vk_close` (unmap and destroy each entry before `vkDestroyDevice`), key the cache by
`rn_vk*` or reset it on device open/close, and add a byte budget with LRU eviction rather than a fixed slot count.
Call `vkUnmapMemory` before `vkFreeMemory` for clarity (it is implicit, but it makes the lifetime explicit), and do
not cache buffers that were created for image staging or that carry a `userPtr`.

**Double-check.** Verified the caching branch and its early return at `217-223`, that the only removal from
`g_buf_cache` is the exact-size hit in `rn_vk_buffer_create` at `184-187`, that `rn_vk_close` (`156-164`) never touches
the cache, and that `rn_vk_shared` opens the device exactly once (`compute.m:107-123`), so the dangling-handle case is
latent today but becomes live the moment a reopen is added. `vkFreeMemory` does implicitly unmap, so this is about
retention and lifetime, not a spec violation.

## 42. Draw calls do not validate vertex/index ranges against the bound buffers (S2)

**Where:** `RedNaviMTL/rn_vk.c:1065-1076` (`rn_vk_render_pass`), `RedNaviMTL/rn_vk.h:92` (`rn_vk_vbuf` has only
buffer + offset), `RedNaviMTL/compute.m:1194-1203` (`drawPrimitives`), `compute.m:1229-1234`
(`drawIndexedPrimitives`), `compute.m:1509-1511` (index binding).

**What the code does.** The only validation is `vbf->offset >= vbf->buffer->size` for each vertex binding and
`dr->index_offset >= dr->index_buffer->size` for an indexed draw. Nothing checks:

- `offset + vertex_count * stride <= buffer->size` (and the per-instance variant);
- `index_offset + index_count * (index32 ? 4 : 2) <= buffer->size`;
- that `index_offset` is a multiple of the index size, or that vertex offsets satisfy the alignment the attribute
  formats require (Vulkan requires the offset to be aligned for the attribute type; Metal allows arbitrary offsets);
- that `first_vertex + vertex_count` and `base_vertex` do not overflow or leave the buffer.

The draw carries `vertex_count` and `first_vertex`, but the stride lives in the pipeline (`rn_vk_vlayout`), so
`rn_vk_render_pass` has everything it needs and still does not use it.

**Why it produces the symptoms.** A client is allowed to bind one large buffer and index anywhere in it; WindowServer
does this for glyph atlases and window geometry. If a client passes a range that is one vertex or one index too
long — or an offset that is not aligned for a 64-bit attribute — RADV reads past the buffer or at the wrong address.
That produces exactly the reported class of artifacts: a few random pixels, a glyph drawn from the wrong texel (text
wider or doubled), or a triangle whose vertices come from the next allocation (green/orange checkerboard corners).
The `offset >= size` check also accepts `offset == size`, which is an empty-but-valid range only if the count is
zero. Because these are validation failures, RADV may instead report a device loss and the frame is dropped.

**Fix.** Validate the full range in `rn_vk_render_pass` using `dr->pipe->vlayouts[...]`: for every declared binding
require `offset + (first_vertex / instance offset) + count * stride <= size`; for an indexed draw require
`index_offset % index_size == 0` and `index_offset + index_count * index_size <= size`; reject negative or
out-of-range `base_vertex` combinations. Return the existing `rc = 4`/`6` so the command buffer fails visibly rather
than drawing garbage. Keep a per-draw `stride` in `rn_vk_vbuf` if the pipeline can declare several layouts for one
binding.

**Double-check.** Verified the two range checks at `1066`/`1072`, that `rn_vk_vbuf` (`rn_vk.h:92`) carries no stride
or length, that `dr[k].index_buffer/index_offset/index32/base_vertex` are copied straight from the Metal call at
`1509-1511`, and that the error text for `rc == 4` already mentions “a buffer offset is not a multiple of the
storage buffer offset alignment, or beyond the buffer” (`compute.m:1546`), i.e. the intended check was never
implemented for draws.

## 43. `instanceCount == 0` is changed to 1, so a zero-instance draw still renders (S3)

**Where:** `RedNaviMTL/rn_vk.c:1074` and `rn_vk.c:1076` (`dr->instance_count ? dr->instance_count : 1`),
`RedNaviMTL/compute.m:1202` (recording).

**What the code does.** Both the indexed and the non-indexed path substitute `1` when `instance_count` is 0. In
Metal, `instanceCount == 0` means “draw nothing”; the draw must be a no-op. The substitution exists because
`RNDraw`'s `instanceCount` is a plain `unsigned` defaulting to 0, so a draw recorded without the instance count would
otherwise disappear — but every Metal entry point that can reach `runRenderPass` sets it explicitly (`1` for the
non-instanced variants, the argument for the instanced ones), so the fallback only masks real zero-instance draws.

**Why it produces the symptoms.** A zero-instance draw is a common way for an app to skip a batch without changing
its pipeline or buffers (e.g. an empty glyph run, a hidden layer with instanced quads). Drawing one instance instead
renders one quad at the first transform — a stray rectangle in the window, or a duplicate icon at the wrong place.
When the instance data is itself empty, the vertex shader reads the first instance slot only, which can be stale data
from an earlier frame: a random-coloured tile in a corner, or the “wider text” artifact when a glyph batch that
should have been skipped paints once with the previous glyph’s data.

**Fix.** Pass `instance_count` through unchanged; `vkCmdDraw(..., 0, ...)` and `vkCmdDrawIndexed(..., 0, ...)` are
legal and draw nothing. If the default matters for a draw recorded by a path that genuinely forgot to set it, add an
explicit `instanceCount = 1` at the recording site instead of in the backend.

**Double-check.** Verified both substitutions at `1074`/`1076`, that every `drawPrimitives:`/`drawIndexedPrimitives:`
overload sets `d->instanceCount` (`compute.m:1202` and the chained overloads at `1236-1243`), and that
`RNDraw::instanceCount` is uninitialised in `@interface RNDraw` (`compute.m:707`), which is why the fallback was
added. The fallback is still wrong for a legitimate zero.

## 44. An indexed draw with a nil or foreign index buffer silently becomes a non-indexed draw from vertex 0 (S2)

**Where:** `RedNaviMTL/compute.m:1229-1234` (`drawIndexedPrimitives:...`), `compute.m:1509-1511` (index binding),
`RedNaviMTL/rn_vk.c:1070-1076` (`if (dr->index_buffer) ... else CmdDraw`).

**What the code does.** `drawIndexedPrimitives:` first calls `drawPrimitives:type vertexStart:0 vertexCount:count
instanceCount:instances`, which records a normal non-indexed draw whose `vertex_count` is actually the *index* count,
and only then stores `indexBuf` in the recorded draw. If `indexBuffer` is nil, `d->indexBuf` stays nil, so
`runRenderPass` takes the `else` branch and calls `vkCmdDraw(cb, dr->vertex_count, ..., dr->first_vertex, ...)` with
`first_vertex == 0`. The index count is thus reinterpreted as a vertex count, and all vertices are taken
consecutively from offset 0 of whatever vertex buffer is bound. If the buffer is not a `RedNaviMTLBuffer` (a foreign
`id<MTLBuffer>`), the cast at `1233` and the `->b` dereference at `1510` read a field of an unrelated object.

**Why it produces the symptoms.** A nil index buffer is not a valid Metal call, but clients do make it during
teardown or when a resource allocation failed, and a driver must not turn that into a different draw. Drawing
`indexCount` consecutive vertices from 0 typically samples the whole vertex buffer, so the window gets a burst of
triangles spanning the screen: random pixels, smeared title bars, “text wider”, checkerboards. If the vertex buffer
is smaller than the index count, the draw reads past it (finding 42) and can lose the device. The same path runs for
an index buffer from another device, where the object layout is not ours.

**Fix.** In `drawIndexedPrimitives:`, reject nil and non-`RedNaviMTLBuffer` index buffers (log once, drop the draw,
or fail the command buffer). In `rn_vk_render_pass`, treat a draw with `index32`/`index_offset` set but no
`index_buffer` as an error (`rc = 6`), and never fall back to `CmdDraw`. Do the same for a foreign buffer: check
`[indexBuffer isKindOfClass:[RedNaviMTLBuffer class]]` before the cast.

**Double-check.** Verified the call order at `1231-1233`, that `dr[k].index_buffer` is only set inside
`if (d->indexBuf)` (`compute.m:1510`), that `rn_vk_render_pass` chooses `CmdDrawIndexed` solely on
`dr->index_buffer` (`rn_vk.c:1071`), and that there is no class check on `indexBuffer` in the entry point.

## 45. 3D and client-memory textures only ever synchronise slice 0, and depth is ignored by the bounds checks (S2)

**Where:** `RedNaviMTL/compute.m:326-328` (`syncFromSurface`), `compute.m:333-339` (`syncToSurface`),
`compute.m:351-357` (`syncFromBuffer`), `compute.m:1722-1737` (`rn_texture_over_bytes`), `compute.m:436` (staging
allocation `w*h*d*texel`), `RedNaviMTL/rn_vk.c:1183` (`rn_vk_image_copy` extent `{w, h, 1}`), `compute.m:1009`
and `1013` (blit rejects `o.z`/`sz.depth > 1`).

**What the code does.** The staging buffer is allocated for all slices (`w*h*d`), and `replaceRegion`/`getBytes`
iterate `z` correctly. But the three automatic synchronisation paths copy only `y < h`: `syncFromSurface` copies the
first plane slice to the staging buffer, `syncToSurface` copies it back, and `syncFromBuffer` (client memory) does
the same. `rn_texture_over_bytes` validates `off + (h-1)*bpr + w*texel <= len` without accounting for `d` or
`bytesPerImage`, so a buffer sized for one slice passes and the remaining slices are read as whatever follows it.
`rn_vk_image_copy` copies only `{w, h, 1}`. The blit encoder explicitly refuses a region with `o.z` or
`sz.depth > 1`, so a 3D texture cannot be uploaded through a blit either.

**Why it produces the symptoms.** WindowServer and CoreAnimation use 3D textures for window transitions, cube-like
shadow/reflection helpers and gradient sources. Only slice 0 is uploaded (or written back), so the other slices stay
zero or stale. A shader that samples slice 1 or 2 returns black or the previous frame's contents: black bands during
a minimise animation, the green/orange gradient checkerboard in a corner of a window that uses a 3D gradient, and
icons that vanish when their atlas slice is not slice 0. For a client-memory 3D texture the copy also reads past the
caller's allocation, which is exactly the kind of out-of-bounds source that produces random pixels. The blit refusal
turns a valid 3D upload into a failed command buffer, so the frame is dropped rather than merely wrong.

**Fix.** Loop `z < d` in `syncFromSurface`, `syncToSurface` and `syncFromBuffer`, offsetting by `z*w*h`, and use
`bytesPerImage`/`srcBpi` when the client supplies it. Include `d` and the per-image stride in the
`rn_texture_over_bytes` bounds check. Make `rn_vk_image_copy` copy `src->d` slices (add the depth to `rn_vk_image`),
and let the blit encoder accept `o.z`/`sz.depth > 1` by iterating slices. Add a round-trip test (create a 3D
texture, upload two slices through `replaceRegion`, sample both, read back).

**Double-check.** Verified the staging size at `436` includes `d`, the three loops at `328`, `339`, `355` use only
`y < h`, the bounds check at `1727` uses `t->h` and ignores `t->d`, `rn_vk_image_copy`'s extent at `1183` is
`{w, h, 1}`, and the blit rejects depth at `1009`/`1013`. `replaceRegion`/`getBytes` themselves handle `z`
(`441-446`, `455-460`), so the defect is confined to the automatic sync and copy paths.

## 46. A command buffer keeps all its records, resources and presented drawables after commit (S3)

**Where:** `RedNaviMTL/compute.m:888-890` (`records`, `presents` ivars), `compute.m:1366` (initialisation),
`compute.m:1571-1693` (`commitBody`), `compute.m:1690` (`for (id<MTLDrawable> dr in presents) [dr present]`),
`compute.m:1551-1557` (`dealloc`), `compute.m:1406` (`presentDrawable:`).

**What the code does.** `records` is an `NSMutableArray` that retains every `RNDraw`, `RNRenderPass`, `RNBlit` and
`RNDispatch` recorded on the command buffer, and each of those retains its textures, buffers, sampler states and
`NSData` copies. `presents` retains every `id<MTLDrawable>` added with `presentDrawable:` until the command buffer
is deallocated. `commitBody` iterates both arrays but never clears them; `dealloc` only logs a message when an
uncommitted buffer is released. In Metal, a command buffer releases the resources it referenced once it completes,
and `presentDrawable:` releases the drawable after presenting it, so the layer can recycle it.

**Why it produces the symptoms.** A command buffer that the client keeps alive after completion (for a completion
handler, for `waitUntilCompleted`, or simply because it is autoreleased later) now pins every texture and buffer
used by that frame, plus the drawable. `CAMetalLayer` only has a small pool of drawables (typically three); if the
plug-in holds them in `presents`, `nextDrawable` blocks, the animation runs at the drawable-pool rate instead of the
frame rate, and the previous frame stays on screen — exactly the minimise/maximise lag and the pixel trails. The
retained textures also keep large IOSurface mappings alive (finding 20), so memory grows with every frame that is
held for more than one commit.

**Fix.** At the end of `commitBody` (after the trace dump and after `present`), call `[records removeAllObjects]` and
`[presents removeAllObjects]`; clear both on the error path and in `dealloc`. If a completion handler still needs a
record (a trace dump), copy only that record. Alternatively, move the per-record processing into a local array that
is released at the end of the method.

**Double-check.** Verified the arrays are created at `1366`, appended at `961`, `1061`, `1244`, `1401`, `1404`,
`1406`, iterated but never cleared in `commitBody` (`1573-1690`), and that `dealloc` at `1551-1557` does not touch
them. Confirmed `presentDrawable:` merely appends (`1406`) and the present loop (`1690`) does not remove the
drawables.

## 47. Object creation takes a 12-frame backtrace on every buffer and texture, on by default (S3)

**Where:** `RedNaviMTL/compute.m:63-95` (`rn_site_enter`/`rn_site_leave`), called at `compute.m:154` (every
`RedNaviMTLBuffer`) and `compute.m:283` (every `RedNaviMTLTexture`), `compute.m:60-62` (comment on the cost).

**What the code does.** Every buffer and texture records its creation site: `rn_site_enter` calls
`backtrace(bt, 12)` unconditionally unless `RN_SITES=0` is in the environment, hashes the frames, takes a global
`os_unfair_lock`, and linearly searches a 128-entry table. For a site that is new it additionally calls
`backtrace_symbols` (malloc + symbol resolution). The switch is “on unless disabled”, which matters because
WindowServer runs with no client environment, so the diagnostic is always on there. `setVertexBytes` creates a
buffer per draw, so this runs thousands of times per frame in the compositor.

**Why it produces the symptoms.** `backtrace` on macOS walks the frame pointer chain and may unwind through signal
frames; it costs microseconds per call even when symbols are not resolved. Thousands of calls per frame add
milliseconds of CPU time and, because the table lock is global, serialise the render threads. The symptom is a
frame that is late by exactly the compositor's own overhead: animation lag, dropped frames, and trails when the
previous frame is presented after the next one is already being drawn. `backtrace_symbols` on a new site is much
more expensive and can stall a frame entirely (a new buffer size class appears during a window resize, which is
when the lag is worst).

**Fix.** Make the diagnostic opt-in (`RN_SITES=1` or a file flag), not opt-out; sample one object in N (e.g. every
256th); and use `__builtin_return_address(0..2)` instead of `backtrace` for the common path. Do not take the global
lock when only incrementing a counter — use atomics for the live/made counters and take the lock only when adding a
new site.

**Double-check.** Verified the default logic (`off = e && e[0] == '0'`, so the diagnostic is enabled when the
variable is absent) at `67-69`, the `backtrace` call at `71`, the global lock and linear search at `72-76`, the
`backtrace_symbols` path at `77-80`, and the two call sites at `154`/`283`. Confirmed the comment at `60-62`
already acknowledges the per-object cost.

## 48. The accelerator's page-table hooks report success without programming page tables, and scanout detection scans raw object memory (S3)

**Where:** `RedNaviAccel2/kern_accel2.cpp:287-300` (comment, `kScanoutRing`, `isScanoutMap`), `kern_accel2.cpp:302-311`
(`commitIntoGPUPageTable`), `kern_accel2.cpp:312-316` (`updateGPUPageTable`), `kern_accel2.cpp:318-321`
(`releaseFromGPUPageTable`), `kern_accel2.cpp:253` (`rebuildPagingBuffer`).

**What the code does.** `commitIntoGPUPageTable` and `updateGPUPageTable` always return `1`, after logging whether
the map was recognised as the scanout, but neither writes a page-table entry. `releaseFromGPUPageTable` returns 0.
`isScanoutMap` identifies a scanout map by scanning the raw 64-bit words at offsets `2*8` through `34*8` of the
`RedNaviMemoryMap` object and comparing each against the pointers in `gScanout`; the comment admits that the offset
of the memory pointer is only logged, not known. `rebuildPagingBuffer` returns 0 (failure). The comment explains
that the family needs `commitIntoGPUPageTable` to answer true for `IOAccelResource2::prepare`, and that the CPU copy
in `performTransaction` does not use the GPU page tables, so the success is a deliberate lie.

**Why it produces the symptoms.** The lie is fine only as long as *every* consumer uses the CPU copy. Any family or
WindowServer path that takes the successful return at face value and then uses the resource's GPU address (a page
flip, a GPU blit, a scanout programmed with a GPU virtual address) will read or write an address that was never
mapped — producing a black band, a torn frame, or memory corruption that looks like random pixels. The scanout
detection is fragile in the other direction: the raw-word scan can read past the end of a smaller map object, and a
reused address can match an unrelated field, so a non-scanout map is occasionally classified as the scanout (or
vice versa). That changes only logging today, but it is the kind of heuristic that turns into a heisenbug when the
page-table code is implemented.

**Fix.** Either implement the page-table writes (the correct fix and the one needed for a GPU-programmed scanout),
or answer failure and make the caller fall back explicitly to the CPU path — do not report success for a mapping
that does not exist. Track scanout memories by identity (a set keyed by the resource, or a field the family exposes)
instead of scanning raw object words; at minimum, bound the scan by the object's real size and log a mismatch
instead of guessing. Make `rebuildPagingBuffer` consistent with `commitIntoGPUPageTable`.

**Double-check.** Verified both success returns and the absence of any page-table write at `302-316`, the raw-word
loop `i = 2..34` at `295-300`, the ring declaration at `292-294`, and `rebuildPagingBuffer` returning 0 at `253`.
Confirmed the explanatory comment at `287-291` states the CPU copy is the reason the hooks are stubbed, so the
severity is a latent/correctness risk rather than an immediate corruption.

## 49. Headless detection is overridden, and the system-memory allocator returns an object that was never initialised (S4)

**Where:** `RedNaviAccel2/kern_accel2.cpp:869-874` (`configIsHeadless`), `kern_accel2.cpp:244-250` (comment on
`IOAccelSysMemory`), `kern_accel2.cpp:977-981` (`newSysMemory`).

**What the code does.** `configIsHeadless` calls the family, then returns `false` unless the boot argument
`rednavi_accel2_headless` is set: on every normal boot it tells WindowServer that a display head exists, even when
the family said the configuration is headless. `newSysMemory` returns `OSTypeAlloc(RedNaviSysMemory)` without
calling `init`; `OSTypeAlloc` returns a new, uninitialised object (the header documents this), and
`RedNaviSysMemory` overrides nothing, relying on the family's `IOAccelSysMemory` implementation. The comment says the
family's `withMemoryDescriptor` wraps each plane's IOSurface through this allocator.

**Why it produces the symptoms.** On a genuinely headless configuration (a remote session, a Mac with no panel
attached, or a display that has not been detected yet) forcing `false` makes WindowServer build a display pipe and
a transaction for a head that has no scanout. The pipe's `performTransaction` then finds no `fb-scanout-phys`
property and returns 0, the frame is dropped, and the desktop is composited without a display — the user sees a
black or frozen screen and a stream of failed transactions. Conversely, if the object returned by `newSysMemory` is
used before the family's `init`/`withMemoryDescriptor` has run, its fields are whatever was on the heap, which can
make a plane's memory descriptor point at the wrong surface. Both are configuration-dependent, which is why they
show up as intermittent glitches rather than a constant failure.

**Fix.** Respect the family's answer by default; keep the override behind an explicit boot argument, and log the
override once. In `newSysMemory`, call the appropriate `init`/`initWithMemoryDescriptor` and check its result before
returning the object; if the family expects an already-constructed object, match that contract. Add a test that
boots a headless VM (or with the panel disconnected) and checks that WindowServer does not keep retrying a pipe with
no scanout.

**Double-check.** Verified the unconditional `return keep ? fam : false` at `873`, the `OSTypeAlloc` at `978` with
no `init`, the class declaration at `246-251` (only `rnPadS0..S3`, no `init` override), and the family-comment at
`244-245` saying `withMemoryDescriptor` is the caller. Because the family may call `init` itself, this is written as
a risk to verify rather than a proven leak; the headless override is unconditional.

## 50. The scanout-memory ring never releases an entry, and stale entries are matched by address (S3)

**Where:** `RedNaviAccel2/kern_accel2.cpp:292-294` (`kScanoutRing`, `gScanout`), `kern_accel2.cpp:529-544`
(`initFramebufferResource`), `kern_accel2.cpp:546-549` (`destroyFramebufferResource`), `kern_accel2.cpp:295-300`
(`isScanoutMap`).

**What the code does.** Every call to `initFramebufferResource` allocates a new `RedNaviScanoutMemory` with
`OSTypeAlloc`, initialises it, and stores the raw pointer in `gScanout[gScanoutNext]`, wrapping after 64 entries;
`gScanoutCount` only grows to 64. Nothing ever releases an entry: `destroyFramebufferResource` logs and calls the
family, but does not remove the matching pointer from the ring. The comment at `290-291` says WindowServer rebuilds
the pipe (and the scanout memory) on every display change, 35 times in 10 minutes in one measurement.

**Why it produces the symptoms.** Each entry owns a reference (from `OSTypeAlloc`/`init`) to a scanout memory and
the resource it wraps; after 64 display changes the ring overwrites live pointers with new ones and the old
objects are never released, so the accelerator leaks one scanout object per display change. The family may also
release the memory in `destroyFramebufferResource`, in which case the ring keeps a dangling pointer: `isScanoutMap`
then compares a new map's words against freed memory, and a later allocation at the same address makes an unrelated
map look like the scanout. Since `commitIntoGPUPageTable` currently answers true for every map (finding 48), the
functional effect is limited to misclassification and logging, but the leak and the dangling pointers are real and
grow with every resolution change — exactly when the black band and the glitches are reported.

**Fix.** Retain/release the ring entries explicitly: remove the entry in `destroyFramebufferResource` (match by the
resource or by the memory pointer), release it, and keep the ring as a small ring of *owned* references. If the
family takes ownership of the object returned by `initFramebufferResource`, keep a weak/borrowed pointer and remove
it before the family can free the object. Bound the ring and evict the oldest with a release instead of silently
overwriting. Replace the raw-word scan with a lookup in the ring by identity.

**Double-check.** Verified the insert at `543` (no retain/release), the ring wrap and count at `292-294`/`543`, that
`destroyFramebufferResource` at `546-549` does not touch `gScanout`, and that `isScanoutMap` scans `w[2..34]` at
`295-300`. Confirmed the comment at `290-291` reports the 35 rebuilds in 10 minutes.

## 51. A compute kernel with no descriptor bindings cannot become a pipeline, although Vulkan allows it (S4)

**Where:** `RedNaviMTL/rn_vk.c:276-277` (`rn_vk_compute_create`), `RedNaviMTL/compute.m:1962`
(`rn_pso_from_function`), `RedNaviMTL/compute.m:2272-2273` (`rn_newComputePipelineWithSPIRV:`).

**What the code does.** `rn_vk_compute_create` starts with
`if (nbind == 0 || nbind > RN_VK_MAX_BINDINGS) return NULL;`. The Metal path in `rn_pso_from_function`
never even calls it when the translated kernel reports `binding_count == 0`
(`n ? rn_vk_compute_create(...) : NULL`) and reports “the kernel uses no resources”; the private
SPIR-V entry point also rejects `nbuf == 0`. But a Vulkan compute pipeline with a descriptor-set layout
that has `bindingCount = 0` is legal, and AIR kernels can take their whole input through the dispatch
grid (push constants) without a single buffer, texture or sampler binding. `rn_vk_compute_create` already
handles the empty cases downstream (`make_layout` passes `pBindings = NULL` when `nb == 0`; the pool loop
produces `nps == 0`), so the early rejection is the only thing that prevents it.

**Why it produces the symptoms.** A client that creates such a kernel gets `nil` from
`newComputePipelineStateWithFunction:error:`; QuartzCore/CoreAnimation then skips that compositing step
or falls back to a slower path. The visible result is a layer that is not processed for that frame —
missing icons, a mask that does not apply, a trail where the previous frame was, and the general
“some things are just not drawn” class of glitch. It is not a common shape, hence S4, but it is a real
API violation: the plug-in advertises Metal 3 compute support.

**Fix.** Allow `nbind == 0`: skip the descriptor-set layout/pool allocation, or create an empty set
layout and pipeline layout (both are legal), and keep the pipeline usable with a NULL argument array.
Accept `nbuf == 0` in the private SPI. Add a test that creates a kernel with only push constants and
dispatches it.

**Double-check.** Verified the guard at `rn_vk.c:277`, the `n ? ... : NULL` at `compute.m:1962` with its
“the kernel uses no resources” error text, and the `nbuf == 0` rejection in the private entry point at
`compute.m:2272`. Verified that the rest of `rn_vk_compute_create` already tolerates a zero binding
count (`bindingCount = nbind`, `pBindings = b`; the pool-size loop leaves `nps == 0`), so the rejection
is not protecting an unhandled path.

## 52. Client-storage textures are copied out of the caller's memory one row per Mach trap (S3)

**Where:** `RedNaviMTL/compute.m:351-356` (`syncFromBuffer`), `RedNaviMTL/compute.m:169-171`
(`rn_copy_from_caller`), call site `RedNaviMTL/compute.m:1603`.

**What the code does.** `syncFromBuffer` loops over the texture's rows and for every row calls
`rn_copy_from_caller`, which is `vm_read_overwrite(mach_task_self(), ...)`: one Mach trap per row per
texture per command buffer. The trap exists for a good reason (the caller may unmap or truncate its
memory while the commit runs, and a plain `memcpy` would SIGSEGV/SIGBUS), but the granularity is one
row. A 512×512 client-storage atlas costs 512 traps; CoreAnimation and CoreDisplay create several such
textures per frame (glyph atlases, icon bitmaps, snapshot tiles), so a frame easily pays thousands of
traps, inside the commit lock (`compute.m:1563-1571`). The IOSurface paths use plain `memcpy`
(`compute.m:328-331`, `339-342`) because that memory is mapped by the plug-in, so this cost is specific
to client memory.

**Why it produces the symptoms.** Every trap is a kernel entry and a copy; thousands per frame add
milliseconds of CPU time to the exact path that presents the compositor's frame, and because the commit
is serialised by the recursive lock, the lag is not hidden by another thread. The minimisation and
maximisation animations are the worst case: they allocate and upload many small client-storage textures
per frame, so the per-row cost grows exactly when the user notices it. The delayed frame leaves the
previous one on screen — the pixel trails.

**Fix.** Copy a contiguous caller range per trap instead of per row: the rows all live in the caller's
`[srcRaw, srcRaw + len)` block, so a single `vm_read_overwrite` of `h * srcBpr` bytes into a reusable
trampoline, followed by a user-space row pack into the staging buffer, costs one trap. When
`srcBpr == w * texel` the copy is already packed and the trap can go straight into the staging buffer.
Keep the current per-row failure semantics (stop at the first unreadable row) but decide it from the
single bulk result.

**Double-check.** Verified the per-row loop and trap at `compute.m:355`, the `vm_read_overwrite` helper at
`compute.m:169-171`, and that `commitBody` calls `syncFromBuffer` on every texture in `surfUsed` at
`compute.m:1603` while holding the commit lock. Compared with `syncFromSurface`/`syncToSurface`
(`compute.m:328-331`, `339-342`), which use `memcpy`; the difference confirms the trap is only for
caller-owned memory and is not required per row.

## 53. `newBufferWithBytesNoCopy:` copies the whole buffer at creation and again on every commit, and dereferences `bytes` unchecked (S3)

**Where:** `RedNaviMTL/compute.m:2148-2158` (`newBufferWithBytesNoCopy:length:options:deallocator:`),
`RedNaviMTL/compute.m:177-188` (`syncNoCopy`), call sites `compute.m:1650` and `1682`.

**What the code does.** The no-copy constructor allocates a private Vulkan buffer and immediately
`memcpy`s the whole `length` from `bytes` into it, without the `bytes != NULL` test that the copying
constructor has (`if (b && bytes)`). It then registers the buffer in `noCopyBuffers`, and every commit
that uses the buffer copies the whole `len` in (`syncNoCopy:in:YES`) and the whole `len` out
(`syncNoCopy:in:NO`). So the “no copy” API costs three full copies of the buffer around its first
frame and two per frame after that, regardless of how much the CPU or GPU actually touched. The
constructor's copy is also made before the caller has necessarily finished filling the memory; the
first commit's `in:YES` is what makes the data correct, so the eager copy is pure overhead. A NULL
`bytes` with a non-zero length faults in `memcpy`.

**Why it produces the symptoms.** These copies run on the caller's thread inside the commit lock and
scale with the buffer size, not with the damage. CoreAnimation uses no-copy buffers for shared image
backing (the whole point is to avoid the copy), and WindowServer's compositor buffers are megabytes:
two full copies per frame add latency and memory bandwidth to the same path that is already copying
surfaces (finding 8). The result is a late frame, a stale scanout and the trails/lag. The unchecked
`memcpy` is a fatal path for a caller that passes a placeholder pointer and fills it later, which is
legal only if it stays valid, but a NULL/zero-length combination is not validated at all.

**Fix.** Do not copy in the constructor; let `syncNoCopy:in:YES` perform the first upload. Validate
`bytes` (`!bytes && length` → return nil) and the advertised `minBufferNoCopyAlignmentBytes`.
Track dirty ranges from `didModifyRange:` (`compute.m:193` is currently empty) and copy in only the
ranges the CPU wrote, and copy out only when the client has asked for the contents or the buffer was
written by the GPU (Metal's storage mode is shared here, so a readback can be lazy like the image
path's `gpudirty`).

**Double-check.** Verified the unconditional `memcpy(rn_vk_buffer_contents(b->b), bytes, length)` at
`compute.m:2157` and the absence of a `bytes` test there, in contrast with the copying constructor's
`if (b && bytes)` at `compute.m:2145`. Verified `syncNoCopy` copies the whole `x->len` at
`compute.m:181` and is called before the records at `1650` and after them at `1682`. Confirmed
`didModifyRange:` at `compute.m:193` is a no-op.

## 54. A rejected client-storage texture leaks the caller's memory because its deallocator is never invoked (S4)

**Where:** `RedNaviMTL/compute.m:1722-1736` (`rn_texture_over_bytes`), callers
`RedNaviMTL/compute.m:2212` and `2220` (`newTiledTextureWithBytesNoCopy:` SPI).

**What the code does.** `rn_texture_over_bytes` creates the texture, then validates the descriptor and
the caller's `bytes`/`len`/`off`/`bpr`. On any failure it logs and returns `nil` **without calling
`dealloc`**. The deallocator is the block the caller passed with the `...deallocator:...` SPI; it is
stored in `t->srcDealloc` and is invoked only from the texture's `-dealloc` (`compute.m:277-278`).
When the texture is never created, the block is never called, so the memory the caller handed over
with an ownership-transferring deallocator is leaked.

**Why it produces the symptoms.** CoreAnimation/CoreDisplay create client-storage textures per glyph
atlas, icon and snapshot tile; a descriptor that this plug-in does not support (a format missing from
`rn_vk_format_of`, a 3D/array shape, a row pitch that does not fit) makes the call fail repeatedly. The
leak grows with UI activity, and once the process is under memory pressure the later allocations that
matter (the surfaces and scanout buffers) fail, which shows up as missing icons or a frame that is not
composited. It also violates the SPI contract, so a caller that relies on the block to release a
mapped or shared region keeps that region alive.

**Fix.** Call `dealloc(bytes, len)` on every failure path before returning `nil`, or validate before
taking ownership and document that the caller keeps ownership when the result is nil. Do the checks
before `[[RedNaviMTLTexture alloc] initWithDevice:...]` so a partially constructed texture is never
left behind.

**Double-check.** Verified the two early `return nil` paths at `compute.m:1725-1730` and `1736` with no
`dealloc` call, that `srcDealloc` is copied and invoked only in `-dealloc` at `compute.m:277-278`, and
that the only caller that passes a block is the `...deallocator:...` SPI at `compute.m:2220`.

## 55. CPU access to a zero-copy host image uses packed rows and skips the IOSurface lock, so padded surfaces are read and written at the wrong offsets (S2)

**Where:** `RedNaviMTL/compute.m:437-457` (`replaceRegion:`/`getBytes:`),
`RedNaviMTL/rn_vk.c:517-521` (`rn_vk_image_staging`), `RedNaviMTL/rn_vk.c:469-520`
(`rn_vk_image_from_host`), `RedNaviMTL/compute.m:305-310` (zero-copy branch of the IOSurface
constructor).

**What the code does.** For a zero-copy IOSurface texture, `img` is a host image over the surface's
pages with the surface's row pitch; `rn_vk_image_from_host` requires `sl.rowPitch == pitch` but keeps
only the base pointer, width, height and texel size, not the pitch. `rn_vk_image_staging` returns the
host pointer directly. `replaceRegion`/`getBytes` then address it as
`((z * h + y) * w + x) * texel`, i.e. with rows packed at `w * texel`, and they never lock the surface
(the zero-copy `syncFromSurface`/`syncToSurface` return immediately). The zero-copy branch is taken
whenever `w == sw && h == sh`, with no pitch check; finding 18's pitch guard exists only in
`rn_scanout_image`, not here. Any surface whose row pitch is larger than `w * texel` (a width that is
not aligned to the surface's alignment) is therefore accessed with the wrong stride.

**Why it produces the symptoms.** A small image uploaded through `replaceRegion` into an
IOSurface-backed texture lands shifted after the first row; `getBytes` returns the same shifted rows.
This is exactly the “text drawn wider”, smeared icon and duplicated-glyph class of bug, and it is
worse in the corners where a padded atlas row wraps into the next one (green/orange checkerboards).
Because the write is made without `IOSurfaceLock`, it can also race the display engine reading the same
surface, which produces the random pixels and the lighter rectangles.

**Fix.** Store the imported pitch in `rn_vk_image` and use it for host-memory row addressing in
`replaceRegion`/`getBytes` (and in the blit encoder, which reads the same staging pointer), or bracket
the access with `IOSurfaceLock`/`IOSurfaceUnlock`. Until the addressing is pitch-aware, disable
zero-copy when `IOSurfaceGetBytesPerRow(io) != w * texel` and let the copy path handle the surface.

**Double-check.** Verified `rn_vk_image_staging` returns `r->hostptr` for host images at `rn_vk.c:518-520`,
that `rn_vk_image_from_host` uses the local `sl` layout only for the equality test and stores no pitch
(`im->hostptr`, `im->w/h/texel` only), that `replaceRegion` and `getBytes` multiply by `w * texel` at
`compute.m:443` and `455`, and that the zero-copy branch at `compute.m:305-310` tests only
`w == sw && h == sh`. Confirmed the zero-copy early returns in the sync methods at `compute.m:319-320`
and `333-334`, so the lock is skipped on exactly the path that needs it most.

## 56. A storage-buffer offset that is not aligned fails the whole command buffer, although Metal allows any offset (S2)

**Where:** `RedNaviMTL/rn_vk.c:549` (`fill_writes`), `RedNaviMTL/rn_vk.c:358`
(`rn_vk_storage_offset_alignment`), `RedNaviMTL/compute.m:1672-1676` (`rc == 4` handling).

**What the code does.** `fill_writes` rejects a storage buffer whose bound offset is not a multiple of
`vk->storageAlign` (Vulkan's `minStorageBufferOffsetAlignment`, typically 16/32/64 bytes) by returning
4. `commitBody` turns any non-zero dispatch result into `[self fail:...]`, so a single unaligned offset
marks the whole command buffer as failed. Metal has no such alignment requirement: a client may bind a
`MTLBuffer` at any byte offset, and translated AIR code commonly passes a struct at its natural
4- or 8-byte offset. The translation layer does not adjust the offset, so the dispatch is refused.

**Why it produces the symptoms.** A compute stage (CoreImage filter, CoreAnimation's image processing,
a client's own kernel) that binds a struct at an unaligned offset fails the frame instead of drawing
it; the previous frame stays on screen. That is the intermittent “stuck” content, the trails and the
stale rectangles, and it is tied to specific windows/effects because only those use compute. It also
explains why the failures are hard to reproduce: the same shader works when its buffer happens to be
aligned.

**Fix.** Satisfy Vulkan's alignment instead of rejecting: bind the descriptor at
`offset & ~(align - 1)` and communicate the remainder to the shader (the translator can fold it into
the access), or allocate a small aligned copy of the affected range for the dispatch. At the very
least, do not fail the entire command buffer: log the binding and skip that dispatch (or fall back to
an aligned copy) so the rest of the frame still presents.

**Double-check.** Verified the check `a->offset % vk->storageAlign || a->offset >= a->buffer->size`
and the `return 4` at `rn_vk.c:549`, that `rn_vk_storage_offset_alignment` exposes `vk->storageAlign`
at `rn_vk.c:358`, and that `compute.m:1673` turns `rc == 4` into a command-buffer failure. Confirmed
`rn_bind_args` passes `d->offs[m]` unchanged (`compute.m:544`), so the plug-in never adjusts the offset.

## 57. An empty `dispatchThreads:` is reported as a missing resource and fails the command buffer (S3)

**Where:** `RedNaviMTL/rn_vk.c:696-698` (`rn_vk_dispatch_threads`), `RedNaviMTL/compute.m:966-969`
(`dispatchThreads:threadsPerThreadgroup:`), `RedNaviMTL/compute.m:1672-1676`.

**What the code does.** `dispatchThreads:` computes the threadgroup grid and records it. If any
dimension of `threads` or `tpt` is zero, `rn_vk_dispatch_threads` returns 6 (“a resource is missing”).
Metal treats a zero-dimension `dispatchThreads:` as a no-op (it is legal for an empty work count),
and the sibling `dispatchThreadgroups:` already submits zero groups to Vulkan, which is a normal no-op.
Here the empty dispatch instead becomes a failed command buffer.

**Why it produces the symptoms.** During window transitions, an empty layer or a clipped region can
make a client/CoreImage kernel dispatch zero threads. The command buffer is marked failed, so nothing
is presented and the previous frame remains — a trail — and the log blames a missing resource, which
sends the investigation in the wrong direction. Because the same kernel works as soon as one thread is
needed, the failure looks random.

**Fix.** Return success without submitting when any `threads[d]` is zero (the grid is empty); keep the
local-size validation at pipeline creation, where a zero `tpt` is a genuine descriptor error. Treat a
zero group count the same way in `rn_vk_dispatch`.

**Double-check.** Verified the zero guard at `rn_vk.c:698` and the `rc` → `[self fail:]` path at
`compute.m:1672-1676`; verified `dispatchThreads:` computes `g` with zero values at `compute.m:966-969`;
and verified that `dispatchThreadgroups:` maps to `rn_vk_dispatch`, which has no zero check and lets
Vulkan execute a zero-group dispatch.

## 58. The render encoder does not implement the LOD-clamp sampler setters that `MTLRenderCommandEncoder` declares (S3)

**Where:** `RedNaviMTL/compute.m:1173-1176` (render sampler setters), `RedNaviMTL/compute.m:1149`
(`+load` adds the protocol), contrast with the compute encoder at `compute.m:950`.

**What the code does.** The render encoder declares conformance to `MTLRenderCommandEncoder` in `+load`
but implements only `setVertexSamplerState:atIndex:`, `setFragmentSamplerState:atIndex:` and their array
forms. The protocol also declares
`-setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:` and the fragment variant; neither exists in
the class. A client that calls one gets `unrecognized selector`, which raises in the WindowServer
render thread instead of degrading. The compute encoder has the analogous method (`compute.m:950`),
which makes the gap easy to miss.

**Why it produces the symptoms.** An exception in WindowServer's render path stops the compositor
thread, so the display freezes on the last presented frame and then recovers when the connection is
torn down — the “glitch, then a stale picture” pattern. The method is a normal way to sample a
mipmapped glyph/atlas without bleeding, so a client that uses it does so on every frame, making the
failure deterministic for that client rather than intermittent.

**Fix.** Implement both LOD-clamp setters, store the `(min, max)` pair per sampler slot, and build a
sampler variant for the binding (or log and ignore the clamps when the slot has no sampler). Audit the
whole class against the protocol with `respondsToSelector:` so any other declared selector has a
deliberate implementation or a safe no-op.

**Double-check.** Verified the two render setters at `compute.m:1173-1174`, the array forms at
`1175-1176`, and that `grep` finds the LOD-clamp method only in the compute encoder at `compute.m:950`
— there is no `setVertexSamplerState:lodMinClamp:` or `setFragmentSamplerState:lodMinClamp:` anywhere
in the file. Confirmed the protocol is added in the render encoder's `+load` at `compute.m:1149`.

## 59. Per-binding LOD clamps on the compute path are silently discarded (S4)

**Where:** `RedNaviMTL/compute.m:950` (`setSamplerState:lodMinClamp:lodMaxClamp:atIndex:`),
`RedNaviMTL/compute.m:500` (`rn_new_sampler_state`), `RedNaviMTL/rn_vk.c:245-250` (`sampler_info`).

**What the code does.** The compute encoder's LOD-clamp setter forwards to the plain setter and drops
`lodMinClamp`/`lodMaxClamp`. Those values override the sampler descriptor for that binding in Metal; the
plug-in keeps only the `MTLSamplerState` object, whose `lodMinClamp`/`lodMaxClamp` come from its own
descriptor (`compute.m:500`). A kernel that clamps a sampler to one mip level therefore samples
whatever the state object's clamps allow.

**Why it produces the symptoms.** Sampling a mipmapped atlas outside the intended level picks up a
neighbouring glyph or a blurred level; text looks wider or doubled, icons get fringes. The per-binding
clamp is exactly the mechanism a careful client uses to prevent that, so discarding it converts a
correct client into the reported “text is wider” symptom. It is S4 because the effect is subtle and
limited to compute kernels, but it is a real semantic loss.

**Fix.** Carry the `(min, max)` pair in the dispatch record and create/cache a sampler variant per
pair (the pipeline cache already keys variants), or apply the clamps as push constants if the
translator's sampling path can express them. If neither is feasible, log once per distinct pair so the
limitation is visible instead of silent.

**Double-check.** Verified the body at `compute.m:950` only calls `setSamplerState:atIndex:`, that the
dispatch record stores only the sampler object (`compute.m:583`, `955`), and that
`rn_new_sampler_state` copies the descriptor's clamps at `compute.m:500` into `sd.lod_min`/`lod_max`,
which `sampler_info` applies at `rn_vk.c:249-250`.

## 60. `waitUntilScheduled` does nothing and `waitUntilCompleted` never actually waits (S3)

**Where:** `RedNaviMTL/compute.m:1393` (`waitUntilScheduled`), `RedNaviMTL/compute.m:1693-1696`
(`waitUntilCompleted`), `RedNaviMTL/compute.m:1368` (`done` semaphore), `compute.m:1383` (`enqueue`),
`compute.m:1563-1571` (`commit` lock), signal at `compute.m:1691`.

**What the code does.** `waitUntilScheduled` is an empty method; `waitUntilCompleted` returns
immediately when the buffer has not been committed, and when it has, waits on `done` with
`DISPATCH_TIME_NOW`, i.e. it polls once instead of blocking. Since `commitBody` is synchronous and
signals `done` at its end, a client that commits and then waits happens to be correct; a client that
`enqueue`s, calls `waitUntilScheduled` and then reuses the buffer is told the work is scheduled while
nothing has run, and a client that calls `waitUntilCompleted` before committing is told it is complete
without any work. Metal's contract is to block until the requested state is reached.

**Why it produces the symptoms.** `enqueue` + `waitUntilScheduled` is the normal way to pace a
triple-buffered client: if the wait returns immediately, the client runs ahead and overwrites a buffer
that the previous frame is still reading (or that has not been uploaded yet), which shows up as
random pixels/tearing in animated content. A premature “completed” lets a client read a buffer before
the GPU wrote it, which shows up as stale tiles and trails. The synchronous commit hides most of the
problem, but not the ordering that the client relies on.

**Fix.** Make `waitUntilScheduled` wait for the commit to have started (for this synchronous
implementation, waiting for completion is equivalent and simpler), block on `done` with
`DISPATCH_TIME_FOREVER` in `waitUntilCompleted`, and signal `done` on every path (including the
`st >= Committed` early return). Return an error/status for a buffer that can never be committed.

**Double-check.** Verified the empty body at `compute.m:1393`, the early return and
`dispatch_semaphore_wait(done, DISPATCH_TIME_NOW)` at `compute.m:1693-1696`, that `done` is created at
`compute.m:1368` and signalled only at the end of `commitBody` (`compute.m:1691`), that `enqueue`
only changes the status (`compute.m:1383`), and that `commit` holds the recursive lock around
`commitBody` (`compute.m:1563-1571`).

---

# Fix order and verification plan

The findings above are ordered by symptom impact, but the fixes should be applied in dependency order: several
symptoms are consequences of the same state machine, and fixing a downstream copy before the ownership model is
stable will only move the corruption.

## Stage 1 — stop presenting wrong pixels (S1 correctness)

1. **Canonical storage per surface** (findings 20, 21, 31, 37): one device image per
   `(IOSurface, plane, format-family)`; direct passes, sampling, views, CPU readback and the final copy all use it.
   Remove the “seed from the scanout” guess; start unseeded surfaces black and let the first full-surface pass fill
   them.
2. **Copy/format correctness** (findings 1, 2, 21): add `TRANSFER_SRC/DST` to host-imported images, use
   `vkCmdBlitImage` for format conversion, create the scanout image in the surface format, and stop hard-coding
   1920×1080.
3. **Ordering and lifetime** (findings 24, 33, 35): present and copy before `Completed`; completion handlers and
   presented handlers after the flip; never present an errored buffer; make `waitUntilCompleted` actually wait.
4. **Single-buffer/flip** (findings 31, 36, 39): double-buffer the scanout, page-flip at vblank, per-surface
   `FRAME_DONE`, and an ownership protocol between the pipe copy and the plug-in.
5. **Thread safety** (findings 25, 26, 27): one submission lock (or per-thread pools + queue lock), no event wait
   under the commit lock, atomic pipeline cache, timeline semaphores for cross-buffer events.

## Stage 2 — stop dropping content (S2 correctness)

6. **Depth/stencil** (finding 28) and **formats/views/arrays/mips/MSAA** (findings 23, 30, 40): implement the
   formats and attachment paths that WindowServer actually uses, or fail pipeline creation explicitly.
7. **Samplers and capability honesty** (findings 22, 23): comparison, anisotropy, reduction, mirror-clamp; answer NO
   for anything not implemented; fix the 31-vs-16 limit mismatch.
8. **Blits** (finding 32): GPU blits with `bytesPerImage`, slices and levels; no full-image CPU copies.
9. **Clear semantics** (finding 29): convert clears per destination format, including sRGB and integer unions.
10. **Device lifecycle** (finding 34): reopen on device loss, tolerate failed opens, null-safe destructors.

## Stage 3 — latency and robustness (S2/S3)

11. **Batch submissions** (finding 38) and remove the per-pass wait; keep CPU/GPU overlap.
12. **Hot-path property churn** (findings 13, 14): no `OSData`/`setProperty` or uncached pixel reads on every frame.
13. **Scanout ring/bookkeeping** (findings 15, 16, 17, 18, 19, 50): VBL registration limit, mode-change
    invalidation, coherent host memory, pitch handling, one scanout source, and an owned/drained scanout ring.
14. **Transactions and CPU paths** (findings 11, 12, 32, 39): honour dirty bits/options/resources, avoid
    map/copy under the transfer lock, and make CPU access coherent with direct mode.
15. **Draw and copy validation** (findings 42, 43, 44): range and alignment checks for vertex/index data, keep
    zero-instance draws as no-ops, and never turn an invalid indexed draw into a non-indexed one.
16. **3D/client-memory slices** (finding 45): sync every depth slice, honour `bytesPerImage`, and allow
    `z`/`depth > 1` in blits and copies.
17. **Allocation and retention** (findings 41, 46, 47): drain the buffer cache on close, clear a command buffer's
    records/presents after commit, and make creation-site tracing opt-in.
18. **Accelerator plumbing honesty** (findings 48, 49): do not report page-table success without programming the
    table, respect the family's headless answer, and initialise the system-memory object.
19. **Zero-copy CPU access** (finding 55): address host images with the surface's real row pitch and lock the
    IOSurface around `replaceRegion:`/`getBytes:`; disable zero-copy when the pitch is not packed.
20. **Client-memory transfer cost and ownership** (findings 52, 53, 54): one Mach copy per contiguous caller range,
    no eager no-copy upload, dirty-range tracking, and the caller's deallocator on every failure path.
21. **Compute API contract** (findings 51, 56, 57): accept zero-binding kernels, satisfy or transparently work
    around the storage-offset alignment, and treat an empty `dispatchThreads:` as a no-op.
22. **Command-buffer and sampler contract** (findings 58, 59, 60): implement every declared sampler setter, honour
    per-binding LOD clamps, and make the schedule/completion waits block as Metal specifies.

## Verification matrix

| Symptom | Primary checks |
|---|---|
| Black band at the top | 31 (flip/vblank), 21/2 (format/size of the final copy), 33 (errored buffer presented), 45 (3D slices), 48 (unmapped GPU addresses) |
| Minimise/maximise lag and trails | 38 (batching), 25 (event wait), 31/37 (stale scanout), 24/35 (pacing), 41/46 (retention), 47 (backtrace cost), 50 (leaked scanout objects) |
| Random pixels left on screen | 1/2/21 (copy state), 27 (queue races), 39 (concurrent pipe copy), 23 (out-of-bounds reads), 42/43/44/45 (draw and slice ranges), 48 (unmapped address) |
| Lighter grey rectangles in the top bar | 29 (sRGB/integer clears), 22 (sampler), 13/14 (diagnostics corrupting timing), 43/46 (stray draws, retained drawables) |
| Icons disappearing | 30/23/40 (formats/arrays/views), 28 (depth/stencil masks), 33 (partial frame), 42/44/45 (range and slice bugs) |
| Texts drawn wider | 30/40 (glyph atlas arrays/mips), 22 (anisotropy/mirror clamp), 32 (blit layout), 42/44 (vertex/index range), 45 (slice 0 only) |
| Green/orange checkerboards in corners | 20/21 (uninitialised seed/format), 29 (integer clear), 28 (missing stencil clip), 39 (tearing), 42/44/45 (garbage vertex/index/slice data) |

The tests that make the fixes observable are already in the tree or trivial to add: `RN_DUMP_PASSES`,
`/Users/Shared/rednavi-trace`, `RN_LOG_BUFFERS`, `RN_METAL_NO_ZEROCOPY`, `/Users/Shared/rednavi-nodirectscan`, and
the bandwidth/latency probes in `compute.m`. For each stage, run WindowServer with a 4K and a scaled mode, a
minimise/maximise loop, a full-screen video, a Metal game in a window, and a second display if available; check that
the panel content matches the trace PNG of the final pass and that `performTransaction` never copies a surface whose
direct image is newer than the IOSurface.


