// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP.Tests;

[CollectionDefinition(nameof(Serial), DisableParallelization = true)]
public sealed class Serial;

[Collection(nameof(Serial))]
public sealed class NativeHeapTests
{
    [Fact]
    public async Task DisposeWithQueuedWorkFreesEveryNativeAllocation()
    {
        long before = NativeHeap.Live;
        var bus = await Bus.StartAsync(BusOptions.Secure());
        for (int i = 0; i < 50; i++)
            _ = bus.Cp.SubmitCommandAsync(Bus.Address, new OutputCommand(0, OutputControlCode.PermanentOff));
        for (int i = 0; i < 20; i++)
            _ = bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1]));
        await bus.DisposeAsync();
        Assert.Equal(before, NativeHeap.Live);
    }

    [Fact]
    public void RejectedSetupFreesEveryNativeAllocation()
    {
        long before = NativeHeap.Live;
        var (a, _) = LoopbackChannel.CreatePair();
        Assert.Throws<ArgumentOutOfRangeException>(() => new ControlPanel(a, [new PdInfo(1), new PdInfo(127)]));
        Assert.Equal(before, NativeHeap.Live);
    }
}
