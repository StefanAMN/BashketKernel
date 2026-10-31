#!/usr/bin/env python3
"""List functions that have no comment saying what they do.

    tools/undocumented.py [DIRECTORY ...]      (default: src lib user tests tools)
    tools/undocumented.py --count [DIRECTORY ...]
    tools/undocumented.py --check              exit 1 if any function has none (make check)

The rule (CLAUDE.md, PLAN.md track E): every function has a comment. A
function that other files call is documented once, at its declaration in a
header; a static one at its definition. So a C function counts as documented
if a comment ends on the line before its definition, or before its declaration
in any header, or its declaration in a header has a comment of its own at the
end of the line (the user library's style: one line per call). A user
program's `main` is documented by the comment the file starts with, which
says what the program does and how it is called. A Python function has a
docstring; `main` may leave that to the docstring of its file.

What does not count: a section banner ("--- Network ---"), which names a part
of the file and says nothing of the function under it, and a comment at the
end of a line of code, which describes that code.

This is a heuristic over the text, not a C parser: it blanks comments, strings
and preprocessor lines and takes whatever has a parameter list and a body at
brace depth 0 for a function, on however many lines, in .c and .h files alike.
TEST(name) counts, being a function by way of a macro. It finds the cases that
matter and is cheap enough to run on every check.
"""

import ast
import collections
import os
import re
import sys

NOT_FUNCTIONS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do',
                 '__attribute__', 'typeof', 'asm', '__asm__'}
ATTRIBUTES = r'(?:__attribute__\s*\(\(.*?\)\)\s*)*'
DEFINITION_HEAD = re.compile(r'\b([A-Za-z_]\w*)\s*\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\s*'
                             + ATTRIBUTES + r'$', re.S)
DECLARATION = re.compile(r'\b([A-Za-z_]\w*)\s*\((?:[^();]|\([^()]*\))*\)\s*' + ATTRIBUTES + r';')
BANNER = re.compile(r'^(//|/\*)\s*---')
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b')


def blank_noncode(text):
    """Return text with comments, strings and preprocessor lines turned to
    spaces, every newline and so every position kept."""
    out = list(text)
    i, n = 0, len(text)

    def blank(start, end):
        """Overwrite text[start:end] with spaces, keeping the newlines."""
        for k in range(start, min(end, n)):
            if out[k] != '\n':
                out[k] = ' '

    while i < n:
        c = text[i]
        if text.startswith('//', i):
            end = text.find('\n', i)
            end = n if end < 0 else end
            blank(i, end)
            i = end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif c in '"\'':
            end = i + 1
            while end < n and text[end] != c:
                end += 2 if text[end] == '\\' else 1
            blank(i, end + 1)
            i = end + 1
        else:
            i += 1

    lines = ''.join(out).split('\n')
    k = 0
    while k < len(lines):
        if lines[k].lstrip().startswith('#'):
            while lines[k].rstrip().endswith('\\') and k + 1 < len(lines):
                lines[k] = ' ' * len(lines[k])
                k += 1
            lines[k] = ' ' * len(lines[k])
        k += 1
    return '\n'.join(lines)


def top_level(code):
    """Yield (head, start, is_body): each run of code at brace depth 0 that ends
    in a `{` (is_body) or a `;`, with the offset of its first character."""
    depth = parens = run = 0
    for i, c in enumerate(code):
        if c == '(':
            parens += 1
        elif c == ')':
            parens -= 1
        elif c == '{':
            if depth == 0:
                yield code[run:i], run, True
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                run = i + 1
        elif c == ';' and depth == 0 and parens == 0:
            yield code[run:i + 1], run, False
            run = i + 1


def first_line(code, head, start):
    """The index of the line where this head's first non-blank character is."""
    return code.count('\n', 0, start + len(head) - len(head.lstrip()))


def comment_above(raw, code, i):
    """Does a real comment end on the line before line i? Conditional-compilation
    lines between the two are looked past; a banner or code is not a comment."""
    j = i - 1
    while j >= 0 and CONDITIONAL.match(raw[j]):
        j -= 1
    if j < 0 or not raw[j].strip() or code[j].strip():
        return False
    return not BANNER.match(raw[j].strip()) and not raw[j].lstrip().startswith('#')


def walk(directories, suffixes):
    """Yield (path, text) of each file under the directories with one of the suffixes."""
    for top in directories:
        for path, _, names in os.walk(top):
            for name in sorted(names):
                if name.endswith(suffixes):
                    full = os.path.join(path, name)
                    yield full, open(full, errors='replace').read()


def documented_in_headers():
    """The names of the functions some header declares under a comment of their own."""
    names = set()
    for _, text in walk(['src', 'lib', 'user'], ('.h',)):
        raw = text.split('\n')
        code = blank_noncode(text)
        code_lines = code.split('\n')
        for head, start, is_body in top_level(code):
            if is_body:
                continue
            m = DECLARATION.search(head + ';' if not head.endswith(';') else head)
            if not m:
                continue
            i = first_line(code, head, start)
            last = code.count('\n', 0, start + len(head.rstrip()) - 1)
            trailing = re.search(r';\s*(//|/\*)', raw[last])
            if trailing or comment_above(raw, code_lines, i):
                names.add(m.group(1))
    return names


def missing_in_c(directories, in_headers):
    """Yield "path:line: name" for each undocumented C function."""
    for path, text in walk(directories, ('.c', '.h')):
        raw = text.split('\n')
        code = blank_noncode(text)
        code_lines = code.split('\n')
        program_described = path.startswith('user/') and text.startswith('/*')
        for head, start, is_body in top_level(code):
            if not is_body or '=' in head.split('(')[0]:
                continue
            m = DEFINITION_HEAD.search(head.rstrip())
            if not m or m.group(1) in NOT_FUNCTIONS:
                continue
            i = first_line(code, head, start)
            name = m.group(1)
            if name == 'TEST':
                name = re.search(r'TEST\s*\(\s*(\w+)', head).group(1)
            elif name == 'main' and program_described:
                continue
            is_static = re.search(r'\bstatic\b', head) is not None
            if comment_above(raw, code_lines, i) or (not is_static and name in in_headers):
                continue
            yield f'{path}:{i + 1}: {name}'


def missing_in_python(directories):
    """Yield "path:line: name" for each Python function without a docstring."""
    for path, text in walk(directories, ('.py',)):
        tree = ast.parse(text)
        for node in ast.walk(tree):
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if ast.get_docstring(node):
                continue
            if node.name == 'main' and ast.get_docstring(tree):
                continue
            yield f'{path}:{node.lineno}: {node.name}'


def main():
    """Print what is missing, per the usage at the top of this file."""
    args = sys.argv[1:]
    count_only = '--count' in args
    check = '--check' in args
    directories = [a for a in args if not a.startswith('--')] or ['src', 'lib', 'user', 'tests', 'tools']

    missing = collections.defaultdict(list)
    found = list(missing_in_c(directories, documented_in_headers())) + list(missing_in_python(directories))
    for entry in found:
        missing[os.path.dirname(entry.split(':')[0])].append(entry)

    total = sum(len(v) for v in missing.values())
    if count_only:
        for directory in sorted(missing, key=lambda d: -len(missing[d])):
            print(f'{len(missing[directory]):5d}  {directory}')
    else:
        for directory in sorted(missing):
            print('\n'.join(missing[directory]))
    print(f'{total} functions without a comment', file=sys.stderr)
    if check and total:
        sys.exit(1)


if __name__ == '__main__':
    main()
