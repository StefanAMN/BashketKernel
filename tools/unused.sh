#!/bin/sh
# List kernel functions that nothing calls.
#
#   tools/unused.sh            the normal kernel
#   tools/unused.sh tests      the test kernel (functions only tests call are then "used")
#
# Builds the kernel once more with every function in a section of its own
# (-ffunction-sections) and has the linker throw away the sections nothing
# refers to (--gc-sections), printing each one. That is the linker's view,
# not a guess from the text: a function only reached through a pointer still
# counts as used. Output: "file: function function ..." per object file.
# One thing it cannot tell apart: a function the compiler copied into every
# caller (inlined) also leaves an unused out-of-line copy, and is listed.
# Check a name with grep before deleting it.
set -e
cd "$(dirname "$0")/.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CFLAGS=$(make -s -p 2>/dev/null | sed -n 's/^CFLAGS = //p' | head -1)
[ "$1" = tests ] && CFLAGS="$CFLAGS -DKERNEL_TESTS" && EXTRA="SRC_DIRS=src lib tests"
make -s -j8 BUILD="$OUT" ${EXTRA:+"$EXTRA"} "$OUT/kernel.elf" \
    CFLAGS="$CFLAGS -ffunction-sections -fdata-sections" \
    LDFLAGS="-nostdlib -T linker.ld -z max-page-size=0x1000 -z text --gc-sections --print-gc-sections" 2>&1 |
    sed -n "s/.*removing unused section '\.text\.\([^']*\)' in file '$(echo "$OUT" | sed 's/[\/&]/\\&/g')\/obj\/\([^']*\)\.o'.*/\2 \1/p" |
    grep -v '\.\(part\|isra\|constprop\|cold\)\b' |
    awk '{ f[$1] = f[$1] " " $2 } END { for (k in f) print k ":" f[k] }' | sort
