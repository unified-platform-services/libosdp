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
