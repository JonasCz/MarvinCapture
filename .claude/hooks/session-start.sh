#!/bin/bash
# Cloud sessions start on a fresh local branch claude/<name> (created by the session, not on GitHub until pushed).
# This repository works directly on main (CLAUDE.md): switch the checkout to main when that branch carries no work of
# its own, and remind Claude where to commit. Local sessions are left alone.
set -uo pipefail

[ "${CLAUDE_CODE_REMOTE:-}" = "true" ] || exit 0
cd "${CLAUDE_PROJECT_DIR:-.}" || exit 0

branch=$(git symbolic-ref --quiet --short HEAD 2>/dev/null || true)
note=""
if [[ "$branch" == claude/* ]]; then
  # the session updates only its own branch: origin/main (and main) can be older than GitHub's main
  timeout 60 git fetch --quiet origin main 2>/dev/null || true
  if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    note="The checkout is on $branch with uncommitted changes, so it was left there."
  elif [ -n "$(git rev-list -n 1 "origin/main..$branch" 2>/dev/null)" ]; then
    note="The checkout is on $branch, which has commits that are not on main, so it was left there."
  elif ! git switch --quiet main 2>/dev/null; then
    note="Switching from $branch to main failed; switch to main before committing."
  elif ! git merge --quiet --ff-only origin/main 2>/dev/null; then
    note="The checkout was switched from $branch to main, but main could not be fast-forwarded to origin/main."
  else
    note="The checkout was switched from the session branch $branch (no commits of its own) to main."
  fi
fi

# a SessionStart hook's stdout is added to Claude's context
echo "This repository works directly on main (CLAUDE.md): commit and push to main, even though the session names a claude/... branch to develop on. Do not push that branch or open a pull request unless the user asks for one.${note:+ $note}"
