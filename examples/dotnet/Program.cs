// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Examples;

return args switch
{
    [] or ["loopback"] => await LoopbackExample.RunAsync(),
    ["cp", var port] => await ControlPanelExample.RunAsync(port),
    ["pd", var port] => await PeripheralDeviceExample.RunAsync(port),
    _ => Usage(),
};

static int Usage()
{
    Console.Error.WriteLine("usage: LibOSDP.Examples [loopback | cp <serial-port> | pd <serial-port>]");
    return 2;
}
