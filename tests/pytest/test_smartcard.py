#
#  Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
import pytest

from osdp import *
from osdp import commands, events
from conftest import (
    MultidropBus,
    assert_command_received,
    wait_for_notification_event,
    wait_for_non_notification_event,
)

pd_addr = 101
no_cap_pd_addr = 102

bus = MultidropBus(2)
chn = bus.multidrop_channel()

key = KeyStore.gen_key()

# Multipart notification families (C enum osdp_mp_msg_type)
MP_MSG_PIV = 2
MP_MSG_GENAUTH = 3
MP_MSG_CRAUTH = 4

pd_cap = PDCapabilities([
    (Capability.SmartCard, 1, 1),
])

pd_info_list = [
    PDInfo(pd_addr, chn, scbk=key,
           flags=[LibFlag.EnforceSecure, LibFlag.EnableNotification]),
    PDInfo(no_cap_pd_addr, chn, flags=[LibFlag.EnableNotification]),
]

pd = PeripheralDevice(
    PDInfo(pd_addr, bus.pd_channel(0), scbk=key,
           flags=[LibFlag.EnforceSecure]),
    pd_cap,
    log_level=LogLevel.Debug,
)

# Advertises no smartcard support: must NAK the PIV commands.
no_cap_pd = PeripheralDevice(
    PDInfo(no_cap_pd_addr, bus.pd_channel(1)),
    PDCapabilities([]),
    log_level=LogLevel.Debug,
)

pd_list = [pd, no_cap_pd]

cp = ControlPanel(pd_info_list, log_level=LogLevel.Debug)

# Fill the whole payload buffer so both the command and the reply legs span
# multiple OSDP packets (multi-part fragmentation) on the wire.
CHALLENGE = bytes((0x10 + i) & 0xFF for i in range(commands.MAX_PIV_DATA_LEN))
REPLY = bytes((0xC0 + i) & 0xFF for i in range(commands.MAX_PIV_DATA_LEN))


@pytest.fixture(scope='module', autouse=True)
def setup_test():
    for dev in pd_list:
        dev.start()
    cp.start()
    if not cp.online_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to bring all PDs online within timeout")
    yield
    teardown_test()


def teardown_test():
    cp.teardown()
    for dev in pd_list:
        dev.teardown()


def drain_events(address):
    while cp.get_event(address, timeout=0.1) is not None:
        pass


def run_smartcard_op(cmd, reply_event, mp_msg, object_id, inline):
    """Round-trip one smartcard op and pin its terminal notification.

    The engine finishes the op (MultipartDone) before the reassembled reply
    event is dispatched, so consume the notification first, then the reply.
    """
    drain_events(pd_addr)

    def cmd_handler(command):
        if inline and isinstance(command, type(cmd)):
            pd.submit_event(reply_event)
        return None

    pd.set_command_handler(cmd_handler)
    try:
        assert cp.submit_command(pd_addr, cmd)
        assert_command_received(pd, cmd)
        if not inline:
            time.sleep(0.2)  # app takes its time; CP must ride the ACKs
            assert pd.submit_event(reply_event)

        done = events.Notification(
            type=NotificationType.MultipartDone,
            mp_type=mp_msg,
            object_id=object_id,
            total=len(reply_event.data),
            offset=len(reply_event.data),
            outcome=int(FileTxOutcome.Ok),
        )
        wait_for_notification_event(cp, pd_addr, done)
        wait_for_non_notification_event(cp, pd_addr, reply_event)
    finally:
        pd.set_command_handler(None)


@pytest.mark.parametrize("inline", [True, False], ids=["inline", "deferred"])
def test_pivdata_roundtrip(inline):
    run_smartcard_op(
        commands.PivData(oid=b"\x5f\xc1\x02", element=7, offset=0),
        events.PivData(data=REPLY),
        MP_MSG_PIV, object_id=7, inline=inline,
    )


@pytest.mark.parametrize("inline", [True, False], ids=["inline", "deferred"])
def test_genauth_roundtrip(inline):
    run_smartcard_op(
        commands.GenAuth(algorithm=0xA7, key=0x9E, data=CHALLENGE),
        events.GenAuth(data=REPLY),
        MP_MSG_GENAUTH, object_id=0x9E, inline=inline,
    )


@pytest.mark.parametrize("inline", [True, False], ids=["inline", "deferred"])
def test_crauth_roundtrip(inline):
    run_smartcard_op(
        commands.CrAuth(algorithm=0x11, key=0x22, data=CHALLENGE),
        events.CrAuth(data=REPLY),
        MP_MSG_CRAUTH, object_id=0x22, inline=inline,
    )


def test_pivdata_rejected_without_smartcard_cap():
    """A PD without OSDP_PD_CAP_SMART_CARD_SUPPORT NAKs the command. The op
    must abort with MultipartDone(Aborted) — and the NAK is an app-level
    refusal, so the link must survive it."""
    drain_events(no_cap_pd_addr)
    cmd = commands.PivData(oid=b"\x01\x02\x03", element=1, offset=0)
    assert cp.submit_command(no_cap_pd_addr, cmd)

    done = events.Notification(
        type=NotificationType.MultipartDone,
        mp_type=MP_MSG_PIV,
        object_id=1,
        total=0,
        offset=0,
        outcome=int(FileTxOutcome.Aborted),
    )
    wait_for_notification_event(cp, no_cap_pd_addr, done)
    time.sleep(0.5)
    assert cp.is_online(no_cap_pd_addr), "app-level NAK must not drop the link"


def test_second_op_rejected_while_busy():
    """Only one smartcard op may be in flight per PD (OSDP 2.2 5.10.2)."""
    drain_events(pd_addr)

    cmd = commands.PivData(oid=b"\x5f\xc1\x02", element=7, offset=0)
    reply = events.PivData(data=REPLY)

    assert cp.submit_command(pd_addr, cmd)
    assert not cp.submit_command(pd_addr, cmd)

    # Let the op finish so later tests start clean.
    assert_command_received(pd, cmd)
    assert pd.submit_event(reply)
    wait_for_non_notification_event(cp, pd_addr, reply)
