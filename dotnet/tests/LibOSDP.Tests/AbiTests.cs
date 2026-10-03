// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP.Tests;

public sealed class AbiTests
{
    [Fact]
    public void LoadedLibraryMatchesTheBindingLayout() =>
        AbiGuard.Verify(AbiGuard.Query(), AbiGuard.Expected());

    [Fact]
    public void DifferentStructSizeIsRejected()
    {
        AbiInfo actual = AbiGuard.Expected() with { SizeofCmd = 1 };
        var error = Assert.Throws<OsdpAbiMismatchException>(
            () => AbiGuard.Verify(actual, AbiGuard.Expected()));
        Assert.Contains("sizeof_cmd", error.Message);
    }

    [Fact]
    public void ZeroCopyBuildIsRejected()
    {
        AbiInfo actual = AbiGuard.Expected() with { Options = 0x1 };
        var error = Assert.Throws<OsdpAbiMismatchException>(
            () => AbiGuard.Verify(actual, AbiGuard.Expected()));
        Assert.Contains("options", error.Message);
    }

    [Fact]
    public void LibraryTooOldToReportFullAbiIsRejected()
    {
        AbiInfo actual = AbiGuard.Expected() with { Size = 8 };
        Assert.Throws<OsdpAbiMismatchException>(
            () => AbiGuard.Verify(actual, AbiGuard.Expected()));
    }

    [Fact]
    public void VersionIsReported() =>
        Assert.Matches(@"^\d+\.\d+\.\d+", Osdp.Version);
}
