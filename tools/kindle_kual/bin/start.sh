#!/bin/sh
set -eu

APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOG="$APP_DIR/pokeemerald-kindle.log"
SAVE="$APP_DIR/pokeemerald-kindle.sav"

export HOME="/mnt/us"

lipc-set-prop com.lab126.powerd preventScreenSaver 1 2>/dev/null || true
lipc-set-prop com.lab126.powerd deferSuspend 1 2>/dev/null || true
cleanup() {
    lipc-set-prop com.lab126.powerd preventScreenSaver 0 2>/dev/null || true
    lipc-set-prop com.lab126.powerd deferSuspend 0 2>/dev/null || true
}
trap cleanup EXIT

cd "$APP_DIR"
trap 'exit 130' INT
trap 'exit 143' TERM

status=0
"$APP_DIR/bin/pokeemerald-kindle" --save "$SAVE" --display-fps "${POKEEMERALD_DISPLAY_FPS:-4}" >> "$LOG" 2>&1 || status=$?
exit "$status"
