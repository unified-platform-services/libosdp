// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.IO.Ports;

namespace LibOSDP.Examples;

internal static class ControlPanelExample
{
    public static async Task<int> RunAsync(string portName)
    {
        try
        {
            await RunCoreAsync(portName);
        }
        catch (OperationCanceledException)
        {
        }
        return 0;
    }

    private static async Task RunCoreAsync(string portName)
    {
        // snippet:control-panel
        using var serial = new SerialPort(portName, 115200);
        serial.Open();
        using var channel = new StreamChannel(serial.BaseStream);

        byte[] scbk = Convert.FromHexString("000102030405060708090A0B0C0D0E0F");
        using var cp = new ControlPanel(channel, [new PdInfo(101) { Scbk = scbk }]);

        cp.EventReceived += (_, e) =>
        {
            if (e.Event is CardReadEvent card)
                Console.WriteLine($"PD {e.Address} read card {Convert.ToHexString(card.Data)}");
        };

        using var stop = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            stop.Cancel();
        };
        Task bus = cp.RunAsync(stop.Token);

        if (await cp.WaitSecureChannelAsync(101, TimeSpan.FromSeconds(10), stop.Token))
        {
            var green = new TemporaryLedParams(TemporaryLedControlCode.Set, OnCount: 5, OffCount: 5,
                                               OnColor: LedColor.Green, OffColor: LedColor.Black, TimerCount: 30);
            await cp.SubmitCommandAsync(101, new LedCommand(Reader: 0, LedNumber: 0, Temporary: green))
                .WaitAsync(stop.Token);
        }
        // end-snippet

        await bus;
    }
}
