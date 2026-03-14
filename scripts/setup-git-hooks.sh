#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [ ! -f ".gitmessage" ]; then
  echo "setup-git-hooks: .gitmessage not found at repo root" >&2
  exit 1
fi

if [ ! -f ".githooks/pre-commit" ] || [ ! -f ".githooks/commit-msg" ] || [ ! -f ".githooks/pre-push" ]; then
  echo "setup-git-hooks: expected hook files under .githooks/" >&2
  exit 1
fi

chmod +x .githooks/pre-commit .githooks/commit-msg .githooks/pre-push

git config --local core.hooksPath .githooks
git config --local commit.template .gitmessage

echo "Configured local git hooks:"
echo "  core.hooksPath=.githooks"
echo "  commit.template=.gitmessage"
echo
echo "Active hooks:"
echo "  - pre-commit (staged whitespace/conflict-marker checks)"
echo "  - commit-msg (conventional subject format check)"
echo "  - pre-push (commit subject check against push range)"
