#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Field checks shared by the command and event dataclasses.

The C layer silently truncates oversized values and out-of-range integers, so
these run at construction time to turn what would be a corrupt packet into an
obvious Python error.
"""

UINT8_MAX = 0xFF
UINT16_MAX = 0xFFFF
UINT32_MAX = 0xFFFFFFFF


def check_range(name: str, value: int, maximum: int) -> None:
    """Raise ValueError unless `value` fits in [0, maximum]."""
    if not 0 <= value <= maximum:
        raise ValueError(
            f"{name} must be between 0 and {maximum}, got {value}"
        )


def check_length(name: str, data: bytes, maximum: int) -> None:
    """Raise ValueError if `data` is longer than the C buffer that holds it."""
    if len(data) > maximum:
        raise ValueError(
            f"{name} must be at most {maximum} bytes, got {len(data)}"
        )


def check_blink(name: str, on_count: int, off_count: int) -> None:
    """Raise ValueError if an LED block that is being set never lights up.

    The spec forbids both durations being zero. A PD would reject such a
    command on the wire, which is a long way from the code that built it. For a
    steady color, set on_count and leave off_count at zero.
    """
    if on_count == 0 and off_count == 0:
        raise ValueError(
            f"{name}: on_count and off_count cannot both be zero when the "
            f"control code is Set. For a steady color use on_count=1, "
            f"off_count=0"
        )
