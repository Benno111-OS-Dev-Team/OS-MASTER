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
EXT4_BLOCK_SIZE = 4096
EXT4_SECTORS_PER_BLOCK = EXT4_BLOCK_SIZE // SECTOR_SIZE
EXT4_BLOCKS_PER_GROUP = EXT4_BLOCK_SIZE * 8
EXT4_GROUP_DESC_SIZE = 32
EXT4_INODE_SIZE = 128
EXT4_INODES_PER_GROUP = 2048
EXT4_SUPERBLOCK_OFFSET = 1024
EXT4_SUPERBLOCK_MAGIC_OFFSET = 56
EXT4_SUPERBLOCK_VOLUME_NAME_OFFSET = 120
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
    "churn_checks": 0,
    "gpt_crc_checks": 0,
    "fs_metadata_checks": 0,
    "partition_range_checks": 0,
    "block_api_checks": 0,
    "detection_precedence_checks": 0,
    "label_checks": 0,
    "management_checks": 0,
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


def parse_storage_function_body(function_name):
    source = storage_source()
    pattern = re.compile(
        rf"(?:\bstatic\b\s+)?[A-Za-z_][A-Za-z0-9_ \t\*]*"
        rf"\b{re.escape(function_name)}\s*"
        r"\([^;{}]*\)\s*\{",
        re.DOTALL,
    )
    for match in pattern.finditer(source):
        brace = source.find("{", match.start())
        if brace < 0:
            continue
        depth = 0
        for pos in range(brace, len(source)):
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
                if depth == 0:
                    return source[brace + 1:pos]
    raise StressFailure(f"could not find {function_name} in storage.c")


def assert_contains_all(haystack, needles, label):
    missing = [needle for needle in needles if needle not in haystack]
    if missing:
        raise StressFailure(
            f"{label} missing hardening guard(s): {', '.join(missing)}"
        )


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
        "STORAGE_EXT4_SUPERBLOCK_OFFSET": EXT4_SUPERBLOCK_OFFSET,
        "STORAGE_EXT4_SUPERBLOCK_MAGIC_OFFSET": EXT4_SUPERBLOCK_MAGIC_OFFSET,
        "STORAGE_EXT4_SUPERBLOCK_VOLUME_NAME_OFFSET": (
            EXT4_SUPERBLOCK_VOLUME_NAME_OFFSET
        ),
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

    source = storage_source()
    assert_contains_all(
        source,
        [
            "storage_partition_range_valid",
            "storage_partition_range_overlaps",
            "storage_has_protective_mbr",
        ],
        "storage loader source",
    )

    gpt_loader = parse_storage_function_body("storage_load_gpt_partitions")
    assert_contains_all(
        gpt_loader,
        [
            "header.header_crc32",
            "storage_crc32(header_crc_sector, header.header_size)",
            "header.partition_entry_array_crc32",
            "storage_partition_range_valid",
            "storage_partition_range_overlaps",
            "storage_clear_partitions(disk_index)",
        ],
        "GPT loader",
    )

    mbr_loader = parse_storage_function_body("storage_load_mbr_partitions")
    assert_contains_all(
        mbr_loader,
        [
            "type == 0xEE",
            "storage_partition_range_valid",
            "storage_partition_range_overlaps",
            "storage_clear_partitions(disk_index)",
        ],
        "MBR loader",
    )

    partition_loader = parse_storage_function_body("storage_load_partitions")
    assert_contains_all(
        partition_loader,
        [
            "storage_has_protective_mbr(disk_index)",
            "storage_clear_partitions(disk_index)",
            "return;",
        ],
        "partition loader fallback",
    )

    read_block = parse_storage_function_body("storage_read_block")
    assert_contains_all(
        read_block,
        [
            "block_size == 512",
            "block_size == 2048",
            "STORAGE_KIND_CDROM",
            "return -1;",
        ],
        "storage_read_block",
    )

    write_block = parse_storage_function_body("storage_write_block")
    assert_contains_all(
        write_block,
        [
            "block_size == 512",
            "return -1;",
        ],
        "storage_write_block",
    )

    write_image = parse_storage_function_body("storage_write_disk_image")
    assert_contains_all(
        write_image,
        [
            "STORAGE_KIND_CDROM",
            "image_sectors > disk_sectors",
            "sector[i] = 0",
            "storage_disk_write_sector",
        ],
        "storage_write_disk_image",
    )

    gpt_decode = parse_storage_function_body("storage_decode_gpt_name")
    assert_contains_all(
        gpt_decode,
        [
            "max - 1",
            "ch >= 32 && ch < 127",
            "?",
        ],
        "GPT label decode",
    )

    trim_ascii = parse_storage_function_body("storage_trim_ascii_field")
    assert_contains_all(
        trim_ascii,
        [
            "src[end - 1] == ' '",
            "src[end - 1] == '\\0'",
            "ch >= 32 && ch < 127",
            "_",
        ],
        "filesystem label trim",
    )

    copy_ascii = parse_storage_function_body("storage_copy_ascii_padded")
    assert_contains_all(
        copy_ascii,
        [
            "ch >= 'a' && ch <= 'z'",
            "ch - 'a' + 'A'",
        ],
        "filesystem label formatter",
    )

    create_partition_body = parse_storage_function_body("storage_create_partition")
    assert_contains_all(
        create_partition_body,
        [
            "storage_partition_space_available",
            "storage_find_free_partition_slot",
            "old_parts",
            "storage_commit_gpt_partitions",
        ],
        "storage_create_partition",
    )

    update_partition_body = parse_storage_function_body("storage_update_partition")
    assert_contains_all(
        update_partition_body,
        [
            "used_without_part",
            "storage_partition_space_available",
            "old_parts",
            "storage_commit_gpt_partitions",
        ],
        "storage_update_partition",
    )

    delete_partition_body = parse_storage_function_body("storage_delete_partition")
    assert_contains_all(
        delete_partition_body,
        [
            "old_parts",
            "storage_commit_gpt_partitions",
            "storage_partitions[disk_index][i].present = 0",
        ],
        "storage_delete_partition",
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

    def supports_partition_writes(self):
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


def storage_read_block(disk, lba, block_size):
    if block_size == SECTOR_SIZE:
        if disk.kind == "CDROM":
            raise StressFailure("CDROM: 512-byte read unexpectedly succeeded")
        return disk.read(lba, 1)
    if block_size == 2048 and disk.kind == "CDROM":
        return disk.read(lba * 4, 4)
    raise StressFailure(f"{disk.kind}: unsupported read block size {block_size}")


def storage_write_block(disk, lba, payload, block_size):
    if block_size != SECTOR_SIZE:
        raise StressFailure(f"{disk.kind}: unsupported write block size {block_size}")
    if len(payload) != SECTOR_SIZE:
        raise StressFailure(f"{disk.kind}: write block payload size mismatch")
    disk.write(lba, payload)


def storage_write_disk_image(disk, data):
    if not data:
        return False
    if disk.kind == "CDROM":
        return False
    image_sectors = (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE
    if image_sectors > disk.sectors:
        return False
    for sector_index in range(image_sectors):
        chunk = data[
            sector_index * SECTOR_SIZE:(sector_index + 1) * SECTOR_SIZE
        ]
        disk.write(sector_index, chunk + bytes(SECTOR_SIZE - len(chunk)))
    return True


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


def put_ext4_inode(buf, off, mode, size, blocks, first_block):
    put16(buf, off + 0, mode)
    put32(buf, off + 4, size)
    put16(buf, off + 26, 2)
    put32(buf, off + 28, blocks)
    put32(buf, off + 40, first_block)


def ascii_padded(text, length):
    raw = (text or "")[:length].encode("ascii", "replace")
    return raw + b" " * (length - len(raw))


def ascii_upper_padded(text, length):
    return ascii_padded((text or "").upper(), length)


def trim_ascii_field(data):
    return data.rstrip(b" \x00").decode("ascii", "replace")


def decode_gpt_name(raw):
    out = []
    for idx in range(36):
        ch = le16(raw, idx * 2)
        if ch == 0:
            break
        out.append(chr(ch) if 32 <= ch < 127 else "?")
        if len(out) >= 31:
            break
    return "".join(out)


def storage_crc32(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = ((crc >> 1) ^ (0xEDB88320 & mask)) & 0xFFFFFFFF
    return (~crc) & 0xFFFFFFFF


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
    put32(header, 88, storage_crc32(entries))
    put32(header, 16, storage_crc32(header[:92]))
    disk.write(1, header)
    disk.write(2, entries)

    backup_lba = disk.sectors - GPT_ENTRY_SECTORS - 1
    disk.write(backup_lba, entries)
    header2 = bytearray(header)
    put32(header2, 16, 0)
    put64(header2, 24, disk.sectors - 1)
    put64(header2, 32, 1)
    put64(header2, 72, backup_lba)
    put32(header2, 16, storage_crc32(header2[:92]))
    disk.write(disk.sectors - 1, header2)


def write_gpt_entries_with_valid_crcs(disk, entries):
    entry_crc = storage_crc32(entries)
    primary = bytearray(disk.read(1))
    backup = bytearray(disk.read(disk.sectors - 1))
    primary_entry_lba = struct.unpack_from("<Q", primary, 72)[0]
    backup_entry_lba = struct.unpack_from("<Q", backup, 72)[0]

    put32(primary, 88, entry_crc)
    put32(primary, 16, 0)
    put32(primary, 16, storage_crc32(primary[:le32(primary, 12)]))
    put32(backup, 88, entry_crc)
    put32(backup, 16, 0)
    put32(backup, 16, storage_crc32(backup[:le32(backup, 12)]))

    disk.write(primary_entry_lba, entries)
    disk.write(backup_entry_lba, entries)
    disk.write(1, primary)
    disk.write(disk.sectors - 1, backup)


def validate_partition_ranges(disk, ranges, label):
    previous_end = 0
    for start_lba, last_lba in sorted(ranges):
        if start_lba < 2048:
            raise StressFailure(f"{label}: partition starts before usable space")
        if last_lba < start_lba:
            raise StressFailure(f"{label}: partition has inverted LBA range")
        if last_lba >= disk.sectors:
            raise StressFailure(f"{label}: partition extends beyond disk")
        if start_lba < previous_end:
            raise StressFailure(f"{label}: partition ranges overlap")
        previous_end = last_lba + 1
    STATS["partition_range_checks"] += 1


def validate_mbr(disk):
    sector = disk.read(0)
    ranges = []
    for idx in range(4):
        entry = MBR_PARTITION_OFFSET + idx * 16
        part_type = sector[entry + 4]
        start_lba = le32(sector, entry + 8)
        sector_count = le32(sector, entry + 12)
        if part_type == 0:
            if sector_count != 0:
                raise StressFailure("MBR empty partition carried sectors")
            continue
        if sector_count == 0:
            raise StressFailure("MBR partition has zero sectors")
        ranges.append((start_lba, start_lba + sector_count - 1))
    validate_partition_ranges(disk, ranges, "MBR")


def validate_gpt_header(disk, lba, expected_current_lba):
    header = bytearray(disk.read(lba))
    if header[0:8] != b"EFI PART":
        raise StressFailure("GPT header signature missing")
    header_size = le32(header, 12)
    if header_size < 92 or header_size > SECTOR_SIZE:
        raise StressFailure("GPT header size invalid")
    if struct.unpack_from("<Q", header, 24)[0] != expected_current_lba:
        raise StressFailure("GPT current LBA mismatch")
    stored_header_crc = le32(header, 16)
    header[16:20] = b"\x00\x00\x00\x00"
    if storage_crc32(header[:header_size]) != stored_header_crc:
        raise StressFailure("GPT header CRC mismatch")

    entry_lba = struct.unpack_from("<Q", header, 72)[0]
    entry_count = le32(header, 80)
    entry_size = le32(header, 84)
    if entry_count != GPT_ENTRY_COUNT or entry_size != GPT_ENTRY_SIZE:
        raise StressFailure("GPT entry geometry mismatch")
    entry_bytes = entry_count * entry_size
    entry_sectors = (entry_bytes + SECTOR_SIZE - 1) // SECTOR_SIZE
    entries = disk.read(entry_lba, entry_sectors)[:entry_bytes]
    if storage_crc32(entries) != le32(header, 88):
        raise StressFailure("GPT partition entry CRC mismatch")
    first_usable_lba = struct.unpack_from("<Q", header, 40)[0]
    last_usable_lba = struct.unpack_from("<Q", header, 48)[0]
    if first_usable_lba < 2048 or last_usable_lba >= disk.sectors:
        raise StressFailure("GPT usable LBA range invalid")
    ranges = []
    for idx in range(entry_count):
        off = idx * entry_size
        if entries[off:off + 16] == bytes(16):
            continue
        first_lba = struct.unpack_from("<Q", entries, off + 32)[0]
        last_lba = struct.unpack_from("<Q", entries, off + 40)[0]
        if first_lba < first_usable_lba or last_lba > last_usable_lba:
            raise StressFailure("GPT partition outside usable LBA range")
        ranges.append((first_lba, last_lba))
    validate_partition_ranges(disk, ranges, "GPT")
    STATS["gpt_crc_checks"] += 1


def validate_gpt(disk):
    validate_gpt_header(disk, 1, 1)
    validate_gpt_header(disk, disk.sectors - 1, disk.sectors - 1)


def detect_partition_table(disk):
    sector = disk.read(0)
    if sector[510:512] != b"\x55\xaa":
        return "none"
    if sector[MBR_PARTITION_OFFSET + 4] == 0xEE:
        validate_gpt(disk)
        return "gpt"
    validate_mbr(disk)
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
    sector[71:82] = ascii_upper_padded(part["label"], 11)
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

    fat_sector = bytearray(SECTOR_SIZE)
    put32(fat_sector, 0, 0x0FFFFFF8)
    put32(fat_sector, 4, 0xFFFFFFFF)
    put32(fat_sector, 8, 0x0FFFFFFF)
    fats_start = reserved
    part_write(disk, part, fats_start, fat_sector)
    part_write(disk, part, fats_start + fat_size, fat_sector)

    empty = bytearray(SECTOR_SIZE)
    for idx in range(1, fat_size):
        part_write(disk, part, fats_start + idx, empty)
        part_write(disk, part, fats_start + fat_size + idx, empty)

    root_dir_first_sector = reserved + fat_size * 2
    for idx in range(spc):
        part_write(disk, part, root_dir_first_sector + idx, empty)
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


def validate_fat32_metadata(disk, part):
    sector = part_read(disk, part, 0, 1)
    backup = part_read(disk, part, 6, 1)
    fsinfo = part_read(disk, part, 1, 1)
    fsinfo_backup = part_read(disk, part, 7, 1)
    if backup != sector:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 backup boot mismatch")
    if fsinfo_backup != fsinfo:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 backup FSInfo mismatch")
    if le16(sector, 11) != SECTOR_SIZE:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 sector size mismatch")
    if le16(sector, 14) != 32 or sector[16] != 2 or le32(sector, 44) != 2:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 layout fields invalid")
    if le32(sector, 32) != part["sectors"]:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 sector count mismatch")
    if trim_ascii_field(sector[71:82]) != part["label"][:11].upper():
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 label mismatch")
    if le32(fsinfo, 0) != 0x41615252 or le32(fsinfo, 484) != 0x61417272:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 FSInfo signatures invalid")
    if le32(fsinfo, 508) != 0xAA550000:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 FSInfo trail signature invalid")
    fat_size = le32(sector, 36)
    first_fat = part_read(disk, part, le16(sector, 14), 1)
    second_fat = part_read(disk, part, le16(sector, 14) + fat_size, 1)
    if first_fat != second_fat:
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 FAT copies differ")
    if (le32(first_fat, 0), le32(first_fat, 4), le32(first_fat, 8)) != (
            0x0FFFFFF8, 0xFFFFFFFF, 0x0FFFFFFF):
        raise StressFailure(f"{disk.kind}/{part['kind']}: FAT32 reserved FAT entries invalid")
    STATS["fs_metadata_checks"] += 1


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


def validate_swap_metadata(disk, part):
    page = part_read(disk, part, 0, 8)
    sig = page[SWAP_SIGNATURE_OFFSET:SWAP_SIGNATURE_OFFSET + 10]
    if sig != b"SWAPSPACE2":
        raise StressFailure(f"{disk.kind}/{part['kind']}: SWAP signature mismatch")
    if any(page[:SWAP_SIGNATURE_OFFSET]) or any(page[SWAP_SIGNATURE_OFFSET + 10:]):
        raise StressFailure(f"{disk.kind}/{part['kind']}: SWAP page was not zero-filled")
    STATS["fs_metadata_checks"] += 1


def format_ext4(disk, part):
    if part["sectors"] < EXT4_SECTORS_PER_BLOCK * 256:
        return False
    total_blocks = part["sectors"] // EXT4_SECTORS_PER_BLOCK
    group_count = (total_blocks + EXT4_BLOCKS_PER_GROUP - 1) // EXT4_BLOCKS_PER_GROUP
    desc_blocks = (
        group_count * EXT4_GROUP_DESC_SIZE + EXT4_BLOCK_SIZE - 1
    ) // EXT4_BLOCK_SIZE
    inode_table_blocks = (
        EXT4_INODES_PER_GROUP * EXT4_INODE_SIZE + EXT4_BLOCK_SIZE - 1
    ) // EXT4_BLOCK_SIZE
    total_inodes = group_count * EXT4_INODES_PER_GROUP
    total_free_blocks = 0
    total_free_inodes = 0
    group_desc = bytearray(desc_blocks * EXT4_BLOCK_SIZE)

    for group in range(group_count):
        group_first_block = group * EXT4_BLOCKS_PER_GROUP
        group_blocks = min(EXT4_BLOCKS_PER_GROUP,
                           total_blocks - group_first_block)
        block_bitmap = 1 + desc_blocks if group == 0 else 0
        inode_bitmap = block_bitmap + 1
        inode_table = inode_bitmap + 1
        used_blocks = inode_table + inode_table_blocks
        if group == 0:
            used_blocks += 1
        if group_blocks <= used_blocks:
            return False
        used_inodes = 11 if group == 0 else 0
        free_blocks = group_blocks - used_blocks
        free_inodes = EXT4_INODES_PER_GROUP - used_inodes
        total_free_blocks += free_blocks
        total_free_inodes += free_inodes

        off = group * EXT4_GROUP_DESC_SIZE
        put32(group_desc, off + 0, group_first_block + block_bitmap)
        put32(group_desc, off + 4, group_first_block + inode_bitmap)
        put32(group_desc, off + 8, group_first_block + inode_table)
        put16(group_desc, off + 12, free_blocks)
        put16(group_desc, off + 14, free_inodes)
        put16(group_desc, off + 16, 1 if group == 0 else 0)

        bitmap = bytearray(EXT4_BLOCK_SIZE)
        for idx in range(used_blocks):
            bitmap[idx // 8] |= 1 << (idx % 8)
        for idx in range(group_blocks, EXT4_BLOCKS_PER_GROUP):
            bitmap[idx // 8] |= 1 << (idx % 8)
        part_write(disk, part,
                   (group_first_block + block_bitmap) * EXT4_SECTORS_PER_BLOCK,
                   bitmap)

        bitmap = bytearray(EXT4_BLOCK_SIZE)
        for idx in range(used_inodes):
            bitmap[idx // 8] |= 1 << (idx % 8)
        part_write(disk, part,
                   (group_first_block + inode_bitmap) * EXT4_SECTORS_PER_BLOCK,
                   bitmap)

        empty = bytearray(EXT4_BLOCK_SIZE)
        for idx in range(inode_table_blocks):
            part_write(disk, part,
                       (group_first_block + inode_table + idx) *
                       EXT4_SECTORS_PER_BLOCK,
                       empty)

        if group == 0:
            root_dir_block = used_blocks - 1
            inode_block = bytearray(part_read(
                disk, part, inode_table * EXT4_SECTORS_PER_BLOCK,
                EXT4_SECTORS_PER_BLOCK))
            put_ext4_inode(inode_block, EXT4_INODE_SIZE,
                           0x4000 | 0o755, EXT4_BLOCK_SIZE,
                           EXT4_SECTORS_PER_BLOCK, root_dir_block)
            part_write(disk, part, inode_table * EXT4_SECTORS_PER_BLOCK,
                       inode_block)

            root_dir = bytearray(EXT4_BLOCK_SIZE)
            put32(root_dir, 0, 2)
            put16(root_dir, 4, 12)
            root_dir[6] = 1
            root_dir[7] = 2
            root_dir[8] = ord(".")
            put32(root_dir, 12, 2)
            put16(root_dir, 16, EXT4_BLOCK_SIZE - 12)
            root_dir[18] = 2
            root_dir[19] = 2
            root_dir[20:22] = b".."
            part_write(disk, part, root_dir_block * EXT4_SECTORS_PER_BLOCK,
                       root_dir)

    for idx in range(desc_blocks):
        start = idx * EXT4_BLOCK_SIZE
        part_write(disk, part, (1 + idx) * EXT4_SECTORS_PER_BLOCK,
                   group_desc[start:start + EXT4_BLOCK_SIZE])

    block = bytearray(EXT4_BLOCK_SIZE)
    put32(block, 1024 + 0, total_inodes)
    put32(block, 1024 + 4, total_blocks)
    put32(block, 1024 + 12, total_free_blocks)
    put32(block, 1024 + 16, total_free_inodes)
    put32(block, 1024 + 24, 2)
    put32(block, 1024 + 28, 2)
    put32(block, 1024 + 32, EXT4_BLOCKS_PER_GROUP)
    put32(block, 1024 + 36, EXT4_BLOCKS_PER_GROUP)
    put32(block, 1024 + 40, EXT4_INODES_PER_GROUP)
    put16(block, 1024 + 56, 0xEF53)
    put16(block, 1024 + 58, 1)
    put16(block, 1024 + 60, 1)
    put32(block, 1024 + 76, 1)
    put32(block, 1024 + 84, 11)
    put16(block, 1024 + 88, EXT4_INODE_SIZE)
    put16(block, 1024 + 254, EXT4_GROUP_DESC_SIZE)
    block[1024 + 120:1024 + 136] = ascii_upper_padded(part["label"], 16)
    part_write(disk, part, 0, block)
    return True


def detect_ext4(disk, part):
    sector = part_read(disk, part, 2, 1)
    return sector[56:58] == b"\x53\xef"


def validate_ext4_metadata(disk, part):
    block = part_read(disk, part, 0, EXT4_SECTORS_PER_BLOCK)
    total_blocks = part["sectors"] // EXT4_SECTORS_PER_BLOCK
    group_count = (total_blocks + EXT4_BLOCKS_PER_GROUP - 1) // EXT4_BLOCKS_PER_GROUP
    desc_blocks = (
        group_count * EXT4_GROUP_DESC_SIZE + EXT4_BLOCK_SIZE - 1
    ) // EXT4_BLOCK_SIZE
    inode_table_blocks = (
        EXT4_INODES_PER_GROUP * EXT4_INODE_SIZE + EXT4_BLOCK_SIZE - 1
    ) // EXT4_BLOCK_SIZE
    if le32(block, 1024 + 4) != total_blocks:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 block count mismatch")
    if le32(block, 1024 + 0) != group_count * EXT4_INODES_PER_GROUP:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 inode count mismatch")
    if le32(block, 1024 + 24) != 2:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 block size log mismatch")
    if le32(block, 1024 + 32) != EXT4_BLOCKS_PER_GROUP:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 group geometry invalid")
    if le32(block, 1024 + 40) != EXT4_INODES_PER_GROUP:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 inode group geometry invalid")
    if le16(block, 1024 + 56) != 0xEF53:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 magic mismatch")
    if le16(block, 1024 + 58) != 1 or le16(block, 1024 + 60) != 1:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 state/error policy invalid")
    if le32(block, 1024 + 84) != 11 or le16(block, 1024 + 88) != EXT4_INODE_SIZE:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 inode metadata invalid")
    if le16(block, 1024 + 254) != EXT4_GROUP_DESC_SIZE:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 group descriptor size invalid")
    if trim_ascii_field(block[1024 + 120:1024 + 136]) != part["label"][:16].upper():
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 label mismatch")

    desc = part_read(disk, part, EXT4_SECTORS_PER_BLOCK,
                     desc_blocks * EXT4_SECTORS_PER_BLOCK)
    total_free_blocks = 0
    total_free_inodes = 0
    for group in range(group_count):
        off = group * EXT4_GROUP_DESC_SIZE
        group_first_block = group * EXT4_BLOCKS_PER_GROUP
        group_blocks = min(EXT4_BLOCKS_PER_GROUP,
                           total_blocks - group_first_block)
        block_bitmap = le32(desc, off + 0)
        inode_bitmap = le32(desc, off + 4)
        inode_table = le32(desc, off + 8)
        expected_block_bitmap = group_first_block + (1 + desc_blocks if group == 0 else 0)
        expected_inode_bitmap = expected_block_bitmap + 1
        expected_inode_table = expected_inode_bitmap + 1
        used_blocks = expected_inode_table - group_first_block + inode_table_blocks
        if group == 0:
            used_blocks += 1
        if (block_bitmap, inode_bitmap, inode_table) != (
                expected_block_bitmap, expected_inode_bitmap, expected_inode_table):
            raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 group descriptor pointers invalid")
        if le16(desc, off + 12) != group_blocks - used_blocks:
            raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 free block count invalid")
        used_inodes = 11 if group == 0 else 0
        if le16(desc, off + 14) != EXT4_INODES_PER_GROUP - used_inodes:
            raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 free inode count invalid")
        if le16(desc, off + 16) != (1 if group == 0 else 0):
            raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 used dir count invalid")
        total_free_blocks += group_blocks - used_blocks
        total_free_inodes += EXT4_INODES_PER_GROUP - used_inodes
    if le32(block, 1024 + 12) != total_free_blocks:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 total free blocks invalid")
    if le32(block, 1024 + 16) != total_free_inodes:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 total free inodes invalid")

    inode_table = le32(desc, 8)
    inode_block = part_read(disk, part, inode_table * EXT4_SECTORS_PER_BLOCK,
                            EXT4_SECTORS_PER_BLOCK)
    root_inode = EXT4_INODE_SIZE
    root_dir_block = le32(inode_block, root_inode + 40)
    if le16(inode_block, root_inode + 0) != (0x4000 | 0o755):
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 root inode mode invalid")
    if le32(inode_block, root_inode + 4) != EXT4_BLOCK_SIZE:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 root inode size invalid")
    if le16(inode_block, root_inode + 26) != 2:
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 root inode link count invalid")
    root_dir = part_read(disk, part, root_dir_block * EXT4_SECTORS_PER_BLOCK,
                         EXT4_SECTORS_PER_BLOCK)
    if le32(root_dir, 0) != 2 or root_dir[6] != 1 or root_dir[8:9] != b".":
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 root dot entry invalid")
    if le32(root_dir, 12) != 2 or root_dir[18] != 2 or root_dir[20:22] != b"..":
        raise StressFailure(f"{disk.kind}/{part['kind']}: EXT4 root dot-dot entry invalid")
    STATS["fs_metadata_checks"] += 1


def write_partition_iso9660_marker(disk, part):
    block = bytearray(2048)
    block[0] = 1
    block[1:6] = b"CD001"
    block[40:72] = ascii_padded("OS8_STRESS_ISO", 32)
    part_write(disk, part, 64, block)


def detect_partition_iso9660(disk, part):
    block = part_read(disk, part, 64, 4)
    return block[0] in (1, 2) and block[1:6] == b"CD001"


def validate_partition_iso9660_metadata(disk, part):
    block = part_read(disk, part, 64, 4)
    if block[0] != 1:
        raise StressFailure(f"{disk.kind}/{part['kind']}: ISO9660 descriptor type mismatch")
    if trim_ascii_field(block[40:72]) != "OS8_STRESS_ISO":
        raise StressFailure(f"{disk.kind}/{part['kind']}: ISO9660 volume label mismatch")
    STATS["fs_metadata_checks"] += 1


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


def validate_cdrom_iso9660_metadata(disk):
    block = disk.read(64, 4)
    if block[0] != 1:
        raise StressFailure(f"{disk.kind}: CD-ROM ISO9660 descriptor type mismatch")
    if trim_ascii_field(block[40:72]) != "OS8_STRESS_ISO":
        raise StressFailure(f"{disk.kind}: CD-ROM ISO9660 volume label mismatch")
    STATS["fs_metadata_checks"] += 1


def write_apfs_marker(disk, part):
    block = bytearray(4096)
    put32(block, 32, 0x4253584E)
    part_write(disk, part, 0, block)


def detect_apfs(disk, part):
    block = part_read(disk, part, 0, 8)
    return le32(block, 32) == 0x4253584E


def detect_filesystem(disk, part):
    if detect_partition_iso9660(disk, part):
        return "ISO9660"
    if detect_apfs(disk, part):
        return "APFS"
    if detect_ext4(disk, part):
        return "EXT4"
    if detect_fat32(disk, part):
        return "FAT32"
    if detect_swap(disk, part):
        return "SWAP"
    return "UNKNOWN"


def validate_apfs_metadata(disk, part):
    block = part_read(disk, part, 0, 8)
    if le32(block, 32) != 0x4253584E:
        raise StressFailure(f"{disk.kind}/{part['kind']}: APFS container magic mismatch")
    STATS["fs_metadata_checks"] += 1


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


def assert_layout_within_disk(kind, parts, disk):
    previous_end = 2048
    for part in parts:
        if part["start"] < previous_end:
            raise StressFailure(f"{kind}: partition layout overlapped")
        if part["start"] + part["sectors"] > disk.sectors:
            raise StressFailure(f"{kind}: partition layout exceeded disk")
        previous_end = part["start"] + part["sectors"]
    STATS["churn_checks"] += 1


def create_partition(parts, kind, size_mib, capacity_mib=None):
    if size_mib <= 0 or len(parts) >= 8:
        return False
    if capacity_mib is not None:
        used_mib = sum(part["sectors"] for part in parts) // SECTORS_PER_MIB
        if used_mib > capacity_mib or size_mib > capacity_mib - used_mib:
            return False
    ordinal = sum(1 for part in parts if part["kind"] == kind)
    start = 2048
    if parts:
        last = parts[-1]
        start = last["start"] + last["sectors"]
    parts.append({
        "kind": kind,
        "label": f"{kind}{ordinal + 1}",
        "start": start,
        "sectors": size_mib * SECTORS_PER_MIB,
    })
    return True


def used_mib(parts):
    return sum(part["sectors"] for part in parts) // SECTORS_PER_MIB


def count_partitions_of_kind(parts, kind):
    return sum(1 for part in parts if part["kind"] == kind)


def update_partition(parts, index, kind, size_mib, capacity_mib=None):
    if index < 0 or index >= len(parts) or size_mib <= 0:
        return False
    if capacity_mib is not None:
        used_without_part = used_mib(parts) - parts[index]["sectors"] // SECTORS_PER_MIB
        if used_without_part > capacity_mib or size_mib > capacity_mib - used_without_part:
            return False
    parts[index]["kind"] = kind
    parts[index]["label"] = f"{kind}{index + 1}"
    parts[index]["sectors"] = size_mib * SECTORS_PER_MIB
    for idx in range(index, len(parts)):
        if idx == 0:
            parts[idx]["start"] = 2048
        else:
            previous = parts[idx - 1]
            parts[idx]["start"] = previous["start"] + previous["sectors"]
    return True


def delete_partition(parts, index):
    if index < 0 or index >= len(parts):
        return False
    del parts[index]
    for idx, part in enumerate(parts):
        if idx == 0:
            part["start"] = 2048
        else:
            previous = parts[idx - 1]
            part["start"] = previous["start"] + previous["sectors"]
    return True


def check_formatter(kind, table_format, part, fs_name):
    disk = Disk(kind, 1024)
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
        if formatted:
            validate_fat32_metadata(disk, clone)
    elif fs_name == "EXT4":
        if not format_ext4(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: EXT4 rejected valid partition"
            )
        if not detect_ext4(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: EXT4 did not self-detect"
            )
        validate_ext4_metadata(disk, clone)
    elif fs_name == "SWAP":
        if not format_swap(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: SWAP rejected valid partition"
            )
        if not detect_swap(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: SWAP did not self-detect"
            )
        validate_swap_metadata(disk, clone)
    else:
        raise StressFailure(f"unknown formatter {fs_name}")

    STATS["formatter_checks"] += 1


def check_detect_only(kind, table_format, part, fs_name):
    disk = Disk(kind, 1024)
    clone = dict(part)

    if fs_name == "ISO9660":
        write_partition_iso9660_marker(disk, clone)
        if not detect_partition_iso9660(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: ISO9660 marker not detected"
            )
        validate_partition_iso9660_metadata(disk, clone)
    elif fs_name == "APFS":
        write_apfs_marker(disk, clone)
        if not detect_apfs(disk, clone):
            raise StressFailure(
                f"{kind}/{table_format}/{clone['kind']}: APFS marker not detected"
            )
        validate_apfs_metadata(disk, clone)
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

    stress_block_api(kind)

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
    stress_detection_precedence(kind)
    stress_label_handling(kind)
    stress_partition_management(kind)
    stress_gpt_partition_churn(kind)


def stress_block_api(kind):
    disk = Disk(kind, 8)
    payload = bytes((idx * 17 + 3) & 0xFF for idx in range(SECTOR_SIZE))
    storage_write_block(disk, 0, payload, SECTOR_SIZE)
    if storage_read_block(disk, 0, SECTOR_SIZE) != payload:
        raise StressFailure(f"{kind}: block API roundtrip mismatch")
    STATS["block_api_checks"] += 1

    expect_stress_failure(
        f"{kind}: unsupported read block size",
        lambda: storage_read_block(disk, 0, 1024),
    )
    expect_stress_failure(
        f"{kind}: unsupported write block size",
        lambda: storage_write_block(disk, 0, payload * 4, 2048),
    )

    image = b"OS8IMAGE" + bytes(range(251)) * 3
    if not storage_write_disk_image(disk, image):
        raise StressFailure(f"{kind}: disk image write rejected valid image")
    first = storage_read_block(disk, 0, SECTOR_SIZE)
    second = storage_read_block(disk, 1, SECTOR_SIZE)
    if first[:len(image[:SECTOR_SIZE])] != image[:SECTOR_SIZE]:
        raise StressFailure(f"{kind}: disk image first sector mismatch")
    if second[:len(image[SECTOR_SIZE:])] != image[SECTOR_SIZE:]:
        raise StressFailure(f"{kind}: disk image second sector mismatch")
    if any(second[len(image[SECTOR_SIZE:]):]):
        raise StressFailure(f"{kind}: disk image tail was not zero padded")
    STATS["block_api_checks"] += 1

    if storage_write_disk_image(disk, bytes(disk.sectors * SECTOR_SIZE + 1)):
        raise StressFailure(f"{kind}: oversize disk image was accepted")
    STATS["block_api_checks"] += 1


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

    crc_disk = Disk(kind, 128)
    write_gpt(crc_disk, parts)
    bad_header = bytearray(crc_disk.read(1))
    bad_header[40] ^= 0x01
    crc_disk.write(1, bad_header)
    expect_stress_failure(
        f"{kind}: GPT header CRC corruption",
        lambda: detect_partition_table(crc_disk),
    )

    crc_disk = Disk(kind, 128)
    write_gpt(crc_disk, parts)
    bad_entries = bytearray(crc_disk.read(2, GPT_ENTRY_SECTORS))
    bad_entries[0] ^= 0x01
    crc_disk.write(2, bad_entries)
    expect_stress_failure(
        f"{kind}: GPT entry CRC corruption",
        lambda: detect_partition_table(crc_disk),
    )

    range_disk = Disk(kind, 128)
    write_gpt(range_disk, parts)
    range_entries = bytearray(range_disk.read(2, GPT_ENTRY_SECTORS))
    put64(range_entries, 0 * GPT_ENTRY_SIZE + 32, 34)
    put64(range_entries, 0 * GPT_ENTRY_SIZE + 40, 4096)
    write_gpt_entries_with_valid_crcs(range_disk, range_entries)
    expect_stress_failure(
        f"{kind}: GPT partition before usable range",
        lambda: detect_partition_table(range_disk),
    )

    range_disk = Disk(kind, 128)
    write_gpt(range_disk, parts)
    range_entries = bytearray(range_disk.read(2, GPT_ENTRY_SECTORS))
    put64(range_entries, 0 * GPT_ENTRY_SIZE + 40, range_disk.sectors - 1)
    write_gpt_entries_with_valid_crcs(range_disk, range_entries)
    expect_stress_failure(
        f"{kind}: GPT partition beyond usable range",
        lambda: detect_partition_table(range_disk),
    )

    range_disk = Disk(kind, 128)
    write_gpt(range_disk, parts)
    range_entries = bytearray(range_disk.read(2, GPT_ENTRY_SECTORS))
    put64(range_entries, 1 * GPT_ENTRY_SIZE + 32, parts[0]["start"] + 8)
    put64(range_entries, 1 * GPT_ENTRY_SIZE + 40, parts[0]["start"] + 64)
    write_gpt_entries_with_valid_crcs(range_disk, range_entries)
    expect_stress_failure(
        f"{kind}: GPT overlapping partitions",
        lambda: detect_partition_table(range_disk),
    )

    range_disk = Disk(kind, 128)
    write_mbr(range_disk, parts)
    range_sector = bytearray(range_disk.read(0))
    put32(range_sector, MBR_PARTITION_OFFSET + 12, range_disk.sectors)
    range_disk.write(0, range_sector)
    expect_stress_failure(
        f"{kind}: MBR partition beyond disk",
        lambda: detect_partition_table(range_disk),
    )

    range_disk = Disk(kind, 128)
    write_mbr(range_disk, parts)
    range_sector = bytearray(range_disk.read(0))
    put32(range_sector, MBR_PARTITION_OFFSET + 16 + 8, parts[0]["start"] + 16)
    put32(range_sector, MBR_PARTITION_OFFSET + 16 + 12, 128)
    range_disk.write(0, range_sector)
    expect_stress_failure(
        f"{kind}: MBR overlapping partitions",
        lambda: detect_partition_table(range_disk),
    )


def stress_detection_precedence(kind):
    disk = Disk(kind, 256)
    part = make_parts([96])[0]

    if not format_fat32(disk, part):
        raise StressFailure(f"{kind}: FAT32 setup failed for precedence test")
    if detect_filesystem(disk, part) != "FAT32":
        raise StressFailure(f"{kind}: FAT32 did not win base detection")
    STATS["detection_precedence_checks"] += 1

    if not format_swap(disk, part):
        raise StressFailure(f"{kind}: SWAP setup failed for precedence test")
    if detect_filesystem(disk, part) != "SWAP":
        raise StressFailure(f"{kind}: SWAP did not win base detection")
    STATS["detection_precedence_checks"] += 1

    if not format_fat32(disk, part):
        raise StressFailure(f"{kind}: FAT32 reset failed for precedence test")
    swap_page = bytearray(part_read(disk, part, 0, 8))
    swap_page[SWAP_SIGNATURE_OFFSET:SWAP_SIGNATURE_OFFSET + 10] = b"SWAPSPACE2"
    part_write(disk, part, 0, swap_page)
    if detect_filesystem(disk, part) != "FAT32":
        raise StressFailure(f"{kind}: FAT32 did not outrank SWAP")
    STATS["detection_precedence_checks"] += 1

    if not format_ext4(disk, part):
        raise StressFailure(f"{kind}: EXT4 setup failed for precedence test")
    format_fat32(disk, part)
    if detect_filesystem(disk, part) != "EXT4":
        raise StressFailure(f"{kind}: EXT4 did not outrank FAT32")
    STATS["detection_precedence_checks"] += 1

    write_apfs_marker(disk, part)
    if detect_filesystem(disk, part) != "APFS":
        raise StressFailure(f"{kind}: APFS did not outrank EXT4/FAT32")
    STATS["detection_precedence_checks"] += 1

    write_partition_iso9660_marker(disk, part)
    if detect_filesystem(disk, part) != "ISO9660":
        raise StressFailure(f"{kind}: ISO9660 did not outrank APFS")
    STATS["detection_precedence_checks"] += 1


def stress_label_handling(kind):
    disk = Disk(kind, 256)
    long_label = "SystemPartitionLabelThatIsTooLongForLoader"
    weird_part = {
        "kind": "DATA",
        "label": long_label,
        "start": 2048,
        "sectors": 64 * SECTORS_PER_MIB,
    }
    write_gpt(disk, [weird_part])
    entries = bytearray(disk.read(2, GPT_ENTRY_SECTORS))
    decoded = decode_gpt_name(entries[56:56 + 72])
    if decoded != long_label[:31]:
        raise StressFailure(f"{kind}: GPT long label truncation mismatch")
    STATS["label_checks"] += 1

    for idx, ch in enumerate((ord("O"), 1, 0x263A, ord("K"))):
        put16(entries, 56 + idx * 2, ch)
    put16(entries, 56 + 8, 0)
    write_gpt_entries_with_valid_crcs(disk, entries)
    decoded = decode_gpt_name(bytearray(disk.read(2, GPT_ENTRY_SECTORS))[56:128])
    if decoded != "O??K":
        raise StressFailure(f"{kind}: GPT non-printable label sanitization mismatch")
    STATS["label_checks"] += 1

    part = {
        "kind": "DATA",
        "label": "mixedCaseLabel",
        "start": 2048,
        "sectors": 96 * SECTORS_PER_MIB,
    }
    if not format_fat32(disk, part):
        raise StressFailure(f"{kind}: FAT32 label setup failed")
    fat_label = trim_ascii_field(part_read(disk, part, 0, 1)[71:82])
    if fat_label != "MIXEDCASELA":
        raise StressFailure(f"{kind}: FAT32 uppercase/truncated label mismatch")
    STATS["label_checks"] += 1

    if not format_ext4(disk, part):
        raise StressFailure(f"{kind}: EXT4 label setup failed")
    ext_label = trim_ascii_field(
        part_read(disk, part, 0, EXT4_SECTORS_PER_BLOCK)[
            1024 + 120:1024 + 136
        ]
    )
    if ext_label != "MIXEDCASELABEL":
        raise StressFailure(f"{kind}: EXT4 uppercase label mismatch")
    STATS["label_checks"] += 1

    write_partition_iso9660_marker(disk, part)
    iso = bytearray(part_read(disk, part, 64, 4))
    iso[40:72] = b"ODD_ISO_LABEL\x00\x00" + b" " * 17
    part_write(disk, part, 64, iso)
    if trim_ascii_field(part_read(disk, part, 64, 4)[40:72]) != "ODD_ISO_LABEL":
        raise StressFailure(f"{kind}: ISO9660 label trimming mismatch")
    STATS["label_checks"] += 1


def stress_partition_management(kind):
    disk = Disk(kind, 384)
    parts = []

    for part_kind, size_mib in (
        ("EFI", 32),
        ("SYSTEM", 80),
        ("DATA", 96),
        ("SWAP", 16),
        ("DATA", 32),
        ("SYSTEM", 24),
        ("DATA", 16),
        ("SWAP", 8),
    ):
        snapshot = [dict(part) for part in parts]
        if not create_partition(parts, part_kind, size_mib, disk.mib):
            raise StressFailure(f"{kind}: failed to fill partition slot table")
        if len(parts) != len(snapshot) + 1:
            raise StressFailure(f"{kind}: create did not add exactly one partition")
        write_gpt(disk, parts)
        assert_layout_within_disk(kind, parts, disk)
        STATS["management_checks"] += 1

    if create_partition(parts, "DATA", 1, disk.mib):
        raise StressFailure(f"{kind}: accepted ninth partition slot")
    STATS["management_checks"] += 1

    before = [dict(part) for part in parts]
    if update_partition(parts, 2, "DATA", disk.mib, disk.mib):
        raise StressFailure(f"{kind}: accepted over-capacity partition update")
    if parts != before:
        raise StressFailure(f"{kind}: failed update mutated partition table")
    STATS["management_checks"] += 1

    if not update_partition(parts, 2, "SYSTEM", 48, disk.mib):
        raise StressFailure(f"{kind}: rejected valid partition update")
    if parts[2]["kind"] != "SYSTEM" or parts[2]["sectors"] != 48 * SECTORS_PER_MIB:
        raise StressFailure(f"{kind}: valid update did not apply expected fields")
    write_gpt(disk, parts)
    assert_layout_within_disk(kind, parts, disk)
    STATS["management_checks"] += 1

    if count_partitions_of_kind(parts, "SYSTEM") < 2:
        raise StressFailure(f"{kind}: partition kind accounting lost SYSTEM entries")
    STATS["management_checks"] += 1

    before = [dict(part) for part in parts]
    if delete_partition(parts, 99):
        raise StressFailure(f"{kind}: accepted invalid partition delete")
    if parts != before:
        raise StressFailure(f"{kind}: invalid delete mutated partition table")
    STATS["management_checks"] += 1

    if not delete_partition(parts, 0):
        raise StressFailure(f"{kind}: rejected valid partition delete")
    write_gpt(disk, parts)
    assert_layout_within_disk(kind, parts, disk)
    if parts[0]["start"] != 2048:
        raise StressFailure(f"{kind}: delete did not compact first partition")
    STATS["management_checks"] += 1


def stress_gpt_partition_churn(kind):
    disk = Disk(kind, 512)
    parts = []

    for part_kind, size_mib in (
        ("EFI", 32),
        ("SYSTEM", 96),
        ("DATA", 128),
        ("SWAP", 32),
    ):
        if not create_partition(parts, part_kind, size_mib, disk.mib):
            raise StressFailure(f"{kind}: failed to create {part_kind} partition")
        write_gpt(disk, parts)
        if detect_partition_table(disk) != "gpt":
            raise StressFailure(f"{kind}: GPT missing after create")
        assert_layout_within_disk(kind, parts, disk)

    if create_partition(parts, "DATA", 4096, disk.mib):
        raise StressFailure(f"{kind}: accepted over-capacity partition")
    STATS["churn_checks"] += 1

    if not update_partition(parts, 1, "DATA", 64):
        raise StressFailure(f"{kind}: failed to update partition")
    write_gpt(disk, parts)
    assert_layout_within_disk(kind, parts, disk)
    check_formatter(kind, "GPT-CHURN", parts[1], "EXT4")

    if not update_partition(parts, 2, "SYSTEM", 192):
        raise StressFailure(f"{kind}: failed to grow partition")
    write_gpt(disk, parts)
    assert_layout_within_disk(kind, parts, disk)
    check_formatter(kind, "GPT-CHURN", parts[2], "FAT32")

    if not delete_partition(parts, 0):
        raise StressFailure(f"{kind}: failed to delete partition")
    write_gpt(disk, parts)
    if detect_partition_table(disk) != "gpt":
        raise StressFailure(f"{kind}: GPT missing after delete")
    assert_layout_within_disk(kind, parts, disk)

    if not create_partition(parts, "EFI", 16, disk.mib):
        raise StressFailure(f"{kind}: failed to recreate partition")
    write_gpt(disk, parts)
    assert_layout_within_disk(kind, parts, disk)
    check_formatter(kind, "GPT-CHURN", parts[-1], "SWAP")

    if update_partition(parts, 42, "DATA", 1):
        raise StressFailure(f"{kind}: accepted invalid update index")
    STATS["churn_checks"] += 1
    if delete_partition(parts, 42):
        raise StressFailure(f"{kind}: accepted invalid delete index")
    STATS["churn_checks"] += 1


def stress_cdrom():
    STATS["drive_kinds"] += 1
    disk = Disk("CDROM", 64)
    write_cdrom_iso9660(disk)
    if not detect_cdrom_iso9660(disk):
        raise StressFailure("CDROM: ISO9660 marker was not detected")
    validate_cdrom_iso9660_metadata(disk)
    STATS["detect_only_checks"] += 1
    block = storage_read_block(disk, 16, 2048)
    if block[0] != 1 or block[1:6] != b"CD001":
        raise StressFailure("CDROM: 2048-byte block read did not expose ISO9660")
    STATS["block_api_checks"] += 1
    if not detect_cdrom_iso9660(disk):
        raise StressFailure("CDROM: ISO9660 lost precedence after block read")
    STATS["detection_precedence_checks"] += 1
    expect_stress_failure(
        "CDROM: 512-byte sector read",
        lambda: storage_read_block(disk, 0, SECTOR_SIZE),
    )
    expect_stress_failure(
        "CDROM: 512-byte sector write",
        lambda: storage_write_block(disk, 0, bytes(SECTOR_SIZE), SECTOR_SIZE),
    )
    expect_stress_failure(
        "CDROM: unsupported read block size",
        lambda: storage_read_block(disk, 0, 1024),
    )
    if storage_write_disk_image(disk, b"not for optical media"):
        raise StressFailure("CDROM: disk image write unexpectedly succeeded")
    STATS["block_api_checks"] += 1
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
        f"{STATS['corruption_checks']} corruption checks, "
        f"{STATS['churn_checks']} churn checks, "
        f"{STATS['gpt_crc_checks']} GPT CRC checks, "
        f"{STATS['fs_metadata_checks']} filesystem metadata checks, "
        f"{STATS['partition_range_checks']} partition range checks, "
        f"{STATS['block_api_checks']} block API checks, "
        f"{STATS['detection_precedence_checks']} detection precedence checks, "
        f"{STATS['label_checks']} label checks, "
        f"{STATS['management_checks']} management checks passed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
