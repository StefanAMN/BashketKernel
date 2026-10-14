#!/usr/bin/env python3
"""Pull the plug on the kernel while it is writing to the ext2 volume, and see
that the volume is whole afterwards.

    tools/crash_test.py [--rounds N] [--seed S] [extra qemu_shell.py arguments]

Each round:

  1. Boot, start /bin/fsstress in /home/alice (creating, growing, renaming
     and removing files and directories without pause), and after a random
     time kill QEMU outright: whatever write was in progress stays half done.
  2. Boot again from the same disk. The kernel checks and repairs the volume
     when it mounts it; it must come up, and still be able to write.
  3. Run e2fsck on the image. It must find nothing wrong.

The same image goes through every round, so damage that a repair left
behind would be found by a later one. Exit status 0 if every round passed.
"""

import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "..", "build")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--seed", type=int, default=1)
    args, extra = parser.parse_known_args()

    if not shutil.which("e2fsck") or not os.path.exists(os.path.join(BUILD, "home.img")):
        print("crash: skipped (no e2fsck, or no ext2 image was built)")
        return 0

    rng = random.Random(args.seed)
    fat = os.path.join(BUILD, "crash-fat.img")
    home = os.path.join(BUILD, "crash-home.img")
    shutil.copy(os.path.join(BUILD, "disk.img"), fat)
    shutil.copy(os.path.join(BUILD, "home.img"), home)
    shell = [os.path.join(HERE, "qemu_shell.py"), "--disk", fat, "--home-disk", home, "--keep-disk-writes"] + extra

    repaired = []
    for round_number in range(1, args.rounds + 1):
        # 1. Run the workload and cut the power. (A process group of its
        # own, so that the kill reaches QEMU and not only the script.)
        delay = rng.uniform(4.0, 8.0)   # Booting takes the first second or two
        victim = subprocess.Popen(shell + ["--timeout", "60", f"!exec /bin/fsstress /home/alice {rng.randrange(1, 10000)}",
                                           "@wait:30"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        time.sleep(delay)
        os.killpg(victim.pid, signal.SIGKILL)
        victim.wait()

        # 2. Boot again: the kernel repairs at mount, and must still work
        result = subprocess.run(shell + ["--timeout", "60",
                                         "--expect", "ext2: mounted at /home", "--expect", "still-writable",
                                         "--expect", "sub0/",     # The workload did run: its directories are there
                                         "--forbid", "does not repair",
                                         "exec /bin/ls /home/alice",
                                         'exec /bin/sh -c "echo still-writable > /home/after-crash.txt"',
                                         "exec /bin/cat /home/after-crash.txt",
                                         "exec /bin/rm /home/after-crash.txt"],
                                capture_output=True)
        output = result.stdout.decode("utf-8", errors="replace")
        if result.returncode != 0:
            sys.stdout.write(output)
            print(f"crash: FAILED in round {round_number}: the kernel did not come back up cleanly "
                  f"(killed {delay:.1f} s after starting)")
            return 1
        found = re.search(r"ext2: repaired after an interrupted write: (.*)", output)
        if found:
            repaired.append(f"round {round_number}: {found.group(1).strip()}")

        # 3. The outside opinion
        check = subprocess.run(["e2fsck", "-fn", home], capture_output=True)
        if check.returncode != 0:
            sys.stdout.write(check.stdout.decode("utf-8", errors="replace"))
            print(f"crash: FAILED in round {round_number}: e2fsck finds the volume damaged "
                  f"(killed {delay:.1f} s after starting)")
            return 1

    for line in repaired:
        print("  " + line)
    print(f"crash: {args.rounds} power cuts during writes, {len(repaired)} needed repair at mount; e2fsck clean after each: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
