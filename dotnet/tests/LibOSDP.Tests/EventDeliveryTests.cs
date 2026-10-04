// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

public sealed class EventDeliveryTests
{
    public static TheoryData<OsdpEvent> Delivered => new()
    {
        new CardReadEvent(1, CardFormat.Wiegand, [0x55, 0xAA], Bits: 16),
        new CardReadEvent(0, CardFormat.Wiegand, [0x12, 0x34, 0x56, 0x40], Bits: 26),
        new KeyPressEvent(1, [9, 1, 9, 2, 6, 3, 1, 7, 7, 0]),
        new ManufacturerReplyEvent(0x153, [0x10, 9, 1, 9, 2]),
        new ManufacturerStatusEvent([0xDE, 0xAD, 0xBE, 0xEF]),
        new ManufacturerErrorEvent([0xBA, 0xAD, 0xF0, 0x0D]),
        new StatusEvent(StatusReportType.Input, [1, 0, 1, 0, 1, 0, 1, 0]),
        new StatusEvent(StatusReportType.Output, [0, 1, 0, 1, 0, 1, 0, 1]),
        new StatusEvent(StatusReportType.Local, [0, 1]),
        new StatusEvent(StatusReportType.Reader, [0, 2]),
    };

    [Theory(DisableDiscoveryEnumeration = true)]
    [MemberData(nameof(Delivered))]
    public async Task EventReachesTheCpIntact(OsdpEvent sent)
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        Task<CompletionStatus> done = bus.Pd.SubmitEventAsync(sent);
        OsdpEvent received = await bus.NextEventAsync(e => e is not NotificationEvent);
        Assert.IsType(sent.GetType(), received);
        Assert.Equivalent(sent, received, strict: true);
        Assert.Equal(CompletionStatus.Ok, await done.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task NotificationsReachTheCpWhenAskedFor()
    {
        var options = BusOptions.Secure() with { CpFlags = OsdpFlags.EnforceSecure | OsdpFlags.EnableNotification };
        await using var bus = await Bus.StartAsync(options);
        OsdpEvent ev = await bus.NextEventAsync(e => e is NotificationEvent { Notification: PdStatusNotification });
        Assert.True(((PdStatusNotification)((NotificationEvent)ev).Notification).Online);
    }

    [Fact]
    public async Task NotificationsStayInternalByDefault()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        // The CP is online, so LibOSDP has sent PD-status and SC-status
        // notifications; none may reach EventReceived without the flag.
        _ = bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1]));
        OsdpEvent first = await bus.NextEventAsync(_ => true);
        Assert.IsType<KeyPressEvent>(first);
    }
}
