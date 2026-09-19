#!/usr/bin/env bash
# Everything that has to pass before a commit, in one command that fails
# loudly. Run from the repository root.
#
# It rebuilds first. A test run against a stale build passes while the change
# under test was never compiled -- which is exactly how a broken binding got
# committed once.
set -euo pipefail

BUILD="${1:-build}"
cd "$(dirname "$0")"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "no configured build in '$BUILD'."
  echo "configure one first, e.g.:"
  echo "  cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Release"
  exit 1
fi

echo "== formatting"
./format.sh --check

echo "== building ($BUILD)"
cmake --build "$BUILD" -j"$(nproc)" >/dev/null

echo "== tests"
ctest --test-dir "$BUILD" --output-on-failure

# Only when this build actually produced the module.
if compgen -G "$BUILD/hux*.so" >/dev/null; then
  echo "== python bindings"
  PYTHONPATH="$BUILD" python3 tests/python/test_bindings.py
fi

echo
echo "all checks passed ($BUILD)"
