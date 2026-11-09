# Roadmap and ownership

[Foundation index](foundation.md) · [Architecture](foundation-architecture.md) · [Research](foundation-research.md) · [Feasibility](foundation-feasibility.md)

This is a staged proposal, not authorization to implement it in this documentation task. No source scaffold, manifest, executable, version or release is created. Each phase depends on preceding gates; dates and performance numbers are not invented.

## Proposed folder ownership

Use existing `doc/` for the foundation and later decisions/verification records, with the root README as entry. Preserve existing archive/source paths including historical `sys/`, `boot/`, `cfg/`, `debian/`, `site/` and app submodules. Their contents are not newly endorsed target code. Do not restore or relocate Rill because an older map names it.

Future paths below are proposed responsibilities only; no source folders are created:

- `kernel/`: Ziran microkernel mechanisms, architecture entry/traps, scheduling and object enforcement.
- `drivers/`: isolated native device services, reset, IRQ and DMA/IOMMU integration.
- `services/`: namespace/routing policy, storage/network, supervision, durable tasks, accounting and inference/agent providers.
- `protocols/`: typed schemas, effects/retry semantics, wire contracts and compatibility fixtures, without a process-pointer ABI.
- `desktop/`: TaijiOS compositor/session policy and Kryon system surfaces. Reusable widgets and hosts stay upstream.
- `apps/`: target app policy, assets and schemas; separate application repositories remain an ownership decision.
- `tests/`: native integration, fault injection and conformance workloads. Existing `scripts/` owns host/test orchestration. Keep important evidence in durable docs, outside disposable build outputs.

Canonical Ziran owns language/compiler/runtime/standard-library prerequisites; canonical Kryon owns reusable UI/backend integration. Both are read only here. Future dependency adoption uses package commands after upstream verification, with local overrides during development. Never edit vendor copies.

## Phase 0: decision and evidence baseline

Resolve hardware and bootstrap interpretation before boot implementation. Candidates, not selections: RISC-V 64 emulator for a bounded privileged-machine experiment; x86-64 emulator/UEFI for desktop relevance; AArch64 emulator or documented board. Compare instruction/firmware requirements, device documentation, interrupt controller, timer, IOMMU, display/input/storage, debugger, emulator fidelity and real hardware access. No candidate is assumed easy or already supported by Ziran.

Record machine, trusted firmware boundary, device list, target-code audit policy and accepted bootstrap. Define workload/latency/memory budgets and failure criteria before tests. Results must record local revisions and dirty state, toolchain, target/configuration, command, repetitions and missing coverage. Hosted experiments are optional schema/state-machine feedback, not native OS gates.

## Phase 1: upstream freestanding capability

Deliver general compiler/runtime prerequisites upstream first. Gate: a Ziran-only entry boots in the selected emulator with the accepted dependency inventory; binary/symbol inspection finds no unapproved runtime, hidden libc/Go dependency or foreign boot shim. Exercise trap frames/return, deliberate faults, volatile MMIO, atomic litmus tests, alignment, no-host allocation/failure paths and source/saved-IR agreement where applicable. Code generation alone fails this gate. Missing features block progression and are recorded upstream, without a local shim.

## Phase 2: kernel isolation and authority

Gate: native multi-task execution with memory separation, timers and preemption; fault one task without corrupting another. Negative tests cover forged/stale handles, rights amplification, forbidden transfer, namespace escapes and revocation raced against IPC/mapping/delegation. Verify bounded exhaustion, scheduler fairness and priority inversion. Add multicore tests before SMP claims. Review assurance independently of seL4 references.

Measure IPC across sizes/hops: round-trip and asynchronous throughput, p50/p95/p99 latency, CPU time, context switches, allocations, copies and memory, idle and contended. Compare coarse-grained services and batching/shared-buffer alternatives with identical work. Set numerical budgets at phase 0; a best-case benchmark is insufficient. Redesign granularity if interaction budgets fail.

## Phase 3: native devices and services

Gate: selected serial/timer, storage, display/input and required network paths work through Ziran drivers and bounded protocols. Verify IRQ storms, malformed descriptors, DMA mappings, reset/hot unplug where supported. Test unauthorized DMA in a disposable emulator/device environment before isolation claims. Without demonstrated IOMMU enforcement, restrict the trust claim and device set. Audit each linked dependency.

Schema tests reject oversized/malformed messages and incompatible versions, enforce routing, and cover ownership, deadlines, backpressure and upgrade/rollback. Demonstrate device support on the chosen native target, not Linux hosted runs.

## Phase 4: state recovery and durable tasks

Gate: inject crashes and simulated power loss around journal append, flush, checkpoint, effect commit and acknowledgement. Compare recovery to an independent task/effect ledger; include truncated/corrupt journals, storage failure, duplicate/reordered requests, expired grants, migrations and restart loops. Measure detection-to-recovery latency, lost acknowledged work, duplicate effects, replay time and storage growth against declared budgets. Require no privilege resurrection or falsely acknowledged durable work.

Exercise cancellation before/after commit, timeout with uncertain external effects, idempotent retry/deduplication and failed compensation. Unknown outcomes remain visible for reconciliation. A restarted process or replayed happy path alone fails this gate.

## Phase 5: native Kryon desktop and applications

Gate: native all-Ziran presentation/input through authorized surfaces, readable fonts/images, focus, clipboard, accessibility and discoverable app operations. Exercise compositor/service failure and session recovery without losing durable work. Compare semantic behavior across applicable Kryon targets, then test actual TaijiOS rendering/input; hosted screenshots are insufficient. Measure frame/input tail latency under IPC/storage load. GUI tests use private displays/emulators with inherited display/auth/session state removed; preserve the owner's running desktop.

## Phase 6: inference and accelerators

Gate: replace CPU inference providers while apps/tasks remain usable; verify numerical results, model/input isolation, denied authority and budgets. Native GPU support is a separate hardware gate: validated kernels, queue/device-memory/pinned-memory accounting, transfers, DMA isolation, reset and bounded interactive interference. Measure cancellation/preemption limits as well as throughput. A hosted foreign runtime cannot pass this target gate.

## Documentation validation now

Validate new relative Markdown links/anchors, canonical evidence paths and root navigation; verify historical README body and tracked files remain unchanged. Review constraints, fact/judgment distinctions, all-Ziran language, reference-only kernels and open hardware/bootstrap decisions. Existing Makefile/scripts/workflows expose historical platform/site smoke tests, not a dedicated documentation checker; unrelated builds are outside this change. No compilation is performed. Future compilation uses `build-queue -- COMMAND`.

Commit only the complete locally validated documentation set in the verified owning repository, as Waozi alone, then push under existing authority. Do not change versions, tags, dependencies or releases. Do not wait for CI. Local documentation tests establish neither OS readiness, deployment nor passing GitHub checks.
