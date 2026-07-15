#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#
# The LibFlags nothing referenced.
#
# EnforceSecure and EnableNotification are used all over the suite. The other
# three -- IgnoreUnsolicited, CapturePackets and AllowEmptyEncryptedDataBlock --
# had zero mentions anywhere, and so did set_flag()/clear_flag().

import pytest

from osdp import (
    Capability,
    ControlPanel,
    KeyStore,
    LibFlag,
    LogLevel,
    PDCapabilities,
    PDInfo,
    PeripheralDevice,
    commands,
    events,
)

pytestmark = pytest.mark.integration

CAPABILITIES = PDCapabilities([(Capability.OutputControl, 1, 8)])


@pytest.fixture
def pair(fifo_pair):
    devices = []

    def make(cp_flags=None, pd_flags=None, sc=True):
        cp_chan, pd_chan = fifo_pair()
        key = KeyStore.gen_key() if sc else None
        pd = PeripheralDevice(
            PDInfo(101, pd_chan, scbk=key, flags=pd_flags or []),
            CAPABILITIES,
            log_level=LogLevel.Error,
        )
        pd.start()
        cp = ControlPanel(
            [PDInfo(101, cp_chan, scbk=key, flags=cp_flags or [])],
            log_level=LogLevel.Error,
        )
        cp.start()
        devices.extend([cp, pd])
        assert cp.online_wait(101, timeout=10), "PD never came online"
        return cp, pd

    yield make

    for device in reversed(devices):
        if device.thread:
            device.teardown()


def first_real_event(cp, address, timeout=3):
    """The next event that is not one of the library's own notifications."""
    for _ in range(int(timeout * 2)):
        event = cp.get_event(address, timeout=0.5)
        if event is not None and not isinstance(event, events.Notification):
            return event
    return None


def test_an_unsolicited_event_normally_reaches_the_cp(pair):
    # The control for the test below.
    cp, pd = pair()
    event = events.KeyPress(data=b"\x0a")
    assert pd.submit_event(event)
    assert first_real_event(cp, 101) == event


def test_ignore_unsolicited_does_not_suppress_a_known_event(pair):
    # The flag is narrower than its name suggests: it stops the CP erroring out
    # on an *unknown* unsolicited reply to a POLL. A KeyPress is a known reply
    # and must still be delivered. (Provoking an unknown one needs a PD that
    # speaks off-spec, which the public API gives no way to build -- the C
    # suite covers that side.)
    cp, pd = pair(cp_flags=[LibFlag.IgnoreUnsolicited])

    event = events.KeyPress(data=b"\x0a")
    assert pd.submit_event(event)
    assert first_real_event(cp, 101) == event

    assert cp.is_online(101)
    command = commands.Output(output_no=1, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command


def test_allow_empty_encrypted_data_block_still_talks(pair):
    cp, pd = pair(
        cp_flags=[
            LibFlag.EnforceSecure,
            LibFlag.AllowEmptyEncryptedDataBlock,
        ],
        pd_flags=[
            LibFlag.EnforceSecure,
            LibFlag.AllowEmptyEncryptedDataBlock,
        ],
    )
    assert cp.sc_wait(101, timeout=10)

    command = commands.Output(output_no=2, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command


def test_capture_packets_writes_a_pcap(pair, tmp_path, monkeypatch):
    # osdp_diag.c names the file osdp-trace-<...>.pcap and drops it in the
    # working directory, so give it one of its own.
    monkeypatch.chdir(tmp_path)

    cp, pd = pair(cp_flags=[LibFlag.CapturePackets])
    command = commands.Output(output_no=3, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command

    # The capture is finalised when the context goes away.
    cp.teardown()

    traces = list(tmp_path.glob("osdp-trace-*.pcap"))
    assert traces, f"no pcap written into {tmp_path}"
    assert traces[0].stat().st_size > 0, "the pcap is empty"


@pytest.mark.parametrize(
    "flag",
    [
        LibFlag.IgnoreUnsolicited,
        LibFlag.EnableNotification,
        LibFlag.AllowEmptyEncryptedDataBlock,
    ],
    ids=lambda f: f.name,
)
def test_a_flag_can_be_set_and_cleared_at_runtime(pair, flag):
    # osdp_cp_modify_flag, which nothing called at all.
    cp, pd = pair()

    assert cp.set_flag(101, flag)
    assert cp.clear_flag(101, flag)
    assert cp.set_flag(101, flag)

    # Toggling a flag on a live link must not disturb it.
    assert cp.is_online(101)
    command = commands.Output(output_no=4, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command


def test_enable_notification_delivers_the_librarys_own_events(pair):
    # The counterpart to the notification-free CPs above: with the flag on, the
    # library's synthesized notifications show up in the event queue.
    cp, pd = pair(cp_flags=[LibFlag.EnableNotification])

    seen = []
    for _ in range(20):
        event = cp.get_event(101, timeout=0.5)
        if isinstance(event, events.Notification):
            seen.append(event)
            break
    assert seen, "EnableNotification produced no notifications"
