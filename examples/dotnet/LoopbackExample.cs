// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Examples;

/// A CP and a PD in one process over an in-memory link: no hardware needed.
internal static class LoopbackExample
{
    public static async Task<int> RunAsync()
    {
        var (cpSide, pdSide) = LoopbackChannel.CreatePair();
        byte[] scbk = Convert.FromHexString("000102030405060708090A0B0C0D0E0F");
        using var cp = new ControlPanel(cpSide, [new PdInfo(101) { Scbk = scbk }]);
        using var pd = new PeripheralDevice(pdSide, new PdInfo(101) { Scbk = scbk },
                                            [new(Capability.Readers, 1, 1)]);

        var cardSeen = new TaskCompletionSource<CardReadEvent>(TaskCreationOptions.RunContinuationsAsynchronously);
        cp.EventReceived += (_, e) =>
        {
            if (e.Event is CardReadEvent card)
                cardSeen.TrySetResult(card);
        };

        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        Task cpLoop = cp.RunAsync(stop.Token);
        Task pdLoop = pd.RunAsync(stop.Token);
        int result = await RunScenarioAsync(cp, pd, cardSeen, cpLoop, pdLoop);
        stop.Cancel();
        result = Math.Max(result, await ObserveAsync("control panel", cpLoop));
        result = Math.Max(result, await ObserveAsync("peripheral device", pdLoop));
        return result;
    }

    private static async Task<int> RunScenarioAsync(ControlPanel cp, PeripheralDevice pd,
        TaskCompletionSource<CardReadEvent> cardSeen, Task cpLoop, Task pdLoop)
    {
        try
        {
            if (!await cp.WaitSecureChannelAsync(101, TimeSpan.FromSeconds(10)))
            {
                return ReportFailure("secure channel did not come up", cpLoop, pdLoop);
            }
            await pd.SubmitEventAsync(new CardReadEvent(0, CardFormat.Wiegand, [0x12, 0x34, 0x56, 0x40], Bits: 26));
            CardReadEvent card = await cardSeen.Task.WaitAsync(TimeSpan.FromSeconds(10));
            Console.WriteLine($"loopback ok: LibOSDP {Osdp.Version}, card {Convert.ToHexString(card.Data)}");
            return 0;
        }
        catch (TimeoutException)
        {
            return ReportFailure("card read did not arrive", cpLoop, pdLoop);
        }
    }

    private static int ReportFailure(string what, Task cpLoop, Task pdLoop)
    {
        foreach (var (name, loop) in new[] { ("control panel", cpLoop), ("peripheral device", pdLoop) })
        {
            if (loop.IsFaulted)
            {
                Console.Error.WriteLine($"loopback: {name} loop failed: {loop.Exception!.GetBaseException()}");
                return 1;
            }
        }
        Console.Error.WriteLine($"loopback: {what}");
        return 1;
    }

    private static async Task<int> ObserveAsync(string name, Task loop)
    {
        try
        {
            await loop;
            return 0;
        }
        catch (OperationCanceledException)
        {
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"loopback: {name} loop failed: {ex}");
            return 1;
        }
    }
}
