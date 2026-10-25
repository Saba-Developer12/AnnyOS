# AnnyOS SDL library

SDL is the AnnyOS source library for AADL applications. Import
`lib/SDL.aadl` to use the shared classes:

- `SDLConsole`: startup text and command help.
- `SDLSystem`: version, date, time, and memory reports.
- `SDLStorage`: the file commands available in the AnnyOS shell.
- `SDLSettings`: list/read/write the persisted console theme settings.
- `SDLAudio`: play the kernel's short PC-speaker tone.
- `SDL`: a convenience facade for startup, system summary, and saved-theme
  reporting.

```aadl
import "lib/SDL.aadl";

let system = new SDL();
system.welcome();
system.systemSummary();
system.showTheme();
```

Build the example with:

```sh
python3 tools/aadlc.py apps/SDLDEMO.aadl build/SDLDEMO.AP
```

The library is written in AADL and compiles to the bounded AnnyOS `.AP` format.
It exposes only operations backed by the current kernel API; for example,
`SDLStorage` documents shell file commands but does not grant AP applications
direct disk access.
