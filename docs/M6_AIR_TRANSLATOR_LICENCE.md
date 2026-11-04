# The AIR to SPIR-V translator: licence options and the decision

The plug-in receives Metal kernels as AIR (LLVM bitcode with Apple's metadata, in a `.metallib` container, read by `RedNaviMTL/metallib.c`) and RADV wants SPIR-V.
**Decision (2026-11-05, project owner): use NullMoth's fork of metal2vulkan, kept in a repository of its own, [Helemen7/rn-air2spirv](https://github.com/Helemen7/rn-air2spirv)** (private for now), written in Rust, loaded by the plug-in at run time. The rest of this page is the reasoning. Something has to translate. `RedNaviMTL/rn_air.h` is the seam: the translator is a separate shared library, `librn_air2spirv.dylib`, found next to RADV and loaded with `dlopen`,
with a four-line C interface. **Nothing of a translator is in this repository today.** (Of NullMoth's repository only the declaration headers of the Apple classes were looked at, once, and not used; see the project notes.) This page is what has to be decided before one is.

## What each candidate brings

| | licence | what it is | notes |
|---|---|---|---|
| metal2vulkan (upstream, steelbrain/metal2vulkan) | LGPL-3.0-or-later (as NullMoth's `NOTICE` states) | an AIR to SPIR-V translator | not read by us; maturity and the Metal subset it covers are unknown (I) |
| NullMoth's `translator/` | the same: a fork of the above, their changes released "under the same licence" (their `NOTICE`) | the fork they ship | lives in a repository whose kexts and plug-in are PolyForm Noncommercial; only the `translator/` directory is LGPL. We have not opened that directory |
| our own, permissively licensed | BSD-3 | LLVM IR to SPIR-V with Apple's `air.*` intrinsics and address spaces lowered | the biggest job; the SPIRV-LLVM-Translator (Apache-2.0 with LLVM exception) could be a base (I: not checked) |

## What LGPL-3.0 asks of a BSD-3 project (my reading, not legal advice)

- Our code stays ours under BSD-3 as long as the LGPL code is a **separate work that the user can replace**: a dynamically loaded library, which is how `rn_air.h` is built.
- We may not copy LGPL code into BSD files, and a BSD file derived from it is no longer BSD. Reading the translator to write the plug-in side does not make the plug-in derived, but
  it is cleaner if the plug-in side is written from the interface above and never from the translator's internals.
- Distributing the translator (a binary, in a package or release) means: the licence text and notices, the source or a written offer for it, changes to the translator under LGPL,
  and (LGPL-3.0 section 4) the user must be able to replace the library with a modified one: a `.dylib` in `/Library/Application Support/RedNavi/vk` that the user owns satisfies that here.
- Simplest shape: a **separate repository** for the translator (its own licence file), a release that carries the built `.dylib` with the source offer, and this repository
  only knowing the interface.

## Questions for the owner of the project

1. Upstream metal2vulkan, NullMoth's fork of it, or our own? (Using their fork is allowed by its licence; the reason to avoid it is only that it ties us to a project whose other code we decided not to read.)
2. Separate repository for the translator, and who publishes the binary?
3. How much of Metal has to run before we accept a translator's limits (compute only, no textures; subgroup operations; atomics)? The translator decides what apps can run.

## Not verified

I have not looked at either translator's source or tests. Whether AIR kernels with `constant uint &`, `thread_position_in_grid` and `device uint *` (the test kernel `mesa-bridge/test/vk_dispatch.metal`)
translate, how `[[buffer(i)]]` maps to descriptors, whether the threadgroup size is a specialization constant, are all unknown. The interface (`rn_air_kernel_info`) assumes storage buffers at set 0
binding = Metal index, one push constant block for the constant data, and a fixed local size; if the translator does otherwise, `rn_air.h` and `compute.m` change.

## What was done after the decision

- `Helemen7/rn-air2spirv` has the history of NullMoth's `translator/` directory (87 commits, original authors kept), `LICENSE`/`COPYING` (LGPL-3.0), a README that says where the code comes from, and
  `rn-capi/`: a small new crate that builds `librn_air2spirv.dylib` with the two functions of `RedNaviMTL/rn_air.h`.
- It builds with `cargo build --release` (Rust 1.99, MacPorts; first build about 6 minutes). The translator needs `llvm-dis` (LLVM 19 from MacPorts works, the binary is `llvm-dis-mp-19`) and
  `spirv-val` at run time. **Distribution problem, open**: an end user without LLVM and SPIRV-Tools cannot use the plug-in; shipping the two tools next to the library is possible
  (`rn_air.c` looks for them there) but the LLVM tool is not small.
- `tools/air_translate_test.c` takes one kernel of a real `.metallib` (`mesa-bridge/test/vk_dispatch.metal`, built with the Metal Toolchain) through the plug-in's code and the library:
  772 bytes of SPIR-V accepted by `spirv-val` for Vulkan 1.2, two storage-buffer bindings (Metal buffer 0 writable, buffer 1 read-only: a `constant uint &` argument is a storage buffer
  too), threadgroup size as specialization constants 0, 1, 2.
- The plug-in side follows that contract (`RedNaviMTL/compute.m`): the binding list is not 0..n-1, `setBytes` becomes a small buffer, the pipeline is made per threadgroup size.
  Not yet run on the GPU.
