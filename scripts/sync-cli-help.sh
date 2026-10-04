#!/usr/bin/env bash
# scripts/sync-cli-help.sh CLI
#
# The CLI's help text lives in one place, src/engine/pin_script.c. This writes
# docs/cli.md from docs/cli.md.template, replacing the line "@CLI_HELP@" with
# the help text (in a fenced block). Both files are tracked. Run by
# scripts/build.sh after the build; safe to run by hand.
set -euo pipefail
cli="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
tmpl="${root}/docs/cli.md.template"
doc="${root}/docs/cli.md"
tmp="$(mktemp)"; trap 'rm -f "$tmp" "$tmp.out"' EXIT
"$cli" --help-text > "$tmp"
awk -v helpfile="$tmp" '
  $0 == "@CLI_HELP@" { print "```text"; while ((getline l < helpfile) > 0) print l; print "```"; next }
  { print }
' "$tmpl" > "$tmp.out"
cmp -s "$tmp.out" "$doc" || cp "$tmp.out" "$doc"
