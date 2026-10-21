#!/usr/bin/env python3
"""Pack a small line-oriented AnnyOS AP bytecode source file."""

import pathlib
import struct
import sys


MAGIC = b"ANNYAP1\0"
HEADER_SIZE = 40
MAX_FILE_SIZE = 64 * 1024
MAX_INSTRUCTIONS = 4096
OP_PRINT = 0x01
OP_API = 0x02
OP_HELP_ROW = 0x03
OP_TITLE = 0x04
OP_EXIT = 0xFF
API_SERVICES = {
    "VERSION": 1,
    "DATE": 2,
    "TIME": 3,
    "MEMORY": 4,
    "REBOOT": 5,
}


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: ap_pack.py input.apasm output.AP", file=sys.stderr)
        return 2

    source = pathlib.Path(sys.argv[1]).read_text(encoding="ascii")
    code = bytearray()
    instruction_count = 0
    for line in source.splitlines():
        if line.startswith("API "):
            service_name = line[4:].strip().upper()
            if service_name not in API_SERVICES:
                raise SystemExit(f"Unknown AnnyOS API service: {service_name}")
            code.extend((OP_API, API_SERVICES[service_name]))
            instruction_count += 1
            continue
        if line.startswith("ROW "):
            parts = line[4:].rsplit(" | ", maxsplit=1)
            if len(parts) != 2:
                raise SystemExit("AP ROW syntax: ROW command | description")
            command = parts[0].strip().encode("ascii")
            description = parts[1].strip().encode("ascii")
            if not 1 <= len(command) <= 16 or len(description) > 78:
                raise SystemExit("AP ROW command must be 1-16 bytes; description at most 78")
            if any(byte < 32 or byte > 126 for byte in command + description):
                raise SystemExit("AP ROW fields must be printable ASCII")
            code.extend((OP_HELP_ROW, len(command), len(description)))
            code.extend(command)
            code.extend(description)
            instruction_count += 1
            continue
        if line.startswith("TITLE "):
            encoded = line[6:].encode("ascii")
            if len(encoded) > 95 or any(byte < 32 or byte > 126 for byte in encoded):
                raise SystemExit("AP title must contain at most 95 printable ASCII bytes")
            code.extend((OP_TITLE, len(encoded)))
            code.extend(encoded)
            instruction_count += 1
            continue
        encoded = line.encode("ascii")
        if len(encoded) > 95 or any(byte < 32 or byte > 126 for byte in encoded):
            raise SystemExit("AP source lines must contain at most 95 printable ASCII bytes")
        code.extend((OP_PRINT, len(encoded)))
        code.extend(encoded)
        instruction_count += 1
    if instruction_count + 1 > MAX_INSTRUCTIONS:
        raise SystemExit(f"AP application exceeds the {MAX_INSTRUCTIONS}-instruction limit")
    code.append(OP_EXIT)

    file_size = HEADER_SIZE + len(code)
    if file_size > MAX_FILE_SIZE:
        raise SystemExit(f"AP application exceeds the {MAX_FILE_SIZE}-byte format limit")
    header = struct.pack(
        "<8sHH7I",
        MAGIC,
        1,
        HEADER_SIZE,
        0,
        file_size,
        HEADER_SIZE,
        len(code),
        0,
        fnv1a(code),
        1,
    )
    pathlib.Path(sys.argv[2]).write_bytes(header + code)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
