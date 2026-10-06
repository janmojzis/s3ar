#!/bin/sh
# Generate the migrated man pages, or check them without modifying the tree.
set -eu

mode=${1:-}
case "$mode" in
    build|check) shift ;;
    *) echo 'usage: generate-man.sh build|check SOURCE.md...' >&2; exit 2 ;;
esac
if [ "$#" -eq 0 ]; then
    echo 'no Markdown manuals specified' >&2
    exit 2
fi
pandoc_command=${PANDOC:-pandoc}
required_version=${PANDOC_VERSION:-3.1.11.1}
if ! command -v "$pandoc_command" >/dev/null 2>&1; then
    echo "Pandoc $required_version is required for make man/check-man" >&2
    exit 2
fi
actual_version=$("$pandoc_command" --version | sed -n '1s/^pandoc //p')
if [ "$actual_version" != "$required_version" ]; then
    echo "Pandoc $required_version is required; found $actual_version" >&2
    exit 2
fi

# Use the same filesystem as the targets so mv replaces each file atomically.
temporary_dir=$(mktemp -d ./.man.XXXXXX)
trap 'rm -rf -- "$temporary_dir"' 0
trap 'exit 2' HUP INT TERM

# Finish all conversions before updating any checked-in manual.
for source in "$@"; do
    target=${source%.md}.1
    output=$temporary_dir/$(basename "$target")
    {
        printf '.\\" Generated from %s; edit the Markdown and run make man.\n' "$source"
        "$pandoc_command" --standalone --from=markdown-smart --to=man \
            --fail-if-warnings --wrap=auto --columns=80 "$source"
    } > "$output"
done

status=0
for source in "$@"; do
    target=${source%.md}.1
    output=$temporary_dir/$(basename "$target")
    if [ "$mode" = build ]; then
        mv -- "$output" "$target"
    elif ! cmp -s -- "$target" "$output"; then
        echo "$target differs from $source; run make man" >&2
        diff -u -- "$target" "$output" || :
        status=1
    fi
done
exit "$status"
