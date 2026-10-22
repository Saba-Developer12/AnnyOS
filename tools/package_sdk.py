#!/usr/bin/env python3
"""Create the versioned AnnyOS AP SDK archive used by tagged releases."""

import pathlib
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: package_sdk.py output.zip", file=sys.stderr)
        return 2
    root = pathlib.Path(__file__).resolve().parents[1]
    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    files = [
        root / "include/annyos/api.hpp",
        root / "include/annyos/ap_format.hpp",
        root / "tools/aadlc.py",
        root / "docs/AP_FORMAT.md",
        root / "docs/AADL.md",
        root / "docs/SDK.md",
        *sorted((root / "apps").rglob("*.aadl")),
    ]
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            if not path.is_file():
                raise SystemExit(f"SDK file is missing: {path}")
            archive.write(path, path.relative_to(root).as_posix())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
