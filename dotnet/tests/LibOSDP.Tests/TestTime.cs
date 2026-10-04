// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

internal static class TestTime
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(10);

    // Emulated runs (linux-arm under QEMU in CI) are an order of magnitude slower.
    internal static readonly TimeSpan Timeout =
        int.TryParse(Environment.GetEnvironmentVariable("LIBOSDP_TEST_TIMEOUT_S"), out int s)
            ? TimeSpan.FromSeconds(s)
            : DefaultTimeout;

    /// Stretches a bound tuned for native speed as much as LIBOSDP_TEST_TIMEOUT_S stretches the default timeout.
    internal static TimeSpan Scaled(TimeSpan nativeBound) =>
        nativeBound * Math.Max(1.0, Timeout / DefaultTimeout);
}
