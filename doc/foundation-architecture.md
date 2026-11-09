# Architecture, decisions and contracts

[Foundation index](foundation.md) · [Research](foundation-research.md) · [Feasibility](foundation-feasibility.md) · [Roadmap](foundation-roadmap.md)

These are proposed design contracts, not implemented APIs. Capability microkernel, per-task namespaces, typed operations, restartable services, replaceable inference, resource accounting and Kryon are the authoritative direction. Representations remain subject to review and measurement.

## Kernel and trust boundary

Kernel mechanisms cover address spaces/mappings, capability objects and transfer, threads/scheduling contexts, IPC endpoints, notifications, time and IRQ delivery. Filesystems, network protocols, driver protocols, session policy, inference and UI belong in isolated Ziran services. A root authority initializes the resource graph and delegates bounded resources. It is a powerful trusted component, not ambient authority for applications.

Threats include malicious apps, compromised drivers or models, malformed messages, resource exhaustion and crashes. Compiler correctness, kernel memory safety, boot integrity, device isolation and service authorization are separate obligations. A new Ziran kernel inherits no seL4 proofs. Any future proof claim must identify its model, implementation, supported configuration, hardware assumptions and exclusions. Kernel proofs do not prove the whole OS, recovery, UI or every driver.

## Authority, routing and revocation

A capability identifies an object and allowed operations; tasks cannot forge it. Names, process IDs and discovered descriptors grant no rights. Derivation can only narrow rights; transfer needs explicit delegation authority and records a derivation relationship. Resource grants include budgets. Separate administration from ordinary use.

Each task gets a private namespace composed from granted handles. Bindings and child namespaces can only expose existing authority. Children receive explicit subsets. Discovery returns authorized descriptors only. A broker must preserve the requesting principal and constrain forwarded authority to avoid a confused deputy. Resolving a name binds an endpoint and identity/version; remounting cannot change an already-open handle into another service.

Proposed revocation: revoke a derivation branch, reject new descendant operations, invalidate relevant transfers/mappings, and acknowledge only after an enforcement barrier. Specify races against IPC, mapping and further delegation; bound traversal or define epochs and reclamation. Generation checks prevent stale handle reuse. Restarted services get new session handles; reconnect requires policy authorization.

Revocation cannot erase copied data or reverse committed external effects. In-flight work reports prevented, cancelled before commit, committed or outcome unknown. Revoking a driver must quiesce DMA before reusing its memory; failed quiescence may require reset and quarantined memory.

## Typed services and IPC

Apps publish discoverable operation schemas: typed inputs/results, error categories, required rights, versions, limits and effect descriptions. Kryon UI, scripts and agents invoke the same authorized operations. Discovery itself is constrained to prevent metadata leaks. Haiku-inspired scripting does not require its C++ ABI.

Define transport-independent schemas before a syscall ABI. Wire contracts require fixed widths, byte order, alignment, bounded lengths/attachments, overflow checks, operation IDs, correlation, deadlines and explicit buffer/capability ownership. Do not send compiler record layouts, process pointers or raw authority tokens as portable wire data. Validate before allocation or effects. Reject unknown required fields and incompatible majors; negotiate optional features. Version kernel ABI, service schemas and persistent state separately, with upgrade/rollback tests.

Use bounded queues and backpressure. Copying is a candidate for small control messages; bulk data may use explicitly granted shared regions with mapping permissions and lifetime rules. Shared memory is not automatically safe or free. Measure synchronous hops, queues, transfer, copies, cache effects and loaded tail latency before choosing service granularity. Do not turn every function into IPC.

## Restart, durable tasks and cancellation

A supervisor starts services with limited handles and versioned recoverable state. Durable task identity is separate from process identity. Proposed records contain principal, operation/schema version, idempotency key, input references, authority reacquisition policy, checkpoint, committed effects and result. Persist references, not live handles or secrets. Recovery reauthorizes against current policy; expired grants do not resurrect.

Task states include accepted, running, cancel-requested, committed, succeeded, failed and outcome-unknown. Define permitted transitions and checkpoints. Acknowledge durable acceptance only after stable journal storage, effects only after their commit boundary. Crash-consistent records, bounded retention, state migration and rollback rules are prerequisites.

Retry only idempotent steps or operations with durable deduplication at the effect owner. Timeout is not evidence of failure. A crash after a remote send/write but before acknowledgement can leave outcome unknown: reconcile externally, never promise universal exactly-once behavior. Compensation is separately authorized and may fail. Cancellation stops future work and releases resources at safe boundaries; committed effects may remain. Distinguish request received, cancellation completed and already committed.

## Scheduling and resources

Budget CPU time, resident memory, queued messages, storage and I/O per task/service. Charge synchronous service work to its origin through bounded donation or explicit service billing; define priority inversion, recursion and replenishment rules. Use preemption, admission control and bounded IRQ work. Separate interactive latency from batch inference and expose throttling without leaking other principals' usage.

GPU accounting separates queue time, device memory, pinned host memory and transfers; energy estimates require hardware evidence. A broker admits bounded jobs while kernel/device mechanisms enforce mappings and DMA authority. Preemption, reset and cancellation depend on the selected device. No immediate-interruption guarantee is made; nonpreemptible jobs need admission limits and measured worst-case interference. CPU-only inference is an initial candidate, not a selected product target.

## Native platform and Kryon

Boot must establish CPU mode, stack, memory map, page tables and Ziran entry without an implicit foreign stub. Traps/syscalls need register frames, privilege transitions, interrupt nesting/masking, exception return, per-CPU state and contained faults. Define allocation/stack exhaustion and early diagnostics before untrusted tasks.

Memory contracts include frame ownership, aliases/lifetimes, bootstrap allocator, volatile MMIO widths, atomics and ordering, barriers, cache maintenance, TLB shootdowns and device memory types. Ordinary loads/stores do not establish these semantics. Scheduling needs timer delivery, context switching, multicore ordering, race and starvation tests.

Drivers require bounded IRQ acknowledgement/deferred work, DMA pin/map/unmap ordering, reset and hot-unplug rules, and IOMMU domains before claiming containment of untrusted DMA drivers. Without an IOMMU or equivalent restriction, explicitly enlarge the trusted driver/device boundary; process separation cannot contain DMA. Serial, storage, display and input choices depend on hardware selection.

Kryon owns reusable widget behavior and host integration. TaijiOS compositor/session services own surface authority, input routing and desktop policy. A native Ziran backend must connect authorized surfaces, fonts/images, timing, clipboard and accessibility to Kryon's current host boundary. CPU rasterization is a candidate before GPU acceleration. Existing SDL/Cairo/raylib hosts are hosted references, not compliant shipped target stacks.

Inference/agent providers remain replaceable userspace services with versioned requests, model/resource declarations and explicit data/network grants. Model output is untrusted input, never authority. Provider failure must not stop the desktop, kernel or ordinary apps; durable tasks remain inspectable without their originating model.
