#!/usr/bin/env bash
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Builds libosdp for this machine with the shipped options and stages it where
# the .NET test and example projects pick it up (dotnet/runtimes/<rid>/native).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)-$(uname -m)" in
Linux-x86_64) rid=linux-x64 ;;
Linux-aarch64) rid=linux-arm64 ;;
Darwin-arm64) rid=osx-arm64 ;;
Darwin-x86_64) rid=osx-x64 ;;
MINGW*-x86_64 | MSYS*-x86_64) rid=win-x64 ;;
*)
	echo "unsupported host $(uname -s)-$(uname -m)" >&2
	exit 1
	;;
esac
if [[ "${rid}" == linux-* ]] && ldd --version 2>&1 | grep -qi musl; then
	rid="linux-musl-${rid#linux-}"
fi

out="${ROOT}/build-native-${rid}/out"
SKIP_UNIT_TESTS="${SKIP_UNIT_TESTS:-1}" "${ROOT}/scripts/build-native.sh" "${rid}" "${out}"

rm -rf "${ROOT}/dotnet/runtimes"
mkdir -p "${ROOT}/dotnet/runtimes/${rid}/native"
for f in "${out}"/lib/libosdp.so "${out}"/lib/libosdp.dylib "${out}"/lib/osdp.dll; do
	if [ -f "${f}" ]; then
		cp "${f}" "${ROOT}/dotnet/runtimes/${rid}/native/"
	fi
done
ls -l "${ROOT}/dotnet/runtimes/${rid}/native/"
