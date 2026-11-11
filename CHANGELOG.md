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
- `syncwatch status` prints the state without a window, as one line or, with `--json`, as a versioned JSON object for the bar and scripts.
- A widget for the Omarchy bar, shipped in the package, showing the same state as the window: last sync, last and next run, what failed, conflict copies, and buttons to sync now, open SyncWatch and open `~/Drive`.
- The window links the widget into Omarchy's plugin folder and offers to put it on the bar (`B`), taking the place of an older drive-sync widget when one is there; `N` says no, and the offer does not come back.
- Follows the Omarchy accent, window rounding and language live; ships English and Brazilian Portuguese.
- Package with `.desktop` entry and icon, installed by `bin/install`, so the app appears in the Omarchy launcher.
- Takeover of an existing drive-sync (`T` in the window, `syncwatch takeover`): SyncWatch's packaged user units run the same rclone command with the same remote, folder, listings and filters (moved with their `.md5`), so no resync is needed. The old units and scripts move to `~/.local/share/syncwatch/taken-over/` with a manifest, nothing is deleted, a failed step undoes every step before it (when an old file cannot move back, every drive-sync timer is left off and the error says where it is), and `syncwatch takeover --undo` goes back. The window names what blocks a takeover (a run going, a failed last run, units it cannot keep).
- Sync now, pause and resume from the window (`S`, `P`, `U`), the bar (`S`, `P`) and the command line (`syncwatch sync`, `pause`, `resume`). A pause lasts one hour, until the next 8:00 or until resumed, survives a reboot and shows as "Paused" instead of a failure or "no sync for a while".
- Desktop notifications from SyncWatch itself: a critical card with the cause when a run fails, a reminder every hour while it keeps failing, a note when runs work again, an hourly check for a timer that is off, not enabled or not loading, and a daily reminder of a pause with no end.
- `syncwatch run` refuses to start rclone when the filters' `.md5` is missing or stale, because one bisync run in that state throws its base away until a resync.
- The status JSON is version 2: `managed`, `paused` and `pausedUntil` join the 17 keys of version 1.
- `T` during a run arms the takeover: the dialog opens by itself once the run ends, since runs take about half of each 15 minute tick. A takeover confirmed just as a run starts waits for that run and then happens, where it used to fail with "running".
- `?` in the bar panel lists its keys, in place of the key names beside the icons, which cut the last one off. The key that opens SyncWatch from the panel is now `W`: `l` moved the cursor, so `L` only worked with Shift.
- `syncwatch run` adds `--fast-list` to the bisync command unless the config turns ListR off. Against Google Drive the listing went from 476 s to 40 s for 33k files, with the same listing, so a run takes about a minute instead of eight; it needs no resync.

[Unreleased]: https://github.com/CaTeIM/syncwatch/commits/master
