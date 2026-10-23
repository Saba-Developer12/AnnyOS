# AnnyOS architecture expansion plan

This document separates current working foundations from future capabilities.
AnnyOS is an experimental UEFI kernel; it is not currently an enterprise-grade
or general-purpose operating system.

## Current implementation

- x64 UEFI application and freestanding kernel entry in one EFI image.
- Polling CLI, FAT32 over legacy ATA PIO, PS/2 keyboard, PIT, COM1, and AP v1
  bytecode applications.
- A fixed-capacity (128 driver, 128 service) startup registry with dependency
  ordering, versioned interface publication, failure state, and no heap use.
- AADL imports, compile-time classes/inheritance, static method resolution,
  and bounded compilation to AP v1.
- `.sys` PE images are produced for packaging, but the kernel does not load
  them dynamically. They are not yet a stable third-party driver ABI.

## Staged engineering work

### 1. Stabilize kernel contracts

- Move kernel services and hardware access behind versioned interfaces.
- Add interrupt-safe synchronization and lifecycle rules before enabling
  concurrent or interrupt-driven driver callbacks.
- Separate device discovery, bus enumeration, function drivers, and filesystem
  drivers. Track resources and initialization failures per device instance.
- Add bounds-checked logging/diagnostics and reproducible build metadata.

### 2. Define a real driver module format

- Specify a signed or explicitly trusted module envelope, architecture/ABI
  version, imports, exports, relocation types, entry points, and unload policy.
- Build a PE/COFF parser and relocation loader with strict range/overflow
  checks; do not invoke arbitrary PE exports as if they were a kernel driver.
- Establish memory allocation, ownership, cleanup, and failure containment.
- Load one in-tree module from FAT32, then test dependency failure and malformed
  modules before expanding the catalog. Add drivers only for hardware with
  implemented and validated protocols; do not generate placeholder `.sys`
  files to reach a count.

### 3. Evolve AADL and AP together

- Define a typed syntax and semantic model (types, scoped names, signatures,
  inheritance validation, and diagnostics) before adding syntax piecemeal.
- Introduce a versioned intermediate representation and AP v2 instruction set
  for locals, parameters, calls, branches, and bounded loops.
- Extend the kernel verifier/interpreter first: validate stack effects,
  control-flow targets, instruction limits, memory bounds, and API versions.
- Then add compiler code generation, standard library APIs, formatter/linter,
  and compiler diagnostics. Keep AP v1 compatibility explicit.

### 4. Improve subsystem boundaries incrementally

- Extract shell, filesystem namespace, boot services, and device management
  from `main.cpp` into small kernel-owned components with explicit interfaces.
- Use C++ classes where ownership and lifecycle are clear; avoid exceptions,
  RTTI, heap-dependent containers, and virtual dispatch in early boot unless
  their runtime and memory costs are defined.
- Keep the SDK's application API separate from privileged kernel interfaces.
- Introduce paging, privilege separation, process lifecycle, and system-call
  validation before treating AP/native applications as isolated user programs.

## Completion bar

Each stage should build on Windows and the POSIX wrapper, document the ABI,
provide malformed-input and hardware-failure coverage, and avoid claiming a
feature until it executes end-to-end. A registry capacity is not a driver
catalog, a packaged PE is not a loaded module, and source-level classes are not
runtime OOP objects.
