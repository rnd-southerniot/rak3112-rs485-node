#!/usr/bin/env bash
# firmware/scripts/apply-patches.sh
# Applies project-vendored ESP-IDF patches to $IDF_PATH.
# Idempotent: safe to re-run (skips already-applied via git apply --reverse --check).
# --dry-run flag verifies all patches apply cleanly without touching the tree.
#
# Specified by CLAUDE.md rev5 §5 EC-1.
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5.4}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES_DIR="$SCRIPT_DIR/../esp-idf-patches"

DRY_RUN=false
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true

# Preconditions — clear errors over confusing apply failures.
test -d "$IDF_PATH/.git" || {
    echo "ERROR: IDF_PATH ($IDF_PATH) is not a git repository" >&2
    exit 2
}
test -d "$PATCHES_DIR" || {
    echo "ERROR: PATCHES_DIR ($PATCHES_DIR) does not exist" >&2
    exit 2
}

# Deterministic ordering across locales (load-bearing for >= 2 patches).
for patch in $(printf '%s\n' "$PATCHES_DIR"/*.patch | LC_ALL=C sort); do
    [[ -f "$patch" ]] || continue
    if git -C "$IDF_PATH" apply --check "$patch" 2>/dev/null; then
        if $DRY_RUN; then
            echo "Would apply: $(basename "$patch")"
        else
            git -C "$IDF_PATH" apply "$patch"
            echo "Applied: $(basename "$patch")"
        fi
    elif git -C "$IDF_PATH" apply --reverse --check "$patch" 2>/dev/null; then
        echo "Already applied: $(basename "$patch")"
    else
        echo "FAIL: $(basename "$patch") does not apply cleanly to $IDF_PATH" >&2
        exit 1
    fi
done
