#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#
# osdp/_sys.pyi is hand-written: nothing generates it from module.c, and a type
# checker cannot tell a constant the C extension never exported from one it
# simply has not been asked about. So a constant added to
# pyosdp_add_module_constants() but forgotten in the stub type-checks fine and
# fails at import; one deleted from C but left in the stub type-checks fine and
# fails at use. The stub is the only copy of these names outside C, so this
# pins it to what the extension actually exports.

import re
from pathlib import Path

import osdp
from osdp import _sys

STUB = Path(osdp.__file__).parent / "_sys.pyi"

_CONSTANT = re.compile(r"^([A-Z][A-Z0-9_]*)\s*:\s*Final\[int\]", re.MULTILINE)


def declared_in_stub() -> set[str]:
    return set(_CONSTANT.findall(STUB.read_text()))


def exported_by_extension() -> set[str]:
    return {
        name
        for name in dir(_sys)
        if name.isupper() and isinstance(getattr(_sys, name), int)
    }


def test_the_stub_ships_with_the_package():
    # package-data in pyproject.toml has to keep carrying *.pyi, or the stub is
    # absent from the wheel and every assertion below is vacuously true.
    assert STUB.is_file()


def test_stub_declares_every_constant_the_extension_exports():
    missing = exported_by_extension() - declared_in_stub()
    assert not missing, (
        f"exported by module.c but absent from _sys.pyi: {sorted(missing)}"
    )


def test_stub_declares_no_constant_the_extension_lacks():
    stale = declared_in_stub() - exported_by_extension()
    assert not stale, (
        f"declared in _sys.pyi but not exported by module.c: {sorted(stale)}"
    )
