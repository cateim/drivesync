# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### ✨ Added

- Status headline for drive-sync: in sync, syncing now (phase, files listed and a progress bar against the usual run time), resync in progress, failed with its cause and how to recover, cut off, runs skipped, timer off, and no sync for a while.
- Warning chips for a timer that is off or will not start on boot, and for conflict copies waiting in `~/Drive`.
- Runs tab with the last 40 runs from the log, each with the changes on each side, file by file, and its errors (selectable to copy).
- Conflict copies tab listing every `*.conflictN` file in `~/Drive`, newest first; `Enter` shows the picked one in the file manager.
- Keyboard shortcuts (`Space`, `Tab`, `1`, `2`, arrows, `J`, `K`, `Enter`, `B`, `N`, `?`, `Q`) and a `?` overlay that lists them.
- A state glyph beside the headline: a cloud with a check when in sync, with arrows while syncing, with an alert on trouble.
- `drivesync status` prints the state without a window, as one line or, with `--json`, as a versioned JSON object for the bar and scripts.
- A widget for the Omarchy bar, shipped in the package, showing the same state as the window: last sync, last and next run, what failed, conflict copies, and buttons to sync now, open DriveSync and open `~/Drive`.
- The window links the widget into Omarchy's plugin folder and offers to put it on the bar (`B`); `N` says no, and the offer does not come back.
- Follows the Omarchy accent, window rounding and language live; ships English and Brazilian Portuguese.
- Package with `.desktop` entry and icon, installed by `bin/install`, so the app appears in the Omarchy launcher.
- Takeover of an existing drive-sync (`T` in the window, `drivesync takeover`): DriveSync's packaged user units run the same rclone command with the same remote, folder, listings and filters (moved with their `.md5`), so no resync is needed. The old units and scripts move to `~/.local/share/drivesync/taken-over/` with a manifest, nothing is deleted, a failed step undoes every step before it (when an old file cannot move back, every drive-sync timer is left off and the error says where it is), and `drivesync takeover --undo` goes back. The window names what blocks a takeover (a run going, a failed last run, units it cannot keep).
- Sync now, pause and resume from the window (`S`, `P`, `U`), the bar (`S`, `P`) and the command line (`drivesync sync`, `pause`, `resume`). A pause lasts one hour, until the next 8:00 or until resumed, survives a reboot and shows as "Paused" instead of a failure or "no sync for a while".
- Desktop notifications from DriveSync itself: a critical card with the cause when a run fails, a reminder every hour while it keeps failing, a note when runs work again, an hourly check for a timer that is off, not enabled or not loading, and a daily reminder of a pause with no end.
- `drivesync run` refuses to start rclone when the filters' `.md5` is missing or stale, because one bisync run in that state throws its base away until a resync.
- The status JSON is version 2: `managed`, `paused` and `pausedUntil` join the 17 keys of version 1.
- `T` during a run arms the takeover: the dialog opens by itself once the run ends, since runs take about half of each 15 minute tick. A takeover confirmed just as a run starts waits for that run and then happens, where it used to fail with "running".
- `?` in the bar panel lists its keys, in place of the key names beside the icons, which cut the last one off. The key that opens DriveSync from the panel is now `W`: `l` moved the cursor, so `L` only worked with Shift.
- `drivesync run` adds `--fast-list` to the bisync command unless the config turns ListR off. Against Google Drive the listing went from 476 s to 40 s for 33k files, with the same listing, so a run takes about a minute instead of eight; it needs no resync.
- `bin/harness` draws the window offscreen over a made-up drive-sync and saves it as a PNG, without opening anything on the desktop. The window pictures in the README come from it.
- The window opens `~/Drive` with `O` or its "Open the folder" button, as the bar does, through the running file manager.
- Fix a broken sync from DriveSync (`F` in the window, `drivesync fix`): after lost listings, listings out of sync or edited filters, it rebuilds the base of both sides with `~/Drive` still in use, as `drive-sync.sh resync` did, in its own unit (`drive-sync-fix.service`), so closing the window or logging out does not stop it halfway. A dry run lists the copies first, each with its direction and the ones to the notebook on top (where a file deleted there would come back), and nothing changes until it is confirmed; files that changed on the notebook during the base run are kept, and files that changed in the cloud, or appeared only there, are asked about one by one (`C` keeps the cloud's, `L` the notebook's, sending a cloud-only file to the Drive trash). Timer ticks, `S` and the bar's sync wait until the fix ends; a fix that waits for an answer or stops sends a card at once and every hour after. The fix is refused where the config's `runOnlyWhen` says no (the emergency home) and after `--max-delete` stopped a run, since a resync would bring the deleted files back; once the base ran, `X` asks twice before throwing it away.
- The fix's dry run and base run pass `--max-lock 2m`, so a base run killed halfway leaves a lock that expires 2 minutes later, and the retry waits it out (the final normal run keeps the config's own, 2m after a takeover). The old script's base run had no `--max-lock`, and a killed one blocked every later run for good.
- The status JSON and the bar know three new statuses, still in version 2: `fixing` (with `resync` true, so the bar shows the resync icon), `fixWaiting` and `fixFailed`.

### 🔧 Changed

- The takeover looks for the bisync listings of its own two paths, by the name rclone gives them, so a stale session left in the work dir no longer passes for them.
- Edited filters are a cause, not an alarm: the window, the bar and a normal card say to press `F`, where a critical "Drive stopped syncing" card used to repeat every hour. Lost listings and listings out of sync say "Press F" too once DriveSync runs drive-sync, instead of `drivesync takeover --undo` and `drive-sync.sh resync`.
- The app is now DriveSync: the command is `drivesync`, the package `drivesync` (it replaces `syncwatch`), and the bar widget id `drivesync`. The first `drivesync` command moves `~/.config/syncwatch`, `~/.local/state/syncwatch` and `~/.local/share/syncwatch` to `drivesync`, keeping the filters and their `.md5` byte for byte, so no resync is needed, and points the config and the takeover backups at the new folders. The widget takes the old one's place on the bar by itself the first time the window opens.

### 🐛 Fixed

- Clock times show 12 or 24 hours like the clock on the Omarchy bar, in the window, the bar panel and the notifications, and follow a right click on that clock. The window showed AM/PM whenever Omarchy was in English, even with the bar clock on 24 hours, and the panel always showed 24 hours.
- `drivesync --help` (and `-h`, `help`) prints the usage. It opened the window, like every argument that starts with a dash.
- A warning on the bar (conflict copies, a late, skipped or interrupted run) turns the cloud and the panel icon orange, the same orange as the window. It looked just like an up-to-date sync.
- A log with no run in it (new, or just rotated) shows "No sync in the log yet" as a warning in the window and on the bar. It showed a green "In sync", and the bar "Synced at -".
- A failure only systemd saw (the log ends in a good run) says it is failing since the service's last start, not since that good run.
- The failing line in the bar panel has a sentence for a run stopped at its time limit and for a failure only systemd reports, and is never blank.
- When `omarchy plugin list` fails as the window opens, the window no longer says it could not put DriveSync on the bar: nobody asked yet. After `B` it still does.
- `N` also hides the notice that someone else's folder or link holds the widget's place, which came back at every start.

[Unreleased]: https://github.com/CaTeIM/drivesync/commits/master
