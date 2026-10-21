#!/usr/bin/env python3
"""Build AnnyOS.img as an MBR-partitioned, FAT32 EFI disk."""

import math
import pathlib
import struct
import sys

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
                content = value.read_bytes()
                child_cluster = self.allocate(content)
                entries.append(dir_entry(encoded_name, 0x20, child_cluster, len(content)))
        entries.append(bytes(32))
        self.write_cluster(cluster, b"".join(entries))
        return cluster

    def write_cluster(self, cluster: int, data: bytes):
        size = SECTOR * SPC
        data = data.ljust(math.ceil(len(data) / size) * size, b"\0")
        self.file.seek((self.data_lba + cluster - 2) * SECTOR)
        self.file.write(data)

    def finish(self, root: dict):
        # Fill the preallocated root cluster, allocating another if needed.
        entries = []
        for name, value in root.items():
            encoded_name = short_name(name)
            if isinstance(value, dict):
                cluster = self.add_directory(2, name, value)
                entries.append(dir_entry(encoded_name, 0x10, cluster))
            else:
                content = value.read_bytes()
                cluster = self.allocate(content)
                entries.append(dir_entry(encoded_name, 0x20, cluster, len(content)))
        entries.append(bytes(32))
        payload = b"".join(entries)
        if len(payload) > SECTOR * SPC:
            raise ValueError("FAT32 root directory exceeds its reserved cluster")
        self.write_cluster(2, payload)
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
    output.parent.mkdir(parents=True, exist_ok=True)
    image = Fat32Image(output)
    image.finish(root)
    print(f"Built {output} ({IMAGE_SECTORS * SECTOR // (1024 * 1024)} MiB FAT32)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
