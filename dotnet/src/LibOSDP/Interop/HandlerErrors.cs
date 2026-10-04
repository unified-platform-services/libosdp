// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

/// Exceptions from user code that ran inside a native callback. They cannot
/// unwind through C, so they are kept here and rethrown from Refresh().
internal sealed class HandlerErrors
{
    private readonly List<Exception> _errors = [];

    internal void Record(Exception error)
    {
        lock (_errors)
            _errors.Add(error);
    }

    internal void ThrowIfAny()
    {
        Exception[] errors;
        lock (_errors)
        {
            if (_errors.Count == 0)
                return;
            errors = [.. _errors];
            _errors.Clear();
        }
        throw new AggregateException("A handler, channel, or file operation threw inside LibOSDP; see InnerExceptions.", errors);
    }
}
