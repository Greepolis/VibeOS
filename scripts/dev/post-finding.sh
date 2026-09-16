#!/usr/bin/env bash
# Fire a security-review finding at the repo's repository_dispatch "webhook", which
# the Finding-to-discussion workflow turns into a comment on the running
# "Security review findings" Discussion.
#
# This POSTs to  /repos/<owner>/<repo>/dispatches  as YOU: it uses gh's auth, and
# that token must have permission to send a repository_dispatch (a classic PAT
# with `repo` scope, or a fine-grained token with Contents: read/write). The
# workflow itself then writes the discussion with the built-in GITHUB_TOKEN.
#
# repository_dispatch only triggers the workflow when the workflow file is on the
# default branch, so this does nothing until finding-to-discussion.yml is on main.
#
# Usage:
#   scripts/dev/post-finding.sh \
#     --id H-030 --sev H \
#     --problema "..." --descrizione "..." --posizione "file:line" --fix "..."
#
#   # or hand it a whole pre-formatted markdown block used verbatim:
#   scripts/dev/post-finding.sh --id H-030 --markdown-file finding.md
#
#   # override the target repo (default: the origin remote of this checkout):
#   REPO=owner/name scripts/dev/post-finding.sh --id ... --problema ...
set -euo pipefail

id=""; sev=""; problema=""; descrizione=""; posizione=""; fix=""; md=""
while [ $# -gt 0 ]; do
  case "$1" in
    --id)          id="$2"; shift 2 ;;
    --sev|--severity) sev="$2"; shift 2 ;;
    --problema)    problema="$2"; shift 2 ;;
    --descrizione) descrizione="$2"; shift 2 ;;
    --posizione)   posizione="$2"; shift 2 ;;
    --fix)         fix="$2"; shift 2 ;;
    --markdown-file) md="$(cat "$2")"; shift 2 ;;
    -h|--help)     grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$id" ]; then echo "--id is required" >&2; exit 2; fi
if [ -z "$problema" ] && [ -z "$md" ]; then
  echo "either --problema or --markdown-file is required" >&2; exit 2
fi

# Target repo: $REPO if set, else parse it out of the origin remote.
repo="${REPO:-}"
if [ -z "$repo" ]; then
  url=$(git config --get remote.origin.url || true)
  repo=$(printf '%s' "$url" | sed -E 's#^.*github\.com[:/]##; s/\.git$//')
fi
if [ -z "$repo" ]; then echo "could not determine repo; set REPO=owner/name" >&2; exit 2; fi

# Build the payload with jq so every field is correctly JSON-escaped, whatever it
# contains. A markdown block, when given, wins over the individual fields.
payload=$(jq -n \
  --arg id "$id" --arg sev "$sev" --arg problema "$problema" \
  --arg descrizione "$descrizione" --arg posizione "$posizione" \
  --arg fix "$fix" --arg markdown "$md" \
  '{event_type:"finding",
    client_payload: ( {id:$id, severity:$sev, problema:$problema,
                       descrizione:$descrizione, posizione:$posizione, fix:$fix}
                      + (if $markdown == "" then {} else {markdown:$markdown} end) )}')

echo "POST /repos/$repo/dispatches  (event_type=finding, id=$id)"
printf '%s' "$payload" | gh api "repos/$repo/dispatches" --input -
echo "dispatched. Watch it at: https://github.com/$repo/actions/workflows/finding-to-discussion.yml"
