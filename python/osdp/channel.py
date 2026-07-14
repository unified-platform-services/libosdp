#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""The transport a CP and PD talk over."""

import abc

__all__ = ["Channel"]


class Channel(abc.ABC):
    """A byte stream between a CP and one or more PDs.

    Subclass this to talk over a serial port, a socket, or anything else. On an
    RS-485 bus every PD shares one channel, so reads may return bytes meant for
    another device; the library sorts that out.

    The library calls these from its refresh thread, so they must not block for
    long: a PD has to be serviced at least every 50ms to stay in spec.
    """

    @abc.abstractmethod
    def read(self, max_bytes: int) -> bytes:
        """Return up to `max_bytes` bytes, or b"" if none are waiting.

        Must not block waiting for data to arrive.
        """

    @abc.abstractmethod
    def write(self, buf: bytes) -> int:
        """Send as much of `buf` as possible; return how much was sent."""

    @abc.abstractmethod
    def flush(self) -> None:
        """Discard anything buffered. May do nothing if that has no meaning."""
