#!/bin/sh
# Build the ext2 image that the kernel mounts at /home (the second disk).
#
#   tools/mkhome.sh IMAGE SIZE SOURCE_DIRECTORY
#
# mke2fs lays the filesystem out and copies the files in; debugfs then sets
# the owners, groups and modes, which mke2fs takes from whoever ran it. The
# features left out are the ones the kernel's driver does not implement.
# Both tools are from e2fsprogs; e2fsck checks the result.
set -e
image=$1; size=$2; source=$3
[ -n "$image" ] && [ -n "$size" ] && [ -d "$source" ] || { echo "usage: $0 IMAGE SIZE SOURCE_DIRECTORY" >&2; exit 2; }

rm -f "$image"
mke2fs -q -t ext2 -O ^resize_inode,^dir_index,^ext_attr -b 1024 -L home -d "$source" "$image" "$size"

# path  uid  gid  mode (with the type: 04 directory, 10 regular file)
debugfs -w "$image" -f - >/dev/null 2>&1 <<COMMANDS
sif / uid 0
sif / gid 0
sif / mode 040755
sif /readme.txt uid 0
sif /readme.txt gid 0
sif /readme.txt mode 0100644
sif /alice uid 1000
sif /alice gid 100
sif /alice mode 040755
sif /alice/notes.txt uid 1000
sif /alice/notes.txt gid 100
sif /alice/notes.txt mode 0100644
sif /alice/diary.txt uid 1000
sif /alice/diary.txt gid 100
sif /alice/diary.txt mode 0100600
sif /bob uid 1001
sif /bob gid 100
sif /bob mode 040755
sif /bob/hello.txt uid 1001
sif /bob/hello.txt gid 100
sif /bob/hello.txt mode 0100644
COMMANDS

e2fsck -fn "$image" >/dev/null || { echo "e2fsck rejects $image" >&2; rm -f "$image"; exit 1; }
