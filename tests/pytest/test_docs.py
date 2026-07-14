#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Checks that the documentation actually reaches the doc site's input.

The doc site consumes Doxygen XML. Doxygen does not read the PEP 258 attribute
docstrings our sources use, so python/doxy-pyfilter.py rewrites them into ##
blocks on the way in. When that filter breaks it does not fail loudly: the XML
simply comes out with empty descriptions, which looks exactly like nobody having
written the documentation yet. Worse, a field's documentation can silently
re-attach itself to the *next* field, crediting one field's docs to another.

So assert on the XML rather than trusting the filter.
"""

import glob
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import pytest

PYTHON_DIR = os.path.realpath(
    os.path.join(os.path.dirname(__file__), "..", "..", "python")
)

# The classes whose every public field we promise to document.
DOCUMENTED_MODULES = ("commands::", "events::", "types::", "errors::")

pytestmark = pytest.mark.skipif(
    shutil.which("doxygen") is None, reason="doxygen is not installed"
)


def _member_doc(member: ET.Element) -> str:
    parts = []
    for tag in ("briefdescription", "detaileddescription"):
        element = member.find(tag)
        if element is not None:
            text = " ".join("".join(element.itertext()).split())
            if text:
                parts.append(text)
    return " ".join(parts)


@pytest.fixture(scope="module")
def doxygen_xml(tmp_path_factory):
    """Run Doxygen over the package and hand back the parsed XML."""
    out_dir = tmp_path_factory.mktemp("doxygen")

    with open(os.path.join(PYTHON_DIR, "Doxyfile")) as f:
        doxyfile = f.read()
    doxyfile += f'\nOUTPUT_DIRECTORY = "{out_dir}"\n'

    with tempfile.NamedTemporaryFile(
        "w", suffix=".doxyfile", dir=PYTHON_DIR, delete=False
    ) as f:
        f.write(doxyfile)
        doxyfile_path = f.name

    try:
        result = subprocess.run(
            ["doxygen", doxyfile_path],
            cwd=PYTHON_DIR,
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, (
            f"doxygen failed:\n{result.stdout}\n{result.stderr}"
        )
        files = glob.glob(os.path.join(out_dir, "xml", "*.xml"))
        assert files, "doxygen produced no XML"
        yield files
    finally:
        os.unlink(doxyfile_path)


def _public_fields(xml_files):
    for path in xml_files:
        for compound in ET.parse(path).getroot().iter("compounddef"):
            name = (compound.findtext("compoundname") or "").replace("osdp::", "")
            if not name.startswith(DOCUMENTED_MODULES):
                continue
            for member in compound.iter("memberdef"):
                if member.get("kind") != "variable":
                    continue
                field = member.findtext("name") or ""
                # ID is the type discriminator, and SCREAMING names are module
                # constants, not fields a user sets.
                if field.startswith("_") or field == "ID" or field.isupper():
                    continue
                yield f"{name}.{field}", _member_doc(member)


def test_every_public_field_is_documented(doxygen_xml):
    undocumented = [
        name for name, doc in _public_fields(doxygen_xml) if not doc
    ]
    assert not undocumented, (
        "These fields reach the doc site with no description. Either they need "
        "an attribute docstring, or doxy-pyfilter.py has stopped picking one "
        f"up: {undocumented}"
    )


def test_documentation_is_not_credited_to_the_wrong_field(doxygen_xml):
    """A field with no default used to donate its docs to the next field.

    Doxygen ignores a Python field that is never assigned, and re-attaches its
    preceding comment to whichever field comes next. PDInfo.address and
    PDInfo.channel have no defaults, and their docs landed on PDInfo.scbk.
    """
    docs = dict(_public_fields(doxygen_xml))

    address = docs.get("types::PDInfo.address", "")
    scbk = docs.get("types::PDInfo.scbk", "")

    assert "address on the bus" in address
    assert "secure channel base key" in scbk
    assert "address on the bus" not in scbk
    assert "transport" not in scbk


def test_field_units_survive_into_the_xml(doxygen_xml):
    """The units are the whole reason these docstrings exist."""
    docs = dict(_public_fields(doxygen_xml))

    assert "100ms" in docs["commands::Buzzer.on_count"]
    assert "BITS" in docs["events::CardRead.bits"]
    assert "forever" in docs["commands::Buzzer.rep_count"]


def test_multi_line_field_docstrings_survive(doxygen_xml):
    """CardRead.bits has a multi-line docstring; an early filter dropped it."""
    docs = dict(_public_fields(doxygen_xml))
    bits = docs["events::CardRead.bits"]

    assert "raw formats" in bits
    assert "Defaults to" in bits  # the second paragraph
