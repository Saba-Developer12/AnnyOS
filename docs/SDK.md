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

AP programs may write text, draw aligned help rows, or call the documented
version 1 and version 2 system services. The API version in each AP header must
be supported by the target AnnyOS kernel.

AADL exposes `print`, `title`, `row`, `api`, and `arg` statements. Applications
can print up to eight command-line arguments using indexes `0` through `7`.
The service set includes version/date/time/memory, reboot, a short sleep, a
PC-speaker tone, and ConfigSave list/get/set operations for console themes.
ConfigSave calls require API v2; the compiler records that minimum version in
the AP header. The SDK's `apps/lib/ConfigSave.aadl` module exposes these calls
as a `ConfigSave` class. Imports resolve recursively relative to each source
file; `-I DIR` adds an import search root. `apps/CFGDEMO.aadl` is a small
example that uses the class to list persisted settings.
