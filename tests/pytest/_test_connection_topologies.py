#
#  Copyright (c) 2021-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
#  SPDX-License-Identifier: Apache-2.0
#

import time

from osdp import *
from osdp import commands
from conftest import MultidropBus

def test_daisy_chained_pds(utils):
    # Two PDs sharing one RS-485 bus with a single CP
    bus = MultidropBus(2)
    pd0_key = utils.ks.gen_key()
    pd1_key = utils.ks.gen_key()
    pd0 = utils.create_pd(
        PDInfo(101, bus.pd_channel(0), scbk=pd0_key, name='daisy_chained_pd')
    )
    pd1 = utils.create_pd(
        PDInfo(102, bus.pd_channel(1), scbk=pd1_key, name='daisy_chained_pd')
    )
    chn = bus.multidrop_channel()
    cp = utils.create_cp([
        PDInfo(101, chn, scbk=pd0_key, name='daisy_chained_pd'),
        PDInfo(102, chn, scbk=pd1_key, name='daisy_chained_pd'),
    ], sc_wait=True)

    # Exercise both the PDs while checking whether they are alive and well
    for _ in range(5):
        test_cmd = commands.Comset(address=101, baud_rate=9600)
        assert cp.is_sc_active(101)
        assert cp.submit_command(101, test_cmd)
        assert pd0.get_command() == test_cmd

        test_cmd = commands.Comset(address=102, baud_rate=9600)
        assert cp.is_sc_active(102)
        assert cp.submit_command(102, test_cmd)
        assert pd1.get_command() == test_cmd

        time.sleep(1)

    # Cleanup
    cp.teardown()
    pd1.teardown()
    pd0.teardown()
