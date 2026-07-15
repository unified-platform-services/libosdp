#!/usr/bin/env bash
#
# Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Cross-version OSDP wire-compatibility check.
#
# Builds libosdp at two git revisions, compiles the public-API-only interop
# harnesses (ftx.c = file transfer, xfer.c = command/event transport) against
# each, then runs a CP from one build against a PD from the other over a socat
# PTY pair (a virtual serial link). It exercises both cross directions plus
# same-version controls, and repeats every case under three channel modes:
#
#   plain    no secure channel
#   secure   pre-shared SCBK on both ends + OSDP_FLAG_ENFORCE_SECURE
#   install  OSDP_FLAG_INSTALL_MODE, channel provisioned over SCBK-D
#
# File-transfer cases assert the received file's sha256 matches; transport
# cases self-verify a fixed command/event sequence and assert both ends exit 0.
# Any failure proves the two revisions are NOT wire-compatible. Because the
# harnesses use only the public API, a harness build failure against either
# revision also flags a public API break.
#
# Usage:
#   tests/interop/interop-check.sh [REV_A] [REV_B]
#     REV_A  first revision  (default: master)
#     REV_B  second revision (default: HEAD)
#
# Env:
#   FT_SIZE   bytes of random file-transfer payload (default: 131072)
#   MODES     space-separated channel modes to run (default: "plain secure install")
#
# Requires: socat, cmake, a C compiler. Revisions must be committed (a dirty
# working tree is not tested — commit first).
set -u

REV_A="${1:-master}"
REV_B="${2:-HEAD}"
FT_SIZE="${FT_SIZE:-131072}"
MODES="${MODES:-plain secure install}"

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel)"
FTX_SRC="$SELF_DIR/ftx.c"
XFER_SRC="$SELF_DIR/xfer.c"

SHA_A="$(git -C "$REPO" rev-parse --short "$REV_A")" || { echo "bad rev: $REV_A"; exit 2; }
SHA_B="$(git -C "$REPO" rev-parse --short "$REV_B")" || { echo "bad rev: $REV_B"; exit 2; }

WORK="$(mktemp -d)"
INFILE="$WORK/infile.bin"
TTY_A="$WORK/ttyA"
TTY_B="$WORK/ttyB"
declare -A LIBDIR FTX XFER
pass=0; fail=0; SOCAT_PID=""

cleanup() {
	[ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null
	git -C "$REPO" worktree remove --force "$WORK/wt_$SHA_A" 2>/dev/null
	git -C "$REPO" worktree remove --force "$WORK/wt_$SHA_B" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

compile_harness() {
	local sha="$1" src="$2" out="$3" wt="$WORK/wt_$1"
	cc -O2 -Wall "$src" -o "$out" \
		-I "$wt/include" -I "$wt/build/include" \
		-L "$wt/build/lib" -losdp 2>"$wt/$(basename "$src").log" || {
		echo "HARNESS $(basename "$src") build failed against $sha" \
		     "-> possible public API break"
		cat "$wt/$(basename "$src").log"; exit 1; }
}

build_rev() {
	local sha="$1" wt="$WORK/wt_$1"
	echo "[*] building $sha ..."
	git -C "$REPO" worktree add --detach "$wt" "$sha" >/dev/null 2>&1 || {
		echo "worktree add failed for $sha"; exit 1; }
	cmake -S "$wt" -B "$wt/build" -DCMAKE_BUILD_TYPE=Release >"$wt/cmake.log" 2>&1 || {
		echo "cmake configure failed for $sha (see $wt/cmake.log)"; exit 1; }
	cmake --build "$wt/build" -j"$(nproc)" >"$wt/build.log" 2>&1 || {
		echo "build failed for $sha (see $wt/build.log)"; exit 1; }
	LIBDIR[$sha]="$wt/build/lib"
	FTX[$sha]="$WORK/ftx_$sha"
	XFER[$sha]="$WORK/xfer_$sha"
	compile_harness "$sha" "$FTX_SRC" "${FTX[$sha]}"
	compile_harness "$sha" "$XFER_SRC" "${XFER[$sha]}"
}

start_socat() {
	rm -f "$TTY_A" "$TTY_B"
	socat pty,raw,echo=0,link="$TTY_A" pty,raw,echo=0,link="$TTY_B" \
		>/dev/null 2>&1 &
	SOCAT_PID=$!
	for _ in $(seq 1 50); do
		[ -e "$TTY_A" ] && [ -e "$TTY_B" ] && return 0
		sleep 0.1
	done
	echo "socat did not create ptys"; return 1
}
stop_socat() { kill "$SOCAT_PID" 2>/dev/null; wait "$SOCAT_PID" 2>/dev/null; SOCAT_PID=""; }

report() {
	local ok="$1" label="$2" cp="$3" pd="$4" extra="$5"
	if [ "$ok" = 1 ]; then
		printf 'PASS  %-30s cp=%s pd=%s\n' "$label" "$cp" "$pd"
		pass=$((pass+1))
	else
		printf 'FAIL  %-30s cp=%s pd=%s (%s)\n' "$label" "$cp" "$pd" "$extra"
		echo "   cp log:"; tail -4 "$WORK/${label}_cp.log" | sed 's/^/     /'
		echo "   pd log:"; tail -4 "$WORK/${label}_pd.log" | sed 's/^/     /'
		fail=$((fail+1))
	fi
}

# ftx_case <label> <cp_sha> <pd_sha> <mode>
ftx_case() {
	local label="$1" cp="$2" pd="$3" mode="$4" out="$WORK/out_$1.bin"
	rm -f "$out"
	start_socat || { fail=$((fail+1)); return; }
	LD_LIBRARY_PATH="${LIBDIR[$pd]}" "${FTX[$pd]}" pd "$TTY_A" "$out" "$mode" \
		>"$WORK/${label}_pd.log" 2>&1 &
	local pd_pid=$!
	sleep 0.3
	LD_LIBRARY_PATH="${LIBDIR[$cp]}" "${FTX[$cp]}" cp "$TTY_B" "$INFILE" "$mode" \
		>"$WORK/${label}_cp.log" 2>&1 &
	local cp_pid=$!
	wait "$cp_pid"; local cp_rc=$?
	wait "$pd_pid"; local pd_rc=$?
	stop_socat

	local want got
	want="$(sha256sum "$INFILE" | awk '{print $1}')"
	got="$( [ -f "$out" ] && sha256sum "$out" | awk '{print $1}' || echo '<none>')"
	if [ "$cp_rc" = 0 ] && [ "$pd_rc" = 0 ] && [ "$want" = "$got" ]; then
		report 1 "$label" "$cp" "$pd" ""
	else
		report 0 "$label" "$cp" "$pd" \
			"cp_rc=$cp_rc pd_rc=$pd_rc want=$want got=$got"
	fi
}

# xfer_case <label> <cp_sha> <pd_sha> <mode>
xfer_case() {
	local label="$1" cp="$2" pd="$3" mode="$4"
	start_socat || { fail=$((fail+1)); return; }
	LD_LIBRARY_PATH="${LIBDIR[$pd]}" "${XFER[$pd]}" pd "$TTY_A" "$mode" \
		>"$WORK/${label}_pd.log" 2>&1 &
	local pd_pid=$!
	sleep 0.3
	LD_LIBRARY_PATH="${LIBDIR[$cp]}" "${XFER[$cp]}" cp "$TTY_B" "$mode" \
		>"$WORK/${label}_cp.log" 2>&1 &
	local cp_pid=$!
	wait "$cp_pid"; local cp_rc=$?
	wait "$pd_pid"; local pd_rc=$?
	stop_socat

	if [ "$cp_rc" = 0 ] && [ "$pd_rc" = 0 ]; then
		report 1 "$label" "$cp" "$pd" ""
	else
		report 0 "$label" "$cp" "$pd" "cp_rc=$cp_rc pd_rc=$pd_rc"
	fi
}

echo "libosdp interop check: A=$SHA_A ($REV_A)  B=$SHA_B ($REV_B)"
echo "[*] modes: $MODES"
build_rev "$SHA_A"
[ "$SHA_A" != "$SHA_B" ] && build_rev "$SHA_B"

head -c "$FT_SIZE" /dev/urandom > "$INFILE"
echo "[*] file payload: $FT_SIZE bytes, sha=$(sha256sum "$INFILE" | awk '{print $1}')"

# Case pairings: cross directions when the revs differ, plus controls.
declare -a PAIRS
if [ "$SHA_A" != "$SHA_B" ]; then
	PAIRS=( "A_cp__B_pd $SHA_A $SHA_B" "B_cp__A_pd $SHA_B $SHA_A"
		"A_ctrl $SHA_A $SHA_A" "B_ctrl $SHA_B $SHA_B" )
else
	PAIRS=( "ctrl $SHA_A $SHA_A" )
fi

for mode in $MODES; do
	echo
	echo "==== mode: $mode ===="
	for p in "${PAIRS[@]}"; do
		set -- $p
		ftx_case  "ft_${mode}_$1"  "$2" "$3" "$mode"
		xfer_case "xf_${mode}_$1"  "$2" "$3" "$mode"
	done
done

echo
echo "==== $pass passed, $fail failed ===="
[ "$fail" = 0 ]
