#!/usr/bin/env bash
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Builds several runtime IDs at once with build-native.sh, each into
# native-<rid>/, in the container scripts/native-targets.tsv names for it.
# One runner builds everything it can; doing it concurrently keeps the
# grouped job as fast as separate jobs were.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGETS="${ROOT}/scripts/native-targets.tsv"
LOGS="${ROOT}/build-logs"

if [ $# -eq 0 ]; then
	echo "usage: $0 <rid>..." >&2
	exit 2
fi

# Same-RID builds would share native-<rid>, its build dir and its log.
for rid in "$@"; do
	seen=0
	for other in "$@"; do
		[ "${other}" = "${rid}" ] && seen=$((seen + 1))
	done
	if [ "${seen}" -gt 1 ]; then
		echo "duplicate rid: ${rid}" >&2
		exit 2
	fi
done

rev="$(git -C "${ROOT}" describe --tags --long --always --abbrev=7 2>/dev/null || true)"
tag="$(git -C "${ROOT}" describe --exact-match --tags HEAD 2>/dev/null || true)"

build_one() {
	local rid="$1" image glibc probe
	local row
	row="$(awk -F'\t' -v r="${rid}" '$1 == r { print $2, $3, $4 }' "${TARGETS}")"
	if [ -z "${row}" ]; then
		echo "unknown rid ${rid} (not in ${TARGETS})" >&2
		return 2
	fi
	read -r image glibc probe <<<"${row}"
	[ "${glibc}" = - ] && glibc=
	[ "${probe}" = - ] && probe=
	if [ "${image}" = - ]; then
		LIBOSDP_GIT_REV="${rev}" LIBOSDP_GIT_TAG="${tag}" \
			BUILD_DIR="${ROOT}/build-native-${rid}" \
			"${ROOT}/scripts/build-native.sh" "${rid}" "${ROOT}/native-${rid}"
	else
		docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
			-v "${ROOT}:/src" -w /src \
			-e LIBOSDP_GIT_REV="${rev}" -e LIBOSDP_GIT_TAG="${tag}" \
			-e MAX_GLIBC="${glibc}" -e BUILD_DIR="/tmp/build-${rid}" \
			-e ABI_PROBE_RUNNER="${probe}" \
			"${image}" scripts/build-native.sh "${rid}" "/src/native-${rid}"
	fi
}

mkdir -p "${LOGS}"
# Indexed arrays only: macOS ships bash 3.2, which has no associative arrays.
rids=("$@")
pids=()
for rid in "${rids[@]}"; do
	build_one "${rid}" >"${LOGS}/${rid}.log" 2>&1 &
	pids+=($!)
done

failed=()
for i in "${!rids[@]}"; do
	rid="${rids[$i]}"
	if wait "${pids[$i]}"; then status=ok; else status=FAILED; failed+=("${rid}"); fi
	echo "::group::native ${rid} (${status})"
	cat "${LOGS}/${rid}.log"
	echo "::endgroup::"
done

if (( ${#failed[@]} )); then
	echo "::error::native build failed for: ${failed[*]}" >&2
	exit 1
fi
