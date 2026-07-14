#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Descriptors used to set up a CP or a PD."""

from dataclasses import dataclass, field
from typing import Any, Protocol

from .channel import Channel
from .enums import Capability, LibFlag

__all__ = [
    "FileOps",
    "FileTxStatus",
    "Metrics",
    "PDCapabilities",
    "PDCapability",
    "PDInfo",
    "PdId",
]


@dataclass(frozen=True, slots=True)
class PdId:
    """A PD's identity, as reported in its ID response.

    @see osdp_pd_id

    Example:
        >>> pd_id = PdId(version=1, model=1, vendor_code=0xCAFEBABE,
        ...              serial_number=0xDEADBEAF, firmware_version=0xDEADDEAD)
        >>> pd_id.model
        1
    """

    version: int = 1
    """The PD's hardware version."""

    model: int = 1
    """The PD's model number."""

    vendor_code: int = 0
    """The vendor's 3-byte IEEE OUI."""

    serial_number: int = 0
    """The PD's serial number."""

    firmware_version: int = 0
    """The PD's firmware version."""


@dataclass(frozen=True, slots=True)
class PDCapability:
    """One thing a PD claims to support.

    @see osdp_pd_cap

    Example:
        >>> cap = PDCapability(function_code=Capability.OutputControl,
        ...                    compliance_level=1, num_items=8)
        >>> cap.num_items
        8
    """

    function_code: Capability
    """Which capability this describes."""

    compliance_level: int = 1
    """How completely the PD implements it; the meaning is per-capability."""

    num_items: int = 1
    """How many of the thing the PD has, where that makes sense."""


class PDCapabilities:
    """The set of capabilities a PD reports to a CP.

    Accepts either `PDCapability` objects or plain
    (function_code, compliance_level, num_items) tuples.

    Example:
        >>> caps = PDCapabilities([
        ...     (Capability.OutputControl, 1, 8),
        ...     PDCapability(function_code=Capability.LEDControl, num_items=2),
        ... ])
        >>> len(caps)
        2
    """

    def __init__(
        self,
        capabilities: "list[PDCapability | tuple[Capability, int, int]] | None" = None,
    ) -> None:
        self._by_code: dict[Capability, PDCapability] = {}
        for cap in capabilities or []:
            if isinstance(cap, tuple):
                function_code, compliance_level, num_items = cap
                cap = PDCapability(
                    function_code=function_code,
                    compliance_level=compliance_level,
                    num_items=num_items,
                )
            self._by_code[cap.function_code] = cap

    def __len__(self) -> int:
        return len(self._by_code)

    def __iter__(self) -> "Any":
        return iter(self._by_code.values())

    def to_dict_list(self) -> list[dict[str, int]]:
        """Render into the form the C extension expects. Internal."""
        return [
            {
                "function_code": int(cap.function_code),
                "compliance_level": cap.compliance_level,
                "num_items": cap.num_items,
            }
            for cap in self._by_code.values()
        ]


@dataclass
class PDInfo:
    """Everything needed to talk to (or be) one PD.

    A CP builds one of these per PD it manages; a PD builds one for itself.
    Only the CP's first PDInfo supplies the channel, since all PDs on an RS-485
    bus share it.

    @see osdp_pd_info_t

    Example:
        >>> info = PDInfo(address=101, channel=None,
        ...               flags=[LibFlag.EnforceSecure])
        >>> info.name
        'PD-101'
    """

    address: int
    """The PD's address on the bus."""

    channel: Channel
    """The transport to reach it over."""

    scbk: bytes | None = None
    """The 16-byte secure channel base key, or None to run without one."""

    name: str = ""
    """A label for logs. Defaults to "PD-" followed by the address."""

    flags: list[LibFlag] = field(default_factory=list)
    """Per-PD behaviour flags."""

    id: PdId = field(default_factory=PdId)
    """The identity this PD reports. Ignored in CP mode."""

    baud_rate: int = 9600
    """The bus baud rate."""

    def __post_init__(self) -> None:
        if not self.name:
            self.name = f"PD-{self.address}"
        if self.scbk is not None and len(self.scbk) != 16:
            raise ValueError(
                f"scbk must be exactly 16 bytes, got {len(self.scbk)}"
            )

    def get_flags(self) -> int:
        """Fold the flag list into the bitmask the C extension expects."""
        mask = 0
        for flag in self.flags:
            mask |= int(flag)
        return mask

    def to_dict(self) -> dict[str, Any]:
        """Render into the form the C extension expects. Internal."""
        return {
            "name": self.name,
            "address": self.address,
            "flags": self.get_flags(),
            "scbk": self.scbk,
            "channel": self.channel,
            "baud_rate": self.baud_rate,
            # Only a PD reports an identity; a CP learns its PDs' identities
            # from them, so these are ignored in CP mode.
            "version": self.id.version,
            "model": self.id.model,
            "vendor_code": self.id.vendor_code,
            "serial_number": self.id.serial_number,
            "firmware_version": self.id.firmware_version,
        }


@dataclass(frozen=True, slots=True)
class FileTxStatus:
    """How far a file transfer has got.

    Example:
        >>> status = FileTxStatus(size=1024, offset=512)
        >>> status.offset
        512
    """

    size: int
    """Total size of the file, in bytes."""

    offset: int
    """How many bytes have been transferred so far."""


@dataclass(frozen=True, slots=True)
class Metrics:
    """Counters the library keeps for one PD.

    @see osdp_get_metrics
    """

    packets_sent: int = 0
    """Packets put on the wire."""

    packets_received: int = 0
    """Packets taken off the wire and accepted."""

    packet_check_errors: int = 0
    """Packets discarded because their checksum did not match."""

    nak_count: int = 0
    """Commands the peer rejected."""

    sc_handshake_count: int = 0
    """Secure channel handshakes attempted."""

    sc_failure_count: int = 0
    """Secure channel handshakes that did not complete."""

    command_count: int = 0
    """Commands sent (CP) or received (PD)."""

    event_count: int = 0
    """Events received (CP) or sent (PD)."""


class FileOps(Protocol):
    """What a file transfer source or sink must implement.

    Pass an object satisfying this to `register_file_ops()`. The library calls
    `open()` once, then `read()` or `write()` until the transfer finishes, then
    `close()`.
    """

    def open(self, file_id: int, size: int) -> int:
        """Begin a transfer. Return the file's size in bytes, or -1 to refuse."""
        ...

    def read(self, size: int, offset: int) -> bytes:
        """Return at most `size` bytes starting at `offset`."""
        ...

    def write(self, data: bytes, offset: int) -> int:
        """Store `data` at `offset`. Return the number of bytes stored."""
        ...

    def close(self, file_id: int) -> int:
        """End the transfer. Return 0 on success."""
        ...
