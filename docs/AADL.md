# AADL compiler preview

`tools/aadlc.py` compiles AADL source into the interpreted AnnyOS `.AP` v1
format. It is a host-side Python 3 compiler; the output is validated bytecode,
not native x86 code.

```sh
python3 tools/aadlc.py apps/AADLDEMO.aadl build/AADLDEMO.AP
python3 tools/aadlc.py app.aadl app.AP -I libraries
```

## Syntax

```aadl
import "lib/Greeter.aadl";

class BrandedGreeter extends Greeter {
    method greet() {
        print("Hello from AADL.");
    }
}

let greeter = new BrandedGreeter();
greeter.greet();
api(VERSION);
```

Imports resolve recursively relative to the importing file first, so an import
such as `import "lib/Console.aadl";` works from `apps/STARTUP.aadl`, and imports
inside `Console.aadl` resolve from that library's directory. `-I DIR` adds
additional search roots. Import paths must be relative `.aadl` files and must
remain under the entry directory or an explicit include directory. The loader
deduplicates normalized paths, reports complete import cycles, and bounds the
graph to 64 modules, 32 nested imports, 1 MiB per file, and 8 MiB total source.
Imported modules may contain imports and classes; only the entry file may
contain top-level statements. Duplicate classes, unknown base classes, and
inheritance cycles are errors.

`apps/lib/` contains the core AADL libraries: `Console`, `SysInfo`,
`TimeManager`, `PowerControl`, `HelpSystem`, `AudioDevice`, `SecurityGuard`,
`MemoryLogger`, `UITheme`, and `AppKernelCore`. These are source-level wrappers
around the AP v1 services and console operations that exist today. In
particular, AP v1 does not provide file logging, a security isolation boundary,
or an application-controlled color API, so `MemoryLogger`, `SecurityGuard`,
and `UITheme` describe/report current behavior rather than claiming those
missing kernel facilities exist.

Supported statements are `print("text");`, `title("text");`,
`row("COMMAND", "Description");`, `api(SERVICE);`, `arg(INDEX);`,
`let name = new ClassName();`, `name.method();`, `this.method();`, and
`new ClassName().method();`. Methods use `method name() { ... }`, take no
runtime parameters, and may call other methods. Inheritance uses
`class Child extends Parent { ... }`; a method declared in the child overrides
the inherited method. API names are `VERSION`, `DATE`, `TIME`, `MEMORY`,
`REBOOT`, `SLEEP`, and `BEEP`.

## AP v1 execution model and bounds

AP v1 has no object heap, runtime variables, function-call opcode, or native
execution. `new` therefore creates a compile-time object binding, and method
calls are statically resolved and inlined into the output bytecode. Inherited
method lookup and overrides are resolved at compile time. Recursive method
calls are rejected because they cannot be safely expanded into bounded AP v1
code. There are no constructors, object fields, or method parameters.

The compiler emits only AP v1 `PRINT`, `TITLE`, `HELP_ROW`, `API`, `ARG`, and
final `EXIT` instructions. It enforces the kernel's 95-byte printable-ASCII
text bound, help-row field limits, API/argument ranges, 4096-instruction bound,
and 64 KiB image bound. It writes the exact 40-byte little-endian AP header and
FNV-1a checksum expected by `src/kernel/ap_loader.cpp`. FNV-1a detects
accidental corruption; it does not authenticate a program.
