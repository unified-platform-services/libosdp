#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Conversion between the typed dataclasses and the dicts `osdp._sys` speaks.

This is the only module that knows the dict format. It is private: the dict is
an implementation detail shared between two halves of this package, not an API.

Enum members are written into the dicts as-is. They are `int` subclasses, so
the C layer takes them unchanged, and a dict that is logged or printed stays
readable.
"""

from typing import Any, Callable

from . import commands as c
from . import events as e
from .enums import (
    BioFormat,
    BioStatus,
    BioType,
    BuzzerControlCode,
    CardFormat,
    CommandId,
    EventId,
    FileTxFlag,
    LEDColor,
    NotificationType,
    OutputControlCode,
    PermanentLEDControlCode,
    StatusReportType,
    TemporaryLEDControlCode,
    TextControlCode,
)
from .errors import MarshalError

Payload = dict[str, Any]

_RAW_CARD_FORMATS = (CardFormat.Unspecified, CardFormat.Wiegand)


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------


def _encode_led_params(
    params: c.TemporaryLEDParams | c.PermanentLEDParams,
) -> Payload:
    payload: Payload = {
        "control_code": params.control_code,
        "on_count": params.on_count,
        "off_count": params.off_count,
        "on_color": params.on_color,
        "off_color": params.off_color,
    }
    if isinstance(params, c.TemporaryLEDParams):
        payload["timer_count"] = params.timer_count
    return payload


def _encode_led(cmd: c.LED) -> Payload:
    payload: Payload = {
        "command": CommandId.LED,
        "reader": cmd.reader,
        "led_number": cmd.led_number,
    }
    if cmd.temporary is not None:
        payload["temporary"] = _encode_led_params(cmd.temporary)
    if cmd.permanent is not None:
        payload["permanent"] = _encode_led_params(cmd.permanent)
    return payload


def _decode_temporary_led_params(payload: Payload) -> c.TemporaryLEDParams:
    return c.TemporaryLEDParams(
        control_code=TemporaryLEDControlCode(payload["control_code"]),
        on_count=payload["on_count"],
        off_count=payload["off_count"],
        on_color=LEDColor(payload["on_color"]),
        off_color=LEDColor(payload["off_color"]),
        timer_count=payload["timer_count"],
    )


def _decode_permanent_led_params(payload: Payload) -> c.PermanentLEDParams:
    return c.PermanentLEDParams(
        control_code=PermanentLEDControlCode(payload["control_code"]),
        on_count=payload["on_count"],
        off_count=payload["off_count"],
        on_color=LEDColor(payload["on_color"]),
        off_color=LEDColor(payload["off_color"]),
    )


def _decode_led(payload: Payload) -> c.LED:
    temporary = payload.get("temporary")
    permanent = payload.get("permanent")
    return c.LED(
        reader=payload["reader"],
        led_number=payload["led_number"],
        temporary=(
            _decode_temporary_led_params(temporary) if temporary else None
        ),
        permanent=(
            _decode_permanent_led_params(permanent) if permanent else None
        ),
    )


_MP_NOTIFICATION_TYPES = frozenset({
    NotificationType.MultipartStart,
    NotificationType.MultipartProgress,
    NotificationType.MultipartDone,
})


def _notification_payload(x) -> Payload:
    """Multipart notifications marshal the structured mp fields (the C codec
    requires them and carries no arg0/arg1); every other type marshals the
    flat arg0/arg1 pair."""
    if x.type in _MP_NOTIFICATION_TYPES:
        return {
            "type": x.type,
            "mp_type": x.mp_type,
            "object_id": x.object_id,
            "total": x.total,
            "offset": x.offset,
            "outcome": x.outcome,
        }
    return {"type": x.type, "arg0": x.arg0, "arg1": x.arg1}


_COMMAND_ENCODERS: dict[type, Callable[[Any], Payload]] = {
    c.Output: lambda x: {
        "command": CommandId.Output,
        "output_no": x.output_no,
        "control_code": x.control_code,
        "timer_count": x.timer_count,
    },
    c.LED: _encode_led,
    c.Buzzer: lambda x: {
        "command": CommandId.Buzzer,
        "reader": x.reader,
        "control_code": x.control_code,
        "on_count": x.on_count,
        "off_count": x.off_count,
        "rep_count": x.rep_count,
    },
    c.Text: lambda x: {
        "command": CommandId.Text,
        "reader": x.reader,
        "control_code": x.control_code,
        "temp_time": x.temp_time,
        "offset_row": x.offset_row,
        "offset_col": x.offset_col,
        "data": x.data,
    },
    c.TDSet: lambda x: {
        "command": CommandId.TDSet,
        "year": x.year,
        "month": x.month,
        "day": x.day,
        "hour": x.hour,
        "minute": x.minute,
        "second": x.second,
    },
    c.Keyset: lambda x: {
        "command": CommandId.Keyset,
        "type": x.type,
        "data": x.data,
    },
    c.Comset: lambda x: {
        "command": CommandId.Comset,
        "address": x.address,
        "baud_rate": x.baud_rate,
    },
    c.ComsetDone: lambda x: {
        "command": CommandId.ComsetDone,
        "address": x.address,
        "baud_rate": x.baud_rate,
    },
    c.Manufacturer: lambda x: {
        "command": CommandId.Manufacturer,
        "vendor_code": x.vendor_code,
        "data": x.data,
    },
    c.BioRead: lambda x: {
        "command": CommandId.BioRead,
        "reader": x.reader,
        "type": x.type,
        "format": x.format,
        "quality": x.quality,
    },
    c.BioMatch: lambda x: {
        "command": CommandId.BioMatch,
        "reader": x.reader,
        "type": x.type,
        "format": x.format,
        "quality": x.quality,
        "data": x.data,
    },
    c.FileTransfer: lambda x: {
        "command": CommandId.FileTransfer,
        "id": x.id,
        "flags": x.flags,
    },
    c.Status: lambda x: {
        "command": CommandId.Status,
        "type": x.type,
        "report": x.report,
    },
    c.PivData: lambda x: {
        "command": CommandId.PivData,
        "oid": x.oid,
        "element": x.element,
        "offset": x.offset,
    },
    c.GenAuth: lambda x: {
        "command": CommandId.GenAuth,
        "algorithm": x.algorithm,
        "key": x.key,
        "data": x.data,
    },
    c.CrAuth: lambda x: {
        "command": CommandId.CrAuth,
        "algorithm": x.algorithm,
        "key": x.key,
        "data": x.data,
    },
    c.Notification: lambda x: {
        "command": CommandId.Notification,
        **_notification_payload(x),
    },
}

_COMMAND_DECODERS: dict[CommandId, Callable[[Payload], c.Command]] = {
    CommandId.Output: lambda p: c.Output(
        output_no=p["output_no"],
        control_code=OutputControlCode(p["control_code"]),
        timer_count=p["timer_count"],
    ),
    CommandId.LED: _decode_led,
    CommandId.Buzzer: lambda p: c.Buzzer(
        reader=p["reader"],
        control_code=BuzzerControlCode(p["control_code"]),
        on_count=p["on_count"],
        off_count=p["off_count"],
        rep_count=p["rep_count"],
    ),
    CommandId.Text: lambda p: c.Text(
        reader=p["reader"],
        control_code=TextControlCode(p["control_code"]),
        temp_time=p["temp_time"],
        offset_row=p["offset_row"],
        offset_col=p["offset_col"],
        data=p["data"],
    ),
    CommandId.TDSet: lambda p: c.TDSet(
        year=p["year"],
        month=p["month"],
        day=p["day"],
        hour=p["hour"],
        minute=p["minute"],
        second=p["second"],
    ),
    CommandId.Keyset: lambda p: c.Keyset(type=p["type"], data=p["data"]),
    CommandId.Comset: lambda p: c.Comset(
        address=p["address"], baud_rate=p["baud_rate"]
    ),
    CommandId.ComsetDone: lambda p: c.ComsetDone(
        address=p["address"], baud_rate=p["baud_rate"]
    ),
    CommandId.Manufacturer: lambda p: c.Manufacturer(
        vendor_code=p["vendor_code"], data=p["data"]
    ),
    CommandId.BioRead: lambda p: c.BioRead(
        reader=p["reader"],
        type=BioType(p["type"]),
        format=BioFormat(p["format"]),
        quality=p["quality"],
    ),
    CommandId.BioMatch: lambda p: c.BioMatch(
        reader=p["reader"],
        type=BioType(p["type"]),
        format=BioFormat(p["format"]),
        quality=p["quality"],
        data=p["data"],
    ),
    CommandId.FileTransfer: lambda p: c.FileTransfer(
        id=p["id"], flags=FileTxFlag(p["flags"])
    ),
    CommandId.Status: lambda p: c.Status(
        type=StatusReportType(p["type"]), report=p["report"]
    ),
    CommandId.PivData: lambda p: c.PivData(
        oid=p["oid"], element=p["element"], offset=p["offset"]
    ),
    CommandId.GenAuth: lambda p: c.GenAuth(
        algorithm=p["algorithm"], key=p["key"], data=p["data"]
    ),
    CommandId.CrAuth: lambda p: c.CrAuth(
        algorithm=p["algorithm"], key=p["key"], data=p["data"]
    ),
    CommandId.Notification: lambda p: c.Notification(
        type=NotificationType(p["type"]),
        arg0=p.get("arg0", 0), arg1=p.get("arg1", 0),
        mp_type=p.get("mp_type", 0),
        object_id=p.get("object_id", 0), total=p.get("total", 0),
        offset=p.get("offset", 0), outcome=p.get("outcome", 0),
    ),
}


def encode_command(cmd: c.Command) -> Payload:
    """Convert a command dataclass into the dict `osdp._sys` expects."""
    encoder = _COMMAND_ENCODERS.get(type(cmd))
    if encoder is None:
        raise MarshalError(f"Not an osdp command: {cmd!r}")
    return encoder(cmd)


def decode_command(payload: Payload) -> c.Command:
    """Convert a dict from `osdp._sys` back into a command dataclass."""
    try:
        return _COMMAND_DECODERS[CommandId(payload["command"])](payload)
    except (KeyError, ValueError) as err:
        raise MarshalError(f"Cannot decode command: {payload!r}") from err


# --------------------------------------------------------------------------
# Events
# --------------------------------------------------------------------------


def _encode_cardread(event: e.CardRead) -> Payload:
    payload: Payload = {
        "event": EventId.CardRead,
        "reader_no": event.reader_no,
        "format": event.format,
        "direction": event.direction,
        "data": event.data,
    }
    # The C layer wants a bit count for the raw formats and no length at all
    # for the others; CardRead.__post_init__ guarantees bits is set iff raw.
    if event.format in _RAW_CARD_FORMATS:
        payload["length"] = event.bits
    return payload


def _decode_cardread(payload: Payload) -> e.CardRead:
    fmt = CardFormat(payload["format"])
    return e.CardRead(
        reader_no=payload["reader_no"],
        format=fmt,
        direction=payload["direction"],
        data=payload["data"],
        bits=payload["length"] if fmt in _RAW_CARD_FORMATS else None,
    )


_EVENT_ENCODERS: dict[type, Callable[[Any], Payload]] = {
    e.CardRead: _encode_cardread,
    e.KeyPress: lambda x: {
        "event": EventId.KeyPress,
        "reader_no": x.reader_no,
        "data": x.data,
    },
    e.ManufacturerReply: lambda x: {
        "event": EventId.ManufacturerReply,
        "vendor_code": x.vendor_code,
        "data": x.data,
    },
    e.ManufacturerStatus: lambda x: {
        "event": EventId.ManufacturerStatus,
        "data": x.data,
    },
    e.ManufacturerError: lambda x: {
        "event": EventId.ManufacturerError,
        "data": x.data,
    },
    e.BioRead: lambda x: {
        "event": EventId.BioRead,
        "reader": x.reader,
        "status": x.status,
        "type": x.type,
        "quality": x.quality,
        "data": x.data,
    },
    e.BioMatch: lambda x: {
        "event": EventId.BioMatch,
        "reader": x.reader,
        "status": x.status,
        "score": x.score,
    },
    e.Status: lambda x: {
        "event": EventId.Status,
        "type": x.type,
        "report": x.report,
    },
    e.PivData: lambda x: {
        "event": EventId.PivData,
        "data": x.data,
    },
    e.GenAuth: lambda x: {
        "event": EventId.GenAuth,
        "data": x.data,
    },
    e.CrAuth: lambda x: {
        "event": EventId.CrAuth,
        "data": x.data,
    },
    e.Notification: lambda x: {
        "event": EventId.Notification,
        **_notification_payload(x),
    },
}

_EVENT_DECODERS: dict[EventId, Callable[[Payload], e.Event]] = {
    EventId.CardRead: _decode_cardread,
    EventId.KeyPress: lambda p: e.KeyPress(
        reader_no=p["reader_no"], data=p["data"]
    ),
    EventId.ManufacturerReply: lambda p: e.ManufacturerReply(
        vendor_code=p["vendor_code"], data=p["data"]
    ),
    EventId.ManufacturerStatus: lambda p: e.ManufacturerStatus(data=p["data"]),
    EventId.ManufacturerError: lambda p: e.ManufacturerError(data=p["data"]),
    EventId.BioRead: lambda p: e.BioRead(
        reader=p["reader"],
        status=BioStatus(p["status"]),
        type=BioType(p["type"]),
        quality=p["quality"],
        data=p["data"],
    ),
    EventId.BioMatch: lambda p: e.BioMatch(
        reader=p["reader"],
        status=BioStatus(p["status"]),
        score=p["score"],
    ),
    EventId.Status: lambda p: e.Status(
        type=StatusReportType(p["type"]), report=p["report"]
    ),
    EventId.PivData: lambda p: e.PivData(data=p["data"]),
    EventId.GenAuth: lambda p: e.GenAuth(data=p["data"]),
    EventId.CrAuth: lambda p: e.CrAuth(data=p["data"]),
    EventId.Notification: lambda p: e.Notification(
        type=NotificationType(p["type"]),
        arg0=p.get("arg0", 0), arg1=p.get("arg1", 0),
        mp_type=p.get("mp_type", 0),
        object_id=p.get("object_id", 0), total=p.get("total", 0),
        offset=p.get("offset", 0), outcome=p.get("outcome", 0),
    ),
}


def encode_event(event: e.Event) -> Payload:
    """Convert an event dataclass into the dict `osdp._sys` expects."""
    encoder = _EVENT_ENCODERS.get(type(event))
    if encoder is None:
        raise MarshalError(f"Not an osdp event: {event!r}")
    return encoder(event)


def decode_event(payload: Payload) -> e.Event:
    """Convert a dict from `osdp._sys` back into an event dataclass."""
    try:
        return _EVENT_DECODERS[EventId(payload["event"])](payload)
    except (KeyError, ValueError) as err:
        raise MarshalError(f"Cannot decode event: {payload!r}") from err
