#!/usr/bin/env bash
# Pinnacle Studio 500-USB open driver
# Copyright (C) 2026 Jonas Cz.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
# for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# scripts/publish-release.sh
#
# Creates the GitHub release v<version> ("version" from the top-level VERSION file)
# with the installers in DIR and a SHA256SUMS.txt, on the commit that is checked
# out. Uses the GitHub CLI (gh, logged in or GH_TOKEN set). The CI workflow
# (.github/workflows/build.yml, see docs/ci.md) runs it after building; it works the
# same from a local checkout with the installers collected in one folder.
#
# Refuses (exit 1) when a release for the tag already exists (drafts included), or
# the tag exists on another commit: bump VERSION first. The commit must be pushed.
#
# Usage:
#   scripts/publish-release.sh [--draft] [--title TITLE] [--notes NOTES] DIR
#   scripts/publish-release.sh --check
#
#   DIR             folder with the installers (*.exe, *.pkg, *.deb), all uploaded
#   --draft         create a draft (published later on the Releases page)
#   --title TITLE   default "MarvinCapture <version>"
#   --notes NOTES   Markdown; default "release_notes" from VERSION
#   --check         only check that the release can be created (nothing is changed)
#   --repo OWNER/NAME   default: gh's (the checkout's GitHub remote)

set -euo pipefail

usage() { sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { echo "publish-release.sh: $*" >&2; exit 1; }

DRAFT=0
CHECK=0
TITLE=""
NOTES=""
DIR=""
REPO_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --draft) DRAFT=1; shift ;;
        --check) CHECK=1; shift ;;
        --title) [[ $# -ge 2 ]] || die "--title needs a value"; TITLE="$2"; shift 2 ;;
        --notes) [[ $# -ge 2 ]] || die "--notes needs a value"; NOTES="$2"; shift 2 ;;
        --repo)  [[ $# -ge 2 ]] || die "--repo needs a value"; REPO_ARGS=(--repo "$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option '$1'" ;;
        *) [[ -z "${DIR}" ]] || die "only one DIR"; DIR="$1"; shift ;;
    esac
done
[[ "${CHECK}" -eq 1 || -n "${DIR}" ]] || { usage >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v gh >/dev/null 2>&1 || die "the GitHub CLI (gh) is not installed"
command -v jq >/dev/null 2>&1 || die "jq is not installed"

VERSION="$(jq -r .version "${ROOT}/VERSION")"
TAG="v${VERSION}"
COMMIT="$(git -C "${ROOT}" rev-parse HEAD)"
REPO="$(gh repo view ${REPO_ARGS[@]+"${REPO_ARGS[@]}"} --json nameWithOwner --jq .nameWithOwner)"

# --- checks --------------------------------------------------------------------------
if gh release view "${TAG}" --repo "${REPO}" >/dev/null 2>&1; then
    die "a release for ${TAG} already exists in ${REPO} (maybe a draft): bump \"version\" in VERSION, or delete that release"
fi
TAG_COMMIT="$(gh api "repos/${REPO}/commits/${TAG}" --jq .sha 2>/dev/null || true)"
if [[ -n "${TAG_COMMIT}" && "${TAG_COMMIT}" != "${COMMIT}" ]]; then
    die "tag ${TAG} already exists on another commit (${TAG_COMMIT}): bump \"version\" in VERSION"
fi
gh api "repos/${REPO}/commits/${COMMIT}" --jq .sha >/dev/null 2>&1 || die "commit ${COMMIT} is not on GitHub: push it first"
if [[ "${CHECK}" -eq 1 ]]; then
    echo "OK: ${TAG} can be released from ${COMMIT} in ${REPO}"
    exit 0
fi

# --- release ---------------------------------------------------------------------------
[[ -d "${DIR}" ]] || die "${DIR} is not a folder"
shopt -s nullglob
FILES=("${DIR}"/*.exe "${DIR}"/*.pkg "${DIR}"/*.deb)
[[ ${#FILES[@]} -gt 0 ]] || die "no installers (*.exe, *.pkg, *.deb) in ${DIR}"
SUMS="${DIR}/SHA256SUMS.txt"
(
    cd "${DIR}"
    names=()
    for f in "${FILES[@]}"; do names+=("$(basename "${f}")"); done
    if command -v sha256sum >/dev/null 2>&1; then sha256sum -- "${names[@]}"; else shasum -a 256 -- "${names[@]}"; fi
) > "${SUMS}"
cat "${SUMS}"

[[ -n "${TITLE}" ]] || TITLE="MarvinCapture ${VERSION}"
[[ -n "${NOTES}" ]] || NOTES="$(jq -r .release_notes "${ROOT}/VERSION")"
ARGS=(--repo "${REPO}" --title "${TITLE}" --notes "${NOTES}" --target "${COMMIT}")
[[ "${DRAFT}" -eq 1 ]] && ARGS+=(--draft)
gh release create "${TAG}" "${ARGS[@]}" "${FILES[@]}" "${SUMS}"
gh release view "${TAG}" --repo "${REPO}" --json url --jq .url
