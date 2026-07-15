# Cross-version interop check

Proves two libosdp git revisions are wire- and API-compatible by running real
OSDP exchanges between them over a virtual serial link.

`interop-check.sh` builds libosdp at both revisions, compiles the two
public-API-only harnesses against each, then runs a CP from one build against a
PD from the other across a `socat` PTY pair — in both cross directions plus
same-version controls, and repeats every case under three channel modes.

## Harnesses

- **`ftx.c`** — file transfer. The CP sends a known file, the PD writes it, and
  the check asserts the received file's `sha256` matches. A mismatch flags a
  file-transfer wire-format break.
- **`xfer.c`** — command/event transport. The CP sends a fixed sequence of
  commands (OUTPUT, LED, BUZZER, MFG) and the PD sends a fixed sequence of
  events (CARDREAD, KEYPRESS, MFGREP). Each end self-verifies every item it
  receives against the shared expected sequence and exits non-zero on any field
  divergence — flagging a command/event wire-format break.

Both harnesses use only the public API from `<osdp.h>`, so the same source must
compile against both revisions; a harness build failure flags a **public API
break**.

## Channel modes

Every case runs under each of these (override with `MODES=...`):

- **`plain`** — no secure channel; frames go on the wire in the clear.
- **`secure`** — pre-shared SCBK on both ends plus `OSDP_FLAG_ENFORCE_SECURE`;
  nothing flows until the secure-channel handshake completes.
- **`install`** — `OSDP_FLAG_INSTALL_MODE`: the CP holds the SCBK, the PD is
  keyless, and the channel is provisioned over the default key (SCBK-D) and then
  re-keyed via KEYSET. Exercises the install/provisioning SC path.

## Usage

```sh
# defaults: A=master, B=HEAD
tests/interop/interop-check.sh

# explicit revisions (tags, branches, or SHAs)
tests/interop/interop-check.sh v3.0.0 HEAD

# larger file payload (more fragments)
FT_SIZE=1048576 tests/interop/interop-check.sh master my-feature-branch

# only a subset of modes
MODES="plain secure" tests/interop/interop-check.sh
```

Exit code is non-zero if any combination fails.

## Requirements

`socat`, `cmake`, a C compiler. Revisions must be committed (a dirty working
tree is not tested — commit first). Uses throwaway `git worktree`s under a temp
dir and cleans them up on exit.
