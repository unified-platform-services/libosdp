#!/usr/bin/env bash
#
# LibOSDP PyTest Coverage Runner
#
# Measures what the pytest suite actually covers, in two layers:
#
#   - python/osdp/*.py       via coverage.py
#   - python/osdp_sys/*.c    via gcov, along with the src/ and utils/ sources
#     plus src/*.c           that setup.py links straight into the extension
#
# The second half is the interesting one: because the extension compiles the
# library in rather than linking a shared object, a single instrumented build
# yields gcov data for the bindings *and* the C core. The pytest suite is a
# full-stack coverage source for src/, complementary to tests/unit-tests/.
#
# This is a local diagnostic; scripts/run_pytests.sh remains the correctness
# gate and deliberately keeps testing the uninstrumented, wheel-installed path.
#
# USAGE:
#   ./scripts/run_coverage.sh [PYTEST_ARGS]
#
#   ./scripts/run_coverage.sh                    # everything
#   ./scripts/run_coverage.sh -m "not slow"      # skip the offline-timeout tests
#   ./scripts/run_coverage.sh -s                 # reuse the existing .venv-cov
#

set -e

SCRIPTS_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
ROOT_DIR="$( cd -- "${SCRIPTS_DIR}/.." &> /dev/null && pwd )"
COV_DIR="${ROOT_DIR}/coverage"

SKIP_SETUP=false
PYTEST_ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        -s|--skip-setup) SKIP_SETUP=true; shift ;;
        *) PYTEST_ARGS+=("$1"); shift ;;
    esac
done

cd "${ROOT_DIR}/tests/pytest"

if [ "$SKIP_SETUP" = false ]; then
    echo "[-] Creating an isolated environment.."
    rm -rf __pycache__/ .venv-cov
    rm -rf "${ROOT_DIR}"/python/{build,dist,libosdp.egg-info,vendor}
    python3 -m venv .venv-cov
    source ./.venv-cov/bin/activate
    pip install --quiet --upgrade pip

    echo "[-] Installing dependencies.."
    pip install --quiet -r requirements.txt

    # Build in-place rather than letting pip drive it. setuptools' PEP 660
    # editable path compiles in a temporary directory it then deletes, taking
    # the .gcno files with it -- leaving an instrumented .so that can never
    # emit a .gcda. Building in-place keeps them under python/build/temp.*/,
    # next to where the .gcda will land at exit.
    echo "[-] Building an instrumented libosdp (this takes a minute).."
    pip install --quiet setuptools
    ( cd "${ROOT_DIR}/python" && \
      OSDP_COVERAGE=1 \
      OSDP_PD_ONLINE_TOUT_MS="${OSDP_PD_ONLINE_TOUT_MS:-1500}" \
        python setup.py --quiet build_ext --inplace )

    # Make python/ importable without pip, which would rebuild the extension
    # and clobber the instrumented .so we just built. A .pth is all an editable
    # install really is, and it keeps --cov=osdp pointing at python/osdp/
    # rather than at a copy of it under site-packages/.
    echo "[-] Making python/ importable.."
    python - "${ROOT_DIR}/python" <<'EOF'
import site, sys, pathlib
pathlib.Path(site.getsitepackages()[0], "libosdp-coverage.pth").write_text(sys.argv[1] + "\n")
EOF
else
    echo "[-] Using existing environment.."
    [ -d .venv-cov ] || { echo "ERROR: no .venv-cov; run without -s first."; exit 1; }
    source ./.venv-cov/bin/activate
fi

# The editable install is the one fragile link here: if it silently falls back
# to a copy, coverage.py measures site-packages and reports a number for a tree
# nobody edits. Fail loudly instead.
echo "[-] Verifying the editable install.."
OSDP_PATH="$(python -c 'import osdp; print(osdp.__file__)')"
if [[ "${OSDP_PATH}" != "${ROOT_DIR}/python/osdp/"* ]]; then
    echo "ERROR: osdp resolves to ${OSDP_PATH}"
    echo "       expected it under ${ROOT_DIR}/python/osdp/"
    echo "       coverage would measure the wrong tree; aborting."
    exit 1
fi

# Counters accumulate across runs; a stale .gcda would inflate the C numbers.
find "${ROOT_DIR}/python/build" -name '*.gcda' -delete 2>/dev/null || true
rm -rf "${COV_DIR}"
mkdir -p "${COV_DIR}/python" "${COV_DIR}/c"

echo "[-] Running the test suite.."
coverage erase
pytest --cov --cov-report= "${PYTEST_ARGS[@]}"

echo "[-] Running the doctests.."
# Can't live in addopts: it would try to collect doctests from conftest.py too.
pytest --doctest-modules --pyargs osdp --cov --cov-append --cov-report= || true

echo
echo "==> Python coverage (python/osdp/)"
coverage report
coverage html -d "${COV_DIR}/python"

echo
echo "==> C coverage (python/osdp_sys/, src/, utils/src/)"
# Paths are recorded under python/vendor/, which setup.py fills with a
# byte-identical copy of the sources; gcov renders the same lines from it.
# Filters must be absolute: gcovr resolves a relative one against the cwd
# (tests/pytest here), not against --root. tinyaes is vendored third party.
gcovr \
    --root "${ROOT_DIR}/python" \
    --filter "${ROOT_DIR}/python/vendor/src/" \
    --filter "${ROOT_DIR}/python/vendor/python/osdp_sys/" \
    --filter "${ROOT_DIR}/python/vendor/utils/src/" \
    --exclude "${ROOT_DIR}/python/vendor/src/crypto/" \
    --exclude-unreachable-branches \
    --html-details "${COV_DIR}/c/index.html" \
    --print-summary \
    --txt

echo
echo "[-] Reports written to:"
echo "      ${COV_DIR}/python/index.html"
echo "      ${COV_DIR}/c/index.html"
