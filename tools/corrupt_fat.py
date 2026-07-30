#!/usr/bin/env python3
"""Make a damaged copy of a FAT32 image, for testing that the kernel copes.

    tools/corrupt_fat.py GOOD.img KIND OUT.img

KIND is one of:
  file-loop    the cluster chain of pattern.bin points back at its start
  dir-loop     the root directory's cluster chain points at itself
  dir-cycle    the directory notes/ claims the root directory as its contents
  fat-garbage  the start of the file allocation table is random bytes
  huge-size    readme.txt claims to be 4 GiB long
  bad-cluster  readme.txt starts at a cluster beyond the end of the volume

The image must be one built by tools/mkfat.py from the disk/ directory.
"""
import random
import shutil
import struct
import sys

KINDS = ("file-loop", "dir-loop", "dir-cycle", "fat-garbage", "huge-size", "bad-cluster")
if len(sys.argv) != 4 or sys.argv[2] not in KINDS:
    sys.exit(__doc__)
src, kind, out = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.copy(src, out)
f = open(out, 'r+b')
boot = f.read(512)
reserved = struct.unpack_from('<H', boot, 14)[0]
fatsz = struct.unpack_from('<I', boot, 36)[0]
root = struct.unpack_from('<I', boot, 44)[0]
data_start = reserved + 2 * fatsz
def fat_set(cluster, value):
    for copy in range(2):
        f.seek((reserved + copy * fatsz) * 512 + cluster * 4); f.write(struct.pack('<I', value))
def fat_get(cluster):
    f.seek(reserved * 512 + cluster * 4); return struct.unpack('<I', f.read(4))[0]
def find(dir_cluster, name11):
    f.seek((data_start + dir_cluster - 2) * 512); d = f.read(512)
    for i in range(0, 512, 32):
        if d[i:i+11] == name11:
            return (data_start + dir_cluster - 2) * 512 + i, (struct.unpack_from('<H', d, i+20)[0] << 16) | struct.unpack_from('<H', d, i+26)[0]
    raise SystemExit('entry not found: %r' % name11)
if kind == 'file-loop':          # pattern.bin's chain points back at its own start
    _, first = find(root, b'PATTERN BIN'); fat_set(first + 5, first)
elif kind == 'dir-loop':         # the root directory's chain loops
    fat_set(root, root)
elif kind == 'dir-cycle':        # notes/ claims the root directory as its contents
    pos, _ = find(root, b'NOTES      '); f.seek(pos + 20); f.write(struct.pack('<H', root >> 16)); f.seek(pos + 26); f.write(struct.pack('<H', root & 0xFFFF))
elif kind == 'fat-garbage':      # the whole first FAT sector range is random
    random.seed(1); f.seek(reserved * 512); f.write(bytes(random.getrandbits(8) for _ in range(8 * 512)))
elif kind == 'huge-size':        # readme.txt claims to be 4 GiB long
    pos, _ = find(root, b'README  TXT'); f.seek(pos + 28); f.write(struct.pack('<I', 0xFFFFFFFF))
elif kind == 'bad-cluster':      # readme.txt starts at a cluster beyond the volume
    pos, _ = find(root, b'README  TXT'); f.seek(pos + 20); f.write(struct.pack('<H', 0x7FFF))
f.close()
