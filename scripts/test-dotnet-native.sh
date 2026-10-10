#!/usr/bin/env bash
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Runs the .NET test suite against the native library a builder job just
# produced for <rid>. Each run gets its own copy of dotnet/ and
# examples/dotnet/ so several RIDs can be tested at once on one runner.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
rid="$1"
native="${ROOT}/native-${rid}/lib"
work="${RUNNER_TEMP:-/tmp}/dotnet-test-${rid}"

rm -rf "${work}"
mkdir -p "${work}"
tar -C "${ROOT}" -cf - --exclude=dotnet/artifacts --exclude=dotnet/runtimes \
	dotnet examples/dotnet | tar -C "${work}" -xf -
mkdir -p "${work}/dotnet/runtimes/${rid}/native"
for f in libosdp.so libosdp.dylib osdp.dll; do
	if [ -f "${native}/${f}" ]; then cp "${native}/${f}" "${work}/dotnet/runtimes/${rid}/native/"; fi
done

# Running as the caller keeps the copy removable without root, and the
# outer timeout turns a hang (e.g. an emulated runtime crashing a build
# node) into a failure instead of a stuck job.
in_docker() { # <platform> <image> <timeout>
	timeout -k 30s 30m docker run --rm --init --platform "$1" \
		--user "$(id -u):$(id -g)" -e HOME=/tmp -e DOTNET_CLI_HOME=/tmp \
		-v "${work}:/src" -w /src/dotnet \
		-e LIBOSDP_TEST_TIMEOUT_S="$3" "$2" \
		dotnet test --solution LibOSDP.slnx -f net10.0
}

case "${rid}" in
linux-musl-x64) in_docker linux/amd64 mcr.microsoft.com/dotnet/sdk:10.0-alpine 10 ;;
linux-musl-arm64) in_docker linux/arm64 mcr.microsoft.com/dotnet/sdk:10.0-alpine 10 ;;
linux-arm) in_docker linux/arm/v7 mcr.microsoft.com/dotnet/sdk:10.0 60 ;;
osx-x64)
	# Built on Apple Silicon; run it under Rosetta with an x64 runtime.
	if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
		sudo softwareupdate --install-rosetta --agree-to-license >/dev/null 2>&1 || true
	fi
	if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
		echo "::error::Rosetta unavailable; cannot test osx-x64"
		exit 1
	fi
	x64="${RUNNER_TEMP:-/tmp}/dotnet-x64"
	curl -fsSL --retry 5 --retry-all-errors https://dot.net/v1/dotnet-install.sh -o "${x64}.sh"
	bash "${x64}.sh" --channel 10.0 --architecture x64 --install-dir "${x64}" >/dev/null
	bash "${x64}.sh" --channel 8.0 --runtime dotnet --architecture x64 --install-dir "${x64}" >/dev/null
	cd "${work}/dotnet"
	DOTNET_ROOT="${x64}" arch -x86_64 "${x64}/dotnet" test --solution LibOSDP.slnx
	;;
*)
	cd "${work}/dotnet"
	dotnet test --solution LibOSDP.slnx
	;;
esac
