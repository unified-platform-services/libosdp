#!/usr/bin/env python3
#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import argparse

import serial

from osdp import (
    Channel,
    ControlPanel,
    KeyStore,
    LEDColor,
    PDInfo,
    commands,
    events,
)


class SerialChannel(Channel):
    def __init__(self, device: str, speed: int):
        self.dev = serial.Serial(device, speed, timeout=0)

    def read(self, max_bytes: int) -> bytes:
        return self.dev.read(max_bytes)

    def write(self, data: bytes) -> int:
        return self.dev.write(data)

    def flush(self) -> None:
        self.dev.flush()

    def __del__(self):
        self.dev.close()


parser = argparse.ArgumentParser(prog='cp_app', description="LibOSDP CP APP Example")
parser.add_argument("device", type=str, metavar="PATH", help="Path to serial device")
parser.add_argument("--baudrate", type=int, metavar="N", default=115200, help="Serial port's baud rate (default: 115200)")
parser.add_argument("--log-level", type=int, metavar="LEVEL", default=6, help="LibOSDP log level; can be 0-7 (default: 6)")
args = parser.parse_args()

## Describe the PD (setting scbk=None puts the PD in install mode)
channel = SerialChannel(args.device, args.baudrate)
pd_info = [
    PDInfo(101, channel, scbk=KeyStore.gen_key()),
]

## Create a CP device and kick-off the handler thread
cp = ControlPanel(pd_info, log_level=args.log_level)
cp.start()
cp.sc_wait_all()

## Flash the reader LED red for one second. Durations are in units of 100ms;
## on_count and off_count cannot both be zero.
led_cmd = commands.LED(
    reader=1,
    led_number=0,
    temporary=commands.TemporaryLEDParams(
        on_color=LEDColor.Red,
        off_color=LEDColor.Black,
        on_count=10,
        off_count=10,
        timer_count=10,
    ),
)

count = 0  # loop counter
while True:
    ## Send LED command to PD-0
    cp.submit_command(pd_info[0].address, led_cmd)

    ## Check if we have an event from PD. Each event is a distinct type, so
    ## match tells them apart and gives us the right fields for free.
    event = cp.get_event(pd_info[0].address, timeout=2)
    match event:
        case events.CardRead(data=data, format=fmt):
            print(f"PD-0 read a {fmt.name} card: {data.hex()}")
        case events.KeyPress(data=keys):
            print(f"PD-0 keypad: {keys!r}")
        case events.Status(type=report_type, report=report):
            print(f"PD-0 {report_type.name} status: {list(report)}")
        case None:
            pass
        case _:
            print(f"PD-0 sent event {event}")

    if count >= 5:
        break
    count += 1

cp.teardown()
