#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import pytest

from osdp import *
from osdp import events
from conftest import make_fifo_pair, cleanup_fifo_pair, wait_for_non_notification_event

pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 8),
    (Capability.ContactStatusMonitoring, 1, 8),
    (Capability.LEDControl, 1, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
    (Capability.Readers, 1, 2),
])

key = KeyStore.gen_key()
f1, f2 = make_fifo_pair("events")

secure_pd = PeripheralDevice(
    PDInfo(101, f1, scbk=key, flags=[ LibFlag.EnforceSecure ]),
    pd_cap,
    log_level=LogLevel.Debug
)

pd_list = [
    secure_pd,
]

cp = ControlPanel([
        PDInfo(101, f2, scbk=key, flags=[ LibFlag.EnforceSecure, LibFlag.EnableNotification ])
    ],
    log_level=LogLevel.Debug
)

@pytest.fixture(scope='module', autouse=True)
def setup_test():
    for pd in pd_list:
        pd.start()
    cp.start()
    if not cp.online_wait_all(timeout=10):
        teardown_test()
        pytest.fail("Failed to bring all PDs online within timeout")
    yield
    teardown_test()

def teardown_test():
    cp.teardown()
    for pd in pd_list:
        pd.teardown()
    cleanup_fifo_pair("events")

def check_event(event):
    wait_for_non_notification_event(cp, secure_pd.address, event)

def test_event_keypad():
    event = events.KeyPress(
        reader_no=1,
        data=bytes([9,1,9,2,6,3,1,7,7,0]),
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_mfg_reply():
    event = events.ManufacturerReply(
        vendor_code=0x153,
        data=bytes([0x10,9,1,9,2,6,3,1,7,7,0]),
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_mfg_status():
    event = events.ManufacturerStatus(
        data=bytes([0xDE, 0xAD, 0xBE, 0xEF]),
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_mfg_error():
    event = events.ManufacturerError(
        data=bytes([0xBA, 0xAD, 0xF0, 0x0D]),
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_mfg_status_no_data():
    # The spec places no lower bound on the payload of these replies
    event = events.ManufacturerStatus(
        data=bytes([]),
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_cardread_wiegand():
    event = events.CardRead(
        reader_no=1,
        direction=0, # has to be zero
        format=CardFormat.Wiegand,
        data=bytes([0x55, 0xAA]),
        bits=16,
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_input():
    event = events.Status(
        type=StatusReportType.Input,
        report=bytes([1, 0, 1, 0, 1, 0, 1, 0])
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_output():
    event = events.Status(
        type=StatusReportType.Output,
        report=bytes([0, 1, 0, 1, 0, 1, 0, 1])
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_local():
    # Local status is always two entries: tamper and power.
    event = events.Status(
        type=StatusReportType.Local,
        report=bytes([0, 1])
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_reader():
    # Reader status carries one byte per attached reader; the PD advertised 2.
    event = events.Status(
        type=StatusReportType.Reader,
        report=bytes([0, 2])  # reader 0 normal, reader 1 tamper
    )
    secure_pd.submit_event(event)
    check_event(event)

def test_event_status_rejects_more_entries_than_capability():
    # The PD advertised 8 contact points (ContactStatusMonitoring, num_items=8),
    # so a status report carrying more entries than that must be rejected at
    # submission time rather than silently dropped later.
    event = events.Status(
        type=StatusReportType.Input,
        report=bytes([1, 0, 1, 0, 1, 0, 1, 0, 1])  # 9 entries > capability of 8
    )
    assert secure_pd.submit_event(event) is False

def test_event_status_rejects_partial_report():
    # A report must be full: one entry per tracked entity. The PD advertised 8
    # contacts, so a partial (under-full) report is rejected too.
    event = events.Status(
        type=StatusReportType.Input,
        report=bytes([1, 0, 1])  # 3 entries < capability of 8
    )
    assert secure_pd.submit_event(event) is False

def test_event_reader_rejects_more_readers_than_capability():
    # The PD advertised 2 readers, so a 3-entry reader status is rejected.
    event = events.Status(
        type=StatusReportType.Reader,
        report=bytes([0, 0, 0])  # 3 entries > 2 readers
    )
    assert secure_pd.submit_event(event) is False

def test_event_local_rejects_wrong_entry_count():
    # Local status must be exactly two entries (tamper, power).
    event = events.Status(
        type=StatusReportType.Local,
        report=bytes([0, 1, 0])  # 3 entries > 2
    )
    assert secure_pd.submit_event(event) is False
