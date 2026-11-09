# Comparative research

[Foundation index](foundation.md) · [Architecture](foundation-architecture.md) · [Feasibility](foundation-feasibility.md) · [Roadmap](foundation-roadmap.md)

Primary sources accessed 2026-11-09. Fact summarizes the source; Decision is TaijiOS judgment, not evidence that the reference implements the proposal. This is a focused comparison, not an exhaustive survey.

## Plan 9

Fact: [The Use of Name Spaces in Plan 9](https://9p.io/sys/doc/names.html) describes per-process namespaces and a message-oriented file protocol for composing local and remote resources.

Decision: retain namespace composition and meaningful names while making authority depend on explicit capabilities. Typed operations need not all masquerade as byte-stream files. Neither Plan 9 kernel code nor 9P compatibility is selected. The archived tree is history, not the new architecture.

## seL4

Fact: [What is seL4?](https://sel4.systems/About/) describes a capability microkernel. Its [kernel documentation](https://docs.sel4.systems/projects/sel4/) separates capability, IPC, scheduling, interrupts and verified configurations. [Proof assumptions](https://sel4.systems/Verification/assumptions.html) distinguishes C/binary proof coverage and assumptions about hardware, boot and low-level code. Coverage depends on configuration and proof.

Decision: borrow explicit authority, small mechanisms and careful assurance boundaries. seL4 is implemented in C with architecture-specific low-level code; shipping it as the kernel conflicts with the entire target being Ziran. Rewriting a similar design transfers no proof. Functional correctness, isolation, information flow and whole-system resilience require separate evidence.

## Genode

Fact: [General overview](https://www.genode.org/documentation/general-overview/index) describes moving OS functions and drivers outside a small kernel. [About Genode](https://www.genode.org/about/index) describes recursive sandboxes with delegated rights and resources.

Decision: borrow hierarchical resource management and explicit service connections. Genode is largely C++, so it is an architectural reference, not a target dependency. Separation adds orchestration, recovery and IPC costs that must be measured.

## Fuchsia

Fact: [Component capabilities](https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities) describes capability routing. Its [security principles](https://fuchsia.dev/fuchsia-src/concepts/principles/secure) centers explicit object access and least privilege.

Decision: describe routes/grants explicitly and test absent or invalid routes. Discovery is not permission. Fuchsia manifests, Zircon and its runtime stack are not selected dependencies.

## Haiku

Fact: the [Haiku Book scripting chapter](https://api.haiku.nz/system-overview/application/scripting.html) describes application-defined scripting suites, commands, properties/specifiers and replies.

Decision: expose discoverable typed operations to UI, scripts and agents, without BMessage/BApplication ABI or a C++ runtime. Knowing an operation never grants authority to invoke it.

## Windows and Darwin

Fact: Microsoft's [kernel object manager](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/windows-kernel-mode-object-manager) covers typed objects, lifetimes, namespaces, resources and access rights. Apple's archived [kernel architecture guide](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/Architecture/Architecture.html) describes Mach, BSD, I/O Kit and other facilities in its kernel environment. This is historical Darwin documentation, not a claim about current macOS internals.

Decision: retain useful lifetime and introspection ideas without adopting a broad privileged compatibility kernel. Neither system is a target code dependency or binary compatibility promise.

## Accelerators and inference

Fact: Linux's [accelerator introduction](https://kernel.org/doc/html/next/accel/introduction.html) discusses DMA, submission queues and accelerator memory management. NVIDIA's [CUDA Linux installation guide](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/) requires supported hardware, OS and toolchain combinations; it does not document a TaijiOS port.

Decision: separate inference policy from accelerator mechanisms and account queue, memory and transfer costs. Hosted CUDA or Linux GPU support does not establish native support. Foreign driver stacks and inference runtimes cannot be silently shipped in an all-Ziran target. Native device support, firmware boundaries and Ziran inference remain future work.

## Synthesis and measured questions

The chosen combination is a new capability microkernel, Plan 9-inspired naming, typed Haiku-inspired app operations, constrained isolated services, durable work and Kryon presentation. This is a design proposal grounded in references, not an existing proven system assembled from them.

Open evidence includes IPC tail latency under contention, copies versus shared buffers, restart/recovery latency, journal durability, GPU reset/cancellation and interactive latency under inference load. Set workload-specific budgets before optimization or success claims. More components, a safer language or one layer's proof does not alone resolve these questions.
