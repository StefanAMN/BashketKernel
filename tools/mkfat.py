#!/usr/bin/env python3
"""Build a FAT32 disk image from a directory tree.

    tools/mkfat.py OUTPUT.img SIZE_MIB SOURCE_DIR [EXTRA_FILE=PATH/IN/IMAGE ...]

Written for this project so that building the disk needs no extra packages
(mtools is often missing). The result is checked with fsck.fat by the
Makefile, which is an independent implementation of the format.

Limits, on purpose: 8.3 file names only (no long names), one sector per
cluster, files and directories laid out contiguously in the order found.
Names are stored upper case with the "lower case" flags Windows NT uses, so
that readme.txt is shown as readme.txt.
"""

import os
import struct
import sys

SECTOR = 512
RESERVED_SECTORS = 32
NUM_FATS = 2
END_OF_CHAIN = 0x0FFFFFFF
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
# A fixed timestamp (2026-01-01 00:00:00) keeps the image reproducible
FAT_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1
FAT_TIME = 0


def short_name(name):
    """'readme.txt' -> (b'README  TXT', nt_flags). Raises ValueError if it does not fit 8.3."""
    if name in (".", ".."):
        return name.encode().ljust(11), 0
    base, dot, ext = name.partition(".")
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^`{}")
    if not base or len(base) > 8 or len(ext) > 3 or "." in ext:
        raise ValueError(f"'{name}' is not an 8.3 name")
    for part in (base, ext):
        if any(c.upper() not in allowed for c in part):
            raise ValueError(f"'{name}' has a character not allowed in an 8.3 name")
        if part != part.lower() and part != part.upper():
            raise ValueError(f"'{name}' mixes upper and lower case, which needs a long name")
    flags = (0x08 if base.islower() else 0) | (0x10 if ext and ext.islower() else 0)
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode(), flags


def lfn_checksum(raw):
    """The checksum of an 11-byte short name that its long-name entries carry."""
    total = 0
    for byte in raw:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def alias_for(name, used):
    """An unused short name ('A LONG~1.TXT' style) for a name that needs long-name entries."""
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-!#$%&'()@^`{}")
    base, dot, ext = name.rpartition(".")
    if not dot or not base:
        base, ext = name, ""
    base = "".join(c for c in base.upper() if c in allowed) or "_"
    ext = "".join(c for c in ext.upper() if c in allowed)[:3]
    for n in range(1, 100000):
        tail = f"~{n}"
        raw = (base[:8 - len(tail)] + tail).ljust(8).encode() + ext.ljust(3).encode()
        if raw not in used:
            return raw
    raise ValueError(f"no free short name for '{name}'")


def dir_entries(name, attr, cluster, size, used):
    """The 32-byte entries for one name: long-name entries, if it needs them, then the short one.

    `used` is the set of short names taken in this directory; the one chosen is added to it.
    """
    flags = 0
    long_part = b""
    try:
        raw, flags = short_name(name)
    except ValueError:
        if len(name) > 127 or any(c in '"*/:<>?\\|' or not 32 <= ord(c) < 127 for c in name):
            raise
        raw = alias_for(name, used)
        # 13 UTF-16 characters per entry: the name, a zero, then 0xFFFF padding.
        # The entry holding the end of the name comes first and is flagged 0x40.
        units = [ord(c) for c in name] + [0]
        count = (len(name) + 12) // 13
        units += [0xFFFF] * (count * 13 - len(units))
        checksum = lfn_checksum(raw)
        for ordinal in range(count, 0, -1):
            part = units[(ordinal - 1) * 13:ordinal * 13]
            long_part += struct.pack("<B5HBBB6HH2H", ordinal | (0x40 if ordinal == count else 0), *part[0:5],
                                     0x0F, 0, checksum, *part[5:11], 0, *part[11:13])
    used.add(raw)
    return long_part + struct.pack("<11sBBBHHHHHHHI", raw, attr, flags, 0, FAT_TIME, FAT_DATE, FAT_DATE,
                                   cluster >> 16, FAT_TIME, FAT_DATE, cluster & 0xFFFF, size)


def dir_entry(name, attr, cluster, size):
    """The entries for one name in a directory that has no other names (see dir_entries)."""
    return dir_entries(name, attr, cluster, size, set())


class Image:
    def __init__(self, size_mib):
        """Lay out a FAT32 volume of `size_mib` MiB, empty; exit if that is too small for FAT32."""
        self.total_sectors = size_mib * 1024 * 1024 // SECTOR
        # Solve for the FAT size: each cluster (one sector) needs 4 bytes of FAT, in each copy
        data_and_fats = self.total_sectors - RESERVED_SECTORS
        self.fat_sectors = (data_and_fats * 4 + (SECTOR + NUM_FATS * 4) - 1) // (SECTOR + NUM_FATS * 4) + 1
        self.data_start = RESERVED_SECTORS + NUM_FATS * self.fat_sectors
        self.cluster_count = self.total_sectors - self.data_start
        if self.cluster_count < 65525:
            sys.exit("mkfat: the image is too small to be FAT32 (needs at least 65525 clusters; use 34 MiB or more)")
        self.fat = [0x0FFFFFF8, END_OF_CHAIN]    # Entries 0 and 1 are reserved
        self.clusters = {}                       # cluster number -> 512 bytes
        self.next_free = 2

    def allocate(self, data):
        """Store data in a new contiguous chain; returns its first cluster (0 for empty data)."""
        if not data:
            return 0
        count = (len(data) + SECTOR - 1) // SECTOR
        first = self.next_free
        if first + count > self.cluster_count + 2:
            sys.exit("mkfat: the files do not fit in the image")
        for i in range(count):
            self.clusters[first + i] = data[i * SECTOR:(i + 1) * SECTOR].ljust(SECTOR, b"\0")
            self.fat.append(first + i + 1 if i + 1 < count else END_OF_CHAIN)
        self.next_free += count
        return first

    def add_directory(self, entries, parent_cluster, is_root):
        """entries: list of (name, attr, cluster, size). Returns the directory's first cluster."""
        # The directory needs to know its own cluster for its "." entry, and
        # allocation is sequential, so that cluster is simply the next free one.
        own = self.next_free
        body = b""
        if not is_root:
            body += dir_entry(".", ATTR_DIRECTORY, own, 0)
            body += dir_entry("..", ATTR_DIRECTORY, parent_cluster, 0)  # 0 means "the root"
        used = set()
        for name, attr, cluster, size in entries:
            body += dir_entries(name, attr, cluster, size, used)
        body = body or b"\0" * SECTOR            # An empty root still occupies a cluster
        first = self.allocate(body)
        assert first == own
        return first

    def write(self, path, root_cluster):
        """Write the boot sector, FATs and data to `path`, with the root directory at `root_cluster`."""
        fat_bytes = b"".join(struct.pack("<I", e) for e in self.fat).ljust(self.fat_sectors * SECTOR, b"\0")
        free_clusters = self.cluster_count - (self.next_free - 2)

        boot = bytearray(SECTOR)
        boot[0:3] = b"\xEB\x58\x90"              # Jump over the parameter block (never executed here)
        boot[3:11] = b"BASHKET "
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, 1, RESERVED_SECTORS, NUM_FATS, 0, 0, 0xF8, 0,
                         63, 255, 0, self.total_sectors)
        struct.pack_into("<IHHIHH", boot, 36, self.fat_sectors, 0, 0, root_cluster, 1, 6)
        boot[64] = 0x80                          # Drive number
        boot[66] = 0x29                          # Extended boot signature: the next three fields exist
        struct.pack_into("<I", boot, 67, 0x0BA5CE70)  # Volume serial number
        boot[71:82] = b"NO NAME    "             # No volume label (there is no label entry in the root either)
        boot[82:90] = b"FAT32   "
        boot[510:512] = b"\x55\xAA"

        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, free_clusters, self.next_free)
        fsinfo[510:512] = b"\x55\xAA"

        with open(path, "wb") as out:
            out.truncate(self.total_sectors * SECTOR)
            for base in (0, 6):                  # The boot sector and its backup copy
                out.seek(base * SECTOR)
                out.write(boot)
                out.write(fsinfo)
            for copy in range(NUM_FATS):
                out.seek((RESERVED_SECTORS + copy * self.fat_sectors) * SECTOR)
                out.write(fat_bytes)
            for cluster, data in self.clusters.items():
                out.seek((self.data_start + cluster - 2) * SECTOR)
                out.write(data)


def add_tree(image, tree, parent_cluster, is_root):
    """tree: {name: bytes | dict}. Children are stored before their directory, because a
    directory's entries need the children's clusters."""
    entries = []
    for name in sorted(tree):
        node = tree[name]
        if isinstance(node, dict):
            # The child's ".." entry needs our cluster, which is not known
            # yet; it is patched in below.
            child = add_tree(image, node, None, False)
            entries.append((name, ATTR_DIRECTORY, child, 0))
        else:
            entries.append((name, ATTR_ARCHIVE, image.allocate(node), len(node)))

    own = image.add_directory(entries, parent_cluster or 0, is_root)

    # Patch ".." in each child directory to point at us (0 if we are the root)
    for name, attr, cluster, _ in entries:
        if attr == ATTR_DIRECTORY:
            data = bytearray(image.clusters[cluster])
            parent = 0 if is_root else own
            struct.pack_into("<H", data, 32 + 20, parent >> 16)
            struct.pack_into("<H", data, 32 + 26, parent & 0xFFFF)
            image.clusters[cluster] = bytes(data)
    return own


def read_tree(path):
    """A directory on the host as a dict: name to bytes, or to a dict for a subdirectory."""
    tree = {}
    for name in os.listdir(path):
        full = os.path.join(path, name)
        if os.path.isdir(full):
            tree[name] = read_tree(full)
        else:
            with open(full, "rb") as f:
                tree[name] = f.read()
    return tree


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    output, size_mib, source = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    tree = read_tree(source)

    # EXTRA_FILE=path/in/image: files that are not in the source directory (built programs)
    for extra in sys.argv[4:]:
        host_path, _, image_path = extra.partition("=")
        parts = image_path.strip("/").split("/")
        node = tree
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        with open(host_path, "rb") as f:
            node[parts[-1]] = f.read()

    image = Image(size_mib)
    try:
        root = add_tree(image, tree, None, True)
    except ValueError as e:
        sys.exit(f"mkfat: {e}")
    image.write(output, root)


if __name__ == "__main__":
    main()
