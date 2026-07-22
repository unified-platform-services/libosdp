#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Events a PD sends to a CP.

Each class here mirrors one `struct osdp_event_*` from `osdp.h`. A PD submits
them with `PeripheralDevice.submit_event()`; a CP receives them in its event
handler, where `match` is the natural way to tell them apart:

    >>> def handler(address: int, event: Event) -> None:
    ...     match event:
    ...         case CardRead(data=data):
    ...             admit(data)
    ...         case KeyPress(data=keys):
    ...             collect(keys)

Every field is validated on construction, so an out-of-range value or an
oversized payload raises ValueError here rather than being truncated on its way
into a packet.
"""

from dataclasses import dataclass
from typing import ClassVar, TypeAlias

from . import _sys
from ._validate import UINT8_MAX, UINT16_MAX, UINT32_MAX, check_length, check_range
from .enums import (
    BioStatus,
    BioType,
    CardFormat,
    CommandId,
    EventId,
    MpOutcome,
    NotificationType,
    StatusReportType,
)
from .types import PdId

__all__ = [
    "BioMatch",
    "BioRead",
    "CardRead",
    "Event",
    "KeyPress",
    "ManufacturerError",
    "ManufacturerReply",
    "ManufacturerStatus",
    "Notification",
    "Status",
]

MAX_CARD_DATA_LEN = _sys.EVENT_CARDREAD_MAX_DATALEN
"""Longest card payload a CardRead can carry."""

MAX_KEYPRESS_DATA_LEN = _sys.EVENT_KEYPRESS_MAX_DATALEN
"""Longest keypress payload a KeyPress can carry."""

MAX_MFG_DATA_LEN = _sys.EVENT_MFGREP_MAX_DATALEN
"""Longest payload a manufacturer event can carry."""

MAX_BIO_TEMPLATE_LEN = _sys.EVENT_BIOREADR_MAX_TEMPLATE_LEN

#: Max smartcard/PIV payload a reply event can carry, in bytes.
MAX_PIV_DATA_LEN = _sys.PIV_DATA_MAX_LEN
"""Longest biometric template a BioRead can carry."""

MAX_STATUS_REPORT_LEN = _sys.STATUS_REPORT_MAX_LEN
"""Most entries a status report can carry."""

_RAW_CARD_FORMATS = (CardFormat.Unspecified, CardFormat.Wiegand)


@dataclass(frozen=True, slots=True, kw_only=True)
class CardRead:
    """A card was presented to a reader.

    For the raw formats the card is a bit string whose length is not
    necessarily a multiple of eight, so `bits` carries the real length and
    `data` is that many bits padded out to whole bytes. For the ASCII format
    the card is bytes and `bits` does not apply.

    @see osdp_event_cardread

    Example:
        A 26-bit Wiegand card needs four bytes to hold its 26 bits:

        >>> event = CardRead(format=CardFormat.Wiegand,
        ...                  data=bytes([0x01, 0x02, 0x03, 0x40]), bits=26)
        >>> event.bits
        26

        Leave `bits` out and it is taken to be all of the data:

        >>> event = CardRead(format=CardFormat.Wiegand, data=b"\\x01\\x02")
        >>> event.bits
        16
    """

    ID: ClassVar[EventId] = EventId.CardRead

    reader_no: int = 0
    """Zero-based reader number."""

    format: CardFormat = CardFormat.Wiegand
    """How to read `data`."""

    direction: int = 0
    """Which way the card was swiped: 0 is forward, 1 is backward."""

    data: bytes = b""
    """The card data, at most 64 bytes."""

    bits: int | None = None
    """Length of the card data in BITS. Only for raw formats; None otherwise.

    Defaults to the whole of `data` when omitted for a raw format.
    """

    def __post_init__(self) -> None:
        check_range("reader_no", self.reader_no, UINT8_MAX)
        check_length("data", self.data, MAX_CARD_DATA_LEN)

        if self.format not in _RAW_CARD_FORMATS:
            if self.bits is not None:
                raise ValueError(
                    f"bits does not apply to the {self.format.name} card format"
                )
            return

        if self.bits is None:
            object.__setattr__(self, "bits", len(self.data) * 8)
            return

        check_range("bits", self.bits, UINT16_MAX)
        needed = (self.bits + 7) // 8
        if needed != len(self.data):
            raise ValueError(
                f"bits={self.bits} needs {needed} bytes of data, "
                f"got {len(self.data)}"
            )


@dataclass(frozen=True, slots=True, kw_only=True)
class KeyPress:
    """Keys were pressed on a reader's keypad.

    @see osdp_event_keypress

    Example:
        >>> event = KeyPress(data=b"1234")
        >>> event.data
        b'1234'
    """

    ID: ClassVar[EventId] = EventId.KeyPress

    reader_no: int = 0
    """Zero-based reader number."""

    data: bytes = b""
    """One byte per key, at most 64."""

    def __post_init__(self) -> None:
        check_range("reader_no", self.reader_no, UINT8_MAX)
        check_length("data", self.data, MAX_KEYPRESS_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class ManufacturerReply:
    """A vendor-specific reply from the PD.

    @see osdp_event_mfgrep

    Example:
        >>> event = ManufacturerReply(vendor_code=0x00030201, data=b"\\x01")
        >>> event.vendor_code
        197121
    """

    ID: ClassVar[EventId] = EventId.ManufacturerReply

    vendor_code: int = 0
    """The vendor's 3-byte IEEE OUI."""

    data: bytes = b""
    """Vendor-defined payload, at most 128 bytes."""

    def __post_init__(self) -> None:
        check_range("vendor_code", self.vendor_code, UINT32_MAX)
        check_length("data", self.data, MAX_MFG_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class ManufacturerStatus:
    """A vendor-specific status reply from the PD.

    Unlike `ManufacturerReply` this carries no vendor code; interpret it using
    the vendor code from the PD's id.

    @see osdp_event_mfgstat

    Example:
        >>> event = ManufacturerStatus(data=b"\\x01\\x02")
        >>> ManufacturerStatus().data
        b''
    """

    ID: ClassVar[EventId] = EventId.ManufacturerStatus

    data: bytes = b""
    """Vendor-defined payload, at most 128 bytes. May be empty."""

    def __post_init__(self) -> None:
        check_length("data", self.data, MAX_MFG_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class ManufacturerError:
    """A vendor-specific error reply from the PD.

    Carries the same payload as `ManufacturerStatus` but is a distinct event,
    so a handler can tell an error apart from a status without inspecting the
    bytes.

    @see osdp_event_mfgstat

    Example:
        >>> event = ManufacturerError(data=b"\\xff")
        >>> event.ID.name
        'ManufacturerError'
    """

    ID: ClassVar[EventId] = EventId.ManufacturerError

    data: bytes = b""
    """Vendor-defined payload, at most 128 bytes. May be empty."""

    def __post_init__(self) -> None:
        check_length("data", self.data, MAX_MFG_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class BioRead:
    """A PD's answer to a `commands.BioRead`.

    The template and quality are only meaningful when `status` is Success.
    Note there is no format field: the reply does not restate the encoding that
    was asked for.

    @see osdp_event_bioreadr

    Example:
        >>> event = BioRead(status=BioStatus.Success,
        ...                 type=BioType.RightThumbPrint, quality=200,
        ...                 data=b"template")
        >>> event.status.name
        'Success'

        A failed scan carries no template:

        >>> BioRead(status=BioStatus.Timeout).data
        b''
    """

    ID: ClassVar[EventId] = EventId.BioRead

    reader: int = 0
    """Zero-based reader number."""

    status: BioStatus = BioStatus.Success
    """Whether the scan succeeded."""

    type: BioType = BioType.NotSpecified
    """Which biometric was captured."""

    quality: int = 0
    """Scan quality, from 0x00 (worst) to 0xFF (best)."""

    data: bytes = b""
    """The captured template. Empty on failure. Larger than one packet only when
    both roles set `LibFlag.BioReadrMultipart`."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("quality", self.quality, UINT8_MAX)
        check_length("data", self.data, MAX_BIO_TEMPLATE_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class BioMatch:
    """A PD's answer to a `commands.BioMatch`.

    The score is only meaningful when `status` is Success.

    @see osdp_event_biomatchr

    Example:
        >>> event = BioMatch(status=BioStatus.Success, score=250)
        >>> event.score
        250
    """

    ID: ClassVar[EventId] = EventId.BioMatch

    reader: int = 0
    """Zero-based reader number."""

    status: BioStatus = BioStatus.Success
    """Whether the match ran."""

    score: int = 0
    """Match score, from 0x00 (no match) to 0xFF (best match)."""

    def __post_init__(self) -> None:
        check_range("reader", self.reader, UINT8_MAX)
        check_range("score", self.score, UINT8_MAX)


@dataclass(frozen=True, slots=True, kw_only=True)
class Status:
    """A PD's status report.

    @see osdp_status_report

    Example:
        >>> event = Status(type=StatusReportType.Input, report=bytes([0, 1, 0]))
        >>> list(event.report)
        [0, 1, 0]
    """

    ID: ClassVar[EventId] = EventId.Status

    type: StatusReportType = StatusReportType.Input
    """Which set of status bits this carries."""

    report: bytes = b""
    """One byte per entry, at most 64."""

    def __post_init__(self) -> None:
        check_length("report", self.report, MAX_STATUS_REPORT_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class Notification:
    """A library notification delivered to a CP app.

    Produced by the library, not by a peer, when `LibFlag.EnableNotification`
    is set. Apps never submit this.

    Each `type` carries its own typed fields; only those relevant to the type
    are meaningful. The properties below name the common readings.

    @see osdp_notification

    Example:
        >>> note = Notification(type=NotificationType.Command,
        ...                     command=int(CommandId.LED), success=True)
        >>> note.command_id.name
        'LED'
        >>> note.succeeded
        True
    """

    ID: ClassVar[EventId] = EventId.Notification

    type: NotificationType = NotificationType.Command
    """What happened. Selects which fields below are meaningful."""

    command: int = 0
    """Which command completed (CommandId). Only for NotificationType.Command."""

    success: bool = False
    """Whether that command succeeded. Only for NotificationType.Command."""

    active: bool = False
    """Whether the secure channel is up.
    Only for NotificationType.SecureChannelStatus."""

    scbk_d: bool = False
    """True when the SC uses SCBK-D (install key) rather than SCBK.
    Only for NotificationType.SecureChannelStatus."""

    online: bool = False
    """Whether the PD is reachable.
    Only for NotificationType.PeripheralDeviceStatus."""

    mp_type: int = 0
    """Multipart family (C enum osdp_mp_msg_type; 1 = file transfer). Only
    for NotificationType.Multipart*."""

    object_id: int = 0
    """File id for a multipart transfer. Only for NotificationType.Multipart*."""

    total: int = 0
    """Full payload length. Only for NotificationType.Multipart*."""

    offset: int = 0
    """Bytes transferred so far. Only for NotificationType.Multipart*."""

    outcome: int = 0
    """Terminal outcome. Only meaningful at NotificationType.MultipartDone."""

    version: int = 0
    """PD hardware version. Only for NotificationType.PdId."""

    model: int = 0
    """PD model number. Only for NotificationType.PdId."""

    vendor_code: int = 0
    """Vendor's 3-byte IEEE OUI. Only for NotificationType.PdId."""

    serial_number: int = 0
    """PD serial number. Only for NotificationType.PdId."""

    firmware_version: int = 0
    """PD firmware version. Only for NotificationType.PdId."""

    @property
    def command_id(self) -> CommandId:
        """Which command completed. Only for NotificationType.Command."""
        return CommandId(self.command)

    @property
    def succeeded(self) -> bool:
        """Whether that command succeeded. Only for NotificationType.Command."""
        return self.success

    @property
    def sc_active(self) -> bool:
        """Whether the secure channel is up.

        Only for NotificationType.SecureChannelStatus.
        """
        return self.active

    @property
    def pd_online(self) -> bool:
        """Whether the PD is reachable.

        Only for NotificationType.PeripheralDeviceStatus.
        """
        return self.online

    @property
    def file_id(self) -> int:
        """Which file was transferred.

        Only for NotificationType.Multipart* (reads object_id).
        """
        return self.object_id

    @property
    def pd_id(self) -> PdId:
        """The collected PD identity. Only for NotificationType.PdId."""
        return PdId(
            version=self.version,
            model=self.model,
            vendor_code=self.vendor_code,
            serial_number=self.serial_number,
            firmware_version=self.firmware_version,
        )

    @property
    def mp_outcome(self) -> MpOutcome:
        """How the transfer ended.

        Only for NotificationType.MultipartDone (reads outcome).
        """
        return MpOutcome(self.outcome)



@dataclass(frozen=True, slots=True, kw_only=True)
class PivData:
    """A PD's answer to a `commands.PivData`: the PIV object contents.

    On the CP, the event carries the payload reassembled from the multi-part
    reply. On the PD, submit this event (from within the command callback for
    an inline reply, or later for delivery on a subsequent poll) and libosdp
    fragments it on the wire.

    @see osdp_event_piv_reply
    """

    ID: ClassVar[EventId] = EventId.PivData

    data: bytes = b""
    """Reassembled reply payload; non-empty."""

    def __post_init__(self) -> None:
        if not self.data:
            raise ValueError("data must not be empty")
        check_length("data", self.data, MAX_PIV_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class GenAuth:
    """A PD's answer to a `commands.GenAuth`: the authenticate response.

    Delivery semantics match `PivData`.

    @see osdp_event_piv_reply
    """

    ID: ClassVar[EventId] = EventId.GenAuth

    data: bytes = b""
    """Reassembled reply payload; non-empty."""

    def __post_init__(self) -> None:
        if not self.data:
            raise ValueError("data must not be empty")
        check_length("data", self.data, MAX_PIV_DATA_LEN)


@dataclass(frozen=True, slots=True, kw_only=True)
class CrAuth:
    """A PD's answer to a `commands.CrAuth`: the challenge response.

    Delivery semantics match `PivData`.

    @see osdp_event_piv_reply
    """

    ID: ClassVar[EventId] = EventId.CrAuth

    data: bytes = b""
    """Reassembled reply payload; non-empty."""

    def __post_init__(self) -> None:
        if not self.data:
            raise ValueError("data must not be empty")
        check_length("data", self.data, MAX_PIV_DATA_LEN)


Event: TypeAlias = (
    CardRead
    | KeyPress
    | ManufacturerReply
    | ManufacturerStatus
    | ManufacturerError
    | BioRead
    | BioMatch
    | PivData
    | GenAuth
    | CrAuth
    | Status
    | Notification
)
"""Any event. Match on it to tell the members apart."""
