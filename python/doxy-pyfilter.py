#!/usr/bin/env python3
#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Doxygen INPUT_FILTER that makes attribute docstrings visible to Doxygen.

Python has two places to document a variable and the tools disagree about which
one to read:

  - Doxygen reads `##` comment blocks, and ignores a docstring that follows an
    assignment.
  - Editors (pyright, Pylance) read the PEP 258 attribute docstring, and ignore
    `##` comments.

Writing both would mean the same sentence in two places, drifting apart. So the
sources carry only the attribute docstring, which is the one an editor shows on
hover, and this filter rewrites those into `##` blocks on the way into Doxygen.
Nothing on disk changes; Doxygen just reads a different stream.

This covers dataclass fields, enum members and module level constants alike,
including the ones whose value spans several lines.

Usage (from the Doxyfile):

    INPUT_FILTER = "python3 doxy-pyfilter.py"
"""

import re
import sys

# Something that could carry a docstring: a dataclass field (`name: int = 0`),
# a field with no default (`name: int`), an enum member (`Red = 1`) or a module
# constant. The =(?!=) keeps `if a == b` from looking like an assignment.
ASSIGNMENT = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"(?:self\.)?[A-Za-z_]\w*"  # `self.` picks up attributes set in __init__
    r"(?:\s*:\s*[^=]+?)?"
    r"\s*=(?!=)\s*\S.*$"
)

# A field declared with a type but no default, which a dataclass still treats
# as a field: `address: int`.
ANNOTATION = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"[A-Za-z_]\w*"
    r"\s*:\s*[^=]+$"
)

QUOTES = ('"""', "'''")

# Blank out quoted spans before counting brackets, so a bracket inside a string
# cannot throw off the search for the end of a multi-line value.
STRING_SPAN = re.compile(r'"[^"\n]*"|\'[^\'\n]*\'')


def _end_of_assignment(lines: list[str], start: int) -> int:
    """Index of the final line of an assignment, following any open brackets."""
    depth = 0
    for index in range(start, len(lines)):
        for char in STRING_SPAN.sub("", lines[index]):
            if char in "([{":
                depth += 1
            elif char in ")]}":
                depth -= 1
        if depth <= 0:
            return index
    return start


def _read_docstring(lines: list[str], start: int) -> tuple[list[str], int]:
    """Body and line count of the docstring at `start`, or ([], 0) if none."""
    if start >= len(lines):
        return [], 0

    stripped = lines[start].strip()
    quote = next((q for q in QUOTES if stripped.startswith(q)), None)
    if quote is None:
        return [], 0

    # Sitting entirely on one line.
    rest = stripped[len(quote) :]
    if rest.endswith(quote) and len(rest) >= len(quote):
        return [rest[: -len(quote)].strip()], 1

    body = [rest] if rest else []
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if line.strip().endswith(quote):
            tail = line.strip()[: -len(quote)]
            if tail:
                body.append(tail)
            return [b.strip() for b in body], index - start + 1
        body.append(line.strip())

    return [], 0  # unterminated; leave the source alone


def convert(lines: list[str]) -> list[str]:
    """Rewrite each attribute docstring as a ## block above its declaration."""
    out: list[str] = []
    index = 0
    while index < len(lines):
        # A docstring reached here belongs to a module, class or function, not
        # to a declaration: the declaration branch below consumes its own. Copy
        # it out whole without looking inside, or prose in a doctest ("...use
        # on_count=1") would be mistaken for a declaration.
        _, docstring_lines = _read_docstring(lines, index)
        if docstring_lines:
            out.extend(lines[index : index + docstring_lines])
            index += docstring_lines
            continue

        match = ASSIGNMENT.match(lines[index])
        annotation_only = match is None
        if annotation_only:
            match = ANNOTATION.match(lines[index])
        if not match:
            out.append(lines[index])
            index += 1
            continue

        last = _end_of_assignment(lines, index)
        body, consumed = _read_docstring(lines, last + 1)
        if not consumed:
            out.extend(lines[index : last + 1])
            index = last + 1
            continue

        indent = match.group("indent")
        for line in body:
            out.append(f"{indent}## {line}" if line else f"{indent}##")
        out.extend(lines[index:last])

        if annotation_only:
            # Doxygen documents a Python field only if it is assigned. Given a
            # field with no default (`address: int`) it drops the comment and
            # re-attaches it to the next field that does have one, quietly
            # crediting one field's documentation to another. Giving it the
            # `= ...` that a stub file would write keeps the two together.
            out.append(f"{lines[last]} = ...")
        else:
            out.append(lines[last])

        index = last + 1 + consumed

    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <source.py>", file=sys.stderr)
        return 1

    with open(sys.argv[1], encoding="utf-8") as f:
        lines = f.read().split("\n")

    sys.stdout.write("\n".join(convert(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
