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

[Unreleased]: https://github.com/CaTeIM/syncwatch/commits/master
