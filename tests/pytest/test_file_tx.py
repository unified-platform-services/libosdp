#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time
import random
import pytest
from osdp import *
from osdp import commands, events
from conftest import make_fifo_pair, cleanup_fifo_pair

FILE_ID = 13
FILE_SIZE = 4096

sender_data = [ random.randint(0, 255) for _ in range(FILE_SIZE) ]
receiver_data = [0] * FILE_SIZE

class SenderFileOps:
    """The CP side of a transfer: reads out of sender_data, never writes."""

    def open(self, file_id: int, size: int) -> int:
        assert file_id == FILE_ID
        assert size == 0 # sender has to return file_size so this must be 0
        return FILE_SIZE # we'll just send 4k of random data

    def read(self, size: int, offset: int) -> bytes:
        assert offset < FILE_SIZE
        if offset + size > FILE_SIZE:
            size = FILE_SIZE - offset
        return bytes(sender_data[offset:offset+size])

    def write(self, data: bytes, offset: int) -> int:
        # sender should not try to write anything!
        assert False

    def close(self, file_id: int) -> int:
        assert file_id == FILE_ID
        return 0

class ReceiverFileOps:
    """The PD side of a transfer: writes into receiver_data, never reads."""

    def open(self, file_id: int, size: int) -> int:
        assert file_id == FILE_ID
        assert size == FILE_SIZE
        return 0 # indicates success. Both CP and PD have the file_size now.

    def read(self, size: int, offset: int) -> bytes:
        # receiver should not read anything
        assert False

    def write(self, data: bytes, offset: int) -> int:
        assert offset + len(data) <= FILE_SIZE
        receiver_data[offset:offset + len(data)] = list(data)
        return len(data)

    def close(self, file_id: int) -> int:
        assert file_id == FILE_ID
        return 0

sender_fops = SenderFileOps()
receiver_fops = ReceiverFileOps()

pd_cap = PDCapabilities([])

f1, f2 = make_fifo_pair("file")
key = KeyStore.gen_key()

pd = PeripheralDevice(
    PDInfo(101, f1, scbk=key, flags=[ LibFlag.EnforceSecure ]),
    pd_cap,
    log_level=LogLevel.Debug
)
cp = ControlPanel([
        PDInfo(101, f2, scbk=key,
               flags=[ LibFlag.EnforceSecure, LibFlag.EnableNotification ]),
    ],
    log_level=LogLevel.Debug
)

def drain_events(address):
    while cp.get_event(address, timeout=0) is not None:
        pass

def wait_for_file_tx_done(address, expected_outcome, timeout=10.0,
                          dupe_window=0.3):
    """Wait for a FileTransferDone notification, then briefly check for dupes.

    The short post-receipt window (`dupe_window`) pins the "notification fires
    exactly once" guarantee from the refactor without stretching the test's
    wall-clock by the full `timeout`.
    """
    deadline = time.monotonic() + timeout
    notif = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        e = cp.get_event(address, timeout=max(0.05, remaining))
        if e is None:
            continue
        if not isinstance(e, events.Notification):
            continue
        if e.type != NotificationType.FileTransferDone:
            continue
        notif = e
        break
    assert notif is not None, "FileTransferDone notification not received"
    assert notif.arg0 == FILE_ID, \
        f"unexpected file_id in notification: {notif.arg0}"
    assert notif.arg1 == expected_outcome, \
        f"unexpected outcome: got {notif.arg1}, want {expected_outcome}"

    # Pin "fires exactly once" — drain for a short window and fail on any dupe
    dupe_deadline = time.monotonic() + dupe_window
    while time.monotonic() < dupe_deadline:
        e = cp.get_event(address, timeout=0.05)
        if e is None:
            continue
        if isinstance(e, events.Notification) and \
           e.type == NotificationType.FileTransferDone:
            raise AssertionError("FileTransferDone fired more than once")
    return notif

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    pd.start()
    cp.start()
    cp.sc_wait_all()
    yield
    teardown_test()

def teardown_test():
    cp.teardown()
    pd.teardown()
    cleanup_fifo_pair("file")

def test_file_transfer(utils):
    # Drain any notifications from prior tests / SC setup
    drain_events(101)

    # Register file OPs and kick off a transfer
    assert cp.register_file_ops(101, sender_fops)
    assert pd.register_file_ops(receiver_fops)
    file_tx_cmd = commands.FileTransfer(id=FILE_ID)
    assert cp.submit_command(101, file_tx_cmd)
    assert pd.get_command() == file_tx_cmd

    # Wait for the CP-side completion notification (the new canonical signal)
    wait_for_file_tx_done(101, FileTxOutcome.Ok)

    # Poll API must report "not in progress" after completion
    assert cp.get_file_tx_status(101) is None

    # Check if the data was sent properly
    assert sender_data == receiver_data

def test_file_tx_abort(utils):
    # Drain any notifications from prior tests / SC setup
    drain_events(101)

    # Register file OPs and kick off a transfer
    assert cp.register_file_ops(101, sender_fops)
    assert pd.register_file_ops(receiver_fops)
    file_tx_cmd = commands.FileTransfer(id=FILE_ID)
    assert cp.submit_command(101, file_tx_cmd)
    assert pd.get_command() == file_tx_cmd

    # Allow some number of transfers to go through
    time.sleep(0.5)

    file_tx_abort = commands.FileTransfer(id=FILE_ID, flags=FileTxFlag.Cancel)
    assert cp.submit_command(101, file_tx_abort)

    # Wait for the abort completion notification
    wait_for_file_tx_done(101, FileTxOutcome.Aborted)

    assert cp.get_file_tx_status(101) is None
    assert pd.get_file_tx_status() is None
