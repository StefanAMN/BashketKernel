#!/usr/bin/env python3
"""Print a line for /etc/passwd (user/rootfs/etc/passwd).

    tools/mkpasswd.py NAME UID GID PASSWORD

The format is the one user/lib/password.c reads:

    name:uid:gid:iterations:salt:hash

with hash = PBKDF2-HMAC-SHA256(password, salt, iterations), from Python's
own implementation. That the kernel's /bin/login accepts the passwords in
lines made here is what shows its PBKDF2 agrees with someone else's.
"""

import hashlib
import os
import sys

ITERATIONS = 4096


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    name, uid, gid, password = sys.argv[1:]
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, ITERATIONS)
    print(f"{name}:{int(uid)}:{int(gid)}:{ITERATIONS}:{salt.hex()}:{digest.hex()}")


if __name__ == "__main__":
    main()
