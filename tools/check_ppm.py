#!/usr/bin/env python3
"""Check pixels of a screenshot taken with QEMU's screendump (a binary PPM).

    tools/check_ppm.py FILE X,Y=RRGGBB ... X,Y!=RRGGBB ...

Each argument names a pixel and the colour it must ('=') or must not ('!=')
have. Exit status 0 if all hold; otherwise each failure is printed.
"""

import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    magic, size, depth, pixels = data.split(b"\n", 3)
    if magic != b"P6" or depth != b"255":
        sys.exit(f"check_ppm: {sys.argv[1]} is not a binary 8-bit PPM")
    width, height = map(int, size.split())

    failures = 0
    for check in sys.argv[2:]:
        wanted_equal = "!=" not in check
        where, colour = check.split("=" if wanted_equal else "!=")
        x, y = map(int, where.split(","))
        if not (0 <= x < width and 0 <= y < height):
            sys.exit(f"check_ppm: {x},{y} is outside the {width}x{height} picture")
        found = pixels[(y * width + x) * 3:(y * width + x) * 3 + 3].hex()
        if (found == colour.lower()) != wanted_equal:
            print(f"check_ppm: pixel {x},{y} is {found}, expected {'' if wanted_equal else 'anything but '}{colour.lower()}")
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
