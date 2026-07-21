#!/bin/sh
# Turn kernel addresses (a RIP from a fault report, a backtrace) into
# function names and source lines.
#
#   tools/symbolize.sh 0xffffffff80004007 0xffffffff80001234
#   tools/symbolize.sh -k build-test/kernel.elf 0xffffffff80004007
KERNEL=build/kernel.elf
if [ "$1" = "-k" ]; then KERNEL=$2; shift 2; fi
[ $# -gt 0 ] || { echo "usage: $0 [-k kernel.elf] ADDR..." >&2; exit 2; }
exec addr2line -e "$KERNEL" -f -p -i -a "$@"
