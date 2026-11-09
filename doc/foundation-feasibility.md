# Ziran and Kryon feasibility

[Foundation index](foundation.md) · [Architecture](foundation-architecture.md) · [Research](foundation-research.md) · [Roadmap](foundation-roadmap.md)

Evidence read 2026-11-09 from canonical checkouts, read only: `/mnt/storage/Projects/ziranlang/ziran` and `/mnt/storage/Projects/kryonlabs/kryon`. Vendored copies in the old OS are not authoritative. This is bounded document/source inspection, not a compiler or platform validation run. No upstream changes are made here.

## Current evidence

Ziran snapshot: `919bb6fe3c58cf703fb1179c2926b655e35ab97b`.

- [Architecture](https://github.com/ziranlang/ziran/blob/919bb6fe3c58cf703fb1179c2926b655e35ab97b/docs/ARCHITECTURE.md) describes checked `.zir` feeding native C/Go output or `.zx` with a runtime and host capabilities. This is not a documented independent freestanding machine-code kernel backend.
- [Usage](https://github.com/ziranlang/ziran/blob/919bb6fe3c58cf703fb1179c2926b655e35ab97b/docs/USAGE.md), especially assembly and compiler commands, documents architecture-specific `#asm` with checked portable bodies, clobbers and a volatile option. It explicitly excludes naked functions and some stack/control-flow forms. A volatile assembly option does not establish a complete typed volatile-MMIO or kernel memory model.
- [Implementation status](https://github.com/ziranlang/ziran/blob/919bb6fe3c58cf703fb1179c2926b655e35ab97b/docs/IMPLEMENTATION_STATUS.md) says compiler/parser/checker/backends and portable runtime still contain C and self-hosting is unfinished. It also reports direct Go ABI0 assembly for represented operations, with restrictions; this is not absence of assembly support and not freestanding trap support.
- Source spot checks: `cmd/zir/zir_runtime.c` includes hosted C/POSIX facilities for runtime-header emission; `std/atomic_go.zi` wraps Go `sync/atomic` Uint64 Add/Load. These demonstrate existing host dependencies in those surfaces, not that all atomics are absent elsewhere. They do not demonstrate a native all-Ziran kernel runtime.

Kryon snapshot: `af027a528c6e42b44b68fdaa90b13a7c1136d161`. The local `docs/BACKENDS.md` also had unrelated uncommitted Android/backend edits during inspection; the broad host list was cross-checked against committed content. No uncommitted Android implementation detail is adopted as a target capability here, and that work is preserved.

- [Architecture](https://github.com/kryonlabs/kryon/blob/af027a528c6e42b44b68fdaa90b13a7c1136d161/docs/ARCHITECTURE.md) and [boundaries](https://github.com/kryonlabs/kryon/blob/af027a528c6e42b44b68fdaa90b13a7c1136d161/docs/BOUNDARIES.md) place widget behavior in Ziran and effects in hosts; applications own product policy. Ziran must not recognize Kryon widget names specially.
- [Backends](https://github.com/kryonlabs/kryon/blob/af027a528c6e42b44b68fdaa90b13a7c1136d161/docs/BACKENDS.md) describes terminal, SDL/Cairo desktop, raylib, browser Canvas/DOM and Android hosts. [API](https://github.com/kryonlabs/kryon/blob/af027a528c6e42b44b68fdaa90b13a7c1136d161/docs/API.md) documents sessions, widgets and presentation surfaces, not a TaijiOS kernel interface. Source `src/backend/desktop_loop.zi` imports Cairo raster and Linux timer/file modules and uses foreign libc calls. Ziran-authored host code is not evidence its external dependencies satisfy this target.

## Upstream work required before target implementation

These are required demonstrations, not invented API names or claims of total feature absence:

1. Ziran: define and validate a freestanding output/link path, entry placement, linker layout/relocations, register and calling conventions, privilege instructions, trap/interrupt return and stack/per-CPU control. Current inline assembly cannot simply be assumed suitable for boot or context switching.
2. Ziran: specify volatile MMIO, atomics and ordering, barriers, alignment/layout, alias/lifetime boundaries, no-host allocation and bounds/panic failure paths. Demonstrate hardware memory types, cache/TLB interactions and multiprocessor behavior. Hosted Go atomics and assembly tests are insufficient platform evidence.
3. Ziran: inventory all linked target runtime helpers and dependencies. Replace required foreign runtime facilities upstream with Ziran implementations or develop an appropriate backend; audit resulting target symbols and execution. `.zx` would also require a compliant Ziran runtime, not the current C runtime merely hidden behind a bundle.
4. TaijiOS: implement the new kernel mechanisms, explicit service contracts, native drivers, durable storage, recovery and userspace protocols in Ziran only after prerequisite gates. No Plan 9 C migration or seL4 binary can stand in for these.
5. Kryon upstream: establish a native TaijiOS host/raster/presentation path using only permitted target dependencies. Reusable UI stays upstream; the OS owns its compositor, surface grants and session policy. Fonts, image decoding, accessibility, input and clipboard need explicit target coverage, not just a window demo.
6. Native inference: implement model execution and any selected GPU backend in Ziran; inventory tokenizer/numerics/device/runtime dependencies. CPU numerical correctness and resource isolation precede accelerated claims. Existing CUDA tooling or Ziran GPU emission is not a native driver stack.

All reusable language/compiler/runtime work belongs in canonical Ziran; reusable UI/backend work belongs in canonical Kryon. Future dependency adoption uses package commands after upstream verification. This documentation task changes neither dependency nor vendor copies.

## Host tools, target code and bootstrap

The shipped target requirement covers kernel, drivers, services, apps and UI, with no silently accepted handwritten C/C++/assembly shim or foreign runtime. Existing compilers, linkers, emulators and test scripts are host tools; using them during research does not make their runtime code eligible to ship.

Two interpretations remain unresolved: all target source must be Ziran while generated C/assembler and external host compilation may be permitted; or bootstrap/compiler/backend and generated-code lineage must also be entirely Ziran. Inline `#asm` is currently part of Ziran syntax, but whether target-specific instruction templates meet the owner's interpretation must be explicit. Neither interpretation authorizes a separate foreign boot stub. Firmware/microcode and any third-party device firmware need an explicit scope decision, not an unmentioned exception. Preserve the strict target requirement while these questions are open; do not announce a working all-Ziran OS on hosted evidence.
