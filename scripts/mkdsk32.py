#!/usr/bin/env python3
"""
Build a real, rootless FAT32 test disk for CFOS QEMU runs.

Replaces the vvfat (FAT16-only) setup: creates a raw image with an MBR
partition table and a proper FAT32 filesystem, then copies the contents
of a directory tree into it. All names used by the repo (EFI/, BOOT,
BOOTX64.EFI, KERNEL.ELF, USERS/, HELLO.ELF ...) fit into 8.3 short
names, so no LFN support is needed.

Kernel fat32_mount constraints are honored:
  bytes/sector=512, fat_size_32 != 0, root_cluster >= 2,
  cluster_count >= 65525.

Usage: mkdsk32.py <image> <source-dir> [size_mb]
"""
import os
import struct
import sys

SECTOR = 512
PART_START_SECTOR = 2048   # 1 MiB
ROOT_CLUSTER = 2
EOC = 0x0FFFFFFF


def round_up(x, a):
    return (x + a - 1) // a * a


def dos_name_ok(name):
    base, _, ext = name.partition(".")
    if not base or len(base) > 8 or len(ext) > 3:
        return False
    return (base.upper() + ("." + ext.upper() if ext else "")) == name \
        and base.isascii() and ext.isascii()


def sfn_encode(name):
    base, _, ext = name.partition(".")
    return (base.upper().ljust(8)[:8] + ext.upper().ljust(3)[:3]).encode("ascii")


def sfn_for(name):
    """Return (sfn11_bytes, needs_lfn) for a directory entry."""
    if dos_name_ok(name):
        return sfn_encode(name), False
    # 8.3 alias: first 6 base chars + ~1 (+ extension), the DOS way
    base, _, ext = name.partition(".")
    alias = (base.upper()[:6] + "~1", "." + ext.upper()[:3] if ext else "")
    sfn = (alias[0].ljust(8)[:8] + alias[1].ljust(3)[:3]).encode("ascii")
    return sfn, True


def sfn_checksum(sfn):
    s = 0
    for b in sfn:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s & 0xFF


def encode_lfn(long_name, sfn, seq):
    """One LFN slot (13 UTF-16 chars). Returns raw 32-byte entry."""
    payload = long_name.upper().encode("utf-16-le") + b"\x00\x00"
    payload += b"\xff\xff" * (13 - len(payload) // 2)
    assert len(payload) == 26
    raw = bytearray(32)
    raw[0] = seq                             # 0x01 = first slot, 0x40 = last
    raw[1:11] = payload[0:10]                # name chars 1-5
    raw[11] = 0x0F                           # ATTR_VOLUME_ID (LFN)
    raw[12] = 0x00                           # type
    raw[13] = sfn_checksum(sfn)
    struct.pack_into("<H", raw, 14, 0)       # reserved
    raw[16:26] = payload[10:20]              # name chars 6-11
    struct.pack_into("<H", raw, 26, 0)       # first cluster = 0
    raw[28:32] = payload[20:24]              # name chars 12-13
    return bytes(raw)


def entry_slots(name, is_dir, first_cluster, size):
    """All raw direntry slots for one child: optional LFN chain + SFN."""
    sfn, needs_lfn = sfn_for(name)
    out = []
    if needs_lfn:
        out.append(encode_lfn(name, sfn, 0x01 | 0x40))   # single LFN slot
    raw = bytearray(32)
    raw[0:11] = sfn
    raw[11] = 0x10 if is_dir else 0x20
    struct.pack_into("<H", raw, 26, first_cluster & 0xFFFF)
    struct.pack_into("<H", raw, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into("<I", raw, 28, size)
    out.append(bytes(raw))
    return out


def encode_entry(name, is_dir, first_cluster, size, dot=None):
    raw = bytearray(32)
    if dot == ".":
        raw[0:11] = b".          "
    elif dot == "..":
        raw[0:11] = b"..         "
    else:
        raw[0:11] = sfn_for(name)[0]
    raw[11] = 0x10 if (is_dir or dot) else 0x20
    struct.pack_into("<H", raw, 26, first_cluster & 0xFFFF)
    struct.pack_into("<H", raw, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into("<I", raw, 28, size)
    return bytes(raw)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    image, src = sys.argv[1], sys.argv[2]
    size_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 256

    spc = 4                      # sectors per cluster (2 KiB clusters)
    reserved = 32
    nfats = 2
    fat_sectors = 1024

    total_sectors = size_mb * 1024 * 1024 // SECTOR
    data_start = PART_START_SECTOR + reserved + nfats * fat_sectors
    cluster_count = (total_sectors - PART_START_SECTOR -
                     reserved - nfats * fat_sectors) // spc
    assert cluster_count > 65525, "image too small for kernel FAT32 check"
    cluster_bytes = spc * SECTOR
    next_free = ROOT_CLUSTER + 1   # root dir starts at cluster 2

    fatbuf = bytearray(4 * (cluster_count + 8))

    f = open(image, "r+b" if os.path.exists(image) else "wb")
    f.truncate(total_sectors * SECTOR)

    def write_at(sector, data):
        f.seek(sector * SECTOR)
        f.write(data)

    def cluster_lba(c):
        return data_start + (c - ROOT_CLUSTER) * spc

    # ---- pass 1: enumerate tree --------------------------------------
    tree = {}
    def walk(d):
        entries = []
        for name in sorted(os.listdir(d)):
            full = os.path.join(d, name)
            if os.path.isdir(full):
                tree[full] = walk(full)
                entries.append((name, True, full))
            elif os.path.isfile(full):
                entries.append((name, False, full))
        tree[d] = entries
        return entries
    tree[src] = walk(src)

    alloc = {}   # path -> (first_cluster, n_clusters, size)

    # ---- pass 2: allocate clusters top-down ---------------------------
    def slots_in_dir(d):
        n = 0
        for name, is_dir, full in tree[d]:
            _, needs_lfn = sfn_for(name)
            n += 2 if needs_lfn else 1
        if d != src:
            n += 2                          # '.' and '..'
        return n

    def alloc_dirs(d):
        nonlocal next_free
        n_entries = slots_in_dir(d)
        need = max(1, round_up(n_entries * 32, cluster_bytes) // cluster_bytes)
        if d == src:
            # root directory always lives in cluster 2
            alloc[d] = (ROOT_CLUSTER, need, n_entries * 32)
        else:
            first = next_free
            next_free += need
            assert next_free - ROOT_CLUSTER <= cluster_count, "image too small"
            alloc[d] = (first, need, n_entries * 32)
        for name, is_dir, full in tree[d]:
            if is_dir:
                alloc_dirs(full)
    alloc_dirs(src)

    for d in tree:
        for name, is_dir, full in tree[d]:
            if not is_dir:
                size = os.path.getsize(full)
                need = max(1, round_up(size, cluster_bytes) // cluster_bytes)
                first = next_free
                next_free += need
                assert next_free - ROOT_CLUSTER <= cluster_count, \
                    "image too small (files too big?)"
                alloc[full] = (first, need, size)

    # ---- pass 3: FAT chains -------------------------------------------
    def chain(first, n_clusters):
        for i in range(first, first + n_clusters - 1):
            struct.pack_into("<I", fatbuf, i * 4, i + 1)
        struct.pack_into("<I", fatbuf, (first + n_clusters - 1) * 4, EOC)

    chain(ROOT_CLUSTER, max(1, round_up(
        alloc[src][2], cluster_bytes) // cluster_bytes))

    for d in tree:
        chain(*alloc[d][:2])
        for name, is_dir, full in tree[d]:
            if not is_dir:
                chain(*alloc[full][:2])

    struct.pack_into("<I", fatbuf, 0, 0x0FFFFFF8)
    struct.pack_into("<I", fatbuf, 4, EOC)

    for i in range(nfats):
        lba = PART_START_SECTOR + reserved + i * fat_sectors
        f.seek(lba * SECTOR)
        f.write(bytes(fatbuf[:fat_sectors * SECTOR]))

    # ---- pass 4: write directory streams and file contents ------------
    for d in tree:
        first, n_clusters, stream_size = alloc[d]
        parts = []
        if d != src:
            parent = os.path.dirname(d) or src
            # fsck.fat expects '..' start = 0 when the parent is the root dir
            dotdot_cluster = 0 if parent == src else alloc[parent][0]
            parts.append(encode_entry(None, True, first, 0, dot="."))
            parts.append(encode_entry(None, True, dotdot_cluster, 0,
                                      dot=".."))
        for name, is_dir, full in tree[d]:
            parts.extend(entry_slots(name, is_dir, alloc[full][0],
                                     0 if is_dir else alloc[full][2]))
        data = b"".join(parts)
        data += b"\x00" * (n_clusters * cluster_bytes - len(data))
        f.seek(cluster_lba(first) * SECTOR)
        f.write(data)

        for name, is_dir, full in tree[d]:
            if is_dir:
                pass
            else:
                first_f, n_clusters_f, size = alloc[full]
                with open(full, "rb") as fh:
                    blob = fh.read()
                blob += b"\x00" * (n_clusters_f * cluster_bytes - len(blob))
                f.seek(cluster_lba(first_f) * SECTOR)
                f.write(blob)

    # ---- boot sector (in-partition, at PART_START_SECTOR) --------------
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x58\x90"
    bs[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = spc
    struct.pack_into("<H", bs, 14, reserved)
    bs[16] = nfats
    struct.pack_into("<H", bs, 17, 0)        # root entries
    struct.pack_into("<H", bs, 19, 0)        # total16
    bs[21] = 0xF8                            # media
    struct.pack_into("<H", bs, 22, 0)        # fat_size_16
    struct.pack_into("<H", bs, 24, 63)       # spt
    struct.pack_into("<H", bs, 26, 255)      # heads
    struct.pack_into("<I", bs, 28, PART_START_SECTOR)
    struct.pack_into("<I", bs, 32, total_sectors - PART_START_SECTOR)
    struct.pack_into("<I", bs, 36, fat_sectors)   # fat_size_32
    struct.pack_into("<H", bs, 40, 0)
    struct.pack_into("<H", bs, 42, 0)
    struct.pack_into("<I", bs, 44, ROOT_CLUSTER)
    struct.pack_into("<H", bs, 48, 1)        # fsinfo
    struct.pack_into("<H", bs, 50, 3)        # backup boot
    bs[64] = 0x80                            # drive number (extended boot)
    bs[65] = 0x00                            # boot flags (not dirty)
    bs[66] = 0x29                            # extended boot signature
    struct.pack_into("<I", bs, 67, 0x20260615)   # volume id
    bs[71:82] = b"NO NAME    "
    bs[82:90] = b"FAT32   "
    bs[510] = 0x55
    bs[511] = 0xAA
    write_at(PART_START_SECTOR, bytes(bs))
    write_at(PART_START_SECTOR + 3, bytes(bs))   # backup

    # FSInfo sector (main + backup)
    used_clusters = next_free - 2                 # root dir + children
    free_clusters = cluster_count - used_clusters

    def fsinfo():
        fs = bytearray(512)
        struct.pack_into("<I", fs, 0, 0x41615252)
        struct.pack_into("<I", fs, 484, 0x61417272)
        struct.pack_into("<I", fs, 488, free_clusters)
        struct.pack_into("<I", fs, 492, next_free)
        fs[510] = 0x55
        fs[511] = 0xAA
        return bytes(fs)
    write_at(PART_START_SECTOR + 1, fsinfo())
    write_at(PART_START_SECTOR + 4, fsinfo())

    # ---- MBR -----------------------------------------------------------
    # Partition entry #1 layout: status(446), chs_start(447-449),
    # type(450), chs_end(451-453), lba_start(454-457), sectors(458-461)
    mbr = bytearray(SECTOR)
    mbr[446] = 0x80                          # bootable
    mbr[447] = 0x20                          # start CHS (unused w/ LBA)
    mbr[448] = 0x21
    mbr[449] = 0x00
    mbr[450] = 0x0C                          # FAT32 LBA
    mbr[451] = 0xFF                          # end CHS (unused w/ LBA)
    mbr[452] = 0xFF
    mbr[453] = 0xFF
    struct.pack_into("<I", mbr, 454, PART_START_SECTOR)
    struct.pack_into("<I", mbr, 458, total_sectors - PART_START_SECTOR)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    write_at(0, bytes(mbr))

    f.close()
    n_files = sum(1 for d in tree for _, isd, _ in tree[d] if not isd)
    print(f"mkdsk32: {image}: {size_mb} MiB, {cluster_count} clusters, "
          f"{len(tree)} dirs, {n_files} files")


if __name__ == "__main__":
    main()
