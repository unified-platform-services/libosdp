// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

public sealed class CommandDeliveryTests
{
    public static TheoryData<OsdpCommand> Delivered => new()
    {
        new OutputCommand(0, OutputControlCode.PermanentOff),
        new LedCommand(0, 0,
            new TemporaryLedParams(TemporaryLedControlCode.Set, 10, 10, LedColor.Red, LedColor.Black, 10),
            new PermanentLedParams(PermanentLedControlCode.Set, 1, 0, LedColor.Green, LedColor.Black)),
        new BuzzerCommand(0, BuzzerControlCode.DefaultTone, 2, 2, 3),
        new TextCommand(0, TextControlCode.PermanentNoWrap, 20, 1, 1, "LIBOSDP"),
        new TdSetCommand(2026, 7, 17, 12, 30, 45),
        new ManufacturerCommand(0x030201, [9, 1, 9, 2, 6, 3, 1, 7, 7, 0]),
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Delivered))]
    public async Task CommandReachesThePdIntact(OsdpCommand sent)
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        OsdpCommand received = await bus.NextCommandAsync<OsdpCommand>();
        Assert.IsType(sent.GetType(), received);
        Assert.Equivalent(sent, received, strict: true);
        Assert.Equal(CompletionStatus.Ok, await done.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task ComsetIsFollowedByComsetDone()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, new ComsetCommand(Bus.Address, 9600));
        Assert.Equal(new ComsetCommand(Bus.Address, 9600), await bus.NextCommandAsync<ComsetCommand>());
        Assert.Equal(new ComsetDoneCommand(Bus.Address, 9600), await bus.NextCommandAsync<ComsetDoneCommand>());
        Assert.Equal(CompletionStatus.Ok, await done.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task KeysetKeepsTheSecureChannelUp()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        byte[] newKey = BusOptions.Secure().CpKey!;
        var sent = new KeysetCommand(1, newKey);
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        Assert.Equivalent(sent, await bus.NextCommandAsync<KeysetCommand>(), strict: true);
        Assert.Equal(CompletionStatus.Ok, await done.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.True(await bus.Cp.WaitSecureChannelAsync(Bus.Address, Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task KeysetWithoutSecureChannelIsRefused()
    {
        await using var bus = await Bus.StartAsync();
        await Assert.ThrowsAsync<OsdpException>(
            () => bus.Cp.SubmitCommandAsync(Bus.Address, new KeysetCommand(1, new byte[16])));
    }

    [Fact]
    public async Task PdAnswersAStatusQuery()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        bus.Reply = cmd => cmd is StatusCommand query
            ? CommandReply.WithAnswer(query with { Report = [0, 1] })
            : CommandReply.Ack;
        _ = bus.Cp.SubmitCommandAsync(Bus.Address, new StatusCommand(StatusReportType.Local, []));
        Assert.Empty((await bus.NextCommandAsync<StatusCommand>()).Report);
        OsdpEvent reply = await bus.NextEventAsync(e => e is StatusEvent);
        Assert.Equivalent(new StatusEvent(StatusReportType.Local, [0, 1]), reply, strict: true);
    }

    [Fact]
    public async Task RejectedCommandCompletesAsFailedAndPdStaysOnline()
    {
        await using var bus = await Bus.StartAsync();
        bus.Reply = _ => CommandReply.Reject(NakCode.Record);
        CompletionStatus status = await bus.Cp
            .SubmitCommandAsync(Bus.Address, new OutputCommand(0, OutputControlCode.PermanentOn))
            .WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.Equal(CompletionStatus.Failed, status);
        Assert.True(bus.Cp.IsOnline(Bus.Address));
    }

    [Fact]
    public async Task ManufacturerReplySubmittedInsideTheHandlerIsTheInlineReply()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        var reply = new ManufacturerReplyEvent(0x030201, [1, 2, 3]);
        bus.Reply = cmd =>
        {
            if (cmd is ManufacturerCommand)
                _ = bus.Pd.SubmitEventAsync(reply);
            return CommandReply.Ack;
        };
        _ = bus.Cp.SubmitCommandAsync(Bus.Address, new ManufacturerCommand(0x030201, [9]));
        Assert.Equivalent(reply, await bus.NextEventAsync(e => e is ManufacturerReplyEvent), strict: true);
    }
}
