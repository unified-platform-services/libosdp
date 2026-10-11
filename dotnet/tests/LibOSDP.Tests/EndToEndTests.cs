// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

/// Every command and event across a live CP/PD link, over each transport,
/// with and without a secure channel. Each peer must receive exactly what
/// the other sent.
internal static class E2E
{
    internal static readonly PdCapability[] Capabilities =
    [
        .. Bus.DefaultCapabilities,
        new(Capability.SmartCard, 1, 1),
    ];

    internal static readonly (Transport Transport, bool Secure)[] Links =
    [
        (Transport.Loopback, false),
        (Transport.Loopback, true),
        (Transport.Tcp, false),
        (Transport.Tcp, true),
    ];

    internal static BusOptions Options(Transport transport, bool secure, OsdpFlags extraFlags = OsdpFlags.None)
    {
        BusOptions o = secure ? BusOptions.Secure() : new BusOptions();
        return o with
        {
            Transport = transport,
            Capabilities = Capabilities,
            CpFlags = o.CpFlags | extraFlags,
            PdFlags = o.PdFlags | extraFlags,
        };
    }

    internal static TheoryData<Transport, bool> AllLinks()
    {
        var data = new TheoryData<Transport, bool>();
        foreach (var (transport, secure) in Links)
            data.Add(transport, secure);
        return data;
    }

    internal static TheoryData<Transport, bool, string> OverEveryLink(IEnumerable<string> names)
    {
        var data = new TheoryData<Transport, bool, string>();
        foreach (var (transport, secure) in Links)
        {
            foreach (string name in names)
                data.Add(transport, secure, name);
        }
        return data;
    }

    internal static byte[] Pattern(int length, int seed) =>
        Enumerable.Range(0, length).Select(i => (byte)(seed + i)).ToArray();

    internal static Task<T> Settled<T>(Task<T> task) =>
        task.WaitAsync(Bus.Timeout * 3, TestContext.Current.CancellationToken);
}

public sealed class EndToEndCommandTests
{
    private static readonly Dictionary<string, OsdpCommand> Commands = new()
    {
        ["Output"] = new OutputCommand(3, OutputControlCode.TemporaryOn, TimerCount: 25),
        ["LedTemporary"] = new LedCommand(0, 0,
            Temporary: new TemporaryLedParams(TemporaryLedControlCode.Set, 5, 7, LedColor.Amber, LedColor.Blue, 300)),
        ["LedPermanent"] = new LedCommand(1, 0,
            Permanent: new PermanentLedParams(PermanentLedControlCode.Set, 2, 3, LedColor.Cyan, LedColor.Magenta)),
        ["LedTemporaryAndPermanent"] = new LedCommand(2, 0,
            new TemporaryLedParams(TemporaryLedControlCode.Cancel, 0, 0, LedColor.Black, LedColor.Black, 0),
            new PermanentLedParams(PermanentLedControlCode.Set, 1, 0, LedColor.Green, LedColor.Black)),
        ["Buzzer"] = new BuzzerCommand(1, BuzzerControlCode.DefaultTone, 4, 6, 9),
        ["Text"] = new TextCommand(0, TextControlCode.TemporaryWrap, 15, 2, 3, "Hello, OSDP 0123456789!"),
        ["TdSet"] = new TdSetCommand(2026, 12, 31, 23, 59, 58),
        ["Manufacturer"] = new ManufacturerCommand(0xA1B2C3, E2E.Pattern(64, 0x40)),
        ["BioRead"] = new BioReadCommand(1, BioType.LeftIndexFingerPrint, BioFormat.AnsiIncits378, 77),
        ["BioMatch"] = new BioMatchCommand(0, BioType.RightIrisScan, BioFormat.RawPgm, 90, E2E.Pattern(128, 0x80)),
    };

    public static TheoryData<Transport, bool, string> Cases => E2E.OverEveryLink(Commands.Keys);

    public static TheoryData<Transport, bool> AllLinks => E2E.AllLinks();

    public static TheoryData<Transport, bool, StatusReportType> StatusCases
    {
        get
        {
            var data = new TheoryData<Transport, bool, StatusReportType>();
            foreach (var (transport, secure) in E2E.Links)
            {
                foreach (StatusReportType type in Enum.GetValues<StatusReportType>())
                    data.Add(transport, secure, type);
            }
            return data;
        }
    }

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task CommandArrivesFieldExactAndCompletes(Transport transport, bool secure, string name)
    {
        OsdpCommand sent = Commands[name];
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure));
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        OsdpCommand received = await bus.NextCommandAsync<OsdpCommand>();
        Assert.IsType(sent.GetType(), received);
        Assert.Equivalent(sent, received, strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task ComsetArrivesThenComsetDone(Transport transport, bool secure)
    {
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure));
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, new ComsetCommand(Bus.Address, 115200));
        Assert.Equal(new ComsetCommand(Bus.Address, 115200), await bus.NextCommandAsync<ComsetCommand>());
        Assert.Equal(new ComsetDoneCommand(Bus.Address, 115200), await bus.NextCommandAsync<ComsetDoneCommand>());
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task KeysetArrivesOnlyOverASecureChannel(Transport transport, bool secure)
    {
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure));
        var sent = new KeysetCommand(1, BusOptions.Secure().CpKey!);
        if (!secure)
        {
            await Assert.ThrowsAsync<OsdpException>(() => bus.Cp.SubmitCommandAsync(Bus.Address, sent));
            return;
        }
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        Assert.Equivalent(sent, await bus.NextCommandAsync<KeysetCommand>(), strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
        Assert.True(await bus.Cp.WaitSecureChannelAsync(Bus.Address, Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task FileArrivesByteExact(Transport transport, bool secure)
    {
        byte[] content = E2E.Pattern(3001, 0x11);
        var receiver = new FileTransferTests.ReceiveFile();
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure) with
        {
            CpFileOperations = new FileTransferTests.SendFile(content),
            PdFileOperations = receiver,
        });

        var sent = new FileTransferCommand(FileId: 7);
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        Assert.Equivalent(sent, await bus.NextCommandAsync<FileTransferCommand>(), strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
        Assert.Equal(content, receiver.Content);
    }

    [Theory]
    [MemberData(nameof(StatusCases))]
    public async Task StatusQueryIsAnsweredWithTheReportThePdFilledIn(Transport transport, bool secure,
                                                                      StatusReportType type)
    {
        // A report must carry one entry per entity the PD advertises (osdp_pd.c pd_status_report_capacity).
        int entries = type is StatusReportType.Input or StatusReportType.Output ? 8 : 2;
        byte[] report = Enumerable.Range(0, entries).Select(i => (byte)(i % 2)).ToArray();
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure));
        bus.Reply = cmd => cmd is StatusCommand query
            ? CommandReply.WithAnswer(query with { Report = report })
            : CommandReply.Ack;

        var sent = new StatusCommand(type, []);
        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, sent);
        Assert.Equivalent(sent, await bus.NextCommandAsync<StatusCommand>(), strict: true);
        OsdpEvent reply = await bus.NextEventAsync(e => e is not NotificationEvent);
        Assert.Equivalent(new StatusEvent(type, report), reply, strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
    }
}

public sealed class EndToEndReplyTests
{
    /// A command the PD answers by submitting an event, and what the CP must
    /// see for it, in order. Multipart operations are framed by MultipartStart
    /// and MultipartDone notifications, which both PIV and multipart BIOREADR
    /// send after the reply event (osdp.h, osdp_cp_submit_command and
    /// struct osdp_cmd_bioread).
    public sealed record Flow(OsdpCommand Command, OsdpEvent Reply, OsdpFlags Flags, params OsdpEvent[] CpSees);

    private static readonly byte[] Challenge = E2E.Pattern(200, 0x10);
    private static readonly byte[] Response = E2E.Pattern(256, 0xC0);

    private static NotificationEvent Start(MpMsgType type, int objectId, int length) =>
        new(new MultipartNotification(NotificationType.MultipartStart, type, objectId, (uint)length, 0, MpOutcome.Ok));

    private static NotificationEvent Done(MpMsgType type, int objectId, int length) =>
        new(new MultipartNotification(NotificationType.MultipartDone, type, objectId, (uint)length, (uint)length,
                                      MpOutcome.Ok));

    private static Flow Simple(OsdpCommand command, OsdpEvent reply) => new(command, reply, OsdpFlags.None, reply);

    private static readonly Dictionary<string, Flow> Flows = new()
    {
        ["ManufacturerReply"] = Simple(new ManufacturerCommand(0x030201, [9, 1, 9, 2]),
                                       new ManufacturerReplyEvent(0x030201, E2E.Pattern(128, 0x20))),
        ["BioRead"] = Simple(new BioReadCommand(1, BioType.RightIndexFingerPrint, BioFormat.AnsiIncits378, 80),
                             new BioReadEvent(1, BioStatus.Success, BioType.RightIndexFingerPrint, 200,
                                              E2E.Pattern(40, 0x30))),
        ["BioReadMultipart"] = BioMultipart(),
        ["BioMatch"] = Simple(new BioMatchCommand(2, BioType.LeftThumbPrint, BioFormat.AnsiIncits378, 60,
                                                  E2E.Pattern(128, 0x50)),
                              new BioMatchEvent(2, BioStatus.Success, 97)),
        // A PIVDATA command carries no payload, so its operation starts with
        // nothing to size; GENAUTH/CRAUTH start sized by the challenge they
        // send (src/osdp_piv.c osdp_piv_cp_submit).
        ["PivData"] = Piv(new PivDataCommand([0x5F, 0xC1, 0x02], 7, 0), new PivDataEvent(Response),
                          MpMsgType.PivData, objectId: 7, startTotal: 0),
        ["GenAuth"] = Piv(new GenAuthCommand(0xA7, 0x9E, Challenge), new GenAuthEvent(Response),
                          MpMsgType.GenAuth, objectId: 0x9E, startTotal: Challenge.Length),
        ["CrAuth"] = Piv(new CrAuthCommand(0x11, 0x22, Challenge), new CrAuthEvent(Response),
                         MpMsgType.CrAuth, objectId: 0x22, startTotal: Challenge.Length),
    };

    private static Flow BioMultipart()
    {
        var reply = new BioReadEvent(0, BioStatus.Success, BioType.RightThumbPrint, 200, E2E.Pattern(256, 0x60));
        return new(new BioReadCommand(0, BioType.RightThumbPrint, BioFormat.AnsiIncits378, 50), reply,
                   OsdpFlags.BioReadrMultipart, Start(MpMsgType.BioRead, 0, 256), reply,
                   Done(MpMsgType.BioRead, 0, 256));
    }

    private static Flow Piv(OsdpCommand command, OsdpEvent reply, MpMsgType type, int objectId, int startTotal) =>
        new(command, reply, OsdpFlags.None, Start(type, objectId, startTotal), reply,
            Done(type, objectId, Response.Length));

    public static TheoryData<Transport, bool, string, bool> Cases
    {
        get
        {
            var data = new TheoryData<Transport, bool, string, bool>();
            foreach (var (transport, secure) in E2E.Links)
            {
                foreach (string name in Flows.Keys)
                {
                    data.Add(transport, secure, name, true);
                    data.Add(transport, secure, name, false);
                }
            }
            return data;
        }
    }

    private static bool IsReplyOrMultipartBoundary(OsdpEvent e) =>
        e is not NotificationEvent ||
        e is NotificationEvent { Notification: MultipartNotification { Stage: not NotificationType.MultipartProgress } };

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task PdAnswerReachesTheCpFieldExact(Transport transport, bool secure, string name, bool inline)
    {
        Flow flow = Flows[name];
        BusOptions options = E2E.Options(transport, secure, flow.Flags);
        await using var bus = await Bus.StartAsync(options with { CpFlags = options.CpFlags | OsdpFlags.EnableNotification });
        var replySubmitted = new TaskCompletionSource<Task<CompletionStatus>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        if (inline)
        {
            // An event submitted from inside the handler is this command's reply (PeripheralDevice.CommandHandler).
            bus.Reply = cmd =>
            {
                if (cmd.GetType() == flow.Command.GetType())
                    replySubmitted.TrySetResult(bus.Pd.SubmitEventAsync(flow.Reply));
                return CommandReply.Ack;
            };
        }

        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, flow.Command);
        OsdpCommand received = await bus.NextCommandAsync<OsdpCommand>(c => c.GetType() == flow.Command.GetType());
        Assert.Equivalent(flow.Command, received, strict: true);
        if (inline)
        {
            // FlushEvents waits for the refresh that ran the handler, so an
            // event libosdp did not take as this command's reply is still queued.
            Assert.Equal(0, bus.Pd.FlushEvents());
        }
        else
        {
            // SubmitEventAsync waits for the PD's bus lock, so this lands
            // after the handler returned and the command was ACKed.
            replySubmitted.SetResult(bus.Pd.SubmitEventAsync(flow.Reply));
        }

        foreach (OsdpEvent expected in flow.CpSees)
        {
            OsdpEvent seen = await bus.NextEventAsync(IsReplyOrMultipartBoundary);
            Assert.Equivalent(expected, seen, strict: true);
        }
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(await E2E.Settled(replySubmitted.Task)));
    }
}

public sealed class EndToEndEventTests
{
    // BIOREADR, BIOMATCHR, PIVDATAR, GENAUTHR and CRAUTHR are answers to a
    // command (osdp.h documents each as sent "in response to" one, and PIV
    // replies need the smartcard context the command opens), so they are
    // covered as replies in EndToEndReplyTests rather than sent unsolicited.
    private static readonly Dictionary<string, OsdpEvent> Events = new()
    {
        ["CardReadWiegand26"] = new CardReadEvent(0, CardFormat.Wiegand, [0x12, 0x34, 0x56, 0x40], Bits: 26),
        ["CardReadWiegand37"] = new CardReadEvent(1, CardFormat.Wiegand, [0xDE, 0xAD, 0xBE, 0xEF, 0xF8], Bits: 37),
        ["CardReadUnspecified13"] = new CardReadEvent(2, CardFormat.Unspecified, [0xA5, 0x28], Bits: 13),
        ["KeyPress"] = new KeyPressEvent(1, [0x31, 0x32, 0x33, 0x0D, 0x7F, 0x2A, 0x23]),
        ["ManufacturerReply"] = new ManufacturerReplyEvent(0xA1B2C3, E2E.Pattern(128, 0x01)),
        ["ManufacturerStatus"] = new ManufacturerStatusEvent(E2E.Pattern(100, 0x70)),
        ["ManufacturerError"] = new ManufacturerErrorEvent([0xBA, 0xAD, 0xF0, 0x0D]),
        ["StatusInput"] = new StatusEvent(StatusReportType.Input, [1, 0, 1, 1, 0, 0, 1, 0]),
        ["StatusOutput"] = new StatusEvent(StatusReportType.Output, [0, 1, 0, 0, 1, 1, 0, 1]),
        ["StatusLocal"] = new StatusEvent(StatusReportType.Local, [1, 0]),
        ["StatusReader"] = new StatusEvent(StatusReportType.Reader, [0, 1]),
    };

    public static TheoryData<Transport, bool, string> Cases => E2E.OverEveryLink(Events.Keys);

    [Theory]
    [MemberData(nameof(Cases))]
    public async Task EventArrivesFieldExactAndCompletes(Transport transport, bool secure, string name)
    {
        OsdpEvent sent = Events[name];
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure));
        Task<CompletionStatus> done = bus.Pd.SubmitEventAsync(sent);
        OsdpEvent received = await bus.NextEventAsync(e => e is not NotificationEvent);
        Assert.IsType(sent.GetType(), received);
        Assert.Equivalent(sent, received, strict: true);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
    }

    // LibOSDP does not support osdp_FMT, so the binding refuses an ASCII read
    // up front with an ArgumentException that names the field.
    [Fact]
    public async Task AsciiCardReadIsRefusedAtSubmission()
    {
        await using var bus = await Bus.StartAsync();
        var error = await Assert.ThrowsAsync<ArgumentException>(
            () => bus.Pd.SubmitEventAsync(new CardReadEvent(0, CardFormat.Ascii, "12345"u8.ToArray())));
        Assert.Equal("Format", error.ParamName);
    }
}

public sealed class EndToEndNotificationTests
{
    public static TheoryData<Transport, bool> AllLinks => E2E.AllLinks();

    public static TheoryData<Transport> Transports => new(Enum.GetValues<Transport>());

    /// Holds the transfer open in its finishing phase until <see cref="Finish"/> is set.
    private sealed class FinishingReceiver(MpOutcome? outcome) : IFileOperations
    {
        private readonly FileTransferTests.ReceiveFile _file = new();

        public TaskCompletionSource Finishing { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public byte[] Content => _file.Content;

        public bool Open(int fileId, ref uint size) => _file.Open(fileId, ref size);

        public int Read(Span<byte> buffer, uint offset) => _file.Read(buffer, offset);

        public int Write(ReadOnlySpan<byte> buffer, uint offset) => _file.Write(buffer, offset);

        public void Close() => _file.Close();

        public bool TryFinish(out MpOutcome result, out ushort delayMs)
        {
            Finishing.TrySetResult();
            result = outcome ?? MpOutcome.Ok;
            delayMs = 0;
            return outcome is not null;
        }
    }

    private static async Task WaitForAsync(Func<Task<OsdpNotification>> next, params Func<OsdpNotification, bool>[] wanted)
    {
        var pending = wanted.ToList();
        while (pending.Count > 0)
        {
            OsdpNotification n = await next();
            pending.RemoveAll(w => w(n));
        }
    }

    private static Func<OsdpNotification, bool> Is(OsdpNotification expected) => n => n.Equals(expected);

    private static Task<OsdpNotification> NextCpNotificationAsync(Bus bus) =>
        bus.NextEventAsync(e => e is NotificationEvent)
            .ContinueWith(t => ((NotificationEvent)t.Result).Notification, TaskScheduler.Default);

    private static Task<OsdpNotification> NextPdNotificationAsync(Bus bus) =>
        bus.NextCommandAsync<NotificationCommand>()
            .ContinueWith(t => t.Result.Notification, TaskScheduler.Default);

    private static MultipartNotification FileDone(int fileId, int length, MpOutcome outcome, int offset) =>
        new(NotificationType.MultipartDone, MpMsgType.FileTransfer, fileId, (uint)length, (uint)offset, outcome);

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task BothSidesSeeLinkAndMultipartNotifications(Transport transport, bool secure)
    {
        var id = new PdId(Version: 2, Model: 5, VendorCode: 0x0A0B0C, SerialNumber: 0x01020304, FirmwareVersion: 0x030201);
        BusOptions options = E2E.Options(transport, secure, OsdpFlags.EnableNotification) with { PdId = id };
        byte[] content = E2E.Pattern(3001, 0x42);
        var receiver = new FileTransferTests.ReceiveFile();
        await using var bus = await Bus.StartAsync(options with
        {
            CpFileOperations = new FileTransferTests.SendFile(content),
            PdFileOperations = receiver,
        });

        List<Func<OsdpNotification, bool>> linkUp = [Is(new PdStatusNotification(true))];
        if (secure)
            linkUp.Add(Is(new SecureChannelStatusNotification(Active: true, UsingDefaultKey: false)));
        await WaitForAsync(() => NextCpNotificationAsync(bus), [.. linkUp, Is(new PdIdNotification(id))]);
        await WaitForAsync(() => NextPdNotificationAsync(bus), [.. linkUp]);

        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, new FileTransferCommand(FileId: 3));

        Func<OsdpNotification, bool>[] transfer =
        [
            Is(new MultipartNotification(NotificationType.MultipartStart, MpMsgType.FileTransfer, 3,
                                         (uint)content.Length, 0, MpOutcome.Ok)),
            n => n is MultipartNotification { Stage: NotificationType.MultipartProgress, MessageType: MpMsgType.FileTransfer,
                                              ObjectId: 3, Offset: > 0, Outcome: MpOutcome.Ok } p &&
                 p.Total == content.Length && p.Offset < p.Total,
            Is(FileDone(3, content.Length, MpOutcome.Ok, content.Length)),
        ];
        await WaitForAsync(() => NextCpNotificationAsync(bus), transfer);
        await WaitForAsync(() => NextPdNotificationAsync(bus), transfer);
        Assert.Equal(CompletionStatus.Ok, await E2E.Settled(done));
        Assert.Equal(content, receiver.Content);
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task CpSeesThePdGoOffline(Transport transport, bool secure)
    {
        // The CP gives up after OSDP_CMD_MAX_RETRIES unanswered polls of
        // OSDP_RESP_TOUT_MS each (src/osdp_tunables.h), about two seconds.
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure, OsdpFlags.EnableNotification));
        await WaitForAsync(() => NextCpNotificationAsync(bus), Is(new PdStatusNotification(true)));
        bus.Pd.Dispose();
        await WaitForAsync(() => NextCpNotificationAsync(bus), Is(new PdStatusNotification(false)));
        Assert.False(bus.Cp.IsOnline(Bus.Address));
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public async Task InstallModeRunsTheSecureChannelOnTheDefaultKey(Transport transport)
    {
        var options = new BusOptions
        {
            Transport = transport,
            Capabilities = E2E.Capabilities,
            CpKey = BusOptions.Secure().CpKey,
            CpFlags = OsdpFlags.EnableNotification,
            PdFlags = OsdpFlags.InstallMode | OsdpFlags.EnableNotification,
        };
        await using var bus = await Bus.StartAsync(options);
        var onDefaultKey = new SecureChannelStatusNotification(Active: true, UsingDefaultKey: true);
        await WaitForAsync(() => NextCpNotificationAsync(bus), Is(onDefaultKey));
        await WaitForAsync(() => NextPdNotificationAsync(bus), Is(onDefaultKey));
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task OutcomeThePdReportsReachesBothSides(Transport transport, bool secure)
    {
        byte[] content = E2E.Pattern(500, 0x24);
        var receiver = new FinishingReceiver(MpOutcome.Unrecognized);
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure, OsdpFlags.EnableNotification) with
        {
            CpFileOperations = new FileTransferTests.SendFile(content),
            PdFileOperations = receiver,
        });

        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, new FileTransferCommand(FileId: 5));
        var failed = FileDone(5, content.Length, MpOutcome.Unrecognized, content.Length);
        await WaitForAsync(() => NextCpNotificationAsync(bus), Is(failed));
        await WaitForAsync(() => NextPdNotificationAsync(bus), Is(failed));
        Assert.Equal(CompletionStatus.Failed, await E2E.Settled(done));
        Assert.Equal(content, receiver.Content);
    }

    [Theory]
    [MemberData(nameof(AllLinks))]
    public async Task CancelledTransferEndsAborted(Transport transport, bool secure)
    {
        byte[] content = E2E.Pattern(500, 0x24);
        var receiver = new FinishingReceiver(outcome: null);
        await using var bus = await Bus.StartAsync(E2E.Options(transport, secure, OsdpFlags.EnableNotification) with
        {
            CpFileOperations = new FileTransferTests.SendFile(content),
            PdFileOperations = receiver,
        });

        Task<CompletionStatus> done = bus.Cp.SubmitCommandAsync(Bus.Address, new FileTransferCommand(FileId: 6));
        await receiver.Finishing.Task.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.True(bus.Cp.Cancel(Bus.Address, MpMsgType.FileTransfer));
        Func<OsdpNotification, bool> aborted = n =>
            n is MultipartNotification { Stage: NotificationType.MultipartDone, MessageType: MpMsgType.FileTransfer,
                                         ObjectId: 6, Outcome: MpOutcome.Aborted };
        await WaitForAsync(() => NextCpNotificationAsync(bus), aborted);
        await WaitForAsync(() => NextPdNotificationAsync(bus), aborted);
        Assert.Equal(CompletionStatus.Failed, await E2E.Settled(done));
    }
}
