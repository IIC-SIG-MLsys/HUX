#!/usr/bin/env bash
# The sanitizer runs, which the ordinary tests cannot stand in for: a data
# race or a use-after-free that does no visible harm on this machine today
# is still there. Run from the repository root.
#
#   ./sanitize.sh          both
#   ./sanitize.sh tsan     threads only
#   ./sanitize.sh asan     memory and undefined behaviour only
#
# The builds are separate directories, because the two sanitizers cannot be
# combined and neither belongs in the build a benchmark is measured from.
set -euo pipefail
cd "$(dirname "$0")"

WHICH="${1:-both}"

configure() { # dir, flags
  if [ ! -f "$1/CMakeCache.txt" ]; then
    cmake -S . -B "$1" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="$2" -DCMAKE_EXE_LINKER_FLAGS="$2" >/dev/null
  fi
  cmake --build "$1" -j"$(nproc)" >/dev/null
}

run_tsan() {
  echo "== thread sanitizer"
  configure build-tsan "-fsanitize=thread -g -O1"
  # setarch -R because ThreadSanitizer maps its shadow memory at fixed
  # addresses, and a kernel handing out more address-space randomisation than
  # it expects makes it exit before the first test with "unexpected memory
  # mapping". Disabling randomisation for this process is the supported way
  # round it and changes nothing about what is being tested.
  setarch "$(uname -m)" -R ./build-tsan/tests/hux_contract_tests
}

run_asan() {
  echo "== address and undefined-behaviour sanitizers"
  configure build-asan "-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    ./build-asan/tests/hux_contract_tests
}

case "$WHICH" in
  tsan) run_tsan ;;
  asan) run_asan ;;
  both) run_tsan; echo; run_asan ;;
  *) echo "usage: $0 [tsan|asan|both]"; exit 1 ;;
esac

echo
echo "clean."
