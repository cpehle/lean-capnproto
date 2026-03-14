#!/usr/bin/env bash
set -euo pipefail

pattern='^(feat|fix|chore|ci|build|docs|refactor|test|perf)\([a-z0-9][a-z0-9._/-]*\): .+'
# Optional rollout guard: set COMMIT_MSG_ENFORCE_FROM=<commit> to skip older ancestors.
default_enforce_from=''

is_allowed_subject() {
  local subject="$1"
  if [[ "${subject}" =~ ^Merge[[:space:]] ]]; then
    return 0
  fi
  if [[ "${subject}" =~ ^Revert[[:space:]] ]]; then
    return 0
  fi
  if [[ "${subject}" =~ ${pattern} ]]; then
    return 0
  fi
  return 1
}

resolve_enforce_from() {
  local ref="${COMMIT_MSG_ENFORCE_FROM:-${default_enforce_from}}"
  if [[ -z "${ref}" ]]; then
    echo ""
    return 0
  fi
  if git rev-parse --verify "${ref}^{commit}" >/dev/null 2>&1; then
    git rev-parse --verify "${ref}^{commit}"
    return 0
  fi
  echo ""
}

resolve_range() {
  if [[ $# -ge 1 ]]; then
    echo "$1"
    return 0
  fi

  local event="${GITHUB_EVENT_NAME:-}"
  local sha="${GITHUB_SHA:-HEAD}"

  if [[ "${event}" == "pull_request" || "${event}" == "pull_request_target" ]]; then
    local base_ref="${GITHUB_BASE_REF:-main}"
    local base_remote="origin/${base_ref}"
    echo "${base_remote}...${sha}"
    return 0
  fi

  if [[ "${event}" == "push" ]]; then
    local before="${GITHUB_EVENT_BEFORE:-}"
    if [[ -n "${before}" && "${before}" != "0000000000000000000000000000000000000000" ]]; then
      echo "${before}..${sha}"
      return 0
    fi
  fi

  echo ""
}

range="$(resolve_range "$@")"
enforce_from="$(resolve_enforce_from)"
if [[ -n "${range}" ]]; then
  echo "Checking commit subjects in range: ${range}"
else
  echo "Checking commit subjects in the most recent reachable history window (up to 20 commits)."
fi
if [[ -n "${enforce_from}" ]]; then
  echo "Enforcing from commit (exclusive ancestors skipped): ${enforce_from}"
fi

if ! git rev-parse --verify HEAD >/dev/null 2>&1; then
  echo "No commits found in repository." >&2
  exit 1
fi

if [[ -n "${range}" ]]; then
  if ! git rev-list --count "${range}" >/dev/null 2>&1; then
    echo "Unable to resolve commit range: ${range}" >&2
    echo "Pass an explicit range, or ensure required refs are fetched." >&2
    exit 1
  fi
fi

if [[ -n "${range}" ]]; then
  commits="$(git rev-list --reverse "${range}" 2>/dev/null || true)"
else
  commits="$(git rev-list --reverse --max-count=20 HEAD 2>/dev/null || true)"
fi
if [[ -z "${commits}" ]]; then
  if [[ -n "${range}" ]]; then
    echo "No commits to validate for range: ${range}"
  else
    echo "No commits to validate."
  fi
  exit 0
fi

invalid=0
while IFS= read -r c; do
  if [[ -z "${c}" ]]; then
    continue
  fi

  if [[ -n "${enforce_from}" ]] && [[ "${c}" != "${enforce_from}" ]]; then
    if git merge-base --is-ancestor "${c}" "${enforce_from}" >/dev/null 2>&1; then
      continue
    fi
  fi

  subject="$(git log -1 --format=%s "${c}")"
  if ! is_allowed_subject "${subject}"; then
    echo "Invalid commit subject:"
    echo "  ${c} ${subject}"
    invalid=1
  fi
done <<< "${commits}"

if [[ ${invalid} -ne 0 ]]; then
  cat <<'EOF'
Expected format:
  type(scope): summary
Allowed types:
  feat, fix, chore, ci, build, docs, refactor, test, perf
Examples:
  feat(runtime): add packed reader bounds checks
  fix(rpc): guard null client hook in shutdown path
EOF
  exit 1
fi

echo "All commit subjects are valid."
