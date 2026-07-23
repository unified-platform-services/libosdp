#!/bin/bash

cat > .git/hooks/pre-commit << ---
#!/bin/bash

# Release consistency: keep the five version files in agreement and refuse a
# broken release changelog. The logic lives in make_release.py (check-staged),
# which inspects the index — i.e. exactly what this commit will contain — and
# understands the prepared (marker "dev") vs released (marker empty) states.
if ! python3 scripts/make_release.py check-staged; then
	echo "pre-commit: release consistency check failed" && exit 1
fi

files=\$(git diff  --cached --name-only | grep -E ".*\.(cpp|cc|c\+\+|cxx|c|h|hpp)$")
if [[ ! -z "\${files}" ]]; then
	git diff -U0 --cached -- \${files} | ./scripts/clang-format-diff.py -p1
fi

---
chmod a+x .git/hooks/pre-commit && echo "Installed hook: .git/hooks/pre-commit"

# Signed-off-by is a commit-*message* requirement, so it must live in the
# commit-msg hook — the message does not exist yet when pre-commit runs.
cat > .git/hooks/commit-msg << ---
#!/bin/bash

# Require a DCO sign-off on every commit; add one with 'git commit -s'.
if ! grep -q '^Signed-off-by: .\+ <.\+@.\+>' "\$1"; then
	echo "commit-msg: missing Signed-off-by line; commit with 'git commit -s'" >&2
	exit 1
fi

---
chmod a+x .git/hooks/commit-msg && echo "Installed hook: .git/hooks/commit-msg"
