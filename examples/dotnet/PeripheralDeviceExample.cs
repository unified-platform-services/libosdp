// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.IO.Ports;

namespace LibOSDP.Examples;

internal static class PeripheralDeviceExample
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
        // snippet:peripheral-device
        using var serial = new SerialPort(portName, 115200);
        serial.Open();
        using var channel = new StreamChannel(serial.BaseStream);

        byte[] scbk = Convert.FromHexString("000102030405060708090A0B0C0D0E0F");
        PdCapability[] capabilities =
        [
            new(Capability.LedControl, ComplianceLevel: 1, NumItems: 1),
            new(Capability.Readers, ComplianceLevel: 1, NumItems: 1),
        ];
        using var pd = new PeripheralDevice(channel, new PdInfo(101) { Scbk = scbk }, capabilities);

        pd.CommandHandler = command =>
        {
            if (command is LedCommand led)
                Console.WriteLine($"LED {led.LedNumber}: {led.Temporary?.OnColor}");
            return CommandReply.Ack;
        };

        using var stop = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            stop.Cancel();
        };
        Task bus = pd.RunAsync(stop.Token);

        var card = new CardReadEvent(ReaderNo: 0, CardFormat.Wiegand, Data: [0x12, 0x34, 0x56, 0x40], Bits: 26);
        await pd.SubmitEventAsync(card).WaitAsync(stop.Token);
        // end-snippet

        await bus;
    }
}
