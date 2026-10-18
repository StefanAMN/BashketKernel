#!/bin/sh
# Build the system disk: an ext2 image that the kernel mounts as "/".
#
#   tools/mkroot.sh IMAGE SIZE SYSTEM_DIRECTORY HOME_DIRECTORY
#
# SYSTEM_DIRECTORY holds what goes in the root (bin/ with the programs, etc/);
# HOME_DIRECTORY the users' files, which go under /home. Empty directories
# are added for what the kernel puts on top at boot (/dev, /proc, /tmp, the
# FAT volume at /disk) and for root's own home (/root).
#
# mke2fs lays the filesystem out and copies the files in, with the
# permission bits they have when staged here; debugfs then sets owners and
# groups, which mke2fs takes from whoever ran it. The features left out are
# the ones the kernel's driver does not implement. Both tools are from
# e2fsprogs; e2fsck checks the result.
set -e
image=$1; size=$2; system=$3; homes=$4
[ -n "$image" ] && [ -n "$size" ] && [ -d "$system" ] && [ -d "$homes" ] ||
    { echo "usage: $0 IMAGE SIZE SYSTEM_DIRECTORY HOME_DIRECTORY" >&2; exit 2; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp -a "$system/." "$stage/"
mkdir -p "$stage/home" "$stage/root" "$stage/tmp" "$stage/dev" "$stage/proc" "$stage/disk"
cp -a "$homes/." "$stage/home/"
# Permission bits: what a checkout happens to have (the builder's umask) is
# not what the system should. Directories 0755, files 0644, programs 0755;
# then the exceptions.
chmod -R u=rwX,go=rX "$stage"
chmod 4755 "$stage/bin/su" "$stage/bin/passwd"   # Set-uid root: they change who you are, or /etc/passwd
chmod 0600 "$stage/etc/passwd"                   # Password hashes: for /bin/login (root) only
chmod 0700 "$stage/root"
chmod 0600 "$stage/home/alice/diary.txt"         # Something only alice may read (the tests rely on it)

rm -f "$image"
mke2fs -q -t ext2 -O ^resize_inode,^dir_index,^ext_attr -b 1024 -L system -d "$stage" "$image" "$size"

# Everything belongs to root, except what is in a user's home directory:
# that is the user's (uid and gid from /etc/passwd)
{
    (cd "$stage" && find . -mindepth 0) | while read -r path; do
        name=${path#.}; [ -z "$name" ] && name=/
        echo "sif \"$name\" uid 0"
        echo "sif \"$name\" gid 0"
    done
    while IFS=: read -r user uid gid _; do       # name:uid:gid:iterations:salt:hash
        [ -d "$stage/home/$user" ] || continue
        (cd "$stage" && find "./home/$user") | while read -r path; do
            echo "sif \"${path#.}\" uid $uid"
            echo "sif \"${path#.}\" gid $gid"
        done
    done < "$stage/etc/passwd"
} | debugfs -w "$image" -f - >/dev/null 2>&1

e2fsck -fn "$image" >/dev/null || { echo "e2fsck rejects $image" >&2; rm -f "$image"; exit 1; }
