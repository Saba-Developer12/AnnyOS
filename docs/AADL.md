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
`REBOOT`, `SLEEP`, `BEEP`, `CONFIG_LIST`, `CONFIG_GET_THEME`,
`CONFIG_SET_THEME_BLUE`, `CONFIG_SET_THEME_GREEN`, `CONFIG_SET_THEME_AMBER`,
and `CONFIG_SET_THEME_MATRIX`. ConfigSave services require API v2; the compiler
sets the AP header's minimum API version automatically. Import
`lib/ConfigSave.aadl` to use the `ConfigSave` wrapper class.

API v3 adds six service groups, exposed by `SystemKernelApi`,
`FileSystemDriverApi`, `MemoryStorageApi`, `AudioSpeakerApi`,
`GraphicsConsoleApi`, and `InputDeviceApi` in `apps/lib/`. Their service names
are `SYS_VERSION`, `SYS_REBOOT`, `SYS_SHUTDOWN`, `SYS_UPTIME`, `SYS_CLEAR`,
`SYS_GET_TICK`, `SYS_SET_ENV`, `SYS_GET_ENV`, `SYS_CPU_INFO`, `SYS_PANIC`;
`FS_CREATE`, `FS_READ`, `FS_EDIT`, `FS_DELETE`, `FS_EXISTS`, `FS_LIST_DIR`,
`FS_MKDIR`, `FS_RMDIR`, `FS_COPY`, `FS_MOVE`, `FS_MOUNT_APFS`, `FS_MOUNT_NTFS`;
`MEM_ALLOC`, `MEM_FREE`, `MEM_TOTAL`, `MEM_FREE_SPACE`, `MEM_COPY`, `MEM_SET`,
`DISK_INFO`, `DISK_FORMAT`; `BEEP`, `PLAY_TONE`, `AUDIO_STOP`, `SOUND_EFFECT`,
`SPEAKER_INIT`, `AUDIO_VOLUME`; `UI_SET_TITLE`, `UI_SET_COLOR`, `UI_DRAW_PIXEL`,
`UI_DRAW_RECT`, `UI_CLEAR_SCREEN`, `UI_PRINT_TEXT`, `UI_CURSOR_POS`,
`UI_OPEN_EDITOR`, `UI_THEME_DARK`, `UI_THEME_LIGHT`; and `INP_READ_KEY`,
`INP_CHECK_KEY`, `MOUSE_GET_POS`, `MOUSE_CHECK_CLICK`, `SERIAL_WRITE`,
`SERIAL_READ`, `USB_SCAN`, `TIME_GET_RTC`. `SYS_VERSION`, `SYS_REBOOT`, and
`BEEP` keep their API v1 service numbers. The other v3 calls require an AP
header declaring API v3.

API v4 adds the `CURL` service (ID 68). Import `lib/Curl.aadl` or run
`apps/CURL.aadl`; the command line is passed through the AP argument table.
The kernel forwards requests over the configured host bridge to upstream curl.
This host-assisted command is not a native guest network client.

Disk management services include `DISK_INFO` plus `DISK_LIST`, `DISK_SPACE`,
and `DISK_USE`. Import `lib/DiskManagement.aadl` for the
`DiskManagementApi` wrapper. `apps/DISKDEMO.aadl` is a sample which lists
detected ATA disks and reports the selected drive's details and FAT32 space.

Service operands come from the application's command line, split into at most
eight quoted or unquoted values of up to 95 bytes each. For example,
`APP.AP "CONFIG" "blue"` passes `CONFIG` as operand 0 and `blue` as operand 1
to every API call in that app. File calls use FAT32 short names in the current
directory. `FS_COPY` and `FS_MOVE` accept source and destination names and are
bounded to 4 KiB; `FS_RMDIR` removes only empty directories. `MEM_ALLOC` uses a
bounded 8 KiB kernel-owned arena and reports a handle, rather than exposing
arbitrary physical memory. Environment entries last for the current boot.

Hardware and storage calls are constrained by existing kernel support:
APFS/NTFS mount requests report unavailable because those drivers do not yet
mount volumes; disk formatting is deliberately disabled; mouse calls report
unavailable because no mouse device is registered; USB scan detects xHCI but
does not enumerate devices. PC speaker volume is mute/full only, and key/serial
reads are non-blocking. `UI_CURSOR_POS` is unavailable in the text console.
These handlers return status text instead of pretending those operations
succeeded. All implemented calls are dispatched through the API v4 host table;
older API v1/v2 application binaries keep using the original callback layout.

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

`apps/lib/SDL.aadl` is the AnnyOS SDL library facade. It groups the currently
available console, system information, clock, storage-command help, saved
theme settings, and audio services into reusable AADL classes. Compile
`apps/SDLDEMO.aadl` for a small example. Storage methods expose the shell's
documented file commands; direct file operations remain a kernel CLI feature.
Each SDL method maps to an existing AnnyOS AP service or AADL library, so the
module does not claim facilities that the current kernel API does not provide.
