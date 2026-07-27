#!/usr/bin/env python3
import struct
import sys

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


class StressFailure(Exception):
    pass


class Disk:
    def __init__(self, kind, mib):
        self.kind = kind
        self.mib = mib
        self.sectors = mib * SECTORS_PER_MIB
        self.data = bytearray(self.sectors * SECTOR_SIZE)

    @property
    def writable(self):
        return self.kind in WRITABLE_KINDS

    def read(self, lba, count=1):
        if count <= 0 or lba < 0 or lba + count > self.sectors:
            raise StressFailure(f"{self.kind}: invalid read {lba}+{count}")
        start = lba * SECTOR_SIZE
        end = start + count * SECTOR_SIZE
        return bytes(self.data[start:end])

    def write(self, lba, payload):
        if not self.writable:
            raise StressFailure(f"{self.kind}: write accepted on read-only disk")
        if len(payload) % SECTOR_SIZE != 0:
            raise StressFailure(f"{self.kind}: unaligned write length {len(payload)}")
        count = len(payload) // SECTOR_SIZE
        if count <= 0 or lba < 0 or lba + count > self.sectors:
            raise StressFailure(f"{self.kind}: invalid write {lba}+{count}")
        start = lba * SECTOR_SIZE
        self.data[start:start + len(payload)] = payload


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
    }.get(kind, bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7"))


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
    start, end = part_slice(disk, part, rel_lba, len(payload) // SECTOR_SIZE)
    disk.data[start:end] = payload


def part_read(disk, part, rel_lba, count):
    start, end = part_slice(disk, part, rel_lba, count)
    return bytes(disk.data[start:end])


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


def write_iso9660(disk):
    if disk.kind != "CDROM":
        raise StressFailure("ISO9660 image was written to non-optical disk")
    block = bytearray(2048)
    block[0] = 1
    block[1:6] = b"CD001"
    block[40:72] = ascii_padded("OS8_STRESS_ISO", 32)
    start = 16 * 2048
    disk.data[start:start + 2048] = block


def detect_iso9660(disk):
    block = bytes(disk.data[16 * 2048:17 * 2048])
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


def stress_writable_kind(kind):
    disk = Disk(kind, 256)
    parts = make_parts([40, 64, 80, 16])
    write_gpt(disk, parts)
    if detect_partition_table(disk) != "gpt":
        raise StressFailure(f"{kind}: GPT was not detected")

    for part in parts:
        if not format_fat32(disk, part):
            if part["sectors"] >= 65536:
                raise StressFailure(f"{kind}: FAT32 rejected valid partition")
        elif not detect_fat32(disk, part):
            raise StressFailure(f"{kind}: FAT32 format did not self-detect")

        if not format_ext4(disk, part):
            raise StressFailure(f"{kind}: EXT4 rejected valid partition")
        if not detect_ext4(disk, part):
            raise StressFailure(f"{kind}: EXT4 format did not self-detect")

        if not format_swap(disk, part):
            raise StressFailure(f"{kind}: SWAP rejected valid partition")
        if not detect_swap(disk, part):
            raise StressFailure(f"{kind}: SWAP format did not self-detect")

    small = make_parts([1])[0]
    if format_fat32(disk, small):
        raise StressFailure(f"{kind}: FAT32 accepted undersized partition")
    if format_ext4(disk, {"start": small["start"], "sectors": 128,
                          "label": "tiny"}):
        raise StressFailure(f"{kind}: EXT4 accepted undersized partition")
    if format_swap(disk, {"start": small["start"], "sectors": 4,
                          "label": "tiny"}):
        raise StressFailure(f"{kind}: SWAP accepted undersized partition")

    mbr_disk = Disk(kind, 128)
    mbr_parts = make_parts([40, 32, 8, 8])
    write_mbr(mbr_disk, mbr_parts)
    if detect_partition_table(mbr_disk) != "mbr":
        raise StressFailure(f"{kind}: MBR was not detected")

    apfs_part = mbr_parts[0]
    write_apfs_marker(mbr_disk, apfs_part)
    if not detect_apfs(mbr_disk, apfs_part):
        raise StressFailure(f"{kind}: APFS marker was not detected")

    for fs in DETECT_ONLY_FILESYSTEMS:
        if fs in FORMATTERS:
            raise StressFailure(f"{kind}: {fs} unexpectedly has a formatter")


def stress_cdrom():
    disk = Disk("CDROM", 64)
    write_iso9660(disk)
    if not detect_iso9660(disk):
        raise StressFailure("CDROM: ISO9660 marker was not detected")
    try:
        disk.write(0, bytes(SECTOR_SIZE))
    except StressFailure:
        pass
    else:
        raise StressFailure("CDROM: raw sector write unexpectedly succeeded")


def main():
    failures = []
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
        f"{len(WRITABLE_KINDS)} writable drive kinds, "
        f"{len(FORMATTERS)} formatters, "
        "GPT/MBR, ISO9660, APFS, and CDROM read-only checks passed"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
