# AnnyOS SDK preview

The current SDK preview targets the interpreted `.AP` v1 format. It contains
the public API and format headers, the AADL compiler, format and
language documentation, and example application sources. It does not include
a native x86 compiler ABI; that requires a future user-mode process and
syscall design.

To compile an application on a host with Python 3:

```sh
python3 tools/aadlc.py apps/HELLO.aadl HELLO.AP
python3 tools/aadlc.py apps/AADLDEMO.aadl AADLDEMO.AP
```

AADL supports imported modules, classes, inheritance, compile-time object
creation, and methods. Since AP v1 has no heap or call instruction, AADL object
bindings are resolved by the compiler and method bodies are inlined. See
[`AADL.md`](AADL.md) for the syntax and limits.

AP programs may write text, draw aligned help rows, or call version 1, 2, and 3
services. The API version in each AP header must be supported by the target
AnnyOS kernel. API v3 declarations and six AADL system libraries are documented
in [`AADL.md`](AADL.md).

AADL exposes `print`, `title`, `row`, `api`, and `arg` statements. Applications
can print up to eight command-line arguments using indexes `0` through `7`.
The service set includes version/date/time/memory, reboot, a short sleep,
PC-speaker control, ConfigSave theme operations, system and kernel calls,
FAT32 file operations, bounded application memory, console drawing, keyboard,
serial, USB-controller probing, and RTC access. ConfigSave calls require API v2;
the newer service calls require API v3. The compiler records the required
minimum version in each AP header. The `apps/lib/` modules expose the API
groups as AADL classes. Imports resolve recursively relative to each source
file; `-I DIR` adds an import search root.

`apps/lib/FileSystem.aadl` supplies the file-command rows used by `HELP`.
Current AP v1 applications can display this help, but filesystem mutation is
available only through the kernel CLI (`CREATE`, `EDIT`, `READ`, and `DELETE`);
the AP API does not grant applications direct file access.

The AnnyOS SDL library in `apps/lib/SDL.aadl` groups the currently available
console, system, clock, storage-help, theme-settings, and audio wrappers. See
[`SDL.md`](SDL.md) and compile `apps/SDLDEMO.aadl` for an example.
