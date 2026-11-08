# M6 code review: the Metal/WindowServer path of commit fa30c28

Scope: commit `fa30c28` ("M6: WindowServer composes on the GPU ... first time, unstable") and the
documents attached to it (`docs/M6_DISPLAY_ACCEL.md`, `docs/M6_WINDOWSERVER_STUDY.md`,
`docs/M6_METAL_ON_RADV.md`). The review asks one question: what in this code makes WindowServer
crash a few seconds after the pointer moves, deform window parts and corrupt shapes around the
pointer, and what else would keep the compositor from being correct once those are fixed.

Every finding below was read back against the current tree, not only against the diff. Line
numbers are from `fa30c28`. Severity is about the effect on WindowServer: `Critical` means a
crash, memory corruption or a wrong frame on the primary path; `High` means visible corruption or
a stalled/failed frame; `Medium` means a latent bug that shows under load or on another display;
`Low` means diagnostics, cleanup or an unverified assumption.

The findings are grouped in the order they were written. At the end there is a suggested fix
order, so that the cheap correctness fixes land before the architectural ones.

---

## 1. Command-buffer commits are not serialised, and they all share one Vulkan queue and command pool

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:1421-1531` (`-commit`, `-commitBody`), `RedNaviMTL/compute.m:160-163`
(`+syncNoCopyIn:`), `RedNaviMTL/rn_vk.c:32-34` (one `VkQueue` and one `VkCommandPool` in the
process-wide `rn_vk`), `RedNaviMTL/rn_vk.c:140-141` (creation), `RedNaviMTL/rn_vk.c:872` and
`RedNaviMTL/rn_vk.c:948` (per-pass command buffer and `vkQueueSubmit`), `RedNaviMTL/rn_vk.c:544-566`,
`760-789`, `946-951` (same pattern in dispatches and blits).

**Symptom:** SIGSEGV or SIGBUS in `memmove`/`memcpy` inside `+[RedNaviMTLBuffer syncNoCopyIn:]`
called from `-commitBody`, a few seconds after the pointer starts moving. With two threads
composing at once the same code can also copy a half-written buffer into the GPU and a
half-written GPU buffer back into the caller's memory.

**Root cause:** `-commitBody` is a plain, unprotected method. WindowServer and CoreAnimation
commit command buffers from more than one thread (the compositor, the display pipe and the
animation thread all have their own `MTLCommandQueue`-like objects; every queue in this plug-in
resolves to the same `rn_vk_shared()` singleton). Inside `commitBody` the code walks global state
(`noCopyBuffers`, the shared `VkQueue`, the shared `VkCommandPool`) with no lock. Vulkan requires
external synchronisation for a `VkQueue` and a `VkCommandPool`; `vkAllocateCommandBuffers` from
one pool and `vkQueueSubmit` on one queue from two threads is undefined behaviour. `syncNoCopyIn:`
takes `@synchronized` only to snapshot the weak table, then releases the lock and copies. Two
commits can therefore run `syncNoCopyIn:YES`, the recorded work and `syncNoCopyIn:NO` in any
interleaving.

**Why it matters:** This is the most direct explanation of a crash that starts when the pointer
moves: pointer motion makes WindowServer submit cursor updates from a second thread while the
compositor is already inside `commitBody`. The crash address is in `memmove` because the memory
being copied belongs to a caller (WindowServer) that has already released or resized it, or
because a concurrent dealloc/copy changed the buffer under the copy. Serialising commits removes
the whole class, including the "sometimes SIGBUS" variation.

**Fix:** Put one recursive or normal mutex around the entire body of `-commit` (not just the copy
loop), so that at most one command buffer is recorded, submitted, waited for and copied back at a
time. Longer term, give each `RedNaviMTLCommandQueue` its own `VkQueue`/`VkCommandPool` (or one
queue plus per-thread command pools with `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`), and
keep a per-queue in-flight fence. Do not share `rn_vk`'s pool between threads even after the
global lock is added, because the lock also serialises frames and costs the compositor its
pipelining.

---

## 2. `syncNoCopyIn:` copies every no-copy buffer in the process, in both directions, for every command buffer

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:160-163` (the method), `RedNaviMTL/compute.m:1494` (call with
`YES`), `RedNaviMTL/compute.m:1526` (call with `NO`), registration at
`RedNaviMTL/compute.m:1984-1992`.

**Symptom:** Stale geometry and cursor data; a buffer is overwritten with the contents the GPU
had after an older frame; under concurrent commits, the crash described in finding 1. Small
surfaces and vertex buffers "jump back" to an older state for a frame.

**Root cause:** `+syncNoCopyIn:` iterates `[[RedNaviMTLBuffer noCopyBuffers] allObjects]`, i.e.
every buffer created with `newBufferWithBytesNoCopy:` in the process, and copies all of them in
or out. It does not know which buffers the current command buffer actually references. The copy
is therefore neither scoped nor ordered with respect to the frame that owns the data.

Two concrete failures follow:

1. On `NO` (after the frame), every buffer is copied from the RADV staging buffer back to the
   caller's memory. A buffer that was not used by this command buffer still gets its old GPU
   contents written over newer CPU data that the caller prepared for the *next* frame. With
   double-buffered client storage this silently rolls the data back by one frame.
2. On `YES` (before the frame), a buffer that belongs to another in-flight command buffer is
   overwritten with the other frame's caller contents. Because commits are not serialised
   (finding 1), the two directions can interleave and both sides see a torn buffer.

**Why it matters:** WindowServer uses `newBufferWithBytesNoCopy:` for pooled client storage (the
plug-in's own comment at `compute.m:1539-1541` says so). Copying all pools on every commit turns
every frame into a race between all clients, and it is the mechanism by which a cursor update can
clobber the compositor's vertex data or vice versa. It is also O(total buffer bytes) per commit,
which is why frame rate collapses as more windows appear.

**Fix:** Record, per command buffer, the set of no-copy buffers actually referenced by its
passes, dispatches and blits (the same walk that builds `surfUsed` at `compute.m:1440-1455` can
collect them), and copy in only those buffers before the work and copy out only those after it.
Better still, make the buffer track a frame/sequence number: copy in only if the caller wrote
since the last copy, and copy out only if the GPU wrote. Keep the buffer's `userPtr` alive for the
duration of the command buffer (retain the buffer in the record, not only in a weak table).

---

## 3. The caller's no-copy memory is read and written without any synchronisation against the caller

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:160-164` (`syncNoCopyIn:`, `-contents`), registration at
`RedNaviMTL/compute.m:1984-1992`, `dealloc` at `compute.m:153-157`.

**Symptom:** Torn vertex/index/constant data, occasional crash inside the copy, and geometry that
deforms for one or more frames. The deformation follows the pointer because the cursor path is
the most frequently updated client storage.

**Root cause:** For a buffer made over caller memory, `-contents` returns the caller's pointer and
the caller is expected to write it directly. `syncNoCopyIn:YES` copies from that pointer on the
commit thread at an arbitrary time. There is no handshake (no lock, no `didModifyRange:` action,
no sequence number) telling the plug-in that a write is complete, and `-didModifyRange:` is an
empty method at `compute.m:172`. Metal's contract for `MTLStorageModeShared` is that writes made
through `contents` are visible to the GPU, but the driver still has to order its own copy against
them; here it does not. `-contents` can be written by WindowServer while `memcpy` reads, and the
copy can also race with WindowServer releasing the allocation.

Note that `dealloc` calls `userDealloc` (which may free the caller's memory) after removing the
object from the weak table, but `syncNoCopyIn:` only holds the weak table snapshot. It does not
retain the buffer while copying. A buffer that is deallocated between `allObjects` and the
`memcpy` leaves `x->userPtr` dangling; `allObjects` returns a strong array in most Foundation
versions, but the code does not rely on a documented guarantee, and the `userDealloc` may free the
underlying bytes earlier than the buffer object dies.

**Why it matters:** Even with the global lock from finding 1, a correct driver cannot copy a
shared buffer at commit time without knowing the caller has stopped writing. This is a large part
of "the parts of the windows deform": a partially updated vertex buffer reaches RADV and the
window is drawn with mixed old and new vertices.

**Fix:** Implement `-didModifyRange:` and use it to mark the buffer dirty (copy in only then, at a
point where the caller has declared the write complete). For buffers that are written through
`contents` without `didModifyRange:`, copy in at commit as now, but take a per-buffer lock that
the caller cannot bypass only if the API allows it; at minimum retain the buffer across the copy
and never free `userPtr` before the copy finishes. Document that `contents` writes are not
synchronised, and make the copy hold a reference to the buffer so dealloc cannot free the bytes
mid-copy.

---

## 4. Texture views do not inherit the buffer/client-memory source, so views of client-storage textures are never synchronised

**Severity:** High
**Where:** `RedNaviMTL/compute.m:346-364` (`-viewWithPixelFormat:swizzle:`), the skip test at
`RedNaviMTL/compute.m:1435-1439`, `RedNaviMTL/compute.m:1543-1555` (buffer-backed textures),
`RedNaviMTL/compute.m:1561-1574` (client-memory textures).

**Symptom:** A texture view made from a texture that lives in a buffer or in client memory samples
stale or zero pixels. In WindowServer this appears as an image that is correct as a whole texture
but blank or garbled when CoreAnimation switches to a swizzled/alternate-format view; it can also
appear as a cursor image that never updates.

**Root cause:** `initWithDevice:` creates a view with `[[RedNaviMTLTexture alloc] init]`, copies
`img`, `dev`, `pf`, `w`, `h`, `d`, `ttype`, `alen`, `texel`, `usage_`, sets `parent` and `surf`,
but does not copy `srcBuf`, `srcOff`, `srcBpr`, `srcRaw`, `srcBase`, `srcLen`, `srcDealloc` or
`surfPlane`. `-syncFromBuffer` returns immediately when `!srcBuf && !srcRaw`
(`compute.m:316-317`). At commit, the `note` block only adds a texture to `surfUsed` when
`t->surf || t->srcBuf || t->srcRaw` (`compute.m:1436`), so a view of a buffer-backed texture is
not even considered. The root texture may not be referenced by the command buffer at all (the
client only binds the view), so nothing copies the buffer rows into the image.

The same omission applies to `surfPlane`: a view of a multi-planar IOSurface keeps `surf` but has
`surfPlane == 0`, so it reads the wrong plane.

**Why it matters:** Views are not an exotic path: QuartzCore creates swizzled views for
component order, and CoreAnimation uses views for client-storage images. A view that silently
loses its source is exactly the "some parts of a window are garbage" class of bug, and it is
deterministic, not a race, so it is worth fixing before the concurrency work is trusted.

**Fix:** Copy all source fields from the root when creating a view (`srcBuf`, `srcOff`, `srcBpr`,
`srcRaw`, `srcBase`, `srcLen`, `srcDealloc`, `surfPlane`). `srcDealloc` must not be called twice;
either keep a single owner on the root and have views not own it, or retain the root and let the
view delegate `syncFromBuffer`/`syncToSurface`/dealloc to it. Also make the `note` test ask
`parent ? parent->srcBuf : srcBuf` (or simply `source() != nil`).

---

## 5. The IOSurface seed shortcut uses `w < 1024` as a proxy for "the surface changed", which is not a correctness rule

**Severity:** High
**Where:** `RedNaviMTL/compute.m:284-297`, in particular the condition at line 287.

**Symptom:** Small IOSurfaces (cursor, status items, small layers) keep the pixels from their
first upload even after WindowServer changes them; large surfaces are copied on every command
buffer even when nothing changed, wasting bandwidth and widening the race window.

**Root cause:**

```objc
if (surfSeed && IOSurfaceGetSeed(surf) == surfSeed && w < 1024) return;
```

The comment explains the intent: the seed only changes for writes made under `IOSurfaceLock`, and
CoreDisplay's source surface is written by the compositor without that lock, so the copy must
happen every time. But the code only disables the shortcut for `w >= 1024`. The threshold is a
guess about which surfaces are written "by other means"; it has nothing to do with how a surface
is actually produced. A 64x64 cursor surface written by a path that does not bump the seed is
skipped forever, while a 1920x1080 surface that is indeed unchanged is copied on every commit.

**Why it matters:** The pointer is the most obvious victim: a cursor surface smaller than 1024
pixels wide will stop updating as soon as its seed happens to match, which is precisely the
"cursor leaves artefacts / shapes corrupt near the mouse" symptom. The inverse half of the
condition is a performance bug that keeps the CPU copies (and their races) in the hot path for the
main display surface.

**Fix:** Decide per surface, once, whether the producer bumps the seed. Observe the seed on every
copy: if a surface is used and its pixels changed while the seed did not, mark it
`seedReliable = NO` and never take the shortcut for it again. Otherwise take the shortcut for all
sizes. Until that bookkeeping exists, remove the `w < 1024` exception and always copy, which is
correct but slow; then optimise with the observed-reliability flag rather than a size threshold.

---

## 6. Whole-surface CPU copies happen at command-buffer borders while the display engine and other consumers are reading the same surface

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:284-315` (`syncFromSurface`, `syncToSurface`),
`compute.m:1456` (copy all used surfaces in), `compute.m:1521` (copy written surfaces out),
`RedNaviAccel2/kern_accel2.cpp:571-620` (`performTransaction`, the separate CPU copy into
scanout).

**Symptom:** Tearing, a horizontal band that trails the cursor, window content that shears or
repeats, and shapes that corrupt near the pointer. The effect changes with mouse speed and with
the number of windows, because those change the timing of the copy relative to scanout.

**Root cause:** Every IOSurface-backed texture is implemented as a packed host copy in RADV
memory. At commit the plug-in copies the whole surface into the host copy row by row, runs the
GPU work, then copies the whole host copy back into the IOSurface. The display pipe independently
copies the composed surface into the scanout in `performTransaction`, also on the CPU, with its
own `gXferLock` but without any coordination with the plug-in's copy. `IOSurfaceLock` is taken for
the copy, but it serialises only other `IOSurfaceLock` users; it does not stop the display engine
from scanning out, and the plug-in's own `syncToSurface` and `performTransaction` do not share a
lock. There is no vblank wait, no double buffering and no sequence number, so a frame can be
scanned out while it is being written.

The whole-surface copy also has no notion of the dirty region: `syncFromSurface` copies all rows
even when the client changed one cursor-sized rectangle. That makes the window during which the
surface is inconsistent as large as possible.

**Why it matters:** This is the most plausible cause of "the parts of the windows deform" and
"shapes corrupt near the mouse" that survives after the crash is fixed: the CPU is rewriting the
surface that the display is reading, and the pointer is the region that changes most often, so the
corruption is most visible there. The display pipe's copy is also synchronous in the transaction
callback, so it lengthens the vulnerable window on every refresh.

**Fix:** Short term, take one process-wide lock around both the plug-in's surface copy and the
display pipe's copy (they are in different processes, so this needs a shared IOSurface lock
protocol, not a mutex). Use `IOSurfaceLock` with the same lock type on both sides and hold it for
the whole copy; the display side should also take it. Medium term, stop copying whole surfaces:
keep a per-surface dirty rectangle and copy only that; the client already reports dirty regions
through the transaction. Long term, the correct design is to let the GPU write the IOSurface
directly (import the surface's memory into RADV or use a shared-tiled image), so that there is no
CPU copy at all; the current "copy model" is documented as a bring-up compromise in
`docs/M6_METAL_ON_RADV.md`, but the display path must not read a surface that is being rewritten.

---

## 7. The private `commitAndWait...` methods are aliases of the synchronous `commit`, and `waitUntilScheduled`/`waitUntilCompleted` do not honour their contracts

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1277-1285`, `compute.m:1532-1535`.

**Symptom:** Frame pacing collapses and WindowServer submits far more command buffers than the
GPU can retire; a client that calls `waitUntilScheduled` and then reuses a buffer can still be
inside the previous commit; a client that calls `waitUntilCompleted` from another thread returns
before the work is done.

**Root cause:** All three private variants are `[self commit]`, and `-commit` is synchronous: it
runs the recorded work with `vkQueueSubmit` + `vkWaitForFences` inside `rv_vk_render_pass` (and
the dispatch/blit paths) and returns only after the copies back. That was a deliberate bring-up
step (the comment at line 1281 says so) and it made the first non-black frame possible, but it
changes the API semantics:

- `waitUntilScheduled` is empty even before commit; SkyLight uses it to bound how far ahead it
  records. Returning immediately lets it run ahead arbitrarily.
- `waitUntilCompleted` calls `dispatch_semaphore_wait(done, DISPATCH_TIME_NOW)`, which never
  waits. In the current synchronous implementation the work is already done when `commit`
  returns, so it accidentally looks correct on the same thread; from another thread it is wrong.
- `commitAndWaitUntilSubmitted` waits for completion, not for submission, so the compositor
  blocks on every pass instead of overlapping passes.

**Why it matters:** WindowServer's frame loop is built around the real semantics. A synchronous
`commit` under the global lock (finding 1) serialises the entire system on one thread and makes
every animation frame wait for the GPU. It also makes the display pipe's "it happens to be right
because commit is synchronous" assumption in `docs/M6_DISPLAY_ACCEL.md:49` load-bearing; once the
commit is made asynchronous (which it must be for performance), the display path will race.

**Fix:** Give the command buffer a real state machine: submission is one queue submit with a
fence; `commitAndWaitUntilSubmitted` waits on a submission semaphore, `...Scheduled` waits on the
scheduled handler, `...Completed` waits on the completion fence. Do not run the recorded work
inside `commit`; run it in a worker or at least submit and return. Then make the display pipe
wait for the surface to be ready (finding 9) instead of relying on the synchronous commit.

---

## 8. Failure paths still copy stale GPU contents back over the caller's memory

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1494-1530`, especially the unconditional
`[RedNaviMTLBuffer syncNoCopyIn:NO]` at line 1526 and the `st != Error` guards at 1521 and 1527.

**Symptom:** After a failed pass or dispatch (unsupported format, too many bindings, a timeout,
a missing texture), the caller's buffers contain whatever the GPU staging buffer held from an
earlier frame. The next frame draws with old vertices or old constants, which looks like a window
that deforms and then stays deformed until the client rewrites the buffer.

**Root cause:** When `runRenderPass:` or `rn_run_blit` fails, `-fail:` sets
`st = MTLCommandBufferStatusError` and the loop breaks. `syncToSurface` is correctly guarded by
`if (st != MTLCommandBufferStatusError)`, but `[RedNaviMTLBuffer syncNoCopyIn:NO]` at line 1526
is not. It copies every no-copy buffer back even though the command buffer did not necessarily
consume the copy-in that preceded it (and even though it may have failed before any GPU work).
The same applies to buffers whose command-buffer work was skipped because `st` was already
`Error`.

There is a second, smaller instance: `-fail:` can be called after `st` was set to `Committed`, and
`commitBody` returns early on a second commit (`if (st >= MTLCommandBufferStatusCommitted)
return;`), so the scheduled/completed handlers are not re-run and the error is not reported to a
client that commits the same buffer again.

**Why it matters:** A failed frame should leave the caller's memory untouched. Writing stale GPU
data back turns a recoverable driver error into persistent visible corruption, and it can
overwrite a correct buffer that another thread prepared for the next frame.

**Fix:** Track which buffers were actually copied in and which GPU work actually read them; copy
back only those, and only if the command buffer completed without error. On error, drop the
staging contents (or restore the snapshot taken at copy-in, if the caller needs it). Do not treat
`Committed` as terminal for error reporting; keep a separate `completed` flag.

---

## 9. The display transaction ignores the transaction's destination, dirty bits and resource form, and does not wait for the GPU

**Severity:** Critical
**Where:** `RedNaviAccel2/kern_accel2.cpp:571-620`, with the plane accessors declared at
`kern_accel2.cpp:325-332`; the probe's scanout properties are read at lines 586-588.

**Symptom:** The screen shows an older frame, a frame from the wrong surface, or a frame copied
while the compositor is still writing it. Window contents shear and repeat near moving objects,
and after a display reconfiguration the pipe can keep copying to a stale physical address.

**Root cause:** `performTransaction` takes `getPlaneIOSurface(0, 0)` and unconditionally copies it
into the physical scanout address published by the probe (`fb-scanout-phys`, `fb-pitch`,
`fb-height`, `fb-width`). It does not look at `getPlaneResource(0, 0)` (which the code itself logs
and then ignores), does not look at the other planes or layers, and does not inspect
`getTransactionDirtyBits()` or `getTransactionOptions()`. If plane 0 layer 0 is a resource rather
than an IOSurface, the function returns 0 having done nothing; if the transaction is a flip to a
different surface, the code still copies to the same fixed scanout. It returns 0 (success) in
every one of those cases, including the "surface does not match the scanout" branch at lines
591-594, so the family believes the frame was shown.

There is no wait for the GPU to finish writing the surface. The comment in
`docs/M6_DISPLAY_ACCEL.md:49` says this "happens to be right because commit is synchronous", but
that is an undocumented coupling between two processes (the plug-in runs in WindowServer, the
pipe in the kernel) and it breaks as soon as finding 7 is fixed. The copy is also done with
`gXferLock` held, but nothing in the plug-in takes that lock, so it does not exclude the writer.

**Why it matters:** The display pipe is the last consumer of every frame. If it reads a surface
that is still being written, or reads the wrong surface, the result is exactly the visible
deformation and corruption reported. Returning success while copying nothing hides the failure
from the family and from WindowServer, which then never retries or reconfigures.

**Fix:** Implement the transaction contract: select the surface/resource the transaction names,
honour the dirty rectangle, and copy only that rectangle. Return a real error when the surface
cannot be used, so the family can retry. Wait for the producer to signal readiness (the reference
driver waits on a per-surface ready flag, up to about 100 ms) or, at minimum, take the same
IOSurface lock the plug-in takes around its copy. Validate pitch, format and size against the
actual scanout and update the cached physical address when the mode changes.

---

## 10. The display pipe caches one framebuffer pointer forever, and the framebuffer resource is built from guessed offsets and never registered

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:45` (`static IOFramebuffer *gFb`),
`kern_accel2.cpp:896-900` (`createDisplayPipe`), `kern_accel2.cpp:522-548`
(`initFramebufferResource`, `destroyFramebufferResource`), `kern_accel2.cpp:424` and `437`
(uses of `gFb`), `kern_accel2.cpp:528` and `586` (`gFb->getProvider()` in the hot paths).

**Symptom:** After a display reconfiguration, sleep/wake or framebuffer teardown, the pipe reads
properties from a freed or replaced framebuffer: `initFramebufferResource` returns NULL, the pipe
never becomes active, or `performTransaction` copies to a stale scanout address. A later
framebuffer can be ignored because `gFb` is non-NULL, so the pipe keeps using the old one.

**Root cause:** `createDisplayPipe` does `if (fb && !gFb) { fb->retain(); gFb = fb; }` and never
clears `gFb` on `stop`/`free`/`destroyFramebufferResource`. The retain prevents the object from
being freed, but not from being detached, terminated or replaced by a new framebuffer; the code
then uses `gFb->getProvider()` and the provider's properties as if they still described the live
display. `initFramebufferResource` also ignores the return value of `m->init(...)`, writes
`bytes` and `phys` at guessed offsets `+0x40` and `+0xc8` (logged at line 539 as "after init"),
and returns the object without calling the family's resource-registration path.
`destroyFramebufferResource` then calls the family with an object the family never registered.

**Why it matters:** A stale `gFb` is a use-after-free waiting to happen: the retain keeps the
memory mapped, but everything it points at (provider, properties, scanout) can be gone. The
guessed offsets are also a silent corruption risk: if the family's layout differs, the code
overwrites a field that means something else. This is not the immediate cursor crash, but it is
the reason a display change can leave the pipe permanently inactive or copying to the wrong
address, which looks like a corrupted desktop.

**Fix:** Keep the framebuffer association in the pipe object, not in a file-static, and clear it
in `stop`/`free`. Use the framebuffer pointer passed to `initFramebufferResource` (the family
passes one) instead of `gFb`. Check `m->init()` and fail the resource if it fails. Do not write
guessed offsets: add a typed accessor or verify the layout against the generated header and the
family's `sizeof`. Register the resource through the family's path, and make
`destroyFramebufferResource` a no-op (or symmetric) if registration never happened.

---

## 11. The image upload state is cleared when the copy is recorded, not when the submission succeeds

**Severity:** Critical
**Where:** `RedNaviMTL/rn_vk.c:489-508` (`images_in`), `rn_vk.c:527-590`
(`dispatch_regions`), `rn_vk.c:753-795` (`clear_targets`), `rn_vk.c:809-957`
(`rn_vk_render_pass`), and the failure exits at `rn_vk.c:950-952`, `572-575`, `789-792`.

**Symptom:** After a pass is rejected (a pipeline that does not match the attachment, a
buffer offset that is not aligned, a missing texture), subsequent frames sample the texture as
zero, black or whatever the image held before. The artefact is stable and appears in an arbitrary
part of a window; it does not heal until the client re-creates the texture.

**Root cause:** `images_in` does two persistent mutations while recording the command buffer:
it sets `r->initialized = 1` after recording the `UNDEFINED -> GENERAL` barrier, and it sets
`r->dirty = 0` after recording `vkCmdCopyBufferToImage`. Both are recorded before the draws. If
the pass then fails at `CmdBindVertexBuffers` (`rc = 6`), at `vkEndCommandBuffer`, at
`vkCreateFence` or at `vkQueueSubmit`, the code goes to `done:` and frees the command buffer
without ever submitting it. The image is still marked initialised (so the next pass skips the
layout transition) and not dirty (so the next pass never uploads the host copy). The same pattern
is in `dispatch_regions` and `clear_targets`.

**Why it matters:** A single unsupported draw permanently poisons the texture. WindowServer's
compositor uses a small set of long-lived surfaces, so one poisoned surface stays on screen and
is re-used for every frame; the corruption therefore looks like a deformed window part rather
than a transient glitch. It also makes the first failure much harder to diagnose because the log
reports the error once and the pixels are silently wrong afterwards.

**Fix:** Do not mutate `initialized` or `dirty` until the submission has succeeded (after
`vkWaitForFences` returns success, or in the completion callback). Keep the intended state in a
temporary local (`need_init`, `need_upload`) and commit it only on success. On failure, leave the
image state exactly as it was so the next use retries the upload and the layout transition.

---

## 12. Compute pipelines share one descriptor set and one variant cache across threads

**Severity:** Critical
**Where:** `RedNaviMTL/rn_vk.c:44-52` (`rn_vk_compute` owns one `ds` and a 16-entry `variants`
array), `rn_vk.c:289-307` (`pipeline_for`), `rn_vk.c:527-538` (`dispatch_regions` writes
`c->ds`), `rn_vk.c:450-485` (`fill_writes`), called from
`RedNaviMTL/compute.m:1506-1519` for every dispatch.

**Symptom:** A compute dispatch reads the buffers or images that another dispatch bound a moment
earlier. With two threads, the wrong texture is written or sampled, and the result can be a band
of garbage in a window. `pipeline_for` can also create two pipelines for the same local size and
lose one, leaking the driver object.

**Root cause:** `RedNaviMTLComputePipelineState` is a long-lived object and `rn_vk_compute` keeps
exactly one descriptor set (`c->ds`) and one descriptor pool (`maxSets = 1`). `dispatch_regions`
calls `UpdateDescriptorSets` on that same set immediately before submitting. Two command buffers
that use the same pipeline (the same shader, which is exactly what WindowServer does for blur,
shadow and colour-conversion passes) overwrite each other's descriptors. There is no lock, and
`pipeline_for` appends to `c->variants` and increments `c->nvariants` without a lock as well.

The dispatch path does submit and wait inside `rn_vk_dispatch`, so the window is "between the
descriptor write and the submit" rather than the whole frame, but that is still enough for two
threads to interleave, and it becomes the dominant bug as soon as the commits are made
asynchronous.

**Why it matters:** Descriptor corruption does not fail loudly; it produces a valid draw or
dispatch with the wrong resources, so the frame is subtly or grossly wrong rather than absent.
The variant race leaks pipeline objects and can return a pipeline whose local size does not match
the dispatch, which makes RADV read the wrong number of threads.

**Fix:** Allocate a descriptor set per dispatch from a per-thread or per-command-buffer pool, or
serialise `dispatch_regions` on the pipeline object with a mutex. Make `pipeline_for` take the
same lock, and never share a `VkDescriptorPool` between threads. The simplest correct version for
this bring-up is one descriptor pool and one set per command buffer, created at commit and
destroyed after the fence.

---

## 13. Texture state (staging copy, `dirty`, `initialized`, `surfSeed`) is not protected against concurrent command buffers

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:284-315` (`syncFromSurface`, `syncToSurface`),
`compute.m:316-321` (`syncFromBuffer`), `compute.m:1456` and `1521` (the loops that call them),
`RedNaviMTL/rn_vk.c:42` (the shared `stage` buffer, `initialized`, `dirty` in
`rn_vk_image`), `rn_vk.c:489-525`.

**Symptom:** The cursor or a window layer flickers between two images, shows half of one frame
and half of another, or displays rows from different frames. The artifacts move with the pointer
because the cursor texture is the one used by the most command buffers.

**Root cause:** The host copy (`rn_vk_image::stage`) is the single source of truth for a texture.
`syncFromSurface` writes it from the IOSurface and `syncToSurface` reads it back; `images_in` /
`images_out` also read and write it during the GPU submission. All of this is done without a
per-texture lock. Two command buffers that bind the same texture (the same window surface, the
same cursor, the same CoreDisplay LUT) can therefore copy in and out at the same time. `dirty`
and `initialized` are also plain `int` fields on the shared image, so a concurrent
`images_in` can clear `dirty` after the other thread's `syncFromSurface` set it but before the
other thread's submission reads it.

**Why it matters:** Even if finding 1 is fixed by serialising entire commits, the driver should
still be correct when two queues are allowed to make progress. WindowServer explicitly overlaps
the cursor and the compositor, so the cursor texture is a genuine concurrent resource, and the
symptom "shapes corrupt near the mouse" matches corruption of exactly that resource.

**Fix:** Give each texture a lock (or an owner/sequence number) covering `syncFromSurface`,
`syncFromBuffer`, the `images_in`/`images_out` staging access and `syncToSurface`. For textures
that are read-only in a command buffer, allow multiple readers but exclude the writer. Since
every texture is copied through a single staging buffer, the simplest correct implementation is
one mutex per texture; the copies are already the bottleneck, so the lock does not make the
common case slower.

---

## 14. The IOSurface seed is read after the copy and after unlocking, so a write that lands in between is recorded as already copied

**Severity:** High
**Where:** `RedNaviMTL/compute.m:284-297`, in particular lines 288-296, and
`compute.m:298-315` (lines 313-314).

**Symptom:** A small surface (the cursor, a status item) occasionally misses exactly one update
and then stays one frame behind until the next write. With a moving pointer this looks like a
cursor that lags, leaves a trail, or jumps back to a previous position.

**Root cause:** `syncFromSurface` does:

1. lock the surface read-only,
2. copy the rows,
3. unlock,
4. `surfSeed = IOSurfaceGetSeed(surf)`.

If a producer writes the surface between step 3 and step 4, the seed stored in step 4 includes
that new write, but the pixels copied in step 2 do not. The next call sees
`IOSurfaceGetSeed(surf) == surfSeed` and (for `w < 1024`, finding 5) skips the copy, so the new
pixels are never uploaded. `syncToSurface` has the same ordering: it writes the surface, unlocks,
then stores the seed, so a client write that lands between the unlock and the seed read is
masked.

**Why it matters:** This is a lost-update race that is invisible in a single-threaded test and
shows up exactly when the pointer and the compositor are both active. It is one of the plausible
explanations for "the cursor crashes/glitches a few seconds after it starts moving".

**Fix:** Read the seed while the surface is locked, before the copy, and store that value. Any
write after that read bumps the seed again and forces the next copy. For `syncToSurface`, read
the seed before the write and store that value, so a later client write is not masked by the
driver's own write. Better: replace the seed heuristic with an explicit dirty region from the
producer (finding 5 and finding 6).

---

## 15. An IOSurface-backed texture accepts any surface with the same bytes per element, including a different pixel format

**Severity:** High
**Where:** `RedNaviMTL/compute.m:267-282` (creation checks), `compute.m:290-293` and
`301-304` (row copy), `compute.m:2025-2036` (the SPI entry point).

**Symptom:** A layer is drawn with red and blue swapped, or with alpha interpreted as colour,
or with wrong gamma. Window parts look "solarised" or have coloured fringes instead of the
expected image.

**Root cause:** `initWithDevice:descriptor:iosurface:plane:` compares only the surface's bytes
per element with the texture's texel size:

```objc
const size_t sb = ... IOSurfaceGetBytesPerElementOfPlane(...);
if (w > sw || h > sh || texel != sb) { ... return nil; }
```

`IOSurfaceGetPixelFormat(io)` is never compared with `desc.pixelFormat`, and
`rn_vk_format_of(desc.pixelFormat)` maps the Metal format to a Vulkan format independently of the
surface. `'BGRA'` (0x42475241) and `'RGBA'` (0x52474241?) both have four bytes per element, so a
BGRA8 surface accepted with an `MTLPixelFormatRGBA8Unorm` descriptor is uploaded without a
channel swap, and every red and blue component is exchanged. The reverse is also accepted.
The code also does not check that the surface's bytes per row is at least `w * texel`; a
narrower pitch makes the row `memcpy` read into the next row.

**Why it matters:** The compositor's destination surface is BGRA (the display pipe checks
`0x42475241`), while QuartzCore may ask for an RGBA view for intermediate work. Accepting a
mismatched pair silently produces wrong colours and, for premultiplied alpha, wrong blending,
which can look like a corrupted shape rather than a simple colour swap. This is deterministic,
so it should be fixed before chasing the remaining timing bugs.

**Fix:** Compare the surface's `IOSurfaceGetPixelFormat` with the expected CoreVideo/IOSurface
format for the Metal pixel format (and the plane index), and either reject the mismatch or
record the swap and apply it in `syncFromSurface`/`syncToSurface` (or create the appropriate
Vulkan view with a component mapping). Add `pitch >= w * texel` and `pitch % texel == 0` to the
creation check, and validate them again in the copy in case the surface was reshaped.

---

## 16. IOSurface and client-memory textures transfer only the first slice of a 3D texture

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:284-297` (`syncFromSurface`, loop `y < h`),
`compute.m:298-315` (`syncToSurface`, loop `y < h`), `compute.m:316-321`
(`syncFromBuffer`, loop `y < h`), `compute.m:1543-1554` and `1561-1574` (the bounds checks),
`RedNaviMTL/rn_vk.c:377-401` (the image and its `w*h*d` staging buffer).

**Symptom:** A 3D texture (a volume LUT, a CoreImage-style 3D table) has its first slice correct
and the remaining slices black or stale. A volume rendering or a 3D lookup shows banding and
wrong colours outside the first slice.

**Root cause:** The image allocates `w * h * d` texels of staging and the Vulkan copy uses
`imageExtent = {w, h, d}` (`rn_vk.c:503` and `517`), but all three CPU-side synchronisers loop
only over `h` rows. `syncFromSurface`, `syncToSurface` and `syncFromBuffer` therefore touch one
slice. The bounds checks in `newTextureWithDescriptor:` and `rn_texture_over_bytes` also compute
`(t->h - 1) * bpr + w * texel` and ignore `d`, so a 3D texture over client memory can be accepted
even when the memory only covers one slice.

**Why it matters:** The plug-in explicitly accepts 3D textures and `docs/M6_METAL_ON_RADV.md`
describes them, so a client that uses a 3D texture will get a silently wrong image rather than a
clear failure. It is not on the WindowServer cursor path today, but it is a correctness hole in
the same copy code and is cheap to fix.

**Fix:** Loop `for z in 0..d` and copy `h * w * texel` bytes per slice from
`base + z * h * pitch` (for a surface) or `base + z * h * srcBpr` (for client memory), and make
the bounds checks use `d`. For an IOSurface, confirm the plane really has `d` slices; if the
surface is 2D, reject a 3D descriptor instead of reading past the plane.

---

## 17. The scanout copy writes through a write-combined mapping without a barrier, so the display engine can scan out a partially written frame

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:554-568` (`gScanMap`, `kIOMapWriteCombineCache`),
`kern_accel2.cpp:599-605` (the row copy), `kern_accel2.cpp:617-620` (release/complete and
return).

**Symptom:** Horizontal tearing that follows the cursor, a band of the previous frame at the
bottom or right of the screen, and rows that mix two frames. The effect is worse when the frame
contains a lot of changed pixels (a dragged window, a moving cursor).

**Root cause:** `scanoutCPU` maps the scanout aperture with `kIOMapWriteCombineCache`, and
`performTransaction` fills it with `memcpy`. Write-combining buffers are weakly ordered: stores
can still be sitting in the CPU's write-combining buffers when `performTransaction` returns and
the display engine reads the surface. There is no `OSSynchronizeIO()`, `__sync_synchronize()` or
other fence between the last store and the return, and the code does not wait for a vertical
blank. On x86 a write-combining store is not ordered with respect to the display engine's reads
without an `sfence`/`mfence`.

**Why it matters:** This is the classic "CPU blit to a scanout while the display scans it"
tearing. Even if the copy were perfectly synchronised with the compositor, the absence of a
flush means the last rows of the frame can be missing on the screen. On a 1920x1080x4 frame the
copy is about 8 MB, which is large enough for the write-combining buffers to be a visible
fraction of the frame.

**Fix:** Call `OSSynchronizeIO()` after the row copy (and before returning success) so the
write-combining stores are visible to the display engine. For correctness across the frame,
either wait for the vertical blank before starting the copy (or use the display engine's
double-buffered flip), or copy into an off-screen buffer and flip. The current design copies
directly into the live scanout, so even a barrier only narrows the window; it does not remove
tearing.

---

## 18. The display resource's size and the transaction's copy size come from different snapshots of the mode

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:522-544` (`initFramebufferResource` captures
`pitch * height` once), `kern_accel2.cpp:586-599` (`performTransaction` re-reads
`fb-scanout-phys`, `fb-pitch`, `fb-height`, `fb-width` on every transaction),
`kern_accel2.cpp:558-567` (`scanoutCPU` re-maps when `phys` or `bytes` changes).

**Symptom:** After a mode change or a display reconfiguration, the first frames are torn or
shifted, the lower part of the screen shows old content, or the pipe stops updating. The display
resource is the size of the old mode while the copy uses the new one.

**Root cause:** `initFramebufferResource` computes `bytes = pitch * height` from the properties at
the moment the pipe is built and stores that size in the `RedNaviScanoutMemory` at `+0x40`. The
family uses that object for `wire`/mapping. `performTransaction` independently reads the same
properties again and calls `scanoutCPU(phys, pitch * height)`, which may map a different range.
Nothing updates the resource object when the mode changes, and nothing rejects a transaction
whose geometry differs from the resource's. If the mode change only alters pitch/height but not
the physical base, `scanoutCPU` can keep an old mapping whose length no longer matches the copy,
and the resource's recorded size is stale.

**Why it matters:** A display mode change is a normal event (login window, resolution change,
monitor hot-plug). A stale resource size can make the family's `wire`/prepare path succeed for a
range that is not the live scanout, and the pipe then writes outside the intended surface. The
result is exactly a corrupted desktop after a resolution change.

**Fix:** Store the geometry in one place (the pipe object) and update it from the framebuffer's
mode-change notification. Reject a transaction whose `phys`, pitch or height differs from the
registered resource, or tear down and rebuild the resource through
`initFramebufferResource`/`destroyFramebufferResource` when the mode changes. Leave the size of
the mapping to the resource object and never re-derive it independently in the transaction.

---

## 19. Every display reconfiguration leaks a scanout memory object, a mapping and the framebuffer reference

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:292-294` (`gScanout` ring),
`kern_accel2.cpp:535-544` (`OSTypeAlloc` and store, no release of the previous entry),
`kern_accel2.cpp:554-567` (`gScanMap`), `kern_accel2.cpp:45` and `898` (`gFb` retain),
`kern_accel2.cpp:554` and `700` (`gXferLock`).

**Symptom:** Kernel memory and wired pages grow with every display change or WindowServer
restart. On a machine that is reconfigured often (the log in
`docs/M6_DISPLAY_ACCEL.md:26` says 35 pipe rebuilds in 10 minutes), this is a slow leak that
eventually causes allocation failures and a black screen.

**Root cause:** `initFramebufferResource` allocates a `RedNaviScanoutMemory` for every pipe build
and stores it in a 64-entry ring. The ring overwrites the oldest pointer without calling
`release()` on the object, so the object (and the `IOAccelResource2` it references) is never
freed. `gScanMap` is released only when the physical address or size changes; on teardown it is
never released. `gFb` is retained in `createDisplayPipe` and never released. `gXferLock` is
allocated in `probe` and never freed.

**Why it matters:** These are kernel objects, so the leak is not recoverable by restarting
WindowServer. The 64-entry ring makes it look bounded, but it only bounds the lookup table, not
the objects. A long-running machine with a hot-plugged display will exhaust memory.

**Fix:** Make the ring an owning ring: `release()` the entry being replaced, and release all
entries in `stop`/`free`. Release `gScanMap` and `gFb` in the same teardown, and free
`gXferLock` in `stop`. Better, keep the scanout memory in the pipe object and destroy it in
`destroyFramebufferResource`, which is the lifecycle the family already calls.

---

## 20. `record()` can write one entry past the end of `attrTable` when two threads add a new attribute at the same time

**Severity:** Critical
**Where:** `RedNaviFB/kern_fb.cpp:298-308` (`attrTable[48]`, `attrTableN`, `record`), with
callers at `kern_fb.cpp:391-457` and the other attribute overrides. `record` is also called from
`enableController`'s timer and from the connection attribute paths, so it runs on more than one
thread.

**Symptom:** A kernel panic or silent corruption of the framebuffer object after WindowServer
queries two new attributes concurrently; in the best case a corrupted `attr-table` property.

**Root cause:**

```cpp
AttrRec *e = nullptr;
for (uint32_t i = 0; i < attrTableN; i++) ...
if (!e && attrTableN < 48) { e = &attrTable[attrTableN++]; e->kind = kind; e->sel = sel; }
```

`attrTableN` is a plain `uint32_t` member. Two threads can both observe `attrTableN == 47`, both
pass the `< 48` test, both increment it (to 49), and one of them writes through
`&attrTable[48]`, which is one element past the array. The array is followed by other members of
`AMDRedNaviFB`, so this is a write into the object's own fields (or padding), not a wild pointer,
but it is still memory corruption in the kernel. The same non-atomic pattern exists for
`attrPos`, `calls[]`, `attrLog[]`, `attrRaw[]` and for `overrideN`/`overrides` in
`setProperties`.

**Why it matters:** WindowServer polls framebuffer attributes continuously, and the diagnostic
code was added to the same hot path that is already under stress during the crash. A race that
writes past an array is exactly the kind of bug that turns a cosmetic glitch into a panic.
Unlike the user-space plug-in, this is in the kernel, so there is no crash report that points at
the line.

**Fix:** Guard `attrTable`, `attrTableN`, `overrides`, `overrideN`, `calls`, `attrLog`,
`attrRaw` and `attrPos` with one `IOLock` (or make the counters atomic and clamp the index
before the store). `record` should take the lock, find or append the entry, copy the values, and
release before calling `setProperty`. Do not call `setProperty` while holding a lock that a
property callback could re-enter. If the table is full, drop the record instead of growing it.

---

## 21. Every framebuffer attribute access publishes registry properties and samples the scanout, in the WindowServer hot path

**Severity:** High
**Where:** `RedNaviFB/kern_fb.cpp:63-69` (`note`), `kern_fb.cpp:71-102` (`publishDiag`),
`kern_fb.cpp:300-308` (`record`), callers at `kern_fb.cpp:391`, `408`, `418`, `425`,
`557`, and the timer at `kern_fb.cpp:485`.

**Symptom:** WindowServer becomes sluggish as soon as it uses the accelerated path; the cursor
lags, animations drop frames, and occasionally the userspace watchdog restarts WindowServer.
The effect scales with the number of framebuffer attribute queries, which is high during window
resize, display reconfiguration and compositor start.

**Root cause:** `note()` is called at the top of every `setAttribute`, `getAttribute`,
`setAttributeForConnection` and `getAttributeForConnection`, and it calls `publishDiag()`.
`publishDiag` allocates an `OSDictionary`, four `OSData` objects and two `OSNumber` objects,
reads 16 pixels from the scanout through an uncached mapping (`kIOMapInhibitCache`), and calls
`setProperty("fb-diag", d)` on every call. `record()` independently builds an `OSData` from the
48-entry attribute table and calls `setProperty("attr-table", d)` on every attribute access.
Both are synchronous kernel allocations and registry updates in a path that WindowServer calls
per frame.

The scanout reads are particularly expensive: `diagMap` is mapped with `kIOMapInhibitCache`, so
each of the 16 reads is an uncached access to VRAM through BAR0. Sixteen of them per attribute
query, plus the allocation of the property dictionary, is milliseconds of work on a path that is
supposed to be a few instructions.

**Why it matters:** This is not a cosmetic logging problem. A slow `getAttribute` stalls the
compositor, which then misses its frame deadline; the display pipe still copies the previous
surface, and the user sees lag and tearing. It also makes the crash investigation harder because
the diagnostic code itself perturbs the timing being investigated.

**Fix:** Remove `publishDiag()` from `note()` and remove the `setProperty` from `record()`.
Update the diagnostic properties from the existing 1 Hz `diagTimer` only, and only while a
boot argument or a registry property asks for diagnostics. Keep the in-memory counters cheap
(atomic increments) and publish them on demand. The hot path should do at most one atomic
increment per attribute.

---

## 22. `publishDiag` can use `diagMap` after `setDisplayMode` released it, and reads it with stale geometry

**Severity:** Critical
**Where:** `RedNaviFB/kern_fb.cpp:57` (`diagMap`), `kern_fb.cpp:71-81` (read in
`publishDiag`), `kern_fb.cpp:283-291` (map creation and `stop`), `kern_fb.cpp:650-668`
(`setDisplayMode` releases and re-maps it), `kern_fb.cpp:485` (timer calls `publishDiag`).

**Symptom:** A kernel panic or a hang during a display mode change, or a random panic while the
desktop is animating. The fault is a read through a released `IOMemoryMap` or past the end of the
mapped scanout.

**Root cause:** `diagMap` is a plain member, read and written without a lock. `publishDiag` runs
from the 1 Hz timer, from `enableController`, from `getApertureRange` and from every attribute
call. `setDisplayMode` runs on the WindowServer thread and does:

```objc
if (diagMap) { diagMap->release(); diagMap = nullptr; }
... diagMap = md->map(kIOMapInhibitCache);
```

Between the `release()` and the assignment, another thread can enter `publishDiag`, see the
non-NULL pointer, and call `getVirtualAddress()` on a freed map. Even without the free, the
geometry (`fbWidth`, `fbHeight`, `fbPitch`) is updated non-atomically, so `publishDiag` can
combine a new height with an old pitch and read past the mapping.

**Why it matters:** This is a kernel use-after-free, not a diagnostic inconvenience. It is
reachable from normal display reconfiguration, which WindowServer performs during login,
resolution changes and monitor hot-plug. It also explains panics that do not point at the Metal
plug-in at all.

**Fix:** Protect `diagMap` and the geometry with one `IOLock`, or take the map out of the hot
path entirely (finding 21). In `setDisplayMode`, build the new map first, swap it under the
lock, and release the old one after no reader can hold it. Never call `getVirtualAddress()` on
a map that another thread may have released.

---

## 23. The graphics-control answer to command 0x980 returns kernel object addresses to user space

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:391-395` (the structure), `kern_accel2.cpp:416-425`
(the answer), `kern_accel2.cpp:396-398` (the user-facing entry point).

**Symptom:** A user process that opens the graphics-control user client can read the kernel
addresses of the PCI device object and of the framebuffer object. With those addresses and a
separate kernel read/write primitive (or a bug in another user client), KASLR is defeated and
the objects become targets.

**Root cause:** `c->pci = reinterpret_cast<uint64_t>(gControlPCI)` and
`c->framebuffer[0] = reinterpret_cast<uint64_t>(gFb)` are written straight into the out buffer
of `vendor_doDeviceAttribute`, which is the user client's method. The kernel pointers are
meaningful only inside the kernel; the reference driver's structure presumably holds
`io_service_t` handles or registry IDs, not raw pointers, but this code publishes the pointers.

**Why it matters:** The driver already exposes several user clients; leaking kernel object
addresses turns any unrelated information leak into a reliable exploit primitive. Even without
a second bug, it violates the usual IOKit rule that user space refers to kernel objects by
handle, not by address.

**Fix:** Put an `io_service_t`/`io_object_t` or a registry entry ID in the field and translate it
on the way back in. If the consumer only needs a nonzero identity, use a small opaque token.
At minimum, build the answer with `kIOReturnBadArgument` when the request comes from a
non-privileged client, and never write raw kernel pointers into a user-visible structure.

---

## 24. The 0x921 timing answer subtracts mode fields without validating them

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:441-460`, especially lines 449-452.

**Symptom:** A monitor with a malformed or unusual EDID, or a mode whose `htotal`/`vtotal` is
not greater than the active size, produces a nonsense detailed timing. CoreDisplay then rejects
the display pipe, or programs a mode with a huge blanking interval and the screen goes black.

**Root cause:** The code computes:

```cpp
tv.horizontalBlanking = m->htotal - m->hactive;
tv.verticalBlanking = m->vtotal - m->vactive;
```

with `m` coming from the probe's `fb-timing` property. `RNModeTiming` is filled from the EDID;
nothing checks `htotal > hactive`, `vtotal > vactive`, `hsync <= htotal`, `vsync <= vtotal`,
`pclk_khz != 0`, or that the refresh is in a sane range. The fields are 16-bit unsigned, so a
smaller total wraps to a large blanking value.

**Why it matters:** The display pipe is what makes the compositor visible. A single bad mode
answer is enough to make the screen black even though the Metal path is working. This is a
cheap, local validation and it protects the bring-up from monitor-specific EDID quirks.

**Fix:** Validate the timing before filling `IODetailedTimingInformationV2`: require
`hactive`, `vactive`, `htotal`, `vtotal` and `pclk_khz` nonzero; require `htotal > hactive` and
`vtotal > vactive`; require the sync offsets and widths to fit inside the totals; and clamp or
reject the refresh. Return `kIOReturnUnsupported` for a bad timing instead of publishing it.

---

## 25. `commitIntoGPUPageTable` and `updateGPUPageTable` report success for every map without writing a page table

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:295-317` (`isScanoutMap`,
`commitIntoGPUPageTable`, `updateGPUPageTable`), `kern_accel2.cpp:308-310` (the comment that
admits nothing is written).

**Symptom:** Today the display path reads surfaces on the CPU, so nothing visible happens. The
moment any consumer uses the accelerator's GPU page tables (a 2D/GL/CL context, a future Metal
backend, or a family path that maps the surface for the engine), the GPU reads or writes
physical address 0 or whatever stale PTE was there. The failure is silent and looks like random
memory corruption.

**Root cause:** The functions return `1` unconditionally. `isScanoutMap` is used only for a log
message; it does not change the answer. No PTE is written for the scanout map or for any other
map. The comment says this is deliberate because `IOAccelMemoryMap::prepare()` only needs a
truthy answer for the CPU-copy path, but the API contract is "the GPU can now access this
memory", and the code does not implement it.

**Why it matters:** A false success in a kernel driver is worse than a clean failure: the family
believes the mapping is valid and proceeds, so the error surfaces later as corruption in an
unrelated object. This is also the kind of assumption that breaks when the display path is
changed to use the GPU (which is the stated direction of the project).

**Fix:** Implement the page-table write for the maps that will really be used, or return failure
for maps the driver cannot map and make the CPU-copy path not depend on the GPU mapping. If the
latter is not possible, keep the truthy answer but make it explicit in a single well-named
function (for example `cpuOnlyMapAllowsPrepare()`), and assert that no engine ever consumes it.
Do not leave a function named `commitIntoGPUPageTable` that does not commit anything.

---

## 26. `allocPhysical` reports success without allocating, and `getPhysicalSegment` returns a fabricated address from guessed offsets

**Severity:** High
**Where:** `RedNaviAccel2/stubs.inc:13-17` (`RedNaviVidMemory`),
`RedNaviAccel2/kern_accel2.cpp:234-242` (`RedNaviScanoutMemory::getPhysicalSegment`),
`kern_accel2.cpp:537-542` (the guessed `+0x40` and `+0xc8` writes).

**Symptom:** A family path that wires or maps the memory believes it has a physical segment and
programs the display engine or the GPU with an address that is not the surface. The screen can
show memory from an unrelated allocation, or the engine can write over it.

**Root cause:** `allocPhysical()` returns `1` with no allocation. `getPhysicalSegment()` reads
the total length and physical address from offsets `+0x40` and `+0xc8` that the code itself
wrote from guesses, then returns `phys + offset` for any offset inside the total. There is no
validation that `phys` is the live scanout, no check that `offset + length` stays inside the
surface, and no check that the object is the one the family thinks it is. The values are correct
only because this driver writes them; the family's own `init` may also write those fields, and
the comment at `kern_accel2.cpp:221-222` says the layout is unverified.

**Why it matters:** The display engine reads the scanout at a physical address. If the guessed
offset changes in a macOS update, or if the family's `init` overwrites `+0xc8`, the engine scans
out an arbitrary address. That is a direct path to a corrupted or blank screen, and it is not
detectable from user space.

**Fix:** Use a typed accessor or a verified layout instead of raw offsets. Store the physical
address and length in the driver's own subclass fields (the class already has 272 bytes and can
add members only if the vtable/size contract allows; otherwise use an external table keyed by
the object). Validate `offset` and `length` in `getPhysicalSegment`, and check
`m->init()` before writing. If the layout cannot be verified, fail the resource rather than
guessing.

---

## 27. The scanout-map detection scans arbitrary object words for pointers, so any map that happens to contain one is treated as scanout

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:292-301` (`kScanoutRing`, `isScanoutMap`),
`kern_accel2.cpp:302-307` (the call in `commitIntoGPUPageTable`),
`kern_accel2.cpp:312-316` (`updateGPUPageTable`).

**Symptom:** A non-scanout memory map is classified as scanout, so the driver treats it as the
display's memory: the log says "scanout" for an unrelated map, and any future code that keys
behaviour off `isScanoutMap` takes the wrong branch. The 64-entry ring also means an old
scanout pointer can match a new object.

**Root cause:** `isScanoutMap` treats the map object as an array of `uint64_t` and looks for a
word equal to one of the 64 `gScanout[]` pointers, scanning words 2 through 34. There is no type
information and no ownership check. Any field that happens to equal a scanout memory pointer
(a copy, a back-pointer, an address in the same slab) is a false positive. Conversely, if the
family changes its layout and the pointer moves outside words 2-34, the real scanout map is
missed. The ring is not an owning ring, so entries stay in the table after the object is gone
(finding 19).

**Why it matters:** The heuristic is the only thing deciding whether a map gets special
treatment. A false positive can make the driver skip a real mapping or apply scanout-specific
behaviour to a texture; a false negative makes the display path fail. Both are silent.

**Fix:** Give the scanout memory a unique tag or a subclass marker and check it through the
object's metadata, not by scanning bytes. If the family has no accessor, keep an explicit set of
`(map, memory)` pairs created in the driver's own `newMemoryMap`/`initFramebufferResource` path,
and remove entries in `destroyFramebufferResource`. Do not infer identity from arbitrary words.

---

## 28. `configIsHeadless` forces a non-headless answer for every call and every display

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:640`, `kern_accel2.cpp:859-865`.

**Symptom:** On a machine with more than one GPU, or with this GPU attached but no display, the
accelerator tells WindowServer it has a head. WindowServer then selects the GPU compositor for a
display the accelerator cannot drive, and the screen stays black or the compositor falls back
after a timeout.

**Root cause:** The override ignores the family's answer unless the
`rednavi_accel2_headless` boot argument is set:

```cpp
bool fam = IOGraphicsAccelerator2::configIsHeadless();
return keep ? fam : false;
```

The default is "there is always a head", which is true for the single RX 7800 XT desktop this
bring-up targets, but not for a laptop with a discrete GPU and no external monitor, or for a
second accelerator instance. The function has no access to the framebuffer's connection state.

**Why it matters:** The compositor choice is made once, early, and is expensive to undo. A wrong
answer here is the difference between a working desktop and a black screen on any configuration
other than the one being tested.

**Fix:** Answer from the actual framebuffer/display state: return `false` only when this
accelerator has at least one active framebuffer with a connected display, and return the family's
answer (or `true`) otherwise. Keep the boot argument as an override, but make the default
correct for the general case.

---

## 29. `createUserGPUTask` creates a task with one allocator while the kernel task needs four

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:939-954`, in particular
`createUserGPUTask()` at line 951 and the comment at lines 952-954.

**Symptom:** A user client that allocates GPU virtual address space through the task reads a
null allocator for indices 1-3 and panics in `IOAccelTask::allocate`, or gets an address from
the wrong range. The kernel task had exactly this failure and was fixed by passing four; the
user task still passes one.

**Root cause:** `makeTask` builds four `IORangeAllocator` objects and passes the count `n` to
`t->init(this, n, ra)`. The comment explains that `IOAccelTask::init` copies `n` allocators and
that `allocate` indexes the table with the index in the memory map; the kernel task was changed
from 0 to 4 after a null-allocator panic. `createUserGPUTask` still calls
`makeTask("createUserGPUTask", 1)`, so any user-space allocation that uses index 1, 2 or 3 reads
past the copied table.

**Why it matters:** The same panic that was diagnosed and fixed for the kernel task is still
reachable from user space. WindowServer or a Metal client that asks for a surface through the
accelerator can trigger it, and the panic is in the kernel, not in the plug-in.

**Fix:** Pass four allocators to both tasks, or pass a count that matches the number of entries
the family actually copies and make `allocate` reject an out-of-range index. Add a log of the
index used by `IOAccelTask::allocate` so the correct count is measured rather than guessed.

---

## 30. `newSysMemory` returns an object that has never been initialised, and the family's initialiser is assumed rather than checked

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:244-254` (`RedNaviSysMemory`),
`kern_accel2.cpp:967-970` (`newSysMemory`), `kern_accel2.cpp:212-217` and
`docs/M6_ACCEL_DISPLAY.md`'s note that the family's `withMemoryDescriptor` wraps the surface.

**Symptom:** If the family's `IOAccelSysMemory::withMemoryDescriptor` does not initialise the
object returned by `newSysMemory`, the object has zeroed fields and the transaction end either
fails or uses a null memory descriptor. The display pipe then never becomes active, which looks
like a black screen with no Metal error.

**Root cause:** `newSysMemory` does `OSTypeAlloc(RedNaviSysMemory)` and returns it without
calling `init`. The comment says the shipped AMD subclass overrides only `init` and `free` and
that the family's implementations do the work, but the factory contract is not verified: a
factory that returns an uninitialised object relies on the caller to call `initWithMemoryDescriptor`
(or `init`) later. There is no log of whether that call happens, and no check of its result.

**Why it matters:** The display path currently works, so the assumption happens to hold on this
macOS build. The same code on a different build, or after the family's `withMemoryDescriptor`
changes, fails silently at the point where the display pipe is set up. A one-line log of the
init call and its result would turn a black screen into a diagnosable failure.

**Fix:** Check the family's initialisation path and call the appropriate `init` from the factory
if the family does not. Log the result of `init` and the object's memory-descriptor field. If the
object must stay uninitialised until the family calls `init`, document that contract next to the
factory and assert that the family's call happened before the first transaction.

---

## 31. The per-primitive pipeline cache is filled without a lock, so two command buffers can race and one of them can use a destroyed pipeline

**Severity:** High
**Where:** `RedNaviMTL/compute.m:640-655` (`renderForPrimitive:cull:clockwise:`), the
`byKey[5 * 6]` array at `compute.m:622`, its use at `compute.m:1355` and `compute.m:1919`,
and the destruction in `-dealloc` at `compute.m:631`.

**Symptom:** Occasional hard crashes or draws that use a stale pipeline while WindowServer
composites, typically seconds after the desktop starts moving (the same window that shows the
cursor bug), and corrupted geometry in the area a draw covers. The crash is often in RADV rather
than in this file.

**Root cause:** `byKey[key]` is a plain C array on a shared `MTLRenderPipelineState`. The
method does a check-then-create:

```c
if (byKey[key]) return byKey[key];
...
byKey[key] = rn_vk_render_create(vk, &d);
```

Nothing serialises it. Two command buffers committed from two threads (WindowServer has more
than one compositor thread) can both see `NULL`, both call `rn_vk_render_create`, and the second
store overwrites the first pointer. The first pipeline is then never destroyed, and the array is
written from two threads with no atomic. Worse, `-dealloc` walks the same array and calls
`rn_vk_render_destroy` while another thread may be inside `commitBody` holding a pointer it read
from the array. Metal guarantees that a pipeline state object is immutable and safe to use from
any thread; this cache violates that by making the object lazily mutable.

**Why it matters:** A destroyed or duplicated `rn_vk_render` is a use-after-free of a Vulkan
pipeline, which is exactly the kind of fault that appears as a RADV or GPU-hang crash rather
than a clean error. Because the crash is in the driver, it also takes down WindowServer, which
matches the user-visible "the cursor crashes everything after a few seconds".

**Fix:** Create every pipeline eagerly in the initialiser (the object already has the SPIR-V and
the vertex/fragment bindings at that point), or protect `byKey` with a lock and keep a reference
count. If lazy creation stays, store into a local, publish with an atomic compare-and-exchange,
destroy the loser, and do not destroy pipelines from `-dealloc` until all command buffers that
could reference the pipeline state have completed.

---

## 32. `replaceRegion:` and `getBytes:` bypass the commit ordering, ignore levels and slices, and do not validate the caller's strides

**Severity:** High
**Where:** `RedNaviMTL/compute.m:400-409` (`replaceRegion:...`), `compute.m:413-421`
(`getBytes:...`), `compute.m:411` and `compute.m:423` (the convenience forms), and the
surface/buffer synchronisation at `compute.m:284-309` and `compute.m:317-322`.

**Symptom:** Cursor images and small UI textures that WindowServer uploads with
`replaceRegion:` appear with stale or partially updated pixels, or the upload is lost entirely
after the next frame. Reads with `getBytes:` return the previous frame. Large textures can also
crash with a heap overflow or read the wrong rows.

**Root cause:** `replaceRegion:` writes the host copy and immediately calls `[self
syncToSurface]`. That is a complete surface copy performed at the moment of the call, outside
any command-buffer ordering:

* If another thread's command buffer is mid-commit on the same surface, the two copies race.
  `syncFromSurface` at the start of the next commit can then overwrite the freshly written host
  copy with whatever the surface held when that command buffer started.
* For a texture with `srcBuf`/`srcRaw` (client storage), `commitBody` calls `syncFromBuffer` at
  `compute.m:1456`, which overwrites the `replaceRegion:` write with the client buffer contents.
* The method returns immediately for any `level != 0` or `slice != 0`
  (`compute.m:401`), although `1DArray` textures are supported and their slices map to rows.
  An upload of slice *n* therefore silently does nothing.
* `bytesPerRow`/`bytesPerImage` are never checked. A `bytesPerRow` smaller than
  `region.size.width * texel` makes the per-row `memcpy` read across rows, and a
  `bytesPerImage` smaller than a plane makes the `z * bpi` arithmetic read past the caller's
  buffer. `getBytes:` has the mirror problem writing into the caller's buffer, including a heap
  overflow on the last row when `bpr` is too small.

**Why it matters:** This is the shortest path by which a client's CPU-side pixels reach a
displayed surface, and both directions currently run unsynchronised against the GPU and the
display engine. The cursor and the small damaged rectangles near the pointer are exactly the
textures that are small enough to be updated this often.

**Fix:** Make `replaceRegion:`/`getBytes:` record an upload/download operation against the
texture and apply it at the commit boundary, in the same queue as the GPU work, rather than
copying immediately. Support `level == 0` with `slice` mapped to the row offset for 1D arrays,
reject only what is genuinely unsupported, and validate that `bpr >= width * texel` and
`bpi >= bpr * region.size.height` before touching the caller's memory. `getBytes:` must also
wait for any in-flight commit that touched the texture.

---

## 33. Vertex and index buffer ranges are not validated against the draw count, so a malformed draw reads past the end of the buffer

**Severity:** High
**Where:** `RedNaviMTL/rn_vk.c:925-937` (`CmdBindVertexBuffers` and `CmdDraw`/`CmdDrawIndexed`),
`RedNaviMTL/compute.m:1365-1377` (where the vertex buffers are collected), and
`RedNaviMTL/rn_vk.c:454-470` (`fill_writes`, which validates only the offset).

**Symptom:** GPU faults or hangs, or corrupted geometry near the window being drawn, when a
draw's `vertexCount`/`indexCount` plus offset extends past the end of a vertex or index buffer.
This is easy for a compositor to trigger with a temporarily wrong buffer size during a resize.

**Root cause:** The only checks are:

```c
if (!vbf || !vbf->buffer || vbf->offset >= vbf->buffer->size) { rc = 6; goto done; }
...
if (dr->index_offset >= dr->index_buffer->size) { rc = 4; goto done; }
```

An offset of `size - 1` passes, then `CmdDraw` reads `vertexCount * stride` bytes from there.
The pipeline knows `vlbind` but the draw does not carry the per-binding stride, and
`first_vertex`/`base_vertex` are not validated either. The index path has the same problem with
`index_offset + vertex_count * (index32 ? 4 : 2)`. RADV will happily issue the fetch; what
happens next depends on whether the neighbouring allocation is mapped, which is why the failure
looks like random corruption rather than a clean validation error.

**Why it matters:** WindowServer reallocates and reuses buffers continuously; a single stale size
during a window resize should be a failed draw, not a GPU read of another process's or the
kernel's memory. This is a direct contributor to "shapes corrupt" and to driver resets.

**Fix:** Carry the vertex stride and the attribute sizes in `rn_vk_draw`, or pass the bound
buffer length with each `rn_vk_vbuf`, and check
`offset + (first_vertex + vertex_count) * stride <= size` before binding. Check
`index_offset + index_count * index_size <= size` for indexed draws, and check
`base_vertex + vertex_count <= bufferSize / stride`. Return a clean error (rc 4) instead of
submitting the draw.

---

## 34. The blit path ignores `bytesPerImage` and the source slice, so copies between an array texture and a buffer read the wrong slice

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1024-1062` (the `copyFromBuffer:`/`copyFromTexture:`
methods), `compute.m:958-1002` (`rn_run_blit`), and the `RNBlit` fields at
`compute.m:934-946`.

**Symptom:** A copy into or out of an array texture (the gamma tables and some cursor/atlas
uploads) shows the first slice's contents in every slice, or reads unrelated bytes from the
source buffer. Depth-3D blits are reported as an error even when the caller asked for a legal
copy.

**Root cause:** `RNBlit` has no `bpi` field at all. The encoder stores `bpr` but drops
`sourceBytesPerImage`/`destinationBytesPerImage` on the floor. For a 1D array the code handles a
slice by adding it to the texture coordinate:

```c
else if (slice) { if (b->dt && b->dt->alen > 1 && slice < b->dt->alen) b->dorg.y += slice; ... }
```

but leaves `b->soff` unchanged, so the source data still starts at slice 0. The reverse
direction has the same bug. In addition `rn_run_blit` rejects `sz.depth > 1` for buffer copies
and never applies `bytesPerImage`, so a caller that legitimately copies several planes gets a
logged failure instead of the copy.

**Why it matters:** The cursor and gamma paths use array textures; a wrong slice means the
cursor can be uploaded as a different image, and a wrong source offset means the copy reads
bytes that belong to another resource. Both are visible as corrupted shapes near the pointer
and can also feed garbage into the compositor's atlases.

**Fix:** Store `sourceBytesPerImage`/`destinationBytesPerImage` in `RNBlit`, advance the source
offset by `slice * bpi` (and the destination for the reverse direction), and implement
`depth > 1` using `bpi` as the plane stride. Validate `bpi >= bpr * height` before the loop.

---

## 35. `performTransaction` leaks the IOSurface memory descriptor and holds the global transfer lock for the whole copy

**Severity:** High
**Where:** `RedNaviAccel2/kern_accel2.cpp:571-620`, especially the descriptor obtained at
`kern_accel2.cpp:595` and the early return at `kern_accel2.cpp:597`; the cached map in
`kern_accel2.cpp:554-568`.

**Symptom:** Kernel memory is exhausted after the compositor has been running for a while
(minutes), followed by allocation failures, a black screen, or a panic in the memory allocator.
During the copy the whole display pipe stalls, so moving the window or the cursor can make the
desktop visibly freeze for the duration of one full-frame copy.

**Root cause:** `surf->getMemoryDescriptor()` returns a newly created `IOMemoryDescriptor` with
a reference that belongs to the caller. The method never calls `md->release()`. The early
return when `gXferLock` is still null also drops it. Every display refresh (`performTransaction`
is called once per frame) leaks one descriptor plus its internal bookkeeping. The same
function then takes `gXferLock` around `prepare`, `map`, the full `pitch * height` memcpy and
`complete`, so a second display pipe or a second transaction waits behind an operation that can
take milliseconds.

**Why it matters:** At 60 frames per second this is 60 descriptor leaks per second, which is
unbounded growth in the kernel. A compositor that runs for minutes will fail allocations that
are not obviously related to the display, which is exactly the kind of failure that looks like
"everything crashes after a few seconds" once memory pressure reaches the wrong allocation.

**Fix:** Release `md` on every path (including the `!gXferLock` return), for example with a
small RAII guard. Narrow `gXferLock` to protect only the cached `gScanMap` lookup, and do the
copy with a per-pipe lock or, better, replace the CPU copy with a GPU copy or an imported
surface so the transaction does not need a full-frame memcpy under a global lock.

---

## 36. A failed `init`/`attach` in `attachGraphicsControl` still calls `detach`, and the global provider pointer is left published

**Severity:** Medium
**Where:** `RedNaviAccel2/kern_accel2.cpp:483-491` (`attachGraphicsControl`), the global at
`kern_accel2.cpp:378`, and its use in the 0x980 answer at `kern_accel2.cpp:423`.

**Symptom:** On a boot where the graphics control fails to initialise (allocation failure or a
family refusal), either an immediate panic during bring-up or a later failure when CoreDisplay
queries the control object: command 0x980 reports a PCI object that was never successfully
attached, so the display code waits for a control object that does not exist.

**Root cause:** The success test is

```c
bool ok = g->init(nullptr) && g->attach(gProv) && g->start(gProv);
if (!ok) { g->detach(gProv); g->release(); }
```

Short-circuit evaluation means `init` can fail and `attach`/`start` are never called, yet
`detach(gProv)` is called unconditionally. Detaching an object that was never attached, and
whose `init` did not complete, calls into base-class state that was never set up. In addition
`gControlPCI = gProv` is assigned before the result is known and is never reset on failure, so
the 0x980 answer keeps advertising a provider for a control object that has been released.
`gProv` is also not retained, so the pointer can dangle if the provider terminates.

**Why it matters:** This is the object the display machine uses to find the accelerator. A
half-initialised or dangling answer here produces a black screen or an immediate panic with no
Metal error, which is hard to attribute to this line.

**Fix:** Track which steps succeeded and undo only those (`if (attached) g->detach(gProv);`),
set `gControlPCI` only after `start` succeeds, retain the provider while it is published, and
clear it (with a release) when the control is detached or the service stops. If `init` can fail,
do not call `detach` on the uninitialised object.

---

## 37. The vertical-blank interrupt is a fixed 60 Hz timer that is not tied to the mode's refresh rate, drifts, and fires while the display is off

**Severity:** Medium
**Where:** `RedNaviFB/kern_fb.cpp:516-522` (the timer source and its `setTimeout(16667, ...)`),
`kern_fb.cpp:532-537` (`registerForInterruptType`), and `kern_fb.cpp:289-290` (teardown).

**Symptom:** Frame pacing judders or runs at the wrong cadence on non-60 Hz modes, the VBL
callback fires while the panel is asleep or the framebuffer is powered down, and repeated
rescheduling accumulates drift. WindowServer's frame deadline misses then show up as cursor
stutter and tearing.

**Root cause:** The timer is created once with a hard-coded 16667 µs period. It ignores the
selected mode's `refreshRate`, ignores whether the framebuffer is powered, and uses
`setTimeout` (relative) from inside the callback, so each tick adds the callback and workloop
latency to the period. There is no relation to the real scanout position; the reference driver
uses the hardware VBL. The callback also dereferences `self->vblProc` after
`unregisterInterrupt` may have cleared it, without any ordering.

**Why it matters:** The compositor uses the VBL callback to decide when it is safe to reuse a
surface. A fake, drifting or early VBL means the compositor writes a surface while the display
engine is still reading it, which is one of the mechanisms behind the observed tearing and
"deformed window parts". Firing during power-off can also touch a framebuffer that is being
reconfigured.

**Fix:** Derive the period from the active mode (`1e9 / refreshHz` nanoseconds), use
`setDeadline` with an absolute schedule to avoid drift, and skip the callback while the display
is not powered or the framebuffer is not active. Guard `vblProc`/`vblTarget` with the same lock
used by register/unregister, and cancel the timer in the power-off path, not only in `stop`.

---

## 38. `getApertureRange` and `getVRAMRange` hand out physical ranges that are never checked against the BAR, and `getVRAMRange` guesses the BAR base by masking

**Severity:** Medium
**Where:** `RedNaviFB/kern_fb.cpp:556-563` (`getApertureRange`) and `kern_fb.cpp:565-572`
(`getVRAMRange`).

**Symptom:** A kernel panic or a machine check when WindowServer maps the aperture, or a mapping
that silently aliases another device's registers. This is more likely on a mode change or on a
GPU with a BAR layout different from the one assumed here.

**Root cause:** `getApertureRange` builds `IODeviceMemory::withRange(scanoutPhys, pitch * height)`
from properties published by the probe. `IODeviceMemory::withRange` does not validate that the
range lies inside a BAR owned by this provider; it will create a memory object for any physical
range. `getVRAMRange` then masks the scanout address with `~0x0fffffff` and returns 256 MB,
assuming both a 256 MB alignment and a 256 MB BAR. If `scanoutPhys` is below 256 MB the base
becomes 0, and if the BAR is smaller than 256 MB the range extends past it. Neither function
consults the provider's actual memory ranges.

**Why it matters:** The aperture is mapped by user space and, in the write-combined case, can be
used to drive MMIO. Mapping beyond the BAR is not a recoverable error on x86; it faults the
machine. This is a stability and security problem independent of the compositor bugs.

**Fix:** Record the BAR's base and length in the probe (from the PCI provider's memory ranges),
validate that `[scanoutPhys, scanoutPhys + pitch * height)` is contained in it, and derive the
VRAM range from the recorded BAR rather than by masking an address. Return `nullptr` and log if
the request is outside the BAR.

---

## 39. `setAttribute` calls `handleEvent` re-entrantly, before and after the base call, producing duplicate or out-of-order power notifications

**Severity:** Medium
**Where:** `RedNaviFB/kern_fb.cpp:390-404` (`setAttribute`), `kern_fb.cpp:376-388`
(`setPowerState`), and the comment at `kern_fb.cpp:357-360` that a `handleEvent` from the power
thread hung the boot.

**Symptom:** Duplicate `WillPowerOff`/`DidPowerOn` notifications, or a deadlock when the
notification path calls back into `setAttribute`. The display machine can end up believing the
framebuffer is off while it is on (or the reverse), which leaves WindowServer without a
composable display after a sleep/wake.

**Root cause:** In the `fbExp() & 8` path the method calls `IOFramebuffer::setAttribute` and then
`handleEvent(kIOFBNotifyDidPowerOn)` (or the reverse for power-off), and the `notifyOn()` path
adds another `handleEvent` before the override check. `handleEvent` can synchronously call back
into the framebuffer and into the display machine, which is the re-entrancy the file's own
comment records as hanging the boot. Notifications are also emitted even when the base call
returned an error, so a failed power transition still tells the display machine that it
succeeded.

**Why it matters:** Power state is what gates the display machine's use of the pipe. A duplicate
or premature notification makes the accelerator compose into a framebuffer that the display
machine has parked, or stop composing while the screen is live. Both look like corruption or a
frozen desktop after the first sleep.

**Fix:** Make the notification a consequence of a completed state transition: call the base
first, check its result, and only then emit exactly one event, outside any lock the base holds.
Do not call `handleEvent` from `setPowerState`/`setAttribute` directly if it can recurse; post
the event to the work loop, or rely on the base class's own notification once
`registerPowerDriver` is made to work.

---

## 40. `powerStateWillChangeTo`/`powerStateDidChangeTo` return "will acknowledge later" but never acknowledge, so the power transition never completes

**Severity:** Medium
**Where:** `RedNaviFB/kern_fb.cpp:359-360` (`powerStateWillChangeTo` and
`powerStateDidChangeTo`), with the surrounding comment that calling the base implementation
panics because its controller is null.

**Symptom:** Sleep or wake hangs, or the power manager times out and logs an unacknowledged
power change; the framebuffer never reaches a new state and the display machine keeps a stale
view of whether the display is on. The compositor then either stops presenting or paints into a
powered-down display.

**Root cause:** Both overrides return 0. In IOKit, 0 is `kIOPMAckImplied`: it means "I have
started the transition and will call `acknowledgePowerChange` when it is done". Neither method
calls `acknowledgePowerChange`, calls the base implementation, nor records the new state. The
power state machine therefore waits forever for an acknowledgement that never comes. The
comment explains why the base cannot be called here, but replacing a panic with a permanent
stall is not a completed transition.

**Why it matters:** The display pipe's activation and the framebuffer's own state both hang off
this machine. A stuck transition is why the code elsewhere has to force `DidPowerOn` by hand,
which is the source of the duplicate-notification problem in finding 39.

**Fix:** Implement the transition explicitly: record the target state, perform the framebuffer
side effects, return `kIOPMAckNow` (1) when the transition is complete, or call
`acknowledgePowerChange(this)` later. If the base implementation cannot be used because it
dereferences the missing controller, do not register a power driver at all and document that
this framebuffer is not power-managed; do not pretend to be mid-transition forever.

---

<!-- Findings 41 onward are appended below. -->



## 41. Buffer-backed and client-memory 3D textures only ever upload the first slice, and their size checks ignore the depth

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:316-322` (`syncFromBuffer`), `compute.m:1542-1555`
(`newTextureWithDescriptor:offset:bytesPerRow:`), `compute.m:1561-1574`
(`rn_texture_over_bytes`), with the image created at `compute.m:256-257`.

**Symptom:** A 3D texture created over a buffer or over client memory (for example a
volume/lookup texture used by a CoreAnimation effect) draws only its first slice; the remaining
slices are transparent black. A buffer that is only large enough for one slice is accepted,
so the check that is supposed to protect the caller's memory does not describe the actual
layout.

**Root cause:** `initWithDevice:descriptor:` creates the image as
`w x (is1d ? alen : height) x (is3d ? depth : 1)`, so `h` is the row count of one slice and
`d` is the slice count. `syncFromBuffer` then copies only `for (y = 0; y < h; y++)`, which is
exactly one slice, and never advances by a slice stride. The two constructors validate
`offset + (h - 1) * bytesPerRow + w * texel <= len`, again only one slice; neither multiplies
by `d` nor adds the per-image stride. For a buffer-backed texture Metal's implied image
stride is `bytesPerRow * height`, so the correct bound is
`offset + (d - 1) * bytesPerRow * h + (h - 1) * bytesPerRow + w * texel`, and
`syncFromBuffer` must walk all `d` slices.

**Why it matters:** This is silent data loss rather than a validation error: the texture is
reported as created, the draw succeeds, and the result is wrong. It also means the bounds check
can pass for a buffer that is too small, so a future change that does sync all slices would
read past the end of the caller's memory.

**Fix:** In `syncFromBuffer`, loop over `z` in `0..d` with a slice stride of
`srcBpr * h` (and add a separate `srcBpi` if the caller's layout is not packed that way). In
both constructors, include `(d - 1) * bytesPerRow * h` in the size check. Reject 3D
buffer/client-memory textures if no well-defined stride is supplied instead of guessing.

---

## 42. `replaceRegion:` on a buffer-backed or client-memory texture is overwritten by the next commit

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:400-409` (`replaceRegion:`), `compute.m:1456`
(`for (RedNaviMTLTexture *t in surfUsed) { [t syncFromSurface]; [t syncFromBuffer]; }`),
`compute.m:316-322`.

**Symptom:** A caller uploads pixels with `replaceRegion:mipmapLevel:withBytes:bytesPerRow:`
and the texture still draws the old contents at the next frame. Reading the texture back with
`getBytes:` before the commit returns the new pixels, which makes the failure look like a
GPU-side problem.

**Root cause:** `replaceRegion:` writes into the host copy (`rn_vk_image_staging(img)`) and
calls `syncToSurface`, which is a no-op when the texture has no `surf`. At the start of every
command buffer that uses the texture, `commitBody` calls `syncFromBuffer` for every texture
whose `srcBuf` or `srcRaw` is set, which copies the buffer/client memory over the host copy
again, discarding the replacement. `getBytes:` does not call `syncFromBuffer`, so it still
observes the replacement, hiding the ordering problem.

**Why it matters:** Uploads through `replaceRegion:` are a normal path for small images and
for layers whose contents come from the CPU. Losing them produces stale or blank window
content and is hard to attribute because the object reports success.

**Fix:** Define one source of truth per texture. For buffer/client-memory textures either
reject `replaceRegion:` with a clear log (Metal does not support CPU writes through a texture
that aliases a buffer in the general case), or track a host-dirty range that `syncFromBuffer`
does not overwrite. Do not let `getBytes:` observe a state that the GPU will never see.

---

## 43. Depth and stencil attachments are silently dropped, and a depth-only pass returns a nil encoder

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1317-1318` (render pass creation),
`compute.m:1142-1150` (`setDepthStencilState:` and the other ignored state),
`RedNaviMTL/rn_vk.c:920-921` (the implicit viewport/scissor).

**Symptom:** Passes that use depth testing draw in encode order with no occlusion, so nearer
and farther geometry overlap incorrectly; window parts that should be hidden show through and
appear deformed. A pass whose only attachment is a depth or stencil texture returns `nil`,
and every subsequent encoder message becomes a no-op, so that pass disappears without an
error in the command buffer.

**Root cause:** The render pass descriptor reads only `colorAttachments[0..7]`; depth and
stencil attachments are logged and ignored. `setDepthStencilState:` is routed to `ignored:`,
so the pipeline has no depth test or write, and `renderCommandEncoderWithDescriptor:` returns
`nil` when `rp->nt == 0`. The emitter then keeps sending `setRenderPipelineState:`,
`drawPrimitives:` and `endEncoding` to `nil`, which Objective-C accepts silently. Vulkan's
viewport and scissor are defaulted to the target size, so the geometry is drawn, just with the
wrong visibility.

**Why it matters:** Depth-tested geometry is common in compositor effects (shadows, rounded
corners built from overlapping quads, 3D transforms). Rendering it without depth is a direct
visual corruption, and the nil-encoder path converts an explicit unsupported feature into an
unexplained missing frame.

**Fix:** Implement a depth/stencil attachment in the render pass and the pipeline (Vulkan depth
attachment, `VkPipelineDepthStencilStateCreateInfo` from `MTLDepthStencilState`, correct
load/store actions), or fail the command buffer explicitly when a descriptor requests depth or
stencil. Never return `nil` from the encoder factory for a pass that the caller considers
valid; return a failing encoder or set the command buffer to the error state so the caller
sees the failure.

---

## 44. Completion and scheduled handlers added after `commit` never run, and the handler arrays are mutated without synchronisation

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:1278-1280` (`addScheduledHandler:`,
`addCompletedHandler:`), `compute.m:1430` (scheduled handlers run at the start of
`commitBody`), `compute.m:1528` (completed handlers run at the end),
`compute.m:1532-1535` (`waitUntilCompleted`).

**Symptom:** A client that commits a command buffer and then attaches a completion handler
(something Metal explicitly allows while the buffer has not completed) never gets the
callback. Resources that the handler was meant to release accumulate, and any code waiting on
the handler stalls. If another thread adds a handler while `commitBody` is enumerating the
array, the mutation races with the enumeration and can crash or skip handlers.

**Root cause:** `commit` is synchronous, so by the time `addCompletedHandler:` is called the
buffer is already `MTLCommandBufferStatusCompleted`, but the method only appends to
`completed`; nothing ever drains that array again. The arrays are plain `NSMutableArray`s with
no lock, and `commitBody` enumerates them on the committing thread while other threads may be
appending. Metal's contract is that a handler added before completion must run exactly once,
and that this is safe from any thread.

**Why it matters:** WindowServer, CoreAnimation and Metal Performance Shaders use completion
handlers to recycle drawables and temporary buffers. A dropped handler is a leak and a lost
frame; a racing append is a use-after-free or an enumeration exception. Both are cumulative in
a long-running compositor, which is consistent with a failure that appears after a few seconds
of activity.

**Fix:** Guard the handler arrays with a lock (or serialise them onto the command buffer's
queue). In `addCompletedHandler:`, if the status is already `Completed` or `Error`, invoke the
block immediately (asynchronously on the queue, matching Metal) instead of appending it.
Snapshot the arrays under the lock before invoking them, and make `st` transitions atomic.

---

## 45. A drawable is presented even when the command buffer failed

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1521-1529` (`commitBody`), `compute.m:1293-1295`
(`presentDrawable:`).

**Symptom:** After a failed pass, dispatch or blit, the layer still presents its drawable. The
screen shows a frame that was never rendered, a partially rendered frame, or the previous
frame with pieces of the new one; because the failure is only logged, the compositor keeps
presenting broken frames instead of dropping them. With a partially written IOSurface this is
exactly the "window parts deform" symptom.

**Root cause:** The drawables are collected in `presents` and the loop
`for (id<MTLDrawable> dr in presents) [dr present];` runs unconditionally at the end of
`commitBody`. The preceding `if (st != MTLCommandBufferStatusError)` guards only the
`syncToSurface` copy-back and the status assignment; it does not guard presentation. The
drawable's own `present` has no way to know that the buffer failed, so it publishes whatever
the surface contains.

**Why it matters:** Presenting after an error turns a recoverable command-buffer failure into
visible corruption, and it also defeats the error signal: the client sees a normal frame, so
it does not rebuild the pipeline or retry the work. In a compositor, that corruption is
composited onto the screen and then copied by other clients, so one bad pass spreads.

**Fix:** Present only when `st == MTLCommandBufferStatusCompleted`. On failure, do not call
`present`; mark the drawable as failed if the protocol allows it (or simply drop it) and make
sure the next frame is fully rendered. Log the drawable and the failure together so the
presentation decision is traceable.

---

## 46. An event wait blocks the committing thread for up to ten seconds, and every listener creates a polling thread

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1499-1504` (`encodeWaitForEvent:` execution),
`compute.m:812-819` (`waitFor:timeoutMS:`), `compute.m:821-827`
(`notifyListener:atValue:block:`).

**Symptom:** When an event that is supposed to be signalled by another command buffer or
another process is late, `commit` spins on the CPU for up to ten seconds and fails the buffer;
the WindowServer compositor thread is stuck for that whole time, so the cursor and all windows
freeze. Each `notifyListener:` registration also leaves a thread polling for up to a minute.

**Root cause:** `waitFor:` is a busy loop over the shared-memory value with a 200 µs sleep, and
it is called inline from the record loop in `commitBody` with a fixed 10 s timeout. There is no
condition variable or mach port to sleep on, and no cancellation when the command buffer is
abandoned. `notifyListener:` does not use the event's shared memory notification mechanism; it
spawns one `dispatch_async` block per registration that also busy-waits for up to 60 s.

**Why it matters:** Events are the ordering primitive between the compositor, the display
machine and client processes. A wait that blocks the compositor's own thread converts a
one-frame ordering delay into a multi-second UI freeze, and the polling threads add CPU load
proportional to the number of registered listeners. Since the whole commit architecture is
synchronous, there is no other thread that can make progress while the wait runs.

**Fix:** Make waiting sleep on a real primitive: a `dispatch_semaphore`/condition variable
signalled by `setSignaledValue:` (and by a shared-memory notification for cross-process
events), with a timeout that fails the *wait*, not the whole compositor. Never wait inline in
the thread that drives composition; either fail fast when the producer is not going to run, or
move the wait to the queue's own worker. Implement `notifyListener:` with one shared
notification source per event and value, not one polling thread per call.

---

## 47. Every pass, dispatch and blit is its own queue submission followed by an up-to-eight-second fence wait, so a commit is entirely serial and can freeze the UI

**Severity:** High
**Where:** `RedNaviMTL/rn_vk.c:544-566` (dispatch submit and wait),
`rn_vk.c:760-789` (render pass submit and wait), `rn_vk.c:946-951` (blit submit and wait),
called from `RedNaviMTL/compute.m:1495-1526`.

**Symptom:** A frame with many render passes takes the sum of every pass's GPU time plus one
submit and one fence wait per pass; the CPU never gets ahead of the GPU and the GPU is idle
between passes. If one submission hangs, `WaitForFences` blocks for eight seconds inside
`commit`, which is the WindowServer thread; the cursor stops and the system looks crashed.

**Root cause:** Each `rn_vk_dispatch`, `rn_vk_render_pass` and blit helper creates a fence,
calls `QueueSubmit` with one command buffer and immediately calls `WaitForFences(..., 8 s)`.
There is no per-commit command buffer that batches the records, and no async `commit`: the
public `commit` calls `commitBody` on the caller's thread. The eight-second timeout is applied
per submission, so a command buffer with N passes can block for N × 8 s before it reports an
error, and the error path does not cancel the remaining records.

**Why it matters:** This is the architectural reason the compositor cannot keep up: a
compositor must record a frame, submit it, and return to the run loop, with the GPU working
ahead. Here the compositor's own thread performs the GPU work synchronously, one record at a
time, so frame latency is the GPU time plus driver overhead and any hiccup is a UI freeze.
It also amplifies every other defect in this review, because a corrupted or slow resource
cannot be retried asynchronously.

**Fix:** Build one Vulkan command buffer per Metal command buffer: record all passes,
dispatches and blits into it, submit once, and signal the Metal completion handlers from a
fence or timeline semaphore on a worker thread. Keep `commit` non-blocking; implement
`waitUntilCompleted`/`commitAndWait*` as real waits on that completion. Use one overall
timeout with device-lost handling instead of an eight-second wait per record. Only then can
the CPU copy-back and the IOSurface handoff be moved off the compositor thread.

---

## 48. Every GPU connection allocates roughly 1.8 MB of fixed kernel tables and scans them linearly on the submit and close paths

**Severity:** Medium
**Where:** `RedNavi/kern_probe.cpp:1795` (`kMaxHandles = 32768`), `kern_probe.cpp:1801`
(`kMaxMaps = 65536`), `kern_probe.cpp:1820` (`kMaxSync = 512`), allocation at
`kern_probe.cpp:2024-2026`, `inMap` at `kern_probe.cpp:1841-1847`, `freeAll` at
`kern_probe.cpp:1852-1865`.

**Symptom:** Kernel wired memory grows by about 1.8 MB for every process that opens the
accelerator (128 KB of handle indices, about 1.5 MB of map entries, about 76 KB of sync
objects), even for a client that maps one buffer. Every `RN_SUBMIT` calls `inMap`, which walks
all 65,536 map slots, and `clientClose` walks both the map and handle tables. With several
clients, submission latency and close latency grow linearly and the fixed cost is paid per
connection.

**Root cause:** The tables are sized for the worst case and allocated eagerly in `start`
(`IOMalloc(sizeof(Map) * kMaxMaps)` and friends). There is no free list, hash or interval
structure: `inMap` is a linear scan of `maps`, `freeAll` is a linear scan of `maps` and
`boIdx`, and the map allocator at `kern_probe.cpp:2273-2279` scans all 65,536 entries to find
a free slot and a free handle. The `Map` struct is 24 bytes after padding, so the map table
alone is 1.5 MB.

**Why it matters:** The accelerator is opened by WindowServer and by every client that gets
the Metal plug-in, so this is a per-application kernel memory cost and a per-submit cost in
the compositor's hottest path. Wired memory cannot be paged, so a modest number of clients
consumes tens of megabytes and increases the chance of allocation failure under pressure.

**Fix:** Allocate the handle and map tables lazily and grow them in chunks, or replace the map
table with a sorted interval list / hash keyed by virtual address so `inMap` and slot lookup
are logarithmic or constant. Keep a free list for slots and handles. Size the sync table from
the actual number of sync objects, and free all of it in `start`'s failure paths.

---

## 49. Only seven GPU address spaces exist, so the eighth Metal client cannot connect and fails with a generic error

**Severity:** Medium
**Where:** `RedNavi/kern_probe.cpp:2585-2600` (`vmAlloc`, loop `for (v = 1; v <= 7; v++)`),
`kern_probe.cpp:2032-2035` (client `start`), `kern_probe.cpp:2503-2518`
(`newUserClient`).

**Symptom:** Once seven clients hold a VMID, the next `newUserClient` fails: `vmAlloc` returns
-1, the client's `start` returns false, and `newUserClient` returns `kIOReturnError`. The
process that was opening the accelerator sees Metal device or command-queue creation fail and
typically aborts or restarts. In a desktop with WindowServer plus several GPU-using
applications this is reachable without any unusual workload.

**Root cause:** The hardware exposes eight VMIDs, one of which the driver keeps, and each
connection is assigned a whole VMID. There is no multiplexing, no queue of waiting clients and
no fallback: `newUserClient` returns a generic error to a caller that has no way to distinguish
"out of address spaces" from a transient failure. The design assumes the number of GPU clients
is small and fixed, which is not true once the Metal plug-in is installed system-wide.

**Why it matters:** A hard limit on concurrent GPU clients in the compositor's own device
stack means an application can take the display down simply by being the eighth to start.
The failure is not recoverable at the Metal API level, so the user sees a crash rather than a
degraded application.

**Fix:** Share one VMID between clients that do not need isolation, or recycle VMIDs when a
client is idle, or return `kIOReturnNoResources` and make the user-space side retry with
backoff and fall back to the software path. At minimum, do not let a failed `newUserClient`
propagate as an indistinguishable error; log the reason and the number of active clients.

---

## 50. `newUserClient` detaches clients that were never attached, and client startup is not transactional

**Severity:** Medium
**Where:** `RedNavi/kern_probe.cpp:2503-2518` (`newUserClient`),
`kern_probe.cpp:2021-2040` (`IOAccelRedNaviClient::start`),
`kern_probe.cpp:2041-2048` (`free`).

**Symptom:** When `initWithTask:` or `attach:` fails, `newUserClient` calls `detach(this)` on
an object that was never attached to the provider. Depending on the IOKit build this is a
no-op, an assertion, or a corruption of the provider's child list; either way the error path
is not the one the code was written for. When `start` fails after `vmClient[vmid] = this` and
the fence buffer were published, the object is half-registered until `free` runs, and the
hang-recovery loop can observe it in that state.

**Root cause:** The failure test is one short-circuit expression:

```c
if (!c->initWithTask(...) || !c->attach(this) || !c->start(this)) {
    c->detach(this);
    c->release();
    return kIOReturnError;
}
```

so the code cannot tell which step failed, and it performs the same cleanup for all three.
`start` also publishes `gpuOwner->vmClient[vmid] = this` before calling
`IOUserClient::start(provider)`, so a failure of the base start leaves a published pointer
that is only cleared later by `freeAll` in `free`.

**Why it matters:** Error paths in a kernel driver are where a user-visible crash becomes a
kernel panic. The recovery loop reads `vmClient[v]` and calls `fenceNow()`/`fenceWord()` on
whatever it finds; a half-started client is not guaranteed to have a valid `fenceIdx`, and
`detach` on an unattached object violates IOKit's ownership contract. This is exactly the kind
of failure that appears when a client disconnects during startup, which a crash-restart loop
produces repeatedly.

**Fix:** Do the three steps separately, tracking `didAttach` and `didStart`; call `detach` only
when `attach` succeeded, `terminate` only when `start` succeeded, and always `release`. In
`start`, register the VMID and fence page only after `IOUserClient::start(provider)` returns
true, or roll them back explicitly on failure. Clear `vmClient[vmid]` and free the VMID and
fence buffer on every failure path, not only from `free`.

---

<!-- Findings 51 onward are appended below. -->

## 51. Buffers are destroyed without unmapping their Vulkan memory, which is invalid API usage

**Severity:** Medium
**Where:** `RedNaviMTL/rn_vk.c:160-190` (`rn_vk_buffer_create` maps the memory at line 177;
`rn_vk_buffer_destroy` at lines 185-190 frees it without unmapping).

**Symptom:** Undefined behaviour at buffer destruction, ranging from a validation error on a
debug driver to a crash or memory corruption on a driver that keeps bookkeeping for mapped
ranges. Because most buffers are destroyed when a texture, a pipeline temporary or a client
arena goes away, this happens continuously in a compositor, not only at quit.

**Root cause:** `rn_vk_buffer_create` calls `vkMapMemory` and stores the pointer in `b->map`,
and nothing ever calls `vkUnmapMemory`. `rn_vk_buffer_destroy` calls `vkDestroyBuffer` and
`vkFreeMemory` directly. The Vulkan specification requires every mapped range of a memory
object to be unmapped before `vkFreeMemory`; the loader and RADV currently tolerate it in
many cases, but it is not a valid lifetime and it leaves the tracker with a stale mapping.

**Why it matters:** The plug-in creates and destroys a large number of host buffers (every
staging copy, every `setBytes` temporary, every texture host copy). A lifetime bug in the
single allocation primitive used by all of them is a systemic risk, and undefined behaviour
in the driver can surface as exactly the kind of sporadic corruption this review is chasing.

**Fix:** In `rn_vk_buffer_destroy`, call `vk->UnmapMemory(vk->dev, b->mem)` when `b->map` is
set before freeing the memory, and clear `b->map`. Add `UnmapMemory` to the loaded entry
points (`rn_vk.c:14` currently loads `MapMemory` but not `UnmapMemory`). As a second step,
consider keeping a persistently mapped arena instead of mapping one allocation per buffer.

---

## 52. The device advertises capabilities the plug-in does not implement

**Severity:** High
**Where:** `RedNaviMTL/compute.m:2189-2212` (`caps[]` and the `supports...` answers),
`compute.m:379-381` (`newRemoteTextureViewForDevice:`, `newSharedTextureHandle`,
`remoteStorageTexture`), `compute.m:1142-1151` (ignored render state),
`compute.m:443-462` (sampler mapping).

**Symptom:** A client that asks the device what it supports and then uses that path either
gets a nil object, a silently different behaviour, or a command-buffer failure. Examples that
follow directly from the table: `supportsSharedTextureHandles` is YES while
`newSharedTextureHandle` logs "not supported" and returns nil; `supportsSamplerCompareFunction`
is YES while the sampler has no compare state and depth testing is not implemented;
`supportsMirrorClampToEdgeSamplerMode` is YES while `MTLSamplerAddressModeMirrorClampToEdge`
is mapped to plain clamp-to-edge; `supportsTextureOutOfBoundsReads`,
`supportsDynamicAttributeStride`, `supportsCountingOcclusionQuery` and
`supportsBufferlessClientStorageTexture` are YES while the corresponding encoder methods are
ignored or only partly implemented.

**Root cause:** The capability table was written as "what a discrete Navi 32 on macOS supports
as far as the plug-in and the translator go", but several entries describe the hardware rather
than the implemented software path, and there is no test that ties every YES to a working
implementation. Metal clients legitimately branch on these answers: CoreAnimation chooses
shared-texture sharing, shadow sampling, mirror-clamp samplers or out-of-bounds reads because
the device told it they are available.

**Why it matters:** These are not cosmetic mismatches. A YES for shared texture handles makes a
client hand a handle that never arrives; a YES for compare samplers makes a compositor choose
a shadow path whose sampler state is ignored; a YES for mirror-clamp changes filtering at the
edges of every clamped layer. The result is wrong pixels or a failed allocation on a path the
client had every reason to take, which is consistent with visual corruption that depends on
what is near the cursor (an effect or a layer uses one of these paths).

**Fix:** Make every advertised capability true: either implement the path, or answer NO and
remove the dead method. Keep the table next to a checklist/test (`tools/mtl_limits.m` already
exists) that exercises each YES. In particular answer NO for shared texture handles, compare
samplers, mirror-clamp, out-of-bounds reads, dynamic attribute stride, occlusion queries and
bufferless client storage until they work end to end.

---

## 53. The advertised binding limits are larger than the encoder arrays, so valid binds are silently dropped at index 16

**Severity:** High
**Where:** `RedNaviMTL/compute.m:2218-2229` (`maxVertexBuffers`, `maxFragmentBuffers`,
`maxComputeBuffers` = 31; `maxFragmentTextures`, `maxComputeTextures` = 128;
`maxFragmentSamplers` = 16), `compute.m:875-908` (compute encoder `index < 16` guards),
`compute.m:1115-1136` (render encoder `index < 16` guards and 16-element arrays),
`compute.m:1904` (vertex buffer index 16 is a pipeline error), `compute.m:507, 529, 533`
(`rn_bind_args` only reads indices below 16).

**Symptom:** A pipeline that binds a resource at an index the device said is valid (17 or
higher for buffers, 16 or higher for textures) has the binding dropped at encode time. At
commit, `rn_bind_args` reports "no buffer bound at index N" or "no texture bound at index N"
and the command buffer fails, so the frame is lost. Vertex pipelines that declare a buffer at
index 16 or above fail at pipeline creation even though the device reports 31 vertex buffers.

**Root cause:** Every encoder stores bindings in fixed 16-element arrays and silently returns
when the index is 16 or more, while the limit table advertises 31 buffers, 128 textures and 16
samplers. `rn_bind_args` and `runRenderPass` also use `< 16`. Nothing validates the requested
index against the advertised limit at bind time, and nothing raises an error then: the drop is
silent until the commit fails, far from the call that caused it.

**Why it matters:** Clients size their binding indices from the limits the device reports.
WindowServer and QuartzCore pack many resources into high indices precisely because the limit
says they can. A silent drop turns a legal draw into a failed command buffer, and the
compositor loses the frame. It can also bind the wrong resource if a later bind reuses the
same slot for a different index.

**Fix:** Use arrays sized to the advertised maximum (32 for buffers and samplers, 128 for
textures) in the compute encoder, render encoder, `RNDraw`, `RNDispatch` and the binding
translation, and make `rn_bind_args` read the same maximum. If a resource must stay at 16,
advertise 16 in the limit table so clients never choose a higher index. In both cases, log an
error at bind time when the index is out of range instead of dropping it silently.

---

## 54. Sampler state loses mirror-clamp, compare, anisotropy and LOD bias, so filtering at edges and shadow paths differ from Metal

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:443-451` (`rn_address_of`), `compute.m:452-469`
(`rn_new_sampler_state`), `RedNaviMTL/rn_vk.c:192-213` (`sampler_info`).

**Symptom:** `MTLSamplerAddressModeMirrorClampToEdge` behaves as clamp-to-edge (no mirroring at
the edge), a compare sampler returns a colour instead of a comparison result, and anisotropic
filtering and LOD bias have no effect. Layers that rely on edge mirroring show a seam; shadow
or depth-compare effects either render nothing or render the sampled depth as colour.

**Root cause:** `rn_address_of` maps `MTLSamplerAddressModeMirrorClampToEdge` to 1
(clamp-to-edge); the `am[5]` table in `sampler_info` has no mirror-clamp entry and index 4 is
clamp-to-border, so the distinction cannot be expressed. `rn_vk_sampler_desc` has no compare
field at all, so `MTLSamplerDescriptor.compareFunction` is ignored. `maxAnisotropy`, `lodBias`
and `supportArgumentBuffers` are never read, and `lodMinClamp`/`lodMaxClamp` are the only LOD
parameters carried. The device table nevertheless advertises compare samplers and
mirror-clamp.

**Why it matters:** Sampling state is global to every layer and every effect. A wrong address
mode is visible at every window edge and every rounded corner; a missing compare path silently
breaks the effect that selected it; missing anisotropy softens text and images. These are
exactly the "shapes look wrong near the edges / near the cursor" symptoms, and they are cheap
to fix compared with the memory-corruption defects.

**Fix:** Add `VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE` as its own value and map
`MTLSamplerAddressModeMirrorClampToEdge` to it. Carry `compareFunction` (and the compare
enable) through `rn_vk_sampler_desc` into `VkSamplerCreateInfo.compareEnable/compareOp`, and
only advertise compare samplers once that works. Carry `maxAnisotropy` and `lodBias` into the
Vulkan sampler (clamping anisotropy to the reported maximum). If a field cannot be honoured,
log it once and answer the corresponding capability NO.

---

## 55. Every blocking wait in the user client runs with the connection lock held, for up to ten seconds

**Severity:** Medium
**Where:** `RedNavi/kern_probe.cpp:2081-2084` (`externalMethod` takes `lock` for the whole
switch), `kern_probe.cpp:2326-2358` (`RN_UC_SYNCOBJ_WAIT`), `kern_probe.cpp:2192-2224`
(`RN_UC_WAIT_SEQ`, including a possible queue reset), `kern_probe.cpp:2248-2262`
(`RN_UC_BO_WAIT`), `kern_probe.cpp:2158-2160` (input sync waits inside `RN_UC_SUBMIT`).

**Symptom:** While one thread of a client is in `RN_UC_SYNCOBJ_WAIT`, `RN_UC_WAIT_SEQ`,
`RN_UC_BO_WAIT` or waiting for an input sync object during submit, every other operation on the
same connection blocks: mapping or freeing a buffer, creating a BO, querying info, and
`clientClose`. A wait that is supposed to be a userspace concern pins a kernel thread and the
connection mutex for up to ten seconds; a hang recovery can then take even longer.

**Root cause:** `externalMethod` acquires the per-client `IOLock` before the `switch` and
releases it after, and the wait selectors implement their timeout as a loop of `IOSleep(1)`
inside that scope. `syncWaitOne` in `RN_UC_SUBMIT` is also called before the queue lock while
the client lock is still held. There is no condition variable or wait queue to sleep on, and no
way for `clientClose` or another thread to make progress while the wait is pending.

**Why it matters:** The client lock is the only protection for the handle and map tables, so a
long wait stalls all resource management for that process. WindowServer's own connection is
the one that creates and maps buffers while it composes; a blocked close or map during a
timeout is a visible stall, and holding a kernel mutex across a ten-second sleep is exactly the
pattern that turns a slow GPU into an unresponsive system.

**Fix:** Do the wait outside the lock: validate and snapshot the handles under the lock, drop
it, wait on a proper wait queue or `IOLockSleepDeadline` with a wakeup from the fence/queue
path, then reacquire to produce the result. Never hold the client lock across `IOSleep`. For
`RN_UC_SUBMIT`, resolve and validate the sync objects under the lock, release it for the wait
(while keeping the objects alive with a reference or the object table locked), and only then
take the queue lock to submit.

---

## 56. A queue reset advances every client's fence over dropped work, so sync objects report lost work as completed

**Severity:** High
**Where:** `RedNavi/kern_probe.cpp:2470-2484` (`resetQueue`), `kern_probe.cpp:2487-2501`
(`resetGfxQueue`), `kern_probe.cpp:1877-1912` (`tlNow`, `isDone`, `signalNow`),
`kern_probe.cpp:2212-2223` (`RN_UC_WAIT_SEQ` result).

**Symptom:** After a hang and reset, timeline or binary sync objects whose work was in the
dropped range report as signalled. A client that waited for a buffer to be filled proceeds with
uninitialised or stale contents; a compositor that waited for the previous frame's copy to
finish starts the next frame on top of it. The only path that notices the loss is
`RN_UC_WAIT_SEQ`, which sets `status = RN_UC_SEQ_LOST`, but the sync-object waits do not
consult `lostFrom/lostTo` at all.

**Root cause:** `resetQueue` records `lostFrom = before + 1`, `lostTo = seqSubmitted` and then
writes `seqSubmitted` into the connection's fence page. `reached(seq)` and `seqDone` compare
only against that fence value, so every sequence up to `seqSubmitted` now looks done.
`tlNow`, `isDone`, `isAttached` and the sync-object wait loop never test whether the point's
sequence falls in the lost range. The timeline therefore moves past work that never executed.

**Why it matters:** A reset is the recovery path for the failures this review is about, so the
behaviour after a reset determines whether the system recovers or silently produces garbage.
Reporting dropped work as complete converts a GPU hang into a data-corruption event and hides
it from the client, which will keep composing frames from buffers that were never written.

**Fix:** Make lost work a first-class result. When a point's sequence is in
`[lostFrom, lostTo]`, `tlNow`/`isDone` must return an error or a `LOST` state, and the sync
object wait must return `RN_UC_SEQ_LOST` instead of success. Do not advance the fence value
over work that did not run; instead publish a separate "lost through" counter that waits check
before the normal fence value, and have `RN_UC_WAIT_SEQ` report it. The client must be able to
distinguish "done" from "dropped" and rebuild its resources.

---

## 57. Buffers can be freed and mappings torn down while submitted GPU work still references them

**Severity:** High
**Where:** `RedNavi/kern_probe.cpp:2132-2143` (`RN_UC_BO_FREE`), `kern_probe.cpp:2049-2058`
(`clientClose`), `kern_probe.cpp:1852-1865` (`freeAll`), `kern_probe.cpp:2526-2540`
(`destroyBo`).

**Symptom:** A client frees a buffer (or exits) while a command buffer that reads or writes it
is still executing. `destroyBo` unmaps the pages and flushes the GART, so the GPU either walks
PTEs that no longer describe the buffer or faults mid-command. Depending on the engine state
this hangs the queue, produces a reset, or writes into pages that have already been reused for
another allocation.

**Root cause:** `RN_UC_BO_FREE` unmaps every mapping of the handle, calls `destroyBo`, and
returns success without consulting the connection's fence or waiting for submitted sequence
numbers that reference the buffer. `clientClose` does the same for all buffers through
`freeAll`. The comment in `destroyBo` says the PTEs are invalidated before the pages are
released, which prevents memory from being *reused* while still mapped, but it does not stop a
command buffer that is already in the ring from using the address; invalidating the PTE in the
middle of execution is itself a fault.

**Why it matters:** This is a direct use-after-free of GPU-visible memory, and it is reachable
from an ordinary client teardown: the compositor frees a surface while its last frame is still
in flight. A fault in the middle of a draw corrupts the frame; a fault during a copy can hit
another allocation after recovery. It also undermines the stated safety property of
`destroyBo`.

**Fix:** Track the last submitted sequence per buffer (or per connection) and defer the unmap
and page release until that fence has passed; on close, wait for the connection's fence with a
bounded timeout before tearing down. If a buffer must be freed immediately, remove it from the
address space only after a queue idle/reset that guarantees no in-flight reference. Record
which handles a submitted IB used so the wait is exact.

---

## 58. Every render pass allocates and destroys a descriptor pool, framebuffer, command buffer and six large arrays

**Severity:** Medium
**Where:** `RedNaviMTL/rn_vk.c:821-956` (`rn_vk_render_pass`): `calloc` of `sets`, `in`,
`out`, `w`, `bi`, `ii` at 827-832, `CreateDescriptorPool`/`CreateFramebuffer`/
`AllocateCommandBuffers` at 848-873, all destroyed at 950-955.

**Symptom:** Each pass pays for six heap allocations whose sizes grow with the draw count
(`nd * 2 * RN_VK_MAX_BINDINGS` entries), a fresh descriptor pool, a framebuffer, a command
buffer, a fence, and the corresponding driver objects. A compositor frame with many passes
spends a large fraction of its CPU time in allocation and driver bookkeeping, and the
allocator traffic competes with the rest of WindowServer.

**Root cause:** Nothing is cached between passes or between frames. The descriptor pool is
sized for exactly `3 * nd` sets and the framebuffer is rebuilt even when the attachments,
formats and size are unchanged. The command buffer is allocated from the global pool and freed
after the fence; the fence is created and destroyed for every submission. The six arrays are
recomputed from scratch for every pass.

**Why it matters:** Even after the correctness fixes, a compositor that allocates a pool and a
framebuffer per pass cannot hold a steady frame rate, and allocation failure on a large pass
becomes a dropped frame. The cost scales with the number of draws, which is exactly where the
compositor is busiest.

**Fix:** Cache the framebuffer and descriptor pool per (target set, size, format) and reuse
them across frames; reset the pool instead of destroying it. Keep a small ring of command
buffers and fences per queue and reuse them. Use stack or per-queue scratch storage for the
write/info arrays instead of `calloc` per pass, sized to the actual binding count. Move the
per-pass submission to one per command buffer (finding 47) so the fixed cost is paid once per
frame.

---

## 59. The leak instrumentation runs a backtrace and symbolication for every buffer and texture creation on the compositor's hot path

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:62-83` (`rn_site_enter`), called from
`compute.m:147` (every buffer) and `compute.m:260` (every texture),
`compute.m:92-101` (`rn_sites_report`), `compute.m:1225-1245` (`rn_commit_stats` calls it
every five seconds).

**Symptom:** Creating a Metal buffer or texture takes far longer than an allocation should,
and the cost is serialised on one global `os_unfair_lock`. With the compositor creating
textures and buffers every frame, this shows up as CPU time in `commit` and in the client's
encode path, and as contention when several threads create resources. `rn_sites_report` then
repeats the scan every five seconds.

**Root cause:** The leak diagnosis added for the 2026-11-07 investigation is enabled by default
(`RN_SITES` unset means on). `rn_site_enter` calls `backtrace(bt, 12)` for every object, hashes
the frames, takes `g_sites_lock`, and on a new site calls `backtrace_symbols` and copies symbol
names into a 360-byte buffer. That is a stack walk plus, for every new site, a symbolication
and several `strlcat`s, all inside the allocation path. `rn_sites_report` then walks 128 entries
and logs the largest site every five seconds while holding the same lock.

**Why it matters:** This is instrumentation, not needed in normal operation, but it is on by
default in the build under review and runs in the exact path that must be fast for a
compositor. It also allocates (via `backtrace_symbols`) while the allocator may be in a
fragile state, and it can turn a memory-pressure situation into a stall. It makes every other
performance measurement in this review unreliable.

**Fix:** Default `RN_SITES` to off and enable it only for a diagnostic build or when the
environment variable asks for it. If it must stay on, capture only the program counter and
symbolicate lazily in `rn_sites_report`, outside the lock; drop `backtrace_symbols` from the
creation path entirely. Never allocate inside the instrumentation while a lock is held.

---

## 60. A command buffer keeps every recorded resource and every handler alive after it completes

**Severity:** Low
**Where:** `RedNaviMTL/compute.m:1258-1264` (arrays created), `compute.m:1426-1531`
(`commitBody` runs the records but never clears them), `compute.m:1411-1414` (`dealloc` only
logs).

**Symptom:** A client that keeps completed command buffers (a common pattern: the layer holds
the last submitted buffer until the next frame, or a pool keeps them) also keeps every
texture, buffer, drawable and completion block that was recorded into them. Memory does not
return to the pool, textures that should be recycled stay alive, and `g_tex_live` grows even
though no frame references them.

**Root cause:** `records`, `scheduled`, `completed` and `presents` are strong `NSMutableArray`s
owned by the command buffer. `commitBody` enumerates them but never removes anything, and
`dealloc` does not clear them explicitly (ARC releases them only when the command buffer
itself dies). With `retainedReferences` returning YES, every `RNDraw`, `RNDispatch`, `RNBlit`
and the Metal objects they point to remain retained for the lifetime of the command buffer.

**Why it matters:** Metal's contract is that a command buffer retains its resources until it
completes, then releases them. Here completion does not release anything, so a client that
caches command buffers — WindowServer does exactly this to avoid allocation — accumulates
textures and buffers. This is a slow leak that matches the observation of thousands of live
buffers and textures, and it contributes to the memory pressure under which the other races
become fatal.

**Fix:** At the end of `commitBody`, after the handlers and presentations, release the recorded
work: remove all objects from `records`, `scheduled`, `completed` and `presents`, or replace
the arrays with empty ones. Keep only what a completed command buffer must still expose
(status, error). This matches Metal's reference semantics and lets textures and buffers be
recycled as soon as the frame is done.

---

## 61. Every commit copies the whole process's no-copy buffers in and out, without synchronisation, and even after a failure

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:158-163` (`+noCopyBuffers`, `+syncNoCopyIn:`),
`compute.m:1494` (copy-in), `compute.m:1526` (copy-back), `compute.m:1984-1992`
(`newBufferWithBytesNoCopy:...` registers the buffer and does the first copy).

**Symptom:** SIGSEGV or SIGBUS inside `memmove`/`memcpy`, reported at
`+[RedNaviMTLBuffer syncNoCopyIn:]` called from `-commitBody` a few seconds after the pointer
starts moving. With more than one active command buffer the same loop also copies buffers that
the committing command buffer never recorded, and writes the GPU-side result back over memory the
client has changed in the meantime.

**Root cause:** `syncNoCopyIn:` takes a weak `NSHashTable` of *every* buffer ever created with
`newBufferWithBytesNoCopy:length:options:deallocator:` in the process and copies `len` bytes
between `userPtr` and the Vulkan mapping for each of them, with no reference to the command
buffer being committed. There is no lock over `userPtr` contents: the client is free to write its
memory while the copy runs, and `commitBody`'s own copy-out at line 1526 runs unconditionally,
including after `fail:` set `st = MTLCommandBufferStatusError` and after a dispatch or pass failed
half-way. The callers are separate `MTLCommandBuffer`s from WindowServer, CoreAnimation and the
display pipe, and those commits already run concurrently (finding 1), so two threads can copy
into and out of the same buffer at once. The table is also populated from the object's deallocator
path (`compute.m:155`) while `allObjects` snapshots it under a different lock, so a buffer can be
removed while the snapshot is being walked.

**Why it matters:** This is the exact frame in the crash report. A weak table avoids a use-after-
free of the object, but it does not make the memory or the length valid: the client's deallocator
for a no-copy buffer runs as soon as the last reference goes, and `userPtr` is not retained by
anything the plug-in controls. A `memcpy` from a stale `userPtr`, or into it after the client has
released it, is a hard fault in the compositor's commit thread. Copy-back after an error is also
silent data corruption: the client's latest writes are overwritten with a buffer state that never
reached the GPU.

**Fix:** Make the no-copy set per command buffer. Each `RedNaviMTLBuffer` already retains its
storage; record the no-copy buffers used by the current records (vertex/fragment/compute binds and
blits) in `commitBody`, copy only those, and copy back only when `st` is still not an error. Guard
the copy with a lock the client's deallocator also takes, and keep a `dispatch_semaphore` or
`os_unfair_lock` per buffer so the copy-in and copy-out cannot overlap. The real fix is to stop
copying: map the caller's memory into the VMID as a host buffer, which the kext already knows how
to do, so the GPU and the CPU share one allocation.

---

## 62. Device entry points trust caller arguments, so a nil pointer or descriptor crashes the process

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1975-1978` (`newBufferWithBytes:length:options:`),
`compute.m:1984-1992` (`newBufferWithBytesNoCopy:...`), `compute.m:1998-2001`
(`newTextureWithDescriptor:`), `compute.m:2042-2052` (tiled texture SPIs),
`compute.m:244-262` (descriptor dereference), `compute.m:1561-1573`
(`rn_texture_over_bytes`).

**Symptom:** A crash on the first frame of a client that passes a nil descriptor (for example a
`MTLTextureDescriptor` built conditionally by CoreAnimation) or a nil `bytes` pointer with a
non-zero length; or, for `newBufferWithBytes:` with `bytes == nil`, a buffer that is silently
returned zeroed instead of nil.

**Root cause:** The hooks go straight to the implementation:
`newTextureWithDescriptor:` calls `initWithDevice:descriptor:`, which reads
`desc.textureType`, `desc.width`, `desc.height` and the rest without a nil check;
`newBufferWithBytesNoCopy:` calls `memcpy(rn_vk_buffer_contents(b->b), bytes, length)` at
`compute.m:1989` without checking `bytes`; `newBufferWithBytes:` guards the copy with
`if (b && bytes)` but still returns a non-nil zeroed buffer, which hides the error; and
`newTiledTextureWithBytesNoCopy:...` dereferences `desc` in `rn_texture_over_bytes` before
validating anything. Nothing checks `width`/`height` for zero, so a zero-sized descriptor asks
Vulkan for a zero-extent image and fails late. Lengths are used as `NSUInteger` and passed
unchanged to allocation and `memcpy`; the only upper bound is whatever `rn_vk_buffer_create`
happens to refuse.

**Why it matters:** Metal returns nil and an `NSError` for bad input; it does not kill the
process. This plug-in sits inside WindowServer, whose whole desktop disappears if one hook
dereferences a nil descriptor. CoreAnimation's private client-storage path is exactly the place
where descriptors and buffers can legitimately be nil (the code already logs "nil client storage
texture from buffer" in the history), so this is not a theoretical API misuse.

**Fix:** Validate at every hook: reject nil descriptors and nil/zero-length data with nil return
and a logged error; require `width`, `height` and (for 3D) `depth` to be non-zero; cap dimensions
and byte lengths to the limits the implementation can honour; make `newBufferWithBytes:` return
nil when `bytes` is nil and `length` is non-zero instead of returning zeroed memory. Do the same
checks in `initWithDevice:descriptor:` so every path (including the IOSurface and tiled SPIs)
benefits.

---

## 63. `waitUntilCompleted` returns immediately even when the command buffer is still running

**Severity:** High
**Where:** `RedNaviMTL/compute.m:1532-1535`, `compute.m:1428-1430`, `compute.m:1262`
(`done` semaphore created with value 0).

**Symptom:** A client that commits a command buffer on one thread and calls `waitUntilCompleted`
from another (or from a completion handler on a different queue) reads textures and buffers
before the GPU work has finished, and sees the previous frame's contents or a half-updated
surface. The call also never observes `MTLCommandBufferStatusError` if the failure happens after
it returned.

**Root cause:** `waitUntilCompleted` does
`if (st < Committed) return; dispatch_semaphore_wait(done, DISPATCH_TIME_NOW);`. The status is
set to `Committed` at the very start of `commitBody` (line 1429), before any pass runs, and the
wait timeout is `DISPATCH_TIME_NOW`, so the call returns at once whether the work is done or not.
It only appears to work today because `-commit` is synchronous on the committing thread: the
same thread cannot call `waitUntilCompleted` until commit has returned. Any other thread, and any
future move to an asynchronous commit, gets a false completion. `done` is signalled only at the
end of `commitBody` (line 1530), so the correct wait is available; it is just not used.

**Why it matters:** Metal's contract is that `waitUntilCompleted` blocks until the command buffer
has completed and that reading its status afterwards reflects the result. WindowServer uses
`waitUntilCompleted` around frames where it reuses the destination surface; returning early hands
it a surface whose GPU writes are still in flight, which is another way to get torn window
contents and overwritten cursor shapes.

**Fix:** Wait on `done` with `DISPATCH_TIME_FOREVER` (or on a condition variable driven by the
status transition), and signal `done` on every exit path including `fail:` and early returns. Set
`st = Committed` only after the command buffer is actually enqueued, and transition to
`Completed`/`Error` at the end. Keep a separate `Scheduled` state if `waitUntilScheduled` is to be
honoured.

---

## 64. IOSurfaces are copied whole on the CPU at every commit without synchronising with their GPU producer, so the compositor can read a half-updated frame

**Severity:** Critical
**Where:** `RedNaviMTL/compute.m:1434-1456` (`surfUsed`, `note`, the `syncFromSurface` loop),
`compute.m:284-297` (`syncFromSurface`), `compute.m:1443-1444` (every sampled texture is noted),
`compute.m:287` (the `w < 1024` seed shortcut).

**Symptom:** Window parts deform, shapes corrupt near the pointer, and the composite shows a
mixture of two frames: one region (often around the cursor, which is redrawn most often) comes
from the new frame while the rest is from the previous one. Large surfaces show it most; small
ones often look correct.

**Root cause:** A texture created with
`newTextureWithDescriptor:iosurface:plane:` is not a view of the surface; it is a private Vulkan
image plus a CPU staging copy. At every commit, `note()` adds every sampled IOSurface texture to
`surfUsed`, and `syncFromSurface` copies the surface rows into the staging copy with
`IOSurfaceLock(surf, kIOSurfaceLockReadOnly, ...)`. The seed shortcut at line 287 is deliberately
disabled for textures at least 1024 pixels wide ("CoreDisplay's source surface is written by other
means"), so the full-screen surfaces WindowServer composites are copied on *every* commit. The
read-only lock only excludes other CPU writers that also take the IOSurface lock; WindowServer's
compositor writes that surface with the GPU and does not take it, and the copy is not ordered
against those writes. The copy also happens before *each* pass of the command buffer, so a surface
used by several passes is copied several times per frame.

**Why it matters:** This is the most direct explanation of "shapes corrupt near the mouse". The
cursor is drawn into the same surface that is being copied; the copy runs concurrently with the
GPU writing the cursor region, so the staging copy can contain the cursor rectangle from the new
frame and the window content from the old one, or a partially written pixel row. The subsequent
`syncToSurface` then writes that mixture back. Copying also costs a full surface read plus a
staging upload per commit, which is why the compositor only stays alive for seconds.

**Fix:** Stop using a CPU copy as the transport. The kext already has the surface's physical pages
and can map them into the client's VMID, so the texture can alias the IOSurface memory and the GPU
can sample and render it directly. If a copy is kept as an interim measure, it must be a correct
snapshot: take the surface lock the producer also uses (or a shared event / fence from the
producer), copy only when the seed changed, and never copy a surface that the same command buffer
is about to write.

---

## 65. `RN_UC_INFO` reads the mode fields without `modeLock`, so one reply can mix two modes

**Severity:** Medium
**Where:** `RedNavi/kern_probe.cpp:2102` (reads `selMode` and `selPitch`),
`kern_probe.cpp:1376-1397` (`setModeLive` writes them under `modeLock`),
`kern_probe.cpp:1305-1317` (`applyMode` rewrites the whole `rn_mode`),
`kern_probe.cpp:2731-2734` (`scanoutSurfaces` recomputes from the same fields).

**Root cause:** `externalMethod` holds the per-connection `lock`, not `modeLock`, when it fills
`rn_uc_info`. `setModeLive` replaces `selMode` (a multi-field struct), `selPitch` and `selIndex`
under `modeLock`, and `applyMode` does so again during a rollback. A live mode change can run
between the `fb_width` load and the `fb_pitch` load, or between `fb_pitch` and
`scanoutSurfaces()`, so the reply can describe a width from one mode with a pitch and a surface
count from another. A client that sizes its buffers from that reply allocates a scanout BO too
small for the width it thinks it has; the next `present` then touches memory outside the BO.

**Why it matters:** A mode change is exactly when the display is being reconfigured and the
compositor is repainting everything. An inconsistent geometry reply turns a transient mode
switch into persistent corruption.

**Fix:** Take `modeLock` around the whole `RN_UC_INFO` fill (or snapshot `rn_mode`, `selPitch` and
`selIndex` into locals under `modeLock`, then use the snapshot). Do the same in `present`,
`createScanoutBo` and `scanoutSurfaces` (finding 68).

---

## 66. Scanout surfaces have no owner, so any client can flip the display and any client's close resets it

**Severity:** High
**Where:** `RedNavi/kern_probe.cpp:2111-2118` (`RN_UC_BO_CREATE` with `RN_BO_SCANOUT` passes
`in->pad` straight to `createScanoutBo`), `kern_probe.cpp:2756-2768` (`createScanoutBo`),
`kern_probe.cpp:2226-2236` (`RN_UC_PRESENT`), `kern_probe.cpp:2739-2754` (`present`),
`kern_probe.cpp:2049-2055` (`clientClose`).

**Root cause:** `surface` is a global index in `0..scanoutSurfaces()-1`; the kext never records
which connection created the scanout BO or which connection is entitled to present it. Every
client can call `RN_UC_PRESENT` with any in-range surface, and `present` writes the HUBP address
registers for the whole display. `frontSurface` is one global, and `clientClose` resets it to 0
whenever *any* connection closes while the display is showing a non-zero surface.

**Why it matters:** WindowServer, CoreDisplay and ordinary applications all connect to the same
accelerator. A client that flips to surface 1 and a second client that flips to surface 2 fight
over the pipe; when either exits, the desktop is forced back to surface 0 even though the other
one still owns the display. The result is a flash of the wrong buffer, a black frame, or a
compositor drawing into a surface that is not the one being scanned out — all of which look like
window deformation and shapes appearing near the pointer.

**Fix:** Store the owning VMID/connection on each scanout BO and on `frontSurface`; accept
`RN_UC_PRESENT` only from the owner of that surface; reference-count scanout BOs so a surface
cannot be freed while it is being scanned out; and in `clientClose` restore surface 0 only if the
closing connection owns `frontSurface`. Consider allowing presentation only for the connection
that owns the framebuffer's display surface (the WindowServer/CoreDisplay path).

---

## 67. A dispatch with a zero dimension is recorded, rejected by the backend and fails the whole command buffer

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:926-930` (`dispatchThreads:threadsPerThreadgroup:`),
`compute.m:911-922` (`record:`), `RedNaviMTL/rn_vk.c:580-581`
(`rn_vk_dispatch_threads` returns 6 for a zero dimension), `compute.m:1516-1519`
(`commitBody` turns any non-zero return into `fail:`).

**Root cause:** `dispatchThreads:` computes the threadgroup grid with a ceiling division. When
the client asks for zero threads in any dimension (a legitimate way to skip work for an empty
region), the grid dimension is zero. `record:` still appends the dispatch, and
`rn_vk_dispatch_threads` rejects it with `!threads[0] || !threads[1] || !threads[2]`, so
`commitBody` marks the whole command buffer as failed. Metal treats an empty grid as a no-op; it
does not fail the unrelated passes and blits that share the command buffer.

**Why it matters:** WindowServer and CoreAnimation routinely compute tile grids from dirty
rectangles; a zero-width or zero-height rectangle then cancels the entire frame, including the
cursor update, and the desktop freezes or shows the previous frame. It also hides the real error
because the failure is attributed to a missing resource in the log.

**Fix:** In `record:` (or `dispatchThreads:`), skip the dispatch when any grid or threadgroup
dimension is zero, and log it once. Do not submit an empty dispatch to the backend. Keep the
backend's check as a defensive assertion, but do not let it fail the command buffer for an empty
grid.

---

## 68. `present`, `createScanoutBo` and `scanoutSurfaces` race live mode changes with a different lock

**Severity:** High
**Where:** `RedNavi/kern_probe.cpp:2728-2734` (`scanoutStride`, `scanoutSurfaces`),
`kern_probe.cpp:2739-2754` (`present`), `kern_probe.cpp:2756-2768` (`createScanoutBo`),
`kern_probe.cpp:1376-1397` and `kern_probe.cpp:1347-1367` (`setModeLive`/`programLive`),
`kern_probe.cpp:1305-1317` (`applyMode`).

**Root cause:** `present` and `createScanoutBo` take only `boLock`; `setModeLive` and
`programLive` rewrite `selMode`, `selPitch`, `selIndex` and the hardware pipe under `modeLock`.
`present` reads `selPitch`/`selMode` several times while computing `scanoutSurfaces()` and
`scanoutOff()`; a mode change between the bounds check and the address computation makes the
surface count and the stride come from different modes. `createScanoutBo` computes `bytes` from
the new `selPitch` but the caller may have chosen the surface index under the old mode, and the
BO is inserted into the shared table with a range that the mode change invalidates. Conversely,
a BO created before the change keeps a size computed for the old stride while `present` now
computes offsets with the new one, so the displayed surface and the memory the client writes no
longer match.

**Why it matters:** A live mode switch (the framebuffer's "rednavi-set-mode" path, used when the
user changes resolution or the display wakes) is followed by a full repaint. If the compositor's
flip lands between the new pitch and the new surface count, the pipe scans out a range that is not
the buffer the compositor drew into: the image is skewed or shifted, and shapes appear duplicated
near the pointer.

**Fix:** Take `modeLock` in `present`, `createScanoutBo` and any caller of `scanoutStride`/
`scanoutSurfaces`, and hold one snapshot of the mode for the whole operation. On a mode change,
invalidate every scanout BO (see finding 69) and force `frontSurface` back to 0 before the new
mode is visible.

---

## 69. A failed live mode change restores the mode but leaves `frontSurface` and the scanout BOs describing the failed mode

**Severity:** High
**Where:** `RedNavi/kern_probe.cpp:1378-1396` (`setModeLive` rollback),
`kern_probe.cpp:1305-1317` (`applyMode` resets `frontSurface`), `kern_probe.cpp:2756-2768`
(`createScanoutBo` stores a fixed range), `RedNaviFB/kern_fb.cpp:640-680` (the caller re-reads
the properties after the call).

**Root cause:** `programLive(m)` calls `applyMode(m)`, which rewrites `selMode`/`selPitch` and sets
`frontSurface = 0`, then reprograms the pipe. If the stream does not come up, `setModeLive`
calls `programLive(prevMode)` and republishes the properties, but it does not tear down or resize
the scanout BOs that clients created for the failed mode, and it does not re-check that the plane
is actually pointing at surface 0. A scanout BO holds `vmd`/`off`/`size` captured at creation.
After a successful or failed mode change the stride changes, so the same surface index maps to a
different VRAM offset and a different length; the old BO still describes the old range. Any client
that keeps its scanout handle across the mode change writes a frame with the old geometry, and
`present` scans out the new geometry.

**Why it matters:** A failed mode change is common while probing a monitor's modes. After it, a
compositor that did not recreate its scanout BO draws into the wrong VRAM range: the desktop shows
a skewed copy of the frame, and cursor-shaped garbage appears where the old and new strides
overlap. The properties and the actual state diverge until the driver is reloaded.

**Fix:** On every mode change, successful or not, invalidate all scanout BOs (mark them dead,
clear the table entries, and make existing handles fail cleanly), reset `frontSurface` to 0 and
perform a flip to surface 0 after the pipe is stable. Publish the new geometry only after the
rollback has completed, and bump a mode generation counter that `present` checks so a stale BO
cannot be flipped.

---

## 70. An indexed draw with a nil index buffer becomes a non-indexed draw of `indexCount` vertices

**Severity:** Medium
**Where:** `RedNaviMTL/compute.m:1189-1195` (`drawIndexedPrimitives:...`),
`compute.m:1154-1188` (`drawPrimitives:...` sets `vertexCount`), `RedNaviMTL/rn_vk.c:932-937`
(`index_buffer` nil selects `vkCmdDraw`), `compute.m:1380` (the draw is built with
`index_buffer` only when `indexBuf` is non-nil).

**Root cause:** `drawIndexedPrimitives:` checks only that `pso` is set, then calls
`drawPrimitives:` with `vertexCount = count` (the index count) and afterwards attaches
`indexBuf`, `indexOffset`, `index32` and `baseVertex` to `pass->draws.lastObject`. If
`indexBuffer` is nil, `d->indexBuf` stays nil and the renderer takes the non-indexed path: it
draws `count` sequential vertices from the currently bound vertex buffer. Nothing validates the
index buffer's length either, so an offset or count beyond the buffer is handled only by the
`offset >= size` check at `rn_vk.c:933`, which does not cover `offset + count * indexSize`.
Metal requires a non-nil index buffer for an indexed draw; returning garbage triangles instead of
failing hides a client error in the worst possible way.

**Why it matters:** A missing index buffer on a compositor quad turns that quad into a fan of
random vertices read from the neighbouring vertex data. On screen this is a triangle smear that
follows the window (and the cursor's quad) as it moves, matching the reported "shapes corrupt near
the mouse". The same happens when an index buffer allocation failed and the client ignored the
nil return.

**Fix:** Require a non-nil index buffer, validate `offset + count * (index32 ? 4 : 2)` against
`indexBuffer.length`, and fail the draw/pass with a clear message instead of falling through to
`vkCmdDraw`. Also validate the vertex buffer ranges against the vertex count and stride, so an
out-of-range draw cannot read another allocation.

---

<!-- Findings 71 onward are appended below. -->
