# TaijiOS foundation

Status: authoritative new direction, documentation only, 2026-11-09. No new OS implementation or executable is supplied by these documents.

The target is a capability-based microkernel OS written entirely in Ziran, with Kryon for the interface: kernel, drivers, services, applications and UI. seL4 and Genode are architectural references, not code dependencies. Linux may host optional experiments; it is not the final kernel.

The owner explicitly made the old TaijiOS plan reference material. The Plan 9 tree, boot recipes, Rill integration and migration reports remain historical evidence. The old `plan9-c` migration does not establish compliance with this target. No archive, source or submodule is relocated.

Read in order:

1. [Architecture, decisions and contracts](foundation-architecture.md): authority, namespaces, messaging, recovery and resource limits.
2. [Comparative research](foundation-research.md): primary sources, facts, design judgments and limits.
3. [Ziran and Kryon feasibility](foundation-feasibility.md): actual evidence, upstream gaps and bootstrap interpretation.
4. [Roadmap and ownership](foundation-roadmap.md): proposed folder responsibilities and local verification gates.

Historical entry points: [upstream README](../README), [preserved integration README](../README.md#historical-plan-9-integration), [unified desktop](unified-desktop.md), [desktop status](unified-desktop-status.md), [application matrix](application-matrix.md), and [Windows application exploration](windows-application-plan.md). These may describe obsolete plans or incomplete work; none overrides this foundation.

## Open decisions

First ISA, board, firmware boundary, exact syscall/wire encodings, persistence medium, performance budgets and initial device set remain open. Whether all-Ziran also covers every bootstrap compiler/tool and firmware component requires clarification before implementation. No foreign shim, runtime or boot stub is silently accepted.

The owning checkout is `ARCHIVE/taijiosnet/taiji`, identified by its root README, Plan 9 inventory, Makefile and Git remote `https://github.com/taijiosnet/taiji.git`. The surrounding directory is a container. The old Rill location named by Projects instructions is absent there; this does not justify creating or moving a repository.
