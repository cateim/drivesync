#!/usr/bin/env bash
# ==============================================================================
# open-history.sh: what the W key and the history button of the SyncWatch bar
# widget open. The widget ships inside the SyncWatch package, so the app is
# always there: focus its window when one is open, otherwise start it.
#
# Takes no arguments, so nothing from the shell is ever interpolated here.
# ==============================================================================
set -uo pipefail

# Address of an open SyncWatch window, matched by the exact app_id only:
# omarchy-launch-or-focus also matches titles, so a terminal open in a folder
# called syncwatch would take the focus instead.
syncwatch_window() {
  hyprctl clients -j 2>/dev/null \
    | jq -r '.[] | select(.class == "syncwatch") | .address' 2>/dev/null \
    | head -n1
}

address=$(syncwatch_window)
if [[ $address =~ ^0x[0-9a-f]+$ ]]; then
  # Same two forms omarchy-launch-or-focus tries (Lua config first).
  hyprctl dispatch "hl.dsp.focus({ window = \"address:$address\" })" >/dev/null 2>&1 \
    || hyprctl dispatch focuswindow "address:$address" >/dev/null
else
  exec uwsm-app -- syncwatch
fi
