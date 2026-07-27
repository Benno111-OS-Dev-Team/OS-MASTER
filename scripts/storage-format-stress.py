#!/usr/bin/env python3
import struct
import sys
from pathlib import Path
import re

SECTOR_SIZE = 512
SECTORS_PER_MIB = 2048
GPT_ENTRY_SIZE = 128
GPT_ENTRY_COUNT = 128
GPT_ENTRY_SECTORS = (GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) // SECTOR_SIZE
MBR_PARTITION_OFFSET = 446
MBR_SIGNATURE_OFFSET = 510
SWAP_SIGNATURE_OFFSET = 4086

DRIVE_KINDS = [
    "IDE",
    "AHCI",
    "SATA",
    "RAID",
    "NVME",
    "CDROM",
    "USB_MASS_STORAGE",
    "APPLE_ANS",
]

WRITABLE_KINDS = {
    "IDE",
    "AHCI",
    "SATA",
    "RAID",
    "NVME",
    "USB_MASS_STORAGE",
    "APPLE_ANS",
}

PARTITION_KINDS = ["EFI", "SYSTEM", "DATA", "SWAP"]
FORMATTERS = ["FAT32", "EXT4", "SWAP"]
DETECT_ONLY_FILESYSTEMS = ["ISO9660", "APFS"]
TABLE_FORMATS = ["GPT", "MBR"]

STATS = {
    "drive_kinds": 0,
    "partition_tables": 0,
    "formatter_checks": 0,
    "detect_only_checks": 0,
    "boundary_checks": 0,
    "corruption_checks": 0,
}


def repo_root():
    return Path(__file__).resolve().parents[1]


def storage_source():
    return (repo_root() / "kernel" / "drivers" / "storage.c").read_text(
        encoding="utf-8"
    )


def parse_storage_enum(enum_name):
    header = repo_root() / "kernel" / "include" / "drivers" / "storage.h"
    lines = header.read_text(encoding="utf-8").splitlines()
    in_enum = False
    values = []

    for line in lines:
        stripped = line.strip()
        if stripped == "typedef enum {":
            in_enum = True
            values = []
            continue
        if not in_enum:
            continue
        if stripped.startswith("}") and enum_name in stripped:
            return values
        if stripped.startswith("}"):
            in_enum = False
            values = []
            continue
        token = stripped.split("=", 1)[0].split(",", 1)[0].strip()
        if token:
            values.append(token)

    raise StressFailure(f"could not find {enum_name} in storage.h")


def parse_storage_define(name):
    source = storage_source()
    match = re.search(rf"^#define\s+{re.escape(name)}\s+([0-9A-Fa-fxX]+)",
                      source, re.MULTILINE)
    if not match:
        raise StressFailure(f"could not find {name} in storage.c")
    return int(match.group(1), 0)


def parse_storage_format_cases():
    source = storage_source()
    match = re.search(
        r"switch\s*\(fs_kind\)\s*\{(?P<body>.*?)\n\s*default:",
        source,
        re.DOTALL,
    )
    if not match:
        raise StressFailure("could not find storage_format_partition switch")
    return re.findall(r"case\s+STORAGE_FILESYSTEM_([A-Z0-9_]+)\s*:",
                      match.group("body"))


def parse_storage_detect_order():
    source = storage_source()
    match = re.search(
        r"static void storage_detect_partition_filesystem"
        r"\(.*?\)\s*\{(?P<body>.*?)\n\}",
        source,
        re.DOTALL,
    )
    if not match:
        raise StressFailure("could not find storage_detect_partition_filesystem")
    return re.findall(r"storage_detect_([a-z0-9_]+)\(", match.group("body"))


def assert_kernel_enum_coverage():
    storage_kinds = parse_storage_enum("storage_kind_t")
    partition_kinds = parse_storage_enum("storage_partition_kind_t")
    filesystem_kinds = parse_storage_enum("storage_filesystem_kind_t")

    expected_drives = [f"STORAGE_KIND_{kind}" for kind in DRIVE_KINDS]
    expected_partitions = [
        f"STORAGE_PARTITION_{kind}" for kind in PARTITION_KINDS
    ]
    expected_filesystems = [
        f"STORAGE_FILESYSTEM_{fs}" for fs in FORMATTERS + DETECT_ONLY_FILESYSTEMS
    ]

    for expected, actual, label in (
        (expected_drives, storage_kinds, "drive kind"),
        (expected_partitions, partition_kinds, "partition kind"),
        (expected_filesystems, filesystem_kinds, "filesystem kind"),
    ):
        missing = sorted(set(actual) - {f"STORAGE_KIND_UNKNOWN",
                                        f"STORAGE_PARTITION_UNKNOWN",
                                        f"STORAGE_FILESYSTEM_UNKNOWN"} -
                         set(expected))
        if missing:
            raise StressFailure(
                f"stress matrix missing kernel {label}s: {', '.join(missing)}"
            )


def assert_kernel_storage_parity():
    define_checks = {
        "STORAGE_SECTOR_SIZE": SECTOR_SIZE,
        "STORAGE_MBR_PARTITION_OFFSET": MBR_PARTITION_OFFSET,
        "STORAGE_MBR_SIGNATURE_OFFSET": MBR_SIGNATURE_OFFSET,
        "STORAGE_GPT_ENTRY_SIZE": GPT_ENTRY_SIZE,
        "STORAGE_GPT_ENTRY_COUNT": GPT_ENTRY_COUNT,
        "STORAGE_SWAP_SIGNATURE_OFFSET": SWAP_SIGNATURE_OFFSET,
    }
    for name, expected in define_checks.items():
        actual = parse_storage_define(name)
        if actual != expected:
            raise StressFailure(
                f"{name} mismatch: stress={expected}, kernel={actual}"
            )

    formatter_cases = parse_storage_format_cases()
    if formatter_cases != FORMATTERS:
        raise StressFailure(
            "formatter matrix mismatch: "
            f"stress={FORMATTERS}, kernel={formatter_cases}"
        )

    expected_detect_order = [
        "iso9660",
        "apfs",
        "ext4",
        "fat32",
        "swap",
    ]
    detect_order = parse_storage_detect_order()
    if detect_order != expected_detect_order:
        raise StressFailure(
            "filesystem detection order mismatch: "
            f"stress={expected_detect_order}, kernel={detect_order}"
        )


class StressFailure(Exception):
    pass


class Disk:
    def __init__(self, kind, mib):
        self.kind = kind
        self.mib = mib
        self.sectors = mib * SECTORS_PER_MIB
        self.sector_data = {}

    @property
    def writable(self):
        return self.kind in WRITABLE_KINDS

    def read(self, lba, count=1):
        if count <= 0 or lba < 0 or lba + count > self.sectors:
            raise StressFailure(f"{self.kind}: invalid read {lba}+{count}")
        empty = bytes(SECTOR_SIZE)
        return b"".join(
            self.sector_data.get(lba + offset, empty) for offset in range(count)
        )

    def write(self, lba, payload):
        if not self.writable:
            raise StressFailure(f"{self.kind}: write accepted on read-only disk")
        self.raw_write(lba, payload)

    def raw_write(self, lba, payload):
        if len(payload) % SECTOR_SIZE != 0:
            raise StressFailure(f"{self.kind}: unaligned write length {len(payload)}")
        count = len(payload) // SECTOR_SIZE
        if count <= 0 or lba < 0 or lba + count > self.sectors:
            raise StressFailure(f"{self.kind}: invalid write {lba}+{count}")
        empty = bytes(SECTOR_SIZE)
        for offset in range(count):
            sector = bytes(payload[offset * SECTOR_SIZE:
                                   (offset + 1) * SECTOR_SIZE])
            if sector == empty:
                self.sector_data.pop(lba + offset, None)
            else:
                self.sector_data[lba + offset] = sector


def le16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put16(buf, off, value):
    struct.pack_into("<H", buf, off, value)


def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)


def put64(buf, off, value):
    struct.pack_into("<Q", buf, off, value)


def ascii_padded(text, length):
    raw = (text or "")[:length].encode("ascii", "replace")
    return raw + b" " * (length - len(raw))


def partition_type_guid(kind):
    return {
        "EFI": bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b"),
        "SWAP": bytes.fromhex("6dfd5706aba4c44384e50933c84b4f4f"),
    }.get(kind, bytes.fromhex("af3dc60f838472478e793d69d8477de4"))


def partition_mbr_type(kind):
    return {
        "EFI": 0xEF,
        "SYSTEM": 0x83,
        "DATA": 0x83,
        "SWAP": 0x82,
    }[kind]


def partition_layout(sizes_mib):
    next_lba = 2048
    layout = []
    for size_mib in sizes_mib:
        sectors = size_mib * SECTORS_PER_MIB
        layout.append((next_lba, sectors))
        next_lba += sectors
    return layout


def write_mbr(disk, parts):
    if len(parts) > 4:
        raise StressFailure("MBR accepted more than four partitions")
    sector = bytearray(disk.read(0))
    for off in range(MBR_PARTITION_OFFSET, MBR_SIGNATURE_OFFSET):
        sector[off] = 0
    active = 0
    for idx, part in enumerate(parts):
        entry = MBR_PARTITION_OFFSET + idx * 16
        sector[entry] = 0x80 if idx == active else 0
        sector[entry + 1:entry + 4] = b"\xfe\xff\xff"
        sector[entry + 4] = partition_mbr_type(part["kind"])
        sector[entry + 5:entry + 8] = b"\xfe\xff\xff"
        put32(sector, entry + 8, part["start"])
        put32(sector, entry + 12, part["sectors"])
    sector[510] = 0x55
    sector[511] = 0xAA
    disk.write(0, sector)


def write_gpt(disk, parts):
    if disk.sectors <= GPT_ENTRY_SECTORS * 2 + 3:
        raise StressFailure("GPT accepted undersized disk")
    entries = bytearray(GPT_ENTRY_SECTORS * SECTOR_SIZE)
    for idx, part in enumerate(parts):
        off = idx * GPT_ENTRY_SIZE
        entries[off:off + 16] = partition_type_guid(part["kind"])
        entries[off + 16:off + 32] = bytes([(idx + 1) & 0xFF]) * 16
        put64(entries, off + 32, part["start"])
        put64(entries, off + 40, part["start"] + part["sectors"] - 1)
        name = part["label"][:36]
        for pos, ch in enumerate(name):
            put16(entries, off + 56 + pos * 2, ord(ch))

    pmbr = bytearray(SECTOR_SIZE)
    entry = MBR_PARTITION_OFFSET
    pmbr[entry + 1:entry + 4] = b"\x00\x02\x00"
    pmbr[entry + 4] = 0xEE
    pmbr[entry + 5:entry + 8] = b"\xff\xff\xff"
    put32(pmbr, entry + 8, 1)
    put32(pmbr, entry + 12, min(disk.sectors - 1, 0xFFFFFFFF))
    pmbr[510] = 0x55
    pmbr[511] = 0xAA
    disk.write(0, pmbr)

    header = bytearray(SECTOR_SIZE)
    header[0:8] = b"EFI PART"
    put32(header, 8, 0x00010000)
    put32(header, 12, 92)
    put64(header, 24, 1)
    put64(header, 32, disk.sectors - 1)
    put64(header, 40, 2048)
    put64(header, 48, disk.sectors - GPT_ENTRY_SECTORS - 2)
    header[56:72] = b"OS8-STRESS-GPT!!"
    put64(header, 72, 2)
    put32(header, 80, GPT_ENTRY_COUNT)
    put32(header, 84, GPT_ENTRY_SIZE)
    disk.write(1, header)
    disk.write(2, entries)

    backup_lba = disk.sectors - GPT_ENTRY_SECTORS - 1
    disk.write(backup_lba, entries)
    header2 = bytearray(header)
    put64(header2, 24, disk.sectors - 1)
    put64(header2, 32, 1)
    put64(header2, 72, backup_lba)
    disk.write(disk.sectors - 1, header2)


def detect_partition_table(disk):
    sector = disk.read(0)
    if sector[510:512] != b"\x55\xaa":
        return "none"
    if sector[MBR_PARTITION_OFFSET + 4] == 0xEE:
        header = disk.read(1)
        if header[0:8] != b"EFI PART":
            raise StressFailure("protective MBR without GPT header")
        return "gpt"
    return "mbr"


def expect_stress_failure(label, fn):
    try:
        fn()
    except StressFailure:
        STATS["corruption_checks"] += 1
        return
    raise StressFailure(f"{label}: expected failure was not raised")


def expect_not_detected(label, fn):
    if fn():
        raise StressFailure(f"{label}: corrupted structure was detected")
    STATS["corruption_checks"] += 1


def choose_fat32_spc(total_sectors):
    size_mib = total_sectors // SECTORS_PER_MIB
    if size_mib < 260:
        return 1
    if size_mib < 8192:
        return 4
    if size_mib < 16384:
        return 8
    if size_mib < 32768:
        return 16
    return 32


def part_slice(disk, part, rel_lba, count):
    start = (part["start"] + rel_lba) * SECTOR_SIZE
    end = start + count * SECTOR_SIZE
    if rel_lba + count > part["sectors"]:
        raise StressFailure("partition access escaped bounds")
    return start, end


def part_write(disk, part, rel_lba, payload):
    if len(payload) % SECTOR_SIZE != 0:
        raise StressFailure("partition write length is not sector aligned")
    part_slice(disk, part, rel_lba, len(payload) // SECTOR_SIZE)
    disk.raw_write(part["start"] + rel_lba, payload)


def part_read(disk, part, rel_lba, count):
    part_slice(disk, part, rel_lba, count)
    return disk.read(part["start"] + rel_lba, count)


def format_fat32(disk, part):
    if part["sectors"] < 65536:
        return False
    reserved = 32
    fat_size = 1
    spc = choose_fat32_spc(part["sectors"])
    for _ in range(8):
        data_sectors = part["sectors"] - reserved - fat_size * 2
        clusters = data_sectors // spc
        fat_size = ((clusters + 2) * 4 + SECTOR_SIZE - 1) // SECTOR_SIZE
    data_sectors = part["sectors"] - reserved - fat_size * 2
    clusters = data_sectors // spc
    if clusters < 65525:
        return False

    sector = bytearray(SECTOR_SIZE)
    sector[0:3] = b"\xeb\x58\x90"
    sector[3:11] = b"OS8FAT32"
    put16(sector, 11, SECTOR_SIZE)
    sector[13] = spc
    put16(sector, 14, reserved)
    sector[16] = 2
    sector[21] = 0xF8
    put16(sector, 24, 0x3F)
    put16(sector, 26, 0xFF)
    put32(sector, 32, part["sectors"])
    put32(sector, 36, fat_size)
    put32(sector, 44, 2)
    put16(sector, 48, 1)
    put16(sector, 50, 6)
    sector[64] = 0x80
    sector[66] = 0x29
    put32(sector, 67, 0x4F533846 ^ part["sectors"] ^ part["start"])
    sector[71:82] = ascii_padded(part["label"], 11)
    sector[82:90] = b"FAT32   "
    sector[510:512] = b"\x55\xaa"
    part_write(disk, part, 0, sector)
    part_write(disk, part, 6, sector)

    fsinfo = bytearray(SECTOR_SIZE)
    put32(fsinfo, 0, 0x41615252)
    put32(fsinfo, 484, 0x61417272)
    put32(fsinfo, 488, 0xFFFFFFFF)
    put32(fsinfo, 492, 0xFFFFFFFF)
    put32(fsinfo, 508, 0xAA550000)
    part_write(disk, part, 1, fsinfo)
    part_write(disk, part, 7, fsinfo)
    return True


def detect_fat32(disk, part):
    sector = part_read(disk, part, 0, 1)
    return (
        sector[510:512] == b"\x55\xaa"
        and le16(sector, 11) == SECTOR_SIZE
        and sector[13] != 0
        and sector[16] == 2
        and le32(sector, 36) != 0
        and sector[82:87] == b"FAT32"
    )


def format_swap(disk, part):
    if part["sectors"] < 8:
        return False
    page = bytearray(4096)
    page[SWAP_SIGNATURE_OFFSET:SWAP_SIGNATURE_OFFSET + 10] = b"SWAPSPACE2"
    part_write(disk, part, 0, page)
    return True


def detect_swap(disk, part):
    page = part_read(disk, part, 0, 8)
    sig = page[SWAP_SIGNATURE_OFFSET:SWAP_SIGNATURE_OFFSET + 10]
    return sig in (b"SWAPSPACE2", b"SWAP-SPACE")


def format_ext4(disk, part):
    sectors_per_block = 8
    if part["sectors"] < sectors_per_block * 256:
        return False
    block = bytearray(4096)
    total_blocks = part["sectors"] // sectors_per_block
    put32(block, 1024 + 0, 2048)
    put32(block, 1024 + 4, total_blocks)
    put32(block, 1024 + 24, 2)
    put32(block, 1024 + 32, 32768)
    put32(block, 1024 + 40, 2048)
    put16(block, 1024 + 56, 0xEF53)
    put16(block, 1024 + 58, 1)
    put16(block, 1024 + 60, 1)
    put32(block, 1024 + 76, 1)
    put32(block, 1024 + 84, 11)
    put16(block, 1024 + 88, 128)
    block[1024 + 120:1024 + 136] = ascii_padded(part["label"], 16)
    part_write(disk, part, 0, block)
    return True


def detect_ext4(disk, part):
    sector = part_read(disk, part, 2, 1)
    return sector[56:58] == b"\x53\xef"


def write_partition_iso9660_marker(disk, part):
    block = bytearray(2048)
    block[0] = 1
    block[1:6] = b"CD001"
    block[40:72] = ascii_padded("OS8_STRESS_ISO", 32)
    part_write(disk, part, 64, block)


def detect_partition_iso9660(disk, part):
    block = part_read(disk, part, 64, 4)
    return block[0] in (1, 2) and block[1:6] == b"CD001"


def write_cdrom_iso9660(disk):
    if disk.kind != "CDROM":
        raise StressFailure("ISO9660 image was written to non-optical disk")
    block = bytearray(2048)
    block[0] = 1
    block[1:6] = b"CD001"
    block[40:72] = ascii_padded("OS8_STRESS_ISO", 32)
    disk.raw_write(64, block)


def detect_cdrom_iso9660(disk):
    block = disk.read(64, 4)
    return block[0] in (1, 2) and block[1:6] == b"CD001"


def write_apfs_marker(disk, part):
    block = bytearray(4096)
    put32(block, 32, 0x4253584E)
    part_write(disk, part, 0, block)


def detect_apfs(disk, part):
    block = part_read(disk, part, 0, 8)
    return le32(block, 32) == 0x4253584E


def make_parts(sizes):
    parts = []
    for idx, (start, sectors) in enumerate(partition_layout(sizes)):
        kind = PARTITION_KINDS[idx % len(PARTITION_KINDS)]
        parts.append({
            "kind": kind,
            "label": f"{kind}{idx + 1}",
            "start": start,
            "sectors": sectors,
        })
    return parts


def check_formatter(kind, table_format, part, fs_name):
    disk = Disk(kind, 256)
    clone = dict(part)

    if fs_name == "FAT32":
        formatted = format_fat32(disk, clone)
        if not formatted and clone["sectors"] >= 65536:
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: FAT32 rejected valid partition"
            )
        if formatted and not detect_fat32(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: FAT32 did not self-detect"
            )
    elif fs_name == "EXT4":
        if not format_ext4(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: EXT4 rejected valid partition"
            )
        if not detect_ext4(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: EXT4 did not self-detect"
            )
    elif fs_name == "SWAP":
        if not format_swap(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: SWAP rejected valid partition"
            )
        if not detect_swap(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: SWAP did not self-detect"
            )
    else:
        raise StressFailure(f"unknown formatter {fs_name}")

    STATS["formatter_checks"] += 1


def check_detect_only(kind, table_format, part, fs_name):
    disk = Disk(kind, 256)
    clone = dict(part)

    if fs_name == "ISO9660":
        write_partition_iso9660_marker(disk, clone)
        if not detect_partition_iso9660(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: ISO9660 marker not detected"
            )
    elif fs_name == "APFS":
        write_apfs_marker(disk, clone)
        if not detect_apfs(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: APFS marker not detected"
            )
    else:
        raise StressFailure(f"unknown detect-only filesystem {fs_name}")

    if fs_name in FORMATTERS:
        raise StressFailure(f"{kind}: {fs_name} unexpectedly has a formatter")
    STATS["detect_only_checks"] += 1


def stress_partition_table(kind, table_format):
    disk = Disk(kind, 256)
    parts = make_parts([40, 64, 80, 16])

    if table_format == "GPT":
        write_gpt(disk, parts)
        expected = "gpt"
    elif table_format == "MBR":
        write_mbr(disk, parts)
        expected = "mbr"
    else:
        raise StressFailure(f"unknown partition table {table_format}")

    if detect_partition_table(disk) != expected:
        raise StressFailure(f"{kind}: {table_format} was not detected")
    STATS["partition_tables"] += 1

    for part in parts:
        for fs_name in FORMATTERS:
            check_formatter(kind, table_format, part, fs_name)
        for fs_name in DETECT_ONLY_FILESYSTEMS:
            check_detect_only(kind, table_format, part, fs_name)


def stress_writable_kind(kind):
    STATS["drive_kinds"] += 1

    for table_format in TABLE_FORMATS:
        stress_partition_table(kind, table_format)

    small = make_parts([1])[0]
    disk = Disk(kind, 16)
    if format_fat32(disk, small):
        raise StressFailure(f"{kind}: FAT32 accepted undersized partition")
    STATS["boundary_checks"] += 1
    if format_ext4(disk, {"start": small["start"], "sectors": 128,
                          "label": "tiny"}):
        raise StressFailure(f"{kind}: EXT4 accepted undersized partition")
    STATS["boundary_checks"] += 1
    if format_swap(disk, {"start": small["start"], "sectors": 4,
                          "label": "tiny"}):
        raise StressFailure(f"{kind}: SWAP accepted undersized partition")
    STATS["boundary_checks"] += 1

    stress_corruption_cases(kind)


def stress_corruption_cases(kind):
    disk = Disk(kind, 128)
    parts = make_parts([40, 32, 8, 8])
    part = parts[0]

    if detect_partition_table(disk) != "none":
        raise StressFailure(f"{kind}: blank disk looked partitioned")
    STATS["corruption_checks"] += 1

    write_mbr(disk, parts)
    bad_mbr = bytearray(disk.read(0))
    bad_mbr[510] = 0
    disk.write(0, bad_mbr)
    if detect_partition_table(disk) != "none":
        raise StressFailure(f"{kind}: bad MBR signature was accepted")
    STATS["corruption_checks"] += 1

    bad_gpt = Disk(kind, 128)
    protective = bytearray(SECTOR_SIZE)
    protective[MBR_PARTITION_OFFSET + 4] = 0xEE
    protective[510:512] = b"\x55\xaa"
    bad_gpt.write(0, protective)
    expect_stress_failure(
        f"{kind}: protective MBR without GPT header",
        lambda: detect_partition_table(bad_gpt),
    )

    expect_stress_failure(
        f"{kind}: partition read escaped bounds",
        lambda: part_read(disk, part, part["sectors"], 1),
    )
    expect_stress_failure(
        f"{kind}: partition write escaped bounds",
        lambda: part_write(disk, part, part["sectors"] - 1, bytes(1024)),
    )

    fs_disk = Disk(kind, 128)
    fat = dict(part)
    if not format_fat32(fs_disk, fat):
        raise StressFailure(f"{kind}: FAT32 setup failed for corruption test")
    fat_sector = bytearray(part_read(fs_disk, fat, 0, 1))
    fat_sector[510] = 0
    part_write(fs_disk, fat, 0, fat_sector)
    expect_not_detected(
        f"{kind}: FAT32 bad boot signature",
        lambda: detect_fat32(fs_disk, fat),
    )
    format_fat32(fs_disk, fat)
    fat_sector = bytearray(part_read(fs_disk, fat, 0, 1))
    fat_sector[82:87] = b"NOT32"
    part_write(fs_disk, fat, 0, fat_sector)
    expect_not_detected(
        f"{kind}: FAT32 bad fs type",
        lambda: detect_fat32(fs_disk, fat),
    )

    ext = dict(part)
    ext["sectors"] = 2048
    if not format_ext4(fs_disk, ext):
        raise StressFailure(f"{kind}: EXT4 setup failed for corruption test")
    ext_sector = bytearray(part_read(fs_disk, ext, 2, 1))
    ext_sector[56:58] = b"\x00\x00"
    part_write(fs_disk, ext, 2, ext_sector)
    expect_not_detected(
        f"{kind}: EXT4 bad superblock magic",
        lambda: detect_ext4(fs_disk, ext),
    )

    iso = dict(part)
    write_partition_iso9660_marker(fs_disk, iso)
    iso_block = bytearray(part_read(fs_disk, iso, 64, 4))
    iso_block[1:6] = b"BAD!!"
    part_write(fs_disk, iso, 64, iso_block)
    expect_not_detected(
        f"{kind}: ISO9660 bad identifier",
        lambda: detect_partition_iso9660(fs_disk, iso),
    )

    apfs = dict(part)
    write_apfs_marker(fs_disk, apfs)
    apfs_block = bytearray(part_read(fs_disk, apfs, 0, 8))
    put32(apfs_block, 32, 0)
    part_write(fs_disk, apfs, 0, apfs_block)
    expect_not_detected(
        f"{kind}: APFS bad magic",
        lambda: detect_apfs(fs_disk, apfs),
    )

    swap = dict(part)
    if not format_swap(fs_disk, swap):
        raise StressFailure(f"{kind}: SWAP setup failed for corruption test")
    swap_page = bytearray(part_read(fs_disk, swap, 0, 8))
    swap_page[SWAP_SIGNATURE_OFFSET:SWAP_SIGNATURE_OFFSET + 10] = b"NOT-SWAP!!"
    part_write(fs_disk, swap, 0, swap_page)
    expect_not_detected(
        f"{kind}: SWAP bad signature",
        lambda: detect_swap(fs_disk, swap),
    )


def stress_cdrom():
    STATS["drive_kinds"] += 1
    disk = Disk("CDROM", 64)
    write_cdrom_iso9660(disk)
    if not detect_cdrom_iso9660(disk):
        raise StressFailure("CDROM: ISO9660 marker was not detected")
    STATS["detect_only_checks"] += 1
    try:
        disk.write(0, bytes(SECTOR_SIZE))
    except StressFailure:
        pass
    else:
        raise StressFailure("CDROM: raw sector write unexpectedly succeeded")


def main():
    failures = []
    for parity_check in (assert_kernel_enum_coverage, assert_kernel_storage_parity):
        try:
            parity_check()
        except StressFailure as exc:
            failures.append(str(exc))

    for kind in DRIVE_KINDS:
        try:
            if kind == "CDROM":
                stress_cdrom()
            else:
                stress_writable_kind(kind)
        except StressFailure as exc:
            failures.append(str(exc))

    if failures:
        print("storage format stress failures:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "storage format stress: "
        f"{STATS['drive_kinds']} drive kinds, "
        f"{STATS['partition_tables']} partition table checks, "
        f"{STATS['formatter_checks']} formatter checks, "
        f"{STATS['detect_only_checks']} detect-only checks, "
        f"{STATS['boundary_checks']} boundary checks, "
        f"{STATS['corruption_checks']} corruption checks passed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
