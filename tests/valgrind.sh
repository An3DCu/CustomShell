#!/bin/sh
# make test-valgrind: the end-to-end tests with acsh and every tool run under
# valgrind. It fails if a test fails, or if valgrind finds a memory error or a
# leak in any of those processes. A process the tests kill on purpose can't
# free its memory first, so for those only memory errors count.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD:-build}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tools" "$work/logs"

vg="valgrind --child-silent-after-fork=yes --leak-check=full --show-leak-kinds=all"
vg="$vg --errors-for-leak-kinds=definite,indirect --log-file=$work/logs/%q{VGNAME}.%p.log"

# A small script in place of each program. It runs the real one under valgrind,
# with its name in the log file's name.
wrap() {
    printf '#!/bin/sh\nVGNAME=%s exec %s "%s" "$@"\n' "$3" "$vg" "$1" > "$2"
    chmod +x "$2"
}
wrap "$root/$build/acsh" "$work/acsh" acsh
for t in "$root/$build/tools/"*; do
    wrap "$t" "$work/tools/${t##*/}" "${t##*/}"
done

cd "$root" || exit 1
ACSH="$work/acsh" TOOLS="$work/tools" .venv/bin/python -m pytest -q -p no:cacheprovider \
    -m "not not_under_valgrind" tests/e2e
tests=$?

errors='Invalid (read|write|free)|uninitialised|Conditional jump|Syscall param|Mismatched free|overlap'
problems=0
for log in "$work"/logs/*.log; do
    if grep -qE "$errors" "$log" \
        || { grep -qE '(definitely|indirectly) lost: [1-9]' "$log" \
             && ! grep -q 'Process terminating' "$log"; }; then
        problems=$((problems + 1))
        echo "== ${log##*/}"
        cat "$log"
    fi
done
echo "valgrind: $(ls "$work/logs" | wc -l) processes, $problems with a problem"
[ "$tests" -eq 0 ] && [ "$problems" -eq 0 ]
