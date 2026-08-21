#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#
# Secure channel, from the other side: what has to *fail*.
#
# The rest of the suite only ever checks that a correctly keyed pair reaches
# sc_active. That says nothing about whether a wrongly keyed one is refused,
# which is the property that actually matters. LibFlag.EnforceSecure in
# particular is used in a dozen places without its enforcement ever being
# asserted.

import time

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
)

pytestmark = [pytest.mark.integration, pytest.mark.slow]

CAPABILITIES = PDCapabilities([(Capability.OutputControl, 1, 8)])


@pytest.fixture
def pair(fifo_pair):
    """Builds a CP/PD over a private FIFO pair, with per-side keys and flags."""
    devices = []

    def make(cp_scbk, pd_scbk, cp_flags=None, pd_flags=None):
        cp_chan, pd_chan = fifo_pair()
        pd = PeripheralDevice(
            PDInfo(
                101, pd_chan, scbk=pd_scbk, flags=pd_flags or [],
            ),
            CAPABILITIES,
            log_level=LogLevel.Error,
        )
        pd.start()
        cp = ControlPanel(
            [PDInfo(101, cp_chan, scbk=cp_scbk, flags=cp_flags or [])],
            log_level=LogLevel.Error,
        )
        cp.start()
        devices.extend([cp, pd])
        return cp, pd

    yield make

    for device in reversed(devices):
        if device.thread:
            device.teardown()


def test_a_mismatched_key_never_brings_the_secure_channel_up(pair):
    store = KeyStore()
    cp, pd = pair(cp_scbk=store.gen_key(), pd_scbk=store.gen_key())

    assert not cp.sc_wait(101, timeout=5), "SC came up with mismatched keys"
    assert not cp.is_sc_active(101)
    assert not pd.is_sc_active()


def test_a_mismatched_key_under_enforce_secure_keeps_the_pd_offline(pair):
    # Without EnforceSecure a PD may fall back to clear text. With it, a failed
    # handshake has to mean no link at all.
    store = KeyStore()
    cp, pd = pair(
        cp_scbk=store.gen_key(),
        pd_scbk=store.gen_key(),
        cp_flags=[LibFlag.EnforceSecure],
        pd_flags=[LibFlag.EnforceSecure],
    )

    assert not cp.online_wait(101, timeout=5), "PD came online without SC"
    assert not cp.is_sc_active(101)


def test_an_enforce_secure_pd_cannot_be_built_without_a_key(fifo_pair):
    # EnforceSecure and "no SCBK" are contradictory -- install mode exists to
    # talk to a PD that has no key yet. The library refuses the combination
    # outright rather than quietly downgrading, so it never reaches the wire.
    _, pd_chan = fifo_pair()
    with pytest.raises(Exception, match="Failed to setup PD"):
        PeripheralDevice(
            PDInfo(101, pd_chan, scbk=None, flags=[LibFlag.EnforceSecure]),
            CAPABILITIES,
            log_level=LogLevel.Error,
        )


def test_enforce_secure_refuses_an_unkeyed_pd(pair):
    # The case that matters: the CP demands a secure channel, and the PD on the
    # other end has no key at all. It must never come online -- this is the
    # whole point of the flag, and nothing asserted it until now.
    #
    # The unkeyed PD now reports communication security at compliance level 0,
    # so the CP refuses it at capability detection rather than after a failed
    # handshake. Either way it stays offline.
    key = KeyStore().gen_key()
    cp, pd = pair(
        cp_scbk=key,
        pd_scbk=None,
        cp_flags=[LibFlag.EnforceSecure],
        pd_flags=[],
    )

    assert not cp.online_wait(101, timeout=5), "an unkeyed PD came online"
    assert not cp.is_sc_active(101)


def test_a_command_is_refused_while_the_secure_channel_is_down(pair):
    store = KeyStore()
    cp, pd = pair(
        cp_scbk=store.gen_key(),
        pd_scbk=store.gen_key(),
        cp_flags=[LibFlag.EnforceSecure],
        pd_flags=[LibFlag.EnforceSecure],
    )
    assert not cp.sc_wait(101, timeout=5)

    # The command may well be queued -- what must not happen is it reaching a
    # PD the CP has not authenticated.
    cp.submit_command(101, commands.Output(output_no=0, control_code=1))
    assert pd.get_command(timeout=2) is None, (
        "a command crossed an unauthenticated link"
    )


def test_the_matching_key_still_works(pair):
    # The control: same harness, same flags, one key. If this fails, the tests
    # above are proving nothing.
    key = KeyStore().gen_key()
    cp, pd = pair(
        cp_scbk=key,
        pd_scbk=key,
        cp_flags=[LibFlag.EnforceSecure],
        pd_flags=[LibFlag.EnforceSecure],
    )

    assert cp.sc_wait(101, timeout=10), "SC did not come up with matching keys"
    assert cp.is_sc_active(101)
    assert pd.is_sc_active()

    command = commands.Output(output_no=1, control_code=1, timer_count=10)
    assert cp.submit_command(101, command)
    assert pd.get_command(timeout=5) == command


def test_a_keyless_pd_refuses_the_secure_channel(pair):
    # A PD with no key and no InstallMode used to answer a challenge on SCBK-D,
    # the key printed in the spec -- anyone on the bus could bring up a
    # "secure" session and then key the PD. It must decline OSDP-SC entirely
    # and let the link run in the clear instead.
    cp, pd = pair(cp_scbk=KeyStore().gen_key(), pd_scbk=None)

    assert cp.online_wait(101, timeout=5), "the plaintext link never came up"
    assert not cp.sc_wait(101, timeout=5), "SC came up without a key"
    assert not cp.is_sc_active(101)
    assert not pd.is_sc_active()
    assert cp.check_capability(101, Capability.CommunicationSecurity) == (0, 0)


def test_install_mode_is_the_only_route_to_scbk_d(pair):
    # The same pair, with the PD opted in, does bring the channel up. This is
    # the line between the two: the flag, not the absence of a key.
    cp, pd = pair(
        cp_scbk=KeyStore().gen_key(),
        pd_scbk=None,
        pd_flags=[LibFlag.InstallMode],
    )

    assert cp.sc_wait(101, timeout=10), "install mode did not reach SC"


def test_a_secured_pd_is_never_downgraded_to_scbk_d(fifo_pair):
    # The swap: a device answering an address that already ran a real secure
    # channel fails the challenge on purpose and offers install mode. Falling
    # back to SCBK-D here would hand it a session on a published key, and the
    # CP would then key it with the real SCBK over that session. Once a PD has
    # come up on the configured key, SCBK-D is off the table for that slot.
    key = KeyStore().gen_key()
    cp_chan, pd_chan = fifo_pair()
    devices = []
    try:
        pd = PeripheralDevice(
            PDInfo(101, pd_chan, scbk=key), CAPABILITIES,
            log_level=LogLevel.Error,
        )
        pd.start()
        devices.append(pd)
        cp = ControlPanel(
            [PDInfo(101, cp_chan, scbk=key)], log_level=LogLevel.Error,
        )
        cp.start()
        devices.append(cp)
        assert cp.sc_wait(101, timeout=10), "the honest PD never reached SC"

        # Swap in a keyless install-mode PD on the same wire. Cycling the
        # slot rather than letting the CP time out keeps the test off the
        # five-minute offline dwell.
        # disable_pd() only posts a request; the FSM acts on it a refresh
        # later. Wait for the slot to actually go quiet, or enable_pd() below
        # is refused and the assertions read the honest session's state.
        assert cp.disable_pd(101)
        deadline = time.monotonic() + 5
        while cp.is_pd_enabled(101) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert not cp.is_pd_enabled(101), "PD never reached the disabled state"

        pd.teardown()
        devices.remove(pd)
        # The honest PD's last reply may still be in the pipe, and the fresh
        # device starts its sequence at zero. Drain both directions so the
        # swapped pair does not have to resync through the offline dwell.
        cp_chan.flush()
        pd_chan.flush()
        impostor = PeripheralDevice(
            PDInfo(101, pd_chan, scbk=None, flags=[LibFlag.InstallMode]),
            CAPABILITIES, log_level=LogLevel.Error,
        )
        impostor.start()
        devices.append(impostor)
        assert cp.enable_pd(101)

        assert cp.online_wait(101, timeout=30), "the swapped PD never answered"
        assert not cp.sc_wait(101, timeout=15), "CP re-keyed on SCBK-D"
        assert not impostor.is_sc_active()
    finally:
        for device in reversed(devices):
            if device.thread:
                device.teardown()
