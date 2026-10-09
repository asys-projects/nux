# APXH

APXH, ΑΡΧΗ, *beginning* in ancient Greek, is an ELF boot loader. It
takes a kernel in ELF form, builds the address space that the ELF
describes, optionally loads a second ELF as the first user program,
and enters the kernel with paging enabled. It boots from multiboot,
EFI and OpenSBI, and it loads i386, amd64 and riscv64 kernels. It is
an independent project: a kernel needs nothing from APXH but the
segment types described below, and APXH knows nothing of the kernel
but its ELF.

This document describes what APXH is, how it is structured so that a
port is small, the interface a machine port implements, and the
interface APXH presents to the kernel it loads.

## 1. The idea

A boot loader usually copies a kernel to memory and jumps to it. The
kernel then discovers memory, builds its page tables, relocates
itself, and only afterwards starts being a kernel. That work is
repeated by every kernel, in assembler, and tied to one machine.

APXH moves that work into the loader, and it does so without
inventing a description language: the ELF program header already is
one. A loadable segment says "these bytes, at this virtual address,
with these permissions". APXH obeys it, with paging enabled, in the
kernel's final address space. Then it extends the vocabulary. A
segment with one of the APXH specific types is not copied from the
file; it is *constructed*. The loader builds what the type names (a
direct map of physical memory, a recursive map of the page tables, a
bitmap of free physical frames, a page of boot information, a region
with its page tables pre-allocated) and maps it where the segment
says.

The result is that a kernel's whole address space is defined by its
linker script, and the loader has no opinion on where anything
lives. The kernel starts with its physical memory allocator already
populated, its direct map already in place, the memory map and the
framebuffer already known. Its startup code installs the root page
table it is handed, sets a stack, and calls C.

## 2. Structure

APXH is three parts: a machine independent core, a machine dependent
part, and an address space builder.

### The core

The core parses the ELF, walks its program headers, and acts on each
segment: copy, zero, or construct. It keeps account of the pages it
hands to the kernel, fills the boot structures once loading is
complete, and orders the whole boot. It is ordinary hosted C, written
against libec, the embedded C library in the same tree: it uses
`printf`, `memset`, `assert` and `exit` and expects nothing else. It
contains no assembler and knows nothing about firmware or about page
table formats. Every address and size it handles is 64 bit, whatever
the word size it is compiled for, because the kernel being loaded may
be wider than the loader.

### The machine dependent part

There is one for each boot environment: `multiboot`, `efi`, `sbi`.
Each is a directory with its own start code, its own linker script and
its own build, and it implements the machine interface of section 4.
The machine dependent part is where the firmware is understood and
where assembler is allowed. It is selected at link time.

### The address space builder

There is one for each page table format: PAE for i386, four-level
paging for amd64, SV48 for riscv64. A builder knows how to write page
tables of one format into memory and nothing else: not ELF, not
segments, not firmware. It implements the builder interface of
section 5. The builder is selected at run time, by the class and
machine of the ELF being loaded, not by the processor APXH runs on.

### Why this is portable

The two things that vary between machines, how the firmware starts
you and what the page tables look like, are each behind an interface,
and the core touches neither. A new boot environment is a new machine
dependent directory. A new page table format is a new builder. The
core is the same file on every port.

Because the builder follows the ELF, the loader's own architecture is
independent of the kernel's. The 32-bit multiboot loader builds a
64-bit address space and switches the processor to long mode on the
way into the kernel. Because the core is hosted C on libec, the EFI
port compiles the same core, unchanged, into one object and embeds it
in a small firmware application.

## 3. The boot, in order

1. The machine dependent part initialises: it locates the payloads
   and reads the memory map. The builder for the kernel's architecture
   allocates the root page table.
2. The kernel ELF is loaded. Loadable segments are copied into fresh
   pages mapped with the requested permissions; the thread local
   storage segment is copied and its layout recorded; APXH specific
   segments are constructed.
3. If a user ELF is present, it is loaded into the same address space
   with user permissions. Its entry point and thread local storage
   layout are recorded for the kernel.
4. Allocation of pages the kernel will inherit stops. What follows
   reports which pages are in use, so nothing may be allocated after
   it.
5. The boot structures are completed: the boot information page is
   written, every page the kernel inherits is marked in use in the
   free frame bitmap and in the frame map, the region list is copied.
6. The machine dependent part enters the kernel and does not return.

## 4. The machine interface

This is what a port implements. Every item is a fact only the firmware
knows or an operation only the firmware or the start code can
perform.

**Start.** Assembler that sets a stack, clears BSS, saves what the
firmware passed in registers, and calls the C library start. From
there the core runs as a normal C program.

**Console and exit.** A character output and an exit. These are the
only two C library functions a port provides; libec supplies the rest.
Output works before anything else, because the boot log is what the
core prints as it goes.

**Payloads.** Where the kernel ELF and the user ELF are in memory, and
their sizes. The user ELF may be absent.

**Memory map.** The physical memory of the machine as a list of
regions of three kinds: RAM, RAM already in use by the boot
environment, and physical addresses that are not RAM. With it, three
bounds: the lowest frame of RAM, the frame after the highest RAM, the
frame after the highest physical address of any kind. The port
translates the firmware's map into these kinds. Memory the firmware
or the boot environment still needs, a device tree, runtime
services, the loader's own image, is reported as RAM in use. It is
not left out.

**Framebuffer.** A description of the framebuffer the firmware set
up, or none.

**Platform description.** Whether the platform describes itself with
ACPI tables or a device tree, and the physical address of the root
structure. APXH does not read it.

**Boot pages.** A zeroed page of RAM on request. The port owns the
allocator and chooses where boot memory comes from: a bump pointer
above the payloads, or the firmware's allocator while it is still
available.

**Verification.** Every virtual range the ELF asks for is shown to
the port before it is used, so the port can refuse a range that would
overwrite something the loader still needs.

**Entry.** Given the target architecture, the physical address of the
root page table and the entry point, enter the kernel. This is the one
operation that depends on the target architecture, and it works the
same way on every machine. The port builds a transitional page table
in the target's format: an identity mapping of the memory the loader
is executing from, plus the kernel's entry page at its virtual
address. It switches to that table, changes processor mode if the
loader's mode differs from the kernel's, and jumps to the entry point
with the root page table in a register. If the firmware must be told
that the loader is finished, that happens first, and no firmware
service is used afterwards.

**Hosted firmware.** When the boot environment has its own C runtime
and calling convention, as EFI does, the port is split in two. A small
application written for the firmware gathers the payloads, the memory
map, the framebuffer and the platform pointer, hands them to APXH, and
calls its `main`. APXH itself, core and machine dependent part
together, is built against libec as one object and uses the firmware
only through the application.

## 5. The builder interface

This is what a page table format implements. The core never reads or
writes a page table entry.

- Allocate the root page table.
- Refuse a virtual range the format cannot represent.
- Populate a range: map it to fresh zeroed pages, allocating page
  tables as needed, with the given user, write and execute
  permissions.
- Translate a virtual address the builder has mapped to its physical
  address. The core writes segment contents through this.
- Map a range to physical memory with a given memory type (write
  back, write combining, uncached), using the largest page size the
  format and the processor allow.
- Pre-allocate the page tables of a range, either every level down to
  the leaf, or only the top level, without mapping anything.
- Make the recursive map: point one root entry back at the root.
- Enter: pass the root page table to the machine dependent entry.

The builder also exposes two low level operations, map one page and
direct-map a range into a given table, which the machine dependent
entry uses to build the transitional table. A builder takes the page
tables the kernel will inherit from the accounted allocator and the
page tables that exist only for the loader from the plain one.

## 6. The interface with the kernel

Everything below is a contract. A kernel's architecture support
depends on it, and so does any driver that reads the areas APXH
constructs. Changing it means changing APXH and every kernel built on
it together.

### 6.1 Segment types

A kernel requests these in its linker script by giving a program
header one of the APXH specific types. The virtual address and the
size are those of the segment. APXH decides nothing about placement.

- **Info.** One page, filled with the boot information structure of
  section 6.2.
- **Regions.** The memory map, as an array of region structures. APXH
  writes as many as fit and reports the count in the boot information
  page.
- **Stree.** The free frame bitmap, in the format of the stree
  searchable bitmap, preceded by its header. A set bit is a free RAM
  frame. Non-RAM, RAM in use by the boot environment, and the pages
  the kernel inherits from APXH are clear.
- **Pfnmap.** One byte per physical frame holding the region type.
- **Physmap.** A direct map of physical memory from address zero,
  write back cached, built with the largest pages available.
- **Linear.** The recursive map. The segment must be aligned to and
  sized as one root page table entry.
- **Framebuf.** The firmware framebuffer, mapped write combining.
  Nothing is mapped if there is none.
- **Empty.** A reserved range. Nothing is mapped or allocated.
- **Ptalloc.** A reserved range whose page tables, down to the leaf
  level, are allocated in advance. Nothing is mapped.
- **Topptalloc.** A reserved range whose top level page tables are
  allocated in advance. Nothing is mapped.

Loadable and thread local storage segments keep their ELF meaning.
The user ELF is loaded with user permissions and may not use the APXH
specific types.

### 6.2 Structures

Every structure APXH writes is packed and uses fixed width fields, so
that a 32-bit loader and a 64-bit loader write the same bytes.

- **Boot information.** A magic number; the frame after the highest
  physical address; the frame after the highest RAM; the number of
  regions written; the user entry point, zero if there is no user
  ELF; the framebuffer description; the platform description; the
  thread local storage layout of the kernel and of the user ELF.
- **Framebuffer description.** Type, physical address, size, width,
  height, pitch, bits per pixel, and the red, green and blue masks.
- **Platform description.** Type, ACPI or device tree, and the
  physical address of the root structure.
- **Region.** Type, first frame, length in frames.
- **Stree header.** A magic number, a version, the order of the tree,
  and the offset and size of the bitmap that follows.
- **Thread local storage layout.** The virtual address of the
  initialised image, its size, and the total size including the
  uninitialised part.

### 6.3 The processor at entry

The processor is in the kernel's mode. Paging is enabled on the
transitional page table: the kernel's entry page is mapped at its
virtual address and the loader's memory is identity mapped.
Interrupts are disabled. There is no stack. The kernel's root page
table is in a register, by a fixed convention for each architecture:
on x86 its physical address is in the second argument register, on
RISC-V the `satp` value is in `a0`. The kernel installs it as its
first instruction. Nothing else is passed.

### 6.4 The payload

Where the boot environment loads a single file, the kernel and the
user ELF travel inside APXH. Each is prefixed with a small header
carrying a magic number, a name and a size, and the sequence is
appended to the APXH image after its last byte of BSS. At run time
APXH walks the sequence from its own end: the first entry is the
kernel, the second, if present, the user ELF. Where the firmware has
a file system, as on EFI, the two are instead read from the boot
volume by name.

## 7. Boot memory

Every page APXH allocates for the kernel is zeroed, and every one is
accounted for: when the free frame bitmap and the frame map are
completed, those pages are marked in use. The kernel's page tables are
among them. Pages that exist only for the loader, the transitional
page table and the trampoline, are not accounted for and are free RAM
to the kernel. APXH never frees memory. It allocates forward, once,
and is gone when the kernel starts.

## 8. How it is written

The simplicity of APXH is deliberate, and a port keeps it.

- The core dispatches to the builder with a `switch` on the
  architecture, and to the machine dependent part by linking. There
  are no function tables.
- The machine dependent part answers questions: how many regions,
  which region, which frame is the highest. The core asks when it
  needs to know. State lives `static` in the file that owns it.
- The core is hosted C on libec. Assembler lives in the start code
  and in the entry, and nowhere else.
- Every address and size that crosses an interface is 64 bit. The
  loader narrows to its own pointer width only when it touches its
  own memory.
- Every step prints what it found and what it did. The boot log is
  the record of the boot.
- Anything unexpected, an ELF for the wrong architecture, a segment
  that cannot be placed, a range the loader needs, stops the boot
  with a message. APXH does not continue on a half-built address
  space.

## 9. Rules

- The kernel's layout is its linker script. APXH never chooses an
  address.
- The core knows no firmware and no page table format.
- The machine dependent part provides facts and primitives. It does
  not decide what is loaded or where.
- The machine dependent part reports all RAM. Memory the firmware
  still needs is reported in use, never omitted.
- The builder is selected by the ELF, never by the processor APXH
  runs on.
- The builder writes page tables and nothing else.
- A segment type is an interface. Adding one means defining what APXH
  constructs and what the kernel may assume, and updating every kernel
  that reads it.
- APXH does not interpret what the kernel does with an area.
- No page the kernel inherits is allocated after the boot structures
  begin to be completed.
- Structures written by APXH are packed and fixed width, identical
  across loader word sizes.
