#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Exceptions raised by the osdp package."""

from .enums import NakCode

__all__ = ["MarshalError", "NakError", "OSDPError"]


class OSDPError(Exception):
    """Base class for every error this package raises."""


class NakError(OSDPError):
    """Raised by a PD command handler to reject a command.

    The library turns this into a NAK reply to the CP. Raising it is the only
    way for a handler to decline a command; returning normally acknowledges it.

    @see osdp_pd_nak_code_e

    Example:
        >>> raise NakError(NakCode.BioType)
        Traceback (most recent call last):
        osdp.errors.NakError: Command rejected: BioType
    """

    def __init__(self, code: NakCode = NakCode.Record) -> None:
        self.code = code
        """Why the command was rejected."""

        super().__init__(f"Command rejected: {code.name}")


class MarshalError(OSDPError):
    """Raised when a command or event cannot cross the C boundary.

    This means a malformed object was submitted, or the library produced
    something this version of the bindings does not understand. Either way it
    is a bug rather than a runtime condition to be handled.
    """
