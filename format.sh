#!/usr/bin/env bash
# Formats the C++ sources in this repository.
#
#   ./format.sh           rewrite files in place
#   ./format.sh --check   report what would change and exit non-zero
#
# The check mode is what a CI job wants: it must not leave a modified tree
# behind, and it has to fail rather than silently fixing things.
#
# One clang-format version is required rather than a range. Different versions
# disagree on details, so allowing several would mean whoever ran last decides
# the diff, and every pull request would carry unrelated reformatting.
set -euo pipefail

REQUIRED_VERSION="14"
DIRECTORIES=("include" "src" "tests" "tools")
EXTENSIONS=("cpp" "cc" "h" "hpp" "cu" "cuh")
EXCLUDE=("build")

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

if [ -n "${CLANG_FORMAT:-}" ]; then
  :
elif command -v clang-format-"$REQUIRED_VERSION" >/dev/null 2>&1; then
  CLANG_FORMAT="clang-format-$REQUIRED_VERSION"
else
  CLANG_FORMAT="clang-format"
fi

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "$CLANG_FORMAT not found. Install clang-format-$REQUIRED_VERSION," >&2
  echo "for example: pip install clang-format==${REQUIRED_VERSION}.0.6" >&2
  exit 1
fi

# sed rather than grep -P, which BSD grep on macOS does not have.
INSTALLED=$("$CLANG_FORMAT" --version |
            sed -nE 's/.*[Vv]ersion[[:space:]]+([0-9]+).*/\1/p' | head -1)
if [ "$INSTALLED" != "$REQUIRED_VERSION" ]; then
  echo "clang-format $REQUIRED_VERSION required, found $INSTALLED ($CLANG_FORMAT)." >&2
  echo "Set CLANG_FORMAT=/path/to/clang-format-$REQUIRED_VERSION to override." >&2
  exit 1
fi

cd "$(dirname "$0")"

prune=()
for e in "${EXCLUDE[@]}"; do prune+=(-path "./$e" -prune -o); done

files=()
for dir in "${DIRECTORIES[@]}"; do
  [ -d "$dir" ] || continue
  for ext in "${EXTENSIONS[@]}"; do
    while IFS= read -r -d '' f; do files+=("$f"); done \
      < <(find "$dir" "${prune[@]}" -type f -name "*.${ext}" -print0)
  done
done

if [ "${#files[@]}" -eq 0 ]; then
  echo "no sources found"
  exit 0
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
  bad=()
  for f in "${files[@]}"; do
    if ! "$CLANG_FORMAT" "$f" | diff -q - "$f" >/dev/null; then bad+=("$f"); fi
  done
  if [ "${#bad[@]}" -eq 0 ]; then
    echo "${#files[@]} files, all formatted"
    exit 0
  fi
  echo "${#bad[@]} of ${#files[@]} files need formatting:" >&2
  printf '  %s\n' "${bad[@]}" >&2
  echo "run ./format.sh to fix" >&2
  exit 1
fi

"$CLANG_FORMAT" -i "${files[@]}"
echo "formatted ${#files[@]} files"
