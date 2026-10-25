#!/usr/bin/env python3
"""Build AnnyOS.img as an MBR-partitioned, FAT32 EFI disk."""

import math
import pathlib
import struct
import sys
from typing import Optional

SECTOR = 512
PARTITION_LBA = 2048
PARTITION_SECTORS = 262144  # 128 MiB FAT32 volume
IMAGE_SECTORS = PARTITION_LBA + PARTITION_SECTORS
RESERVED_SECTORS = 32
FAT_COUNT = 2
SPC = 1
EOC = 0x0FFFFFFF
ALIASES = {
    "Segoe UI.ttf": "SEGOEUI.TTF",
    "Segoe UI Bold.ttf": "SEGOEBOL.TTF",
    "Segoe UI Italic.ttf": "SEGOEIT.TTF",
    "Segoe UI Bold Italic.ttf": "SEGOEBI.TTF",
}
MAX_PRESERVED_FILE_BYTES = 64 * 1024 * 1024
MAX_PRESERVED_DIRECTORY_BYTES = 16 * 1024 * 1024


def short_name(name: str) -> bytes:
    alias = ALIASES.get(name, name).upper()
    base, dot, ext = alias.partition(".")
    if not base or len(base) > 8 or len(ext) > 3 or (dot and not ext):
        raise ValueError(f"FAT 8.3 name required or add an alias: {name}")
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$~!#%&-{}()@'`"
    if any(char not in allowed for char in base + ext):
        raise ValueError(f"Unsupported FAT short name: {name}")
    return base.encode("ascii").ljust(8, b" ") + ext.encode("ascii").ljust(3, b" ")


def dir_entry(name: bytes, attr: int, cluster: int, size: int = 0) -> bytes:
    entry = bytearray(32)
    entry[:11] = name
    entry[11] = attr
    struct.pack_into("<H", entry, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", entry, 26, cluster & 0xFFFF)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def existing_files(image_path: pathlib.Path) -> dict:
    """Read the previous FAT32 tree so user files survive a rebuild."""
    if not image_path.is_file():
        return {}
    try:
        with image_path.open("rb") as image:
            image_size = image_path.stat().st_size

            def read_sector(lba: int) -> Optional[bytes]:
                offset = lba * SECTOR
                if offset < 0 or offset + SECTOR > image_size:
                    return None
                image.seek(offset)
                data = image.read(SECTOR)
                return data if len(data) == SECTOR else None

            mbr = read_sector(0)
            if mbr is None or mbr[510:512] != b"\x55\xaa":
                return {}
            partition_lba = None
            partition_size = 0
            for index in range(4):
                entry = mbr[446 + index * 16:462 + index * 16]
                if entry[4] in (0x0B, 0x0C, 0x0E, 0xEF):
                    partition_lba = struct.unpack_from("<I", entry, 8)[0]
                    partition_size = struct.unpack_from("<I", entry, 12)[0]
                    break
            if partition_lba is None or partition_size == 0:
                return {}

            boot = read_sector(partition_lba)
            if boot is None or boot[510:512] != b"\x55\xaa":
                return {}
            bytes_per_sector = struct.unpack_from("<H", boot, 11)[0]
            sectors_per_cluster = boot[13]
            reserved_sectors = struct.unpack_from("<H", boot, 14)[0]
            fat_count = boot[16]
            total_sectors = (struct.unpack_from("<H", boot, 19)[0] or
                             struct.unpack_from("<I", boot, 32)[0])
            sectors_per_fat = struct.unpack_from("<I", boot, 36)[0]
            root_cluster = struct.unpack_from("<I", boot, 44)[0] & 0x0FFFFFFF
            if (bytes_per_sector != SECTOR or sectors_per_cluster == 0 or
                    sectors_per_cluster & (sectors_per_cluster - 1) or
                    reserved_sectors == 0 or fat_count == 0 or sectors_per_fat == 0 or
                    total_sectors == 0 or total_sectors > partition_size or root_cluster < 2):
                return {}

            fat_start = partition_lba + reserved_sectors
            data_start = fat_start + fat_count * sectors_per_fat
            cluster_count = (total_sectors - reserved_sectors - fat_count * sectors_per_fat) // sectors_per_cluster
            if cluster_count < 1 or root_cluster >= cluster_count + 2:
                return {}
            cluster_bytes = sectors_per_cluster * SECTOR

            def fat_next(cluster: int) -> Optional[int]:
                fat_sector = (cluster * 4) // SECTOR
                if fat_sector >= sectors_per_fat:
                    return None
                raw = read_sector(fat_start + fat_sector)
                if raw is None:
                    return None
                offset = (cluster * 4) % SECTOR
                return struct.unpack_from("<I", raw, offset)[0] & 0x0FFFFFFF

            def read_chain(first: int, byte_limit: int) -> Optional[bytes]:
                if byte_limit == 0:
                    return b""
                if first < 2 or first >= cluster_count + 2:
                    return None
                output = bytearray()
                cluster = first
                visited = set()
                for _ in range(cluster_count):
                    if cluster < 2 or cluster >= cluster_count + 2 or cluster in visited:
                        return None
                    visited.add(cluster)
                    for sector_index in range(sectors_per_cluster):
                        lba = data_start + (cluster - 2) * sectors_per_cluster + sector_index
                        chunk = read_sector(lba)
                        if chunk is None:
                            return None
                        output.extend(chunk)
                        if len(output) >= byte_limit:
                            return bytes(output[:byte_limit])
                    following = fat_next(cluster)
                    if following is None:
                        return None
                    if following >= 0x0FFFFFF8:
                        return bytes(output)
                    if following < 2 or following == 0x0FFFFFF7 or following >= cluster_count + 2:
                        return None
                    cluster = following
                return None

            def decode_name(raw: bytes) -> str:
                base = raw[:8].decode("ascii", errors="replace").rstrip(" ")
                extension = raw[8:11].decode("ascii", errors="replace").rstrip(" ")
                return f"{base}.{extension}" if extension else base

            visited_directories = set()

            def read_directory(first: int, depth: int = 0) -> dict:
                if depth > 32 or first in visited_directories:
                    return {}
                visited_directories.add(first)
                data = read_chain(first, MAX_PRESERVED_DIRECTORY_BYTES)
                if data is None:
                    return {}
                entries = {}
                for offset in range(0, len(data) - 31, 32):
                    item = data[offset:offset + 32]
                    if item[0] == 0:
                        break
                    if item[0] == 0xE5 or item[11] == 0x0F or item[11] & 0x08:
                        continue
                    name = decode_name(item[:11])
                    if name in (".", "..") or not name:
                        continue
                    cluster = ((struct.unpack_from("<H", item, 20)[0] << 16) |
                               struct.unpack_from("<H", item, 26)[0])
                    if item[11] & 0x10:
                        entries[name] = read_directory(cluster, depth + 1) if cluster >= 2 else {}
                        continue
                    size = struct.unpack_from("<I", item, 28)[0]
                    if size > MAX_PRESERVED_FILE_BYTES:
                        continue
                    contents = read_chain(cluster, size) if size else b""
                    # Do not silently turn a damaged/truncated old file into a
                    # shorter file when carrying it into the rebuilt image.
                    if contents is not None and len(contents) == size:
                        entries[name] = contents
                return entries

            return read_directory(root_cluster)
    except (OSError, struct.error, OverflowError, RecursionError):
        return {}


def merge_missing(generated: dict, previous: dict) -> int:
    """Preserve old user entries while letting fresh build files take priority."""
    preserved = 0
    generated_names = {name.upper(): name for name in generated}
    for old_name, old_value in previous.items():
        current_name = generated_names.get(old_name.upper())
        if current_name is None:
            generated[old_name] = old_value
            generated_names[old_name.upper()] = old_name
            if isinstance(old_value, dict):
                preserved += count_files(old_value)
            else:
                preserved += 1
        elif isinstance(generated[current_name], dict) and isinstance(old_value, dict):
            preserved += merge_missing(generated[current_name], old_value)
    return preserved


def count_files(tree: dict) -> int:
    return sum(count_files(value) if isinstance(value, dict) else 1 for value in tree.values())


def file_contents(value) -> bytes:
    return value if isinstance(value, bytes) else value.read_bytes()


class Fat32Image:
    def __init__(self, image_path: pathlib.Path):
        self.path = image_path
        self.fat_sectors = 1
        while True:
            self.cluster_count = (PARTITION_SECTORS - RESERVED_SECTORS - FAT_COUNT * self.fat_sectors) // SPC
            required_fat_sectors = math.ceil((self.cluster_count + 2) * 4 / SECTOR)
            if required_fat_sectors == self.fat_sectors:
                break
            self.fat_sectors = required_fat_sectors
        if self.cluster_count < 65525:
            raise ValueError("FAT32 volume is too small")
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, EOC
        self.next_cluster = 3
        self.file = image_path.open("wb+")
        self.file.truncate(IMAGE_SECTORS * SECTOR)
        self.data_lba = PARTITION_LBA + RESERVED_SECTORS + FAT_COUNT * self.fat_sectors
        self.fat[2] = EOC  # FAT32 root directory cluster
        self.write_mbr()
        self.write_boot_sectors()

    def write_mbr(self):
        mbr = bytearray(SECTOR)
        entry = 446
        mbr[entry] = 0x80
        mbr[entry + 1:entry + 4] = b"\xfe\xff\xff"
        mbr[entry + 4] = 0xEF  # EFI System Partition, accepted by FAT32 driver
        mbr[entry + 5:entry + 8] = b"\xfe\xff\xff"
        struct.pack_into("<II", mbr, entry + 8, PARTITION_LBA, PARTITION_SECTORS)
        mbr[510:512] = b"\x55\xaa"
        self.file.seek(0)
        self.file.write(mbr)

    def write_boot_sectors(self):
        boot = bytearray(SECTOR)
        boot[:3] = b"\xeb\x58\x90"
        boot[3:11] = b"ANNYOS  "
        struct.pack_into("<HBHB", boot, 11, SECTOR, SPC, RESERVED_SECTORS, FAT_COUNT)
        # BPB fields after FAT count: root entries, total16, media, fat16, geometry, hidden, total32
        struct.pack_into("<HHBH", boot, 17, 0, 0, 0xF8, 0)
        struct.pack_into("<HH", boot, 24, 63, 255)
        struct.pack_into("<II", boot, 28, PARTITION_LBA, PARTITION_SECTORS)
        struct.pack_into("<IHHIHH", boot, 36, self.fat_sectors, 0, 0, 2, 1, 0)
        boot[64] = 0x80
        boot[66] = 0x29
        struct.pack_into("<I", boot, 67, 0x20261021)
        boot[71:82] = b"ANNYOS     "
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xaa"
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<I", fsinfo, 484, 0x61417272)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        self.write_sector(PARTITION_LBA, boot)
        self.write_sector(PARTITION_LBA + 1, fsinfo)
        self.write_sector(PARTITION_LBA + 6, boot)
        self.write_sector(PARTITION_LBA + 7, fsinfo)

    def write_sector(self, lba: int, data: bytes):
        self.file.seek(lba * SECTOR)
        self.file.write(data)

    def allocate(self, data: bytes = b"") -> int:
        count = max(1, math.ceil(len(data) / (SECTOR * SPC)))
        first = self.next_cluster
        end = first + count
        if end > len(self.fat):
            raise ValueError("FAT32 image is full")
        for cluster in range(first, end):
            self.fat[cluster] = cluster + 1 if cluster + 1 < end else EOC
        self.next_cluster = end
        padded = data.ljust(count * SECTOR * SPC, b"\0")
        offset = (self.data_lba + first - 2) * SECTOR
        self.file.seek(offset)
        self.file.write(padded)
        return first

    def add_directory(self, parent_cluster: int, name: str, contents: dict):
        cluster = self.allocate()
        entries = [dir_entry(b".          ", 0x10, cluster),
                   dir_entry(b"..         ", 0x10, parent_cluster)]
        for child_name, value in contents.items():
            encoded_name = short_name(child_name)
            if isinstance(value, dict):
                child_cluster = self.add_directory(cluster, child_name, value)
                entries.append(dir_entry(encoded_name, 0x10, child_cluster))
            else:
                content = file_contents(value)
                child_cluster = self.allocate(content)
                entries.append(dir_entry(encoded_name, 0x20, child_cluster, len(content)))
        entries.append(bytes(32))
        self.write_directory_payload(cluster, b"".join(entries))
        return cluster

    def write_cluster(self, cluster: int, data: bytes):
        size = SECTOR * SPC
        data = data.ljust(math.ceil(len(data) / size) * size, b"\0")
        self.file.seek((self.data_lba + cluster - 2) * SECTOR)
        self.file.write(data)

    def write_directory_payload(self, first_cluster: int, payload: bytes):
        cluster_bytes = SECTOR * SPC
        required = max(1, math.ceil(len(payload) / cluster_bytes))
        chain = []
        cluster = first_cluster
        while cluster < EOC:
            if cluster < 2 or cluster >= len(self.fat) or cluster in chain:
                raise ValueError("Invalid FAT32 directory cluster chain")
            chain.append(cluster)
            cluster = self.fat[cluster]
        if len(chain) < required:
            extra_count = required - len(chain)
            extra_first = self.allocate(bytes(extra_count * cluster_bytes))
            self.fat[chain[-1]] = extra_first
            cluster = extra_first
            for _ in range(extra_count):
                chain.append(cluster)
                cluster = self.fat[cluster]
        padded = payload.ljust(len(chain) * cluster_bytes, b"\0")
        for index, cluster in enumerate(chain):
            self.write_cluster(cluster, padded[index * cluster_bytes:(index + 1) * cluster_bytes])

    def finish(self, root: dict):
        # Fill the preallocated root cluster, allocating another if needed.
        entries = []
        for name, value in root.items():
            encoded_name = short_name(name)
            if isinstance(value, dict):
                cluster = self.add_directory(2, name, value)
                entries.append(dir_entry(encoded_name, 0x10, cluster))
            else:
                content = file_contents(value)
                cluster = self.allocate(content)
                entries.append(dir_entry(encoded_name, 0x20, cluster, len(content)))
        entries.append(bytes(32))
        payload = b"".join(entries)
        self.write_directory_payload(2, payload)
        fat_bytes = bytearray(self.fat_sectors * SECTOR)
        for index, value in enumerate(self.fat):
            struct.pack_into("<I", fat_bytes, index * 4, value)
        for copy in range(FAT_COUNT):
            lba = PARTITION_LBA + RESERVED_SECTORS + copy * self.fat_sectors
            self.file.seek(lba * SECTOR)
            self.file.write(fat_bytes)
        self.file.flush()
        self.file.close()


def directory_tree(path: pathlib.Path) -> dict:
    tree = {}
    for child in sorted(path.iterdir(), key=lambda item: item.name.lower()):
        if child.is_dir():
            tree[child.name] = directory_tree(child)
        elif child.is_file():
            tree[child.name] = child
    return tree


def main() -> int:
    if len(sys.argv) != 3:
        print("Usage: build_disk_image.py <esp-directory> <output.img>", file=sys.stderr)
        return 2
    esp = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    if not (esp / "EFI/BOOT/BOOTX64.EFI").is_file():
        raise SystemExit("ESP is missing EFI/BOOT/BOOTX64.EFI")
    root = directory_tree(esp)
    previous = existing_files(output)
    preserved_count = merge_missing(root, previous)
    output.parent.mkdir(parents=True, exist_ok=True)
    image = Fat32Image(output)
    image.finish(root)
    suffix = f"; preserved {preserved_count} user files" if preserved_count else ""
    print(f"Built {output} ({IMAGE_SECTORS * SECTOR // (1024 * 1024)} MiB FAT32{suffix})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
