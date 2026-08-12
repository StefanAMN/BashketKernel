#!/usr/bin/env python3
"""Damage an ext2 image in a chosen way, to see that the kernel survives it.

    tools/corrupt_ext2.py SOURCE KIND OUTPUT

The image is the one tools/mkroot.sh builds (1 KiB blocks).
Kinds:

  dir-reclen     the first entry of /home/alice has a record length of 0
  dir-overrun    ...or one that runs past the end of the block
  block-outside  a file's first block number is beyond the end of the volume
  indirect-loop  a file's table of block numbers names itself, for a file
                 that claims to be large
  huge-size      a file claims 3 MiB with one block behind it (the rest reads
                 as zeros: such a file is legal, and must not be a problem)
  inode-table    the group's inode table is said to be outside the volume
                 (the volume must be refused altogether)
  bitmap-full    the group claims free blocks but its bitmap has none
  root-not-dir   the root inode says it is a regular file
"""

import struct
import sys

BLOCK = 1024


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, kind, output = sys.argv[1:]
    image = bytearray(open(source, "rb").read())

    sb = 1024
    inodes_per_group, = struct.unpack_from("<I", image, sb + 40)
    inode_size, = struct.unpack_from("<H", image, sb + 88)
    blocks_count, = struct.unpack_from("<I", image, sb + 4)
    group = 2 * BLOCK                                   # The group descriptors follow the superblock's block
    block_bitmap, inode_bitmap, inode_table = struct.unpack_from("<III", image, group)

    def inode_offset(ino):
        # Inodes are numbered across the block groups: inodes_per_group in
        # each, in that group's own table (its descriptor says where)
        g, index = divmod(ino - 1, inodes_per_group)
        table, = struct.unpack_from("<I", image, group + g * 32 + 8)
        return table * BLOCK + index * inode_size

    def first_block(ino):
        return struct.unpack_from("<I", image, inode_offset(ino) + 40)[0]

    def lookup(dir_ino, name):
        """The inode number of `name` in a directory (its first block only)."""
        base = first_block(dir_ino) * BLOCK
        offset = 0
        while offset < BLOCK:
            ino, length, name_len = struct.unpack_from("<IHB", image, base + offset)
            if ino and image[base + offset + 8:base + offset + 8 + name_len] == name.encode():
                return ino
            offset += length
        sys.exit(f"corrupt_ext2: no '{name}' in the image")

    alice = lookup(lookup(2, "home"), "alice")
    notes = lookup(alice, "notes.txt")

    if kind == "dir-reclen":
        struct.pack_into("<H", image, first_block(alice) * BLOCK + 4, 0)
    elif kind == "dir-overrun":
        struct.pack_into("<H", image, first_block(alice) * BLOCK + 4, 4000)
    elif kind == "block-outside":
        struct.pack_into("<I", image, inode_offset(notes) + 40, blocks_count + 1000)
    elif kind == "indirect-loop":
        table = first_block(notes)                      # Its data block now serves as its table of numbers
        struct.pack_into("<I", image, inode_offset(notes) + 40 + 12 * 4, table)
        struct.pack_into("<I", image, inode_offset(notes) + 40 + 13 * 4, table)
        for i in range(BLOCK // 4):
            struct.pack_into("<I", image, table * BLOCK + i * 4, table)
        struct.pack_into("<I", image, inode_offset(notes) + 4, 2 * 1024 * 1024)
    elif kind == "huge-size":
        struct.pack_into("<I", image, inode_offset(notes) + 4, 3 * 1024 * 1024)
    elif kind == "inode-table":
        struct.pack_into("<I", image, group + 8, blocks_count + 5)
    elif kind == "bitmap-full":
        image[block_bitmap * BLOCK:(block_bitmap + 1) * BLOCK] = b"\xff" * BLOCK
    elif kind == "root-not-dir":
        mode, = struct.unpack_from("<H", image, inode_offset(2))
        struct.pack_into("<H", image, inode_offset(2), (mode & 0x0FFF) | 0x8000)
    else:
        sys.exit(f"corrupt_ext2: unknown kind '{kind}'\n{__doc__}")

    open(output, "wb").write(image)


if __name__ == "__main__":
    main()
