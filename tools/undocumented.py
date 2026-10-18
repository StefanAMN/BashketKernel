#!/usr/bin/env python3
"""List functions that have no comment saying what they do.

    tools/undocumented.py [DIRECTORY ...]      (default: src lib user tests)
    tools/undocumented.py --count [DIRECTORY ...]

The rule (CLAUDE.md, PLAN.md track E): every function has a comment. A
function that other files call is documented once, at its declaration in a
header; a static one at its definition. So a function counts as documented
if the line before its definition, or before its declaration in any header,
ends a comment.

This is a heuristic over the text, not a C parser: a definition is a line at
column 0 that looks like "type name(arguments) {". It finds the cases that
matter and is cheap enough to run on every check.
"""

import collections
import os
import re
import sys

DEFINITION = re.compile(r'^(?!if\b|for\b|while\b|switch\b|else\b|return\b)'
                        r'[A-Za-z_][\w\s\*]*?\**\s*\b(\w+)\s*\([^;]*\)\s*\{\s*$')
DECLARATION = re.compile(r'^[A-Za-z_][\w\s\*]*?\**\s*\b(\w+)\s*\(.*\)\s*;')


def ends_comment(lines, i):
    """Does the last non-blank line before line i end a comment?"""
    j = i - 1
    while j >= 0 and lines[j].strip() == '':
        j -= 1
    previous = lines[j].strip() if j >= 0 else ''
    return previous.startswith('//') or previous.endswith('*/')


def walk(directories, suffix):
    for top in directories:
        for path, _, names in os.walk(top):
            for name in sorted(names):
                if name.endswith(suffix):
                    full = os.path.join(path, name)
                    yield full, open(full, errors='replace').read().split('\n')


def main():
    args = sys.argv[1:]
    count_only = '--count' in args
    directories = [a for a in args if not a.startswith('--')] or ['src', 'lib', 'user', 'tests']

    documented_in_header = set()
    for _, lines in walk(['src', 'lib', 'user'], '.h'):
        for i, line in enumerate(lines):
            m = DECLARATION.match(line)
            if m and ends_comment(lines, i):
                documented_in_header.add(m.group(1))

    missing = collections.defaultdict(list)
    for path, lines in walk(directories, '.c'):
        for i, line in enumerate(lines):
            m = DEFINITION.match(line)
            if m and not ends_comment(lines, i) and m.group(1) not in documented_in_header:
                missing[os.path.dirname(path)].append(f'{path}:{i + 1}: {m.group(1)}')

    total = sum(len(v) for v in missing.values())
    if count_only:
        for directory in sorted(missing, key=lambda d: -len(missing[d])):
            print(f'{len(missing[directory]):5d}  {directory}')
    else:
        for directory in sorted(missing):
            print('\n'.join(missing[directory]))
    print(f'{total} functions without a comment', file=sys.stderr)


if __name__ == '__main__':
    main()
