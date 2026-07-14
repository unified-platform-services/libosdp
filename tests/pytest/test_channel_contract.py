#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#
# What the binding layer does with a channel that misbehaves.
#
# osdp_sys/utils.c wraps a Python Channel in C callbacks that the library calls
# from inside refresh(). These tests drive refresh() directly rather than
# through a refresh thread, so a failure is deterministic and lands on the
# assertion instead of in a thread nobody is watching.

import pytest

from osdp import Channel, PDCapabilities, _sys

pytestmark = pytest.mark.unit


class WellBehavedChannel(Channel):
    """Idle, but honest: the contract every other channel here breaks."""

    def read(self, max_bytes: int) -> bytes:
        return b""

    def write(self, buf: bytes) -> int:
        return len(buf)

    def flush(self) -> None:
        pass


class RecordingChannel(WellBehavedChannel):
    def __init__(self) -> None:
        self.reads = 0
        self.writes = 0
        self.flushes = 0

    def read(self, max_bytes: int) -> bytes:
        self.reads += 1
        return b""

    def write(self, buf: bytes) -> int:
        self.writes += 1
        return len(buf)

    def flush(self) -> None:
        self.flushes += 1


class ReadReturnsNonBytes(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        return "not bytes"  # type: ignore[return-value]


class ReadIgnoresMaxBytes(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        return b"\x00" * (max_bytes + 64)


class ReadRaises(WellBehavedChannel):
    def read(self, max_bytes: int) -> bytes:
        raise RuntimeError("read blew up")


class WriteReturnsNonInt(WellBehavedChannel):
    def write(self, buf: bytes) -> int:
        return None  # type: ignore[return-value]


class WriteRaises(WellBehavedChannel):
    def write(self, buf: bytes) -> int:
        raise RuntimeError("write blew up")


def make_cp(channel):
    return _sys.ControlPanel([{"address": 101, "flags": 0, "channel": channel}])


def make_pd(channel):
    return _sys.PeripheralDevice(
        {
            "address": 101,
            "flags": 0,
            "version": 1,
            "model": 1,
            "vendor_code": 0,
            "firmware_version": 0,
            "serial_number": 0,
            "channel": channel,
        }
    )


def test_channel_is_driven_from_c():
    channel = RecordingChannel()
    cp = make_cp(channel)
    for _ in range(20):
        cp.refresh()

    # The CP has a PD to poll, so it must have written; it always drains the
    # read side; and flush() -- which nothing else in the suite asserts -- is
    # called by the library, not by us.
    assert channel.writes > 0
    assert channel.reads > 0
    assert channel.flushes > 0


@pytest.mark.parametrize("channel", [ReadReturnsNonBytes(), WriteReturnsNonInt()])
def test_a_channel_of_the_wrong_type_is_a_failed_transfer_not_a_crash(channel):
    # The C side cannot do anything with a str from read() or a None from
    # write(), so it reports -1 to the library and the link simply never comes
    # up. No exception, and above all no crash.
    cp = make_cp(channel)
    for _ in range(10):
        cp.refresh()
    assert cp.status() == 0


def test_a_read_longer_than_requested_is_refused():
    cp = make_cp(ReadIgnoresMaxBytes())
    with pytest.raises(TypeError, match="read callback maxlen not respected"):
        for _ in range(10):
            cp.refresh()


@pytest.mark.parametrize(
    "channel, message",
    [
        (ReadRaises(), "read blew up"),
        (WriteRaises(), "write blew up"),
    ],
)
def test_an_exception_from_a_channel_reaches_the_caller(channel, message):
    # The callbacks run inside refresh() and can only tell the library "-1".
    # refresh() used to return None with the exception still pending, which
    # CPython reported as a SystemError naming some unrelated object. The
    # caller should get their own exception back instead.
    cp = make_cp(channel)
    with pytest.raises(RuntimeError, match=message):
        for _ in range(10):
            cp.refresh()


def test_a_pd_also_surfaces_a_channel_exception():
    # Only the read path: a PD writes when it answers a command, and nothing
    # ever arrives over an idle channel, so its write() is never reached.
    pd = make_pd(ReadRaises())
    with pytest.raises(RuntimeError, match="read blew up"):
        for _ in range(10):
            pd.refresh()


def test_a_command_callback_returning_the_wrong_shape_does_not_kill_the_pd():
    # pd.c wants a (status, reply) tuple back. Anything else is reported to the
    # library as -1 -- the PD just does not answer that command.
    pd = make_pd(WellBehavedChannel())
    pd.set_command_callback(lambda command: "not a tuple")
    for _ in range(10):
        pd.refresh()
    assert pd.is_online() is False


def test_an_incomplete_channel_cannot_be_built():
    # Channel is an ABC; a subclass that forgets a method is caught by Python
    # long before the C layer ever sees it.
    class MissingFlush(Channel):
        def read(self, max_bytes: int) -> bytes:
            return b""

        def write(self, buf: bytes) -> int:
            return len(buf)

    with pytest.raises(TypeError):
        MissingFlush()  # type: ignore[abstract]
