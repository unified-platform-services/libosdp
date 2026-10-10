#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

"""Per-wheel smoke test, run by cibuildwheel against each built wheel.

Proves the wheel's native extension loads and works on this platform and
Python: a CP and a PD in one process bring up a secure channel over an
in-memory link and exchange a command and an event. Portable on purpose:
no FIFOs or fcntl, so it also runs on Windows.
"""

import faulthandler
import os
import sys
import threading
import time
from importlib.metadata import version

# This file sits next to the osdp source package; without this the source tree
# would shadow the installed wheel that is supposed to be under test.
_HERE = os.path.dirname(os.path.realpath(__file__))
sys.path[:] = [p for p in sys.path
               if os.path.normcase(os.path.realpath(p or ".")) !=
               os.path.normcase(_HERE)]

import osdp  # noqa: E402
from osdp import (BuzzerControlCode, Capability, Channel, ControlPanel,
                  KeyStore, LibFlag, PDCapabilities, PDInfo, PeripheralDevice,
                  commands, events)

ADDRESS = 101
TIMEOUT = 20


class _Link:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.to_pd = bytearray()
        self.to_cp = bytearray()


class _End(Channel):
    def __init__(self, link: _Link, rx: bytearray, tx: bytearray) -> None:
        super().__init__()
        self._link, self._rx, self._tx = link, rx, tx

    def read(self, max_bytes: int) -> bytes:
        with self._link.lock:
            data = bytes(self._rx[:max_bytes])
            del self._rx[:max_bytes]
            return data

    def write(self, buf: bytes) -> int:
        with self._link.lock:
            self._tx.extend(buf)
            return len(buf)

    def flush(self) -> None:
        with self._link.lock:
            self._rx.clear()


def _next_real_event(cp: ControlPanel) -> object:
    deadline = time.monotonic() + TIMEOUT
    while time.monotonic() < deadline:
        ev = cp.get_event(ADDRESS, timeout=1)
        if ev is not None and not isinstance(ev, events.Notification):
            return ev
    raise SystemExit("smoke: no event reached the CP")


def _teardown_quietly(started: list) -> None:
    for obj in started:
        try:
            obj.teardown()
        except Exception as exc:
            print(f"smoke: teardown failed: {exc!r}", file=sys.stderr)


def main() -> int:
    # A hang inside native code must not stall the CI job.
    faulthandler.dump_traceback_later(TIMEOUT * 4, exit=True)
    if os.path.realpath(osdp.__file__).startswith(_HERE + os.sep):
        raise SystemExit(f"smoke: osdp imported from source tree: {osdp.__file__}")
    print(f"smoke: libosdp {version('libosdp')} on Python {sys.version.split()[0]}")
    link = _Link()
    key = KeyStore.gen_key()
    flags = [LibFlag.EnforceSecure]
    cp = ControlPanel([PDInfo(ADDRESS, _End(link, link.to_cp, link.to_pd),
                              scbk=key, flags=flags)])
    pd = PeripheralDevice(PDInfo(ADDRESS, _End(link, link.to_pd, link.to_cp),
                                 scbk=key, flags=flags),
                          PDCapabilities([(Capability.AudibleControl, 1, 1)]))
    started: list = []
    try:
        pd.start()
        started.append(pd)
        cp.start()
        started.append(cp)
        if not cp.sc_wait_all(timeout=TIMEOUT):
            raise SystemExit("smoke: secure channel did not come up")

        sent = commands.Buzzer(control_code=BuzzerControlCode.DefaultTone,
                               on_count=1, off_count=1, rep_count=1)
        if not cp.submit_command(ADDRESS, sent):
            raise SystemExit("smoke: CP refused the command")
        got = pd.get_command(timeout=TIMEOUT)
        if got != sent:
            raise SystemExit(f"smoke: PD got {got!r}, expected {sent!r}")

        event = events.KeyPress(reader_no=0, data=b"1234")
        if not pd.submit_event(event):
            raise SystemExit("smoke: PD refused the event")
        got_event = _next_real_event(cp)
        if got_event != event:
            raise SystemExit(f"smoke: CP got {got_event!r}, expected {event!r}")
    finally:
        _teardown_quietly(started)
    print("smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
