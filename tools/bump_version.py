"""
Firmware version (version.txt, M.m.b, built into the firmware by ESP-IDF): raised once per change, before its
build and commit.

  python tools/bump_version.py fix      # b: a fix                              1.4.2 -> 1.4.3
  python tools/bump_version.py feature  # m: a new feature made without a branch 1.4.2 -> 1.5.0
  python tools/bump_version.py major    # M: a big change that needed a branch  1.4.2 -> 2.0.0

Without argument, the version is only shown. "Quelle est ta version ?" makes the assistant say it.
"""

import os
import sys

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "version.txt")


def main():
    with open(PATH, encoding="utf-8") as f:
        major, minor, fix = (int(n) for n in f.read().strip().split("."))
    kind = sys.argv[1] if len(sys.argv) > 1 else ""
    if kind == "fix":
        fix += 1
    elif kind == "feature":
        minor, fix = minor + 1, 0
    elif kind == "major":
        major, minor, fix = major + 1, 0, 0
    elif kind:
        sys.exit("usage: bump_version.py [fix|feature|major]")
    version = "%d.%d.%d" % (major, minor, fix)
    if kind:
        with open(PATH, "w", encoding="utf-8", newline="\n") as f:
            f.write(version + "\n")
    print(version)


if __name__ == "__main__":
    main()
