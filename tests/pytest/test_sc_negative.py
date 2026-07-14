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
