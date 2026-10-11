#!/usr/bin/env python3
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Checks a directory of wheels against the cibuildwheel build identifiers in
# a targets file: every identifier needs exactly one wheel, and every wheel
# must belong to some identifier. A wheel quietly dropped from (or added to)
# the build would otherwise go out with the release unnoticed.
#
# usage: check-wheels.py <dist-dir> <wheel-targets.txt>

import re
import sys
from pathlib import Path

# {name}-{ver}(-{build})?-{py}-{abi}-{plat}.whl
WHEEL_RE = re.compile(
    r"^(?P<name>[^-]+)-(?P<ver>[^-]+)(?:-(?P<build>\d[^-]*))?"
    r"-(?P<py>[^-]+)-(?P<abi>[^-]+)-(?P<plat>[^-]+)\.whl$"
)
IDENT_RE = re.compile(r"^(?P<interp>cp\d+t?)-(?P<plat>[a-z]+)_(?P<arch>\w+)$")

# cibuildwheel's platform part of an identifier -> wheel platform tag shape
PLATFORM_TAG_RE = {
    "manylinux": r"manylinux(?:_\d+_\d+|1|2010|2014)_{arch}",
    "musllinux": r"musllinux_\d+_\d+_{arch}",
    "macosx": r"macosx_\d+_\d+_{arch}",
    "win": r"win_{arch}",
}


def read_identifiers(path):
    idents = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            idents.append(line)
    return idents


def identifier_matcher(ident):
    m = IDENT_RE.match(ident)
    if not m or m["plat"] not in PLATFORM_TAG_RE:
        raise ValueError(ident)
    interp = m["interp"]
    plat_re = re.compile(
        PLATFORM_TAG_RE[m["plat"]].format(arch=re.escape(m["arch"])) + "$"
    )

    # A free-threaded build (cp314t) carries the "t" on its ABI tag; its
    # python tag is plain cp314.
    def matches(wheel):
        return (
            wheel["abi"] == interp
            and interp.rstrip("t") in wheel["py"].split(".")
            and any(plat_re.match(p) for p in wheel["plat"].split("."))
        )

    return matches


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <dist-dir> <wheel-targets.txt>", file=sys.stderr)
        return 2
    dist, targets = argv[1], argv[2]

    matchers, unknown = {}, []
    for ident in read_identifiers(targets):
        try:
            matchers[ident] = identifier_matcher(ident)
        except ValueError:
            unknown.append(ident)
    wheels = {}
    unexpected = []
    for path in sorted(Path(dist).glob("*.whl")):
        m = WHEEL_RE.match(path.name)
        if m:
            wheels[path.name] = m.groupdict()
        else:
            unexpected.append(path.name)

    owners = {name: [] for name in wheels}
    missing, duplicated, rows = [], [], []
    for ident, matches in matchers.items():
        found = [name for name, w in wheels.items() if matches(w)]
        for name in found:
            owners[name].append(ident)
        if not found:
            missing.append(ident)
        elif len(found) > 1:
            duplicated.append((ident, found))
        rows.append((ident, ", ".join(found) or "MISSING"))
    unexpected += [name for name, idents in owners.items() if not idents]

    width = max((len(r[0]) for r in rows), default=0)
    for ident, found in rows:
        print(f"{ident:<{width}}  {found}")

    for ident in unknown:
        print(f"error: unrecognised build identifier {ident}", file=sys.stderr)
    for ident in missing:
        print(f"error: no wheel for {ident}", file=sys.stderr)
    for ident, found in duplicated:
        print(f"error: {len(found)} wheels for {ident}: {', '.join(found)}", file=sys.stderr)
    for name in unexpected:
        print(f"error: unexpected wheel {name}", file=sys.stderr)
    if unknown or missing or duplicated or unexpected:
        return 1
    print(f"{len(matchers)} wheels, one per build identifier")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
