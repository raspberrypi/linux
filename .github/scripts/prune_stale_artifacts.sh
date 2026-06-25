#!/usr/bin/env bash
# Delete artifacts that exceed the retention periods defined in kernel-build.yml.
# Parses build names and retention values directly from the workflow file, so
# it stays correct as the matrix evolves.
# Dry-run by default; set DRY_RUN=0 to actually delete.
#
# Prerequisites:
#   - gh CLI authenticated (gh auth status)
#   - jq installed
#   - Token needs actions:write scope (or fine-grained Actions: Read and write)

set -euo pipefail

REPO="raspberrypi/linux"
DRY_RUN="${DRY_RUN:-1}"
TODAY_EPOCH=$(date -u +%s)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKFLOW_FILE="${SCRIPT_DIR}/../workflows/kernel-build.yml"

# Extract build names and retention values from the workflow matrix.
# Uses a minimal line-by-line parser: tracks the current '- name:' entry
# and emits name=days when it encounters the matching 'retention:' field.
declare -A RETENTION
while IFS='=' read -r name days; do
  RETENTION["${name}_build"]="$days"
done < <(python3 - "$WORKFLOW_FILE" <<'EOF'
import sys, re

current_name = None
with open(sys.argv[1]) as f:
    for line in f:
        m = re.match(r'\s+- name:\s+(\S+)', line)
        if m:
            current_name = m.group(1)
            continue
        m = re.match(r'\s+retention:\s+(\d+)', line)
        if m and current_name:
            print(f"{current_name}={m.group(1)}")
            current_name = None
EOF
)

if [[ ${#RETENTION[@]} -eq 0 ]]; then
  echo "Error: no matrix entries with retention found in ${WORKFLOW_FILE}" >&2
  exit 1
fi

echo "Retention policy from $(basename "$WORKFLOW_FILE"):"
for name in $(echo "${!RETENTION[@]}" | tr ' ' '\n' | sort); do
  echo "  ${name}: ${RETENTION[$name]} days"
done
echo ""

IS_INTERACTIVE=0
[[ -t 1 ]] && IS_INTERACTIVE=1

deleted=0
stale=0
kept=0

for artifact_name in "${!RETENTION[@]}"; do
  max_age_days="${RETENTION[$artifact_name]}"
  cutoff_epoch=$(( TODAY_EPOCH - max_age_days * 86400 ))

  # Collect all stale IDs before deleting anything, so that deletions
  # during iteration don't shift pages and cause artifacts to be missed.
  stale_ids=()
  stale_meta=()  # parallel array: "created_at age_days" per entry

  page=1
  while true; do
    response=$(gh api \
      "/repos/${REPO}/actions/artifacts?per_page=100&page=${page}&name=${artifact_name}")

    items=$(echo "$response" | jq -c '.artifacts[]')
    [[ -z "$items" ]] && break

    while IFS= read -r item; do
      id=$(echo "$item" | jq -r '.id')
      created_at=$(echo "$item" | jq -r '.created_at')
      created_epoch=$(date -u -d "$created_at" +%s)

      age_days=$(( (TODAY_EPOCH - created_epoch) / 86400 ))
      if (( created_epoch < cutoff_epoch )); then
        stale_ids+=("$id")
        stale_meta+=("${created_at} ${age_days}")
        if [[ "$DRY_RUN" != "0" ]]; then
          echo "STALE  id=$id  name=$artifact_name  created=$created_at  age=${age_days}d"
        elif [[ "$IS_INTERACTIVE" == "1" ]]; then
          echo -ne "\r  ${artifact_name}  age=${age_days}d      "
        fi
        (( stale++ )) || true
      else
        if [[ "$IS_INTERACTIVE" == "1" ]]; then
          echo -ne "\r  ${artifact_name}  age=${age_days}d      "
        fi
        (( kept++ )) || true
      fi
    done <<< "$items"

    item_count=$(echo "$response" | jq '.artifacts | length')
    (( item_count < 100 )) && break
    (( page++ ))
  done

  if [[ "$DRY_RUN" != "0" ]]; then
    continue
  fi

  # End the progress line before printing deletion output
  [[ "$IS_INTERACTIVE" == "1" && ${#stale_ids[@]} -gt 0 ]] && echo

  for i in "${!stale_ids[@]}"; do
    id="${stale_ids[$i]}"
    created_at="${stale_meta[$i]% *}"
    age_days="${stale_meta[$i]##* }"
    gh api --method DELETE "/repos/${REPO}/actions/artifacts/${id}"
    echo "DELETED  id=$id  name=$artifact_name  created=$created_at  age=${age_days}d"
    (( deleted++ )) || true
  done
done

echo ""
if [[ "$DRY_RUN" != "0" ]]; then
  echo "Done. Stale: $stale  Within-policy: $kept  (dry run)"
else
  echo "Done. Deleted: $deleted  Within-policy: $kept"
fi
