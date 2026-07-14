#!/usr/bin/env python3
#
#  Copyright (c) 2020-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import argparse
import signal

import serial

from osdp import (
    Capability,
    CardFormat,
    Channel,
    Command,
    NakCode,
    NakError,
    PDCapabilities,
    PDInfo,
    PeripheralDevice,
    StatusReportType,
    commands,
    events,
)

exit_event = 0


def signal_handler(sig, frame):
    global exit_event
    print('Received SIGINT, quitting...')
    exit_event = 1


signal.signal(signal.SIGINT, signal_handler)


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


parser = argparse.ArgumentParser(prog='pd_app', description="LibOSDP PD APP Example")
parser.add_argument("device", type=str, metavar="PATH", help="Path to serial device")
parser.add_argument("--baudrate", type=int, metavar="N", default=115200, help="Serial port's baud rate (default: 115200)")
parser.add_argument("--log-level", type=int, metavar="N", default=6, help="LibOSDP log level; can be 0-7 (default: 6)")
args = parser.parse_args()

## Describe the PD (setting scbk=None puts the PD in install mode)
channel = SerialChannel(args.device, args.baudrate)
pd_info = PDInfo(101, channel, scbk=None)

## Indicate the PD's capabilities to LibOSDP.
pd_cap = PDCapabilities([
    (Capability.OutputControl, 1, 1),
    (Capability.LEDControl, 2, 1),
    (Capability.AudibleControl, 1, 1),
    (Capability.TextOutput, 1, 1),
    (Capability.ContactStatusMonitoring, 1, 4),
])

## Pretend these are the PD's input contacts.
INPUT_CONTACTS = bytes([0, 1, 0, 1])


def command_handler(cmd: Command) -> Command | None:
    """Handle a command from the CP.

    Return None to accept it, return a command to answer it inline, or raise
    NakError to decline it.
    """
    print(f"PD: Received command: {cmd}")

    match cmd:
        case commands.Status(type=StatusReportType.Input):
            ## Answer the query inline with our contact states.
            return commands.Status(
                type=StatusReportType.Input, report=INPUT_CONTACTS
            )
        case commands.LED(permanent=permanent) if permanent is not None:
            print(f"PD: LED set to {permanent.on_color.name}")
        case commands.Buzzer(rep_count=rep_count):
            print(f"PD: buzz {rep_count} times")
        case commands.BioRead() | commands.BioMatch():
            ## We have no biometric reader; decline rather than pretend.
            raise NakError(NakCode.BioType)

    return None


## Create a PD device and kick-off the handler thread
pd = PeripheralDevice(
    pd_info, pd_cap, log_level=args.log_level, command_handler=command_handler
)
pd.start()
pd.sc_wait(timeout=-1)

## A card read to report to the CP. For the raw formats the length is in bits;
## here the card is ASCII, so it is just bytes.
card_event = events.CardRead(
    reader_no=1,
    direction=1,
    format=CardFormat.ASCII,
    data=bytes([9, 1, 9, 2, 6, 3, 1, 7, 7, 0]),
)

while not exit_event:
    ## Commands are dispatched to command_handler above; this loop just reports
    ## a card read every few seconds.
    if pd.get_command(timeout=5) is not None:
        pd.submit_event(card_event)

pd.teardown()
