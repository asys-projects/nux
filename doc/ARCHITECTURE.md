# NUX Architecture

This document describes what NUX is, the contract between NUX and the
kernel built on it, and the rules that keep the design intact. It is
written for whoever works on NUX or on a NUX kernel, person or agent.
It is about intent. Where it disagrees with the code, the code is
wrong.

## 1. What a kernel is

A kernel is an executable that runs in privileged mode. It is an event
driven program: hardware raises exceptions, interrupts and system
calls, the kernel reacts, and the processor goes back to running
something else. It does this on every CPU of the machine at once.

Everything that makes writing one hard is bootstrap: the processor
starts in a mode nobody wants, memory has no shape, there is no stack,
no virtual memory, no idea of what hardware exists. A kernel project
spends its first year rediscovering this. NUX exists to remove that
burden, once, portably, so that the author can spend the time on the
kernel itself.

With NUX, a kernel is a C program. It has a `main`, it is linked
against a small C library, and it defines a handful of functions that
say what happens on each kind of event. That is the whole of it.

## 2. The pieces

**APXH**, ΑΡΧΗ, the Greek for beginning, is the boot loader. It
loads an ELF, builds the address space
the ELF asks for, loads a second ELF as the first user program, and
jumps to the kernel's entry point in a fully formed virtual address
space. It is the closest thing to `exec()` one can have at boot. It
has its own document, `apxh/README.md`.

**libec** is the C library. It is small, embedded, mostly NetBSD code,
and non standard where the standard does not help. Every NUX binary is
built against it: APXH, the kernel, the example user programs.

**libhal** is the hardware abstraction layer. It knows the CPU: how
page tables are laid out, what a trap frame looks like, how to enter
and leave the kernel, how to bring up another processor, how to find
the memory the boot loader described. There is one per architecture.

**libplt** is the platform library. It knows the board: how to find
the CPUs, the interrupt controllers, the timer, how to send an
interrupt to another processor, how to turn the machine off. There is
one per platform description, ACPI or SBI, and it does not know which
CPU it runs on.

**libnux** is the machine independent kernel library. It owns memory
management, CPU management, user address spaces, user contexts, and
the dispatch of events to the kernel. It is the only layer a kernel
talks to.

**libnux_user** is the user side: how a program on top of a NUX kernel
issues a system call.

The split between HAL and platform is deliberate and possibly unique.
It lets an architecture be ported one layer at a time, and it means a
new CPU does not touch the platform code. When amd64 was added, the
ACPI library did not change.

## 3. Boot

APXH reads the kernel's program headers. Ordinary loadable segments
are copied. Segments with NUX specific types are not copied but
*constructed*: APXH builds what the type asks for and maps it where the
segment says. These types, and the structures APXH fills through them,
are the interface between loader and kernel:

- the boot information page: memory limits, the platform description
  pointer (ACPI tables or device tree), the framebuffer description,
  the user entry point, thread local storage layout;
- the memory region list: what is RAM, what is device memory, what
  the boot took;
- the searchable bitmap of free physical pages, built by APXH, so that
  the kernel's physical allocator is ready the instant it starts;
- the direct physical map;
- the linear (recursive) map of the page tables;
- areas whose page tables are allocated in advance, in full or only at
  the top level, so that parts of the kernel can map memory without
  ever allocating a page table;
- the framebuffer.

The kernel's address space is therefore defined by its linker script.
There is no code that decides where kernel memory lives: the layout is
data, and APXH honours it. A kernel, a HAL, or a driver that needs a
region of the address space with a given property asks for it in the
linker script.

APXH loads a user ELF too, into the user half of the same address
space, and tells the kernel where its entry point is. A NUX system can
boot straight into its first user program with nothing but the loader.

When APXH jumps to the kernel, the HAL takes over, verifies the boot
information, sets up what the CPU needs to take traps, and starts the
C runtime. Before `main` runs, NUX initialises itself in order: memory
management, then the platform (CPU discovery, interrupt controllers,
timer), then CPU management, then the secondary processors. Each stage
unlocks a set of operations. A function that needs the platform cannot
be called before the platform exists; a function that needs CPU
operations cannot be called before CPUs are known. Every function in
libnux is annotated with the stage it requires.

Then `main` runs on the boot processor with every other processor
already up and waiting. Each secondary processor has run `main_ap` and
gone idle. `main` initialises the kernel, and returns. The return
value says whether this CPU halts or goes idle. Going idle is how a
kernel hands control to events.

## 4. The contract

What follows is the contract between NUX and a kernel. It is not a
list of implementation choices. Changing any of it changes what NUX
is.

### 4.1 Events in, context out

A NUX kernel is a set of entry functions, one per kind of event.
There is nothing else. The kernel's whole behaviour is what these
functions do.

`main` runs once, on the boot processor, when NUX is initialised and
before any other processor runs kernel code. It sets up the kernel and
returns. `main_ap` runs once on every secondary processor, after the
boot processor has finished initialising NUX, and sets up what that
CPU needs. Both return an exit value: halt this CPU, or make it idle
and let events drive it from now on.

`entry_sysc` is a system call. It receives the caller's context and
seven argument words, the first of which is by convention the system
call number. The return value is written into the context, through
the context operations, before it is resumed.

`entry_pf` is a page fault in user space. It receives the faulting
address and what the hardware knows about the fault: whether the page
was not present or the access was not permitted, and whether it was a
write, a user access, an instruction fetch. Page faults in the kernel
are not events; they are bugs, and NUX panics on them, except for
faults inside the user memory helpers, which NUX recovers from itself.

`entry_ex` is any other exception raised by a user program: an invalid
instruction, a division by zero, a breakpoint, an attempt to use the
FPU while it is disabled. The exception number is the architecture's
own. Exceptions in the kernel are bugs, and NUX panics on them.

`entry_alarm` is the platform timer firing. The alarm is one shot: the
kernel asks for an alarm some nanoseconds in the future, receives this
event once, and asks again if it wants another. The platform's notion
of time is exposed as a monotonic counter in nanoseconds.

`entry_irq` is a device interrupt. It receives the platform IRQ number
and whether the line is level triggered. The kernel enables and
disables IRQs through the platform interface; NUX acknowledges the
interrupt controller when the entry returns.

`entry_ipi` is an inter-processor interrupt sent by the kernel itself
to this CPU, from any CPU. It carries no payload; what it means is the
kernel's business.

The timer, IRQ and IPI events arrive only against a user or idle
context, because the kernel runs with interrupts off. System calls,
page faults and exceptions can only come from user programs. The
end of interrupt is NUX's job, not the kernel's.

Every entry receives a user context and returns a user context. The
context received is the state of the user program that was running
when the event happened. The context returned is the state the CPU
will resume when the entry finishes. They need not be the same.

Returning the context received resumes the interrupted program. The
kernel may have modified it first: this is how a system call returns a
value. Returning a different context resumes a different program: this
is how threads, processes and scheduling exist. Returning the idle
context puts the CPU to sleep until the next event.

Some events happen while the CPU is idle. Then the context received is
the idle context: there was no program to interrupt.

### 4.2 Contexts are values

The context an entry receives lives on the CPU's kernel stack. It is
gone when the entry returns. The context an entry returns is a value:
a pointer to a frame anywhere in kernel memory that the kernel owns.

This is the whole process model. A thread's saved state *is* a
context. To suspend a thread, copy the context received into the
thread's structure. To resume one, return the stored context, or copy
it over the one received and return that. NUX does not know what a
thread or a process is. It knows contexts, and it resumes the one it is
given.

NUX provides the operations a kernel needs on a context without
knowing the architecture: set and read the instruction and stack
pointers, set argument registers, set the return value, set the thread
pointer, and build a fresh context for a new program, including the
boot time user program APXH loaded.

### 4.3 The kernel is not preemptible

Kernel code runs with interrupts disabled. Each CPU has one kernel
stack. An entry runs to completion, returns a context, and the CPU
leaves the kernel. Nothing in NUX sleeps, blocks, or switches kernel
stacks, and nothing in a NUX kernel can either.

This is why the kernel can be written as plain functions, why there is
no kernel scheduler to reason about, and why the only concurrency a
kernel author faces is the other CPUs.

### 4.4 The NMI belongs to NUX, the IPI to the kernel

The one thing that interrupts kernel code is the non-maskable
interrupt, and it is NUX's own. NUX uses it to signal other CPUs:
flush TLBs, pick up changes to the kernel address space, stop every
CPU on panic. Its handlers do atomic operations and nothing else. A
kernel never sees it.

The inter-processor interrupt is the kernel's. It is delivered like
any other event, only against a user or idle context, and the kernel
decides what it means.

Architectures without a real NMI emulate one. The emulation preserves
the model: the kernel still never sees a NUX interrupt, and still
receives its IPI only at a context boundary.

### 4.5 NUX is a library, not a thread

NUX provides mechanism and keeps no policy. It does not lock the
structures it hands out. When a kernel changes a kernel mapping or a
user address space, it is changing its own global state, and it
already holds whatever lock protects that state. NUX adding a second
lock underneath would be a second opinion. The same applies to
contexts, address spaces and frames: NUX tells the kernel what
happened, and the kernel decides what it means.

What NUX does lock is its own private state: its allocators, its
caches, its per-CPU bookkeeping. A kernel never sees those locks and
never needs to.

## 5. Memory

Memory is built in layers, each one only on the ones below it.

**Physical frames.** The unit is the page frame number. Frames come
from the searchable bitmap APXH built, which finds the lowest or
highest free frame in logarithmic time. A kernel that needs to account
for physical memory, as an exokernel does, installs its own allocator
and from then on owns every frame, including the ones NUX allocates
for its page tables and caches.

**Touching a frame.** Any frame can be reached for a short time on one
CPU. Frames inside the direct physical map are reached through it.
Frames beyond it are mapped on demand into a small, cached area whose
page tables APXH allocated in advance, so that mapping never needs to
allocate. Every access is paired with a release.

**Kernel mappings.** The kernel address space is shared by all CPUs
and edited directly, one leaf entry at a time, through the HAL's view
of a page table entry: a pointer to a leaf entry, and the value in it.
Each edit records which kind of TLB invalidation it requires. CPUs
catch up lazily: every CPU keeps the generation of kernel mappings its
TLB reflects, and flushes when it notices it is behind, or when told
to by NUX's interrupt. Committing a batch of kernel mapping changes
means telling every CPU to look.

**Kernel virtual memory.** Above the mapping layer sit an allocator of
kernel virtual ranges and a kernel heap. The heap grows from both ends
of its area toward the middle, so that two kinds of allocation, long
lived and short lived, do not fragment each other, and it can give
pages back when told to.

**User address spaces.** A user address space is a small object: the
top level page table entries that cover the user half of the address
space, plus the set of CPUs that currently have it loaded. Loading one
into a CPU copies those entries into that CPU's own root table. Every
CPU has its own root table and its own kernel half; what changes is
the user slice. Editing a user address space is the same leaf entry
operation as the kernel's, whether the space is loaded on the current
CPU or sitting in memory. Committing the changes flushes the TLBs of
exactly the CPUs that have it loaded.

**User memory.** The kernel reads and writes user memory through
helpers that recover from page faults. A fault during a copy is
reported back to the kernel, which can repair the mapping and let the
copy continue, or fail the operation. The kernel never has to guess
whether a user pointer is good.

## 6. Panic and the console

When something is wrong in the kernel, NUX panics: it stops every
other CPU with its interrupt, prints the state of the faulting CPU,
the frame, a stack trace resolved against the kernel's own symbol
table, and the page table walk of the faulting address, and halts.

The console is designed for that moment. Output goes to the serial
line and to the screen. On a framebuffer the screen is divided into
columns: text fills one column from top to bottom, then moves to the
next, and wraps around only when every column is full. Nothing
scrolls. After a crash, several screens of history are still visible.

Early boot output and the panic report always use the HAL console.
The ordinary log is the kernel's to take over once it has a console
of its own.

## 7. Rules

The rules are few because the contract does most of the work.

- An entry returns a context. It does not loop forever, it does not
  wait for another CPU, it does not switch stacks. If there is nothing
  to run, it returns idle.
- The context received dies with the entry. Anything a kernel wants to
  keep from it, it copies.
- The context returned is the kernel's memory, kernel mapped, and
  alive until the hardware has resumed it.
- The NMI is NUX's. A kernel does not send it, handle it, or depend on
  it beyond the fact that panic stops all CPUs.
- NUX does not lock what it hands to the kernel. The kernel locks its
  own state.
- The address space layout is in the linker script. Code does not
  decide where things go.
- Every libnux function says which boot stage it needs. Calling it
  earlier is a bug in the caller.
- Mechanism goes in NUX; policy goes in the kernel. If a change to NUX
  encodes what a kernel should do, it belongs in the kernel.

## 8. Style

Style is part of the architecture because it is how the architecture
stays visible.

Names are short, lower case, and read as subsystem, object, verb. Verbs
come in pairs: get and put, map and unmap, init and free, enter and
exit, box and unbox, alloc and free. A typedef with a `_t` suffix is a
type: a frame number, an address, a generation, a handle. A structure
is a `struct` and carries no suffix.

Invalid values are named sentinels. Success is a boolean, results come
back through pointers. The comment above a declaration in a public
header is the specification of that function; code that disagrees with
it is wrong.

Layout is fixed by the indent script in the tree, GNU style: two space
indentation, braces on their own lines, a space before a parenthesis,
the return type on its own line above the function name. Nobody argues
about layout.

Clear C. No long descriptive identifiers, no layers of accessors, no
abstraction that exists to be an abstraction. A system programmer
reading a function should see what the hardware is doing.
