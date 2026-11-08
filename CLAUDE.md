# CLAUDE.md - syncwatch

Single source of truth for agents and humans working on this repository.
`AGENTS.md` only points here.

## Overview

SyncWatch is an Omarchy desktop app (Qt 6, C++ and QML) that shows the state
of `drive-sync`, the rclone bisync of `~/Drive` with Google Drive that runs
from the `drive-sync.timer` user unit every 15 minutes, in a window, on the
Omarchy bar (its widget ships in the package) and through
`syncwatch status --json`. It reads the rclone log, asks systemd, and looks
for conflict copies. It never starts a resync and never touches the synced
files; it writes only its own state (`~/.local/state/syncwatch/`), the link
to its bar widget, and, when the user accepts, the bar layout through
`omarchy plugin`. The bar widget can ask systemd for one normal run. The plan for growing it into the
owner of drive-sync is `docs/superpowers/specs/2026-11-07-drive-sync-manager-design.md`; `docs/` is git-ignored,
so that file exists only in the maintainer's working copy. The `drive-sync` scripts themselves live
in the DIY repository (`assets/omarchy/home/.local/bin/drive-sync.sh` and
`drive-sync-aviso`), not here.

Built following the `omarchy-app` skill (`~/.claude/skills/omarchy-app/`):
one window, one job, keyboard first, follows the Omarchy theme and language.

## Stack

- C++17, Qt 6 (core, gui, qml, quick, quickcontrols2, dbus), qmake.
- QML with the Material style, dark theme, Omarchy accent.
- Qt Test for tests, run offscreen.
- Packaged with a local `PKGBUILD` and `makepkg`. No other dependencies.

## Structure

```text
syncwatch.pro           qmake project
bin/build               qmake6 + make into build/syncwatch
bin/test                builds and runs build/tests/syncwatch_tests offscreen
bin/install             bin/build, then makepkg -fsi with BUILDDIR/PKGDEST under build/
src/main.cpp            "status" runs headless; otherwise the window with translator, theme, backend, pluginSetup
src/synclog.{h,cpp}     pure parser of the rclone bisync log into SyncRun records
src/snapshot.{h,cpp}    buildSnapshot(): the one place the state is decided; tone; the JSON contract
src/backend.{h,cpp}     the window's state: log watch, systemctl calls, conflict scan, headline rules (summarize)
src/conflictcache.{h,cpp}  conflict count shared through ~/.local/state/syncwatch/conflicts.json
src/statuscommand.{h,cpp}  `syncwatch status [--json]`: same sources, read once, synchronously
src/pluginsetup.{h,cpp} links the shipped bar widget and enables it through `omarchy plugin`
src/theme.{h,cpp}       Omarchy accent and Hyprland rounding (from the skill template)
src/translator.{h,cpp}  Omarchy language, JSON catalogs (from the skill template)
src/Main.qml            window, shortcuts, layout
src/StatusPanel.qml     headline, details, progress, warning chips
src/RunsPage.qml        run list; RunDetail.qml shows the picked run
src/ConflictsPage.qml   conflict copies
src/HelpOverlay.qml     "?" overlay
src/BarBanner.qml       offer to put the widget on the bar (B), or why it could not
src/format.js           relative times, durations, sizes
src/i18n/*.json         en (plural forms only) and pt-BR
tests/                  Qt Test suite
plugin/                 Omarchy bar widget (id syncwatch), installed to /usr/share/syncwatch/omarchy-plugin; reads `syncwatch status --json`
docs/superpowers/       git-ignored, local only: design (specs/) and step plans (plans/)
pkgbuild/               PKGBUILD, .desktop
assets/icons/           launcher icon, installed by the PKGBUILD
assets/images/          README pictures: real screens that show no file names; window-run-details.png is the offscreen harness over a made-up log
```

## Commands

```sh
bin/build            # about 30 s from clean
bin/test             # 48 cases, about 2 s to run
bin/install          # needs sudo (pacman -U); run it in a real terminal
makepkg -f           # from pkgbuild/, with BUILDDIR and PKGDEST set as in bin/install, to check packaging without sudo
build/syncwatch status --json   # the state as the bar sees it (0.13 s with a fresh conflict cache)
(cd plugin && node --test tests/*.test.mjs)   # bar widget, 16 cases; the glob is required on Node 22
omarchy plugin validate plugin                # needs the folder path, not the plugin id
```

## How the state is decided

`buildSnapshot()` (`src/snapshot.cpp`, pure, tested) is the only place: the
window's `Backend`, `syncwatch status` and, through its JSON, the bar widget
all show what it decides. It finds the running run, the last success, the
last run, `failingSince`, `canSyncNow` and the `tone` (`ok`, `busy`,
`warning`, `error`), and calls `Backend::summarize`, which picks one
headline, most urgent first:
`unavailable` (systemctl failed), `notLoaded`, `running`, `resync`, `noLog`,
`failed`, `interrupted`, `skipped` (ExecCondition: emergency home), `timerOff`,
`stale` (no success for 45 min), `noDrive`, `conflicts`, `ok`. What the
headline leaves out goes to `warnings`: `timerOff`, `timerDisabled`,
`conflicts`.

- A run is "running" when systemd says the service is active, when the resync
  pause mark holds a live `drive-sync.sh` pid, or when the open run wrote to
  the log in the last 150 s (the log is watched, systemd is only polled).
- systemd's `Result` counts only when no log run started after the service's
  `ExecMainStartTimestamp`: `drive-sync.sh resync` runs rclone outside the unit.
- Failure causes match `drive-sync-aviso`: lost listings, out of sync, else the
  first error that is not the generic "Bisync aborted" line.
- The JSON (`snapshotToJson`, version 1) always has the same 17 keys; times
  are Unix seconds, `0` when unknown. The widget rejects any other version, so
  a change to the keys means a new version and a widget that reads it.

## Conventions

- C++ owns logic and anything that can fail; QML owns layout and keys. Every
  shortcut is a `Shortcut` with `Qt.ApplicationShortcut`.
- Every visible string goes through `qsTr()`/`tr()` with `%1` placeholders and
  `%n`/`%Ln` plurals; add each new one to `src/i18n/pt-BR.json` (and to
  `en.json` when it is plural). Bindings that format times read
  `translator.language`.
- Corners use `theme.radius`; small elements `Math.min(4, theme.radius)`.
- Comments explain why, in full sentences, in English.
- No em or en dashes anywhere (house rule of the owner).

## Gotchas

- **Never use local-time `QDateTime` in hot paths.** On this machine each
  local-time conversion or comparison costs about 0.2 ms (Qt resolves the
  zone every time). `QDateTime::fromString` per log line made opening the app
  take 14 s of CPU. `synclog.cpp` builds fixed-offset times by hand, and
  `Backend` compares against `currentDateTimeUtc()`. Parsing the 3 MB tail of
  the real log takes about 90 ms.
- **Do not watch `~/.local/state` itself**: other programs write there all
  the time. The folder is watched only while the log does not exist.
- **The repo lives under `~/Drive`**, mirrored by drive-sync. Its filters skip
  `/GitHub/**/build/**`, so every build output (qmake, tests, makepkg) goes
  under `build/`. Do not let makepkg write `pkgbuild/src` or `pkgbuild/pkg`.
- **The bar widget depends on two names.** It runs `syncwatch status --json`
  from the PATH, and its `L` key (`plugin/open-history.sh`) focuses the window
  whose Hyprland class is exactly `syncwatch`. Renaming the binary or changing
  `setDesktopFileName` breaks it, so change the widget in the same commit.
- `QString::arg` reads `%1000` as placeholder `%10`: build microsecond values
  as numbers, never by appending zeros after `%1`.
- clangd shows "file not found" for every Qt header because there is no
  `compile_commands.json`; the qmake build is the truth.
- Looking at the window without touching the desktop: an offscreen harness
  that loads `qrc:/Main.qml` with fake `BackendPaths` (log, Drive, state dir,
  a fake `systemctl` script) and a `PluginSetup` with fake `PluginPaths` (a
  fake `omarchy` script), and saves `window->grabWindow()`. Set
  `QT_FORCE_STDERR_LOGGING=1` to see QML warnings.
- The Material `Button` background (an elevation effect) does not render in
  the offscreen harness; `BarBanner.qml` draws its button's background itself.

### Bar plugin (`plugin/`)

- Omarchy loads third-party plugins only from `~/.config/omarchy/plugins/<id>/`
  (`PluginRegistry.qml` scans `"$dir"/*/`, which follows a symlink). A pacman
  package cannot install there, so `PluginSetup` links
  `~/.config/omarchy/plugins/syncwatch` to the package's copy and, after the
  user agrees (`B`), runs `omarchy plugin enable syncwatch` (`--after
  cateim.drive-sync`, then `disable cateim.drive-sync`, when the older widget
  is on the bar). A folder or a foreign link at that path is never touched.
  `N` says no and is remembered in `~/.local/state/syncwatch/bar-declined`:
  no offer and no new link afterwards, so a removal stays removed.
- A package upgrade reaches the bar only after a shell reload
  (`omarchy restart shell`): the plugin watcher's inotify does not follow the
  link into `/usr/share`.
- `omarchy plugin validate ~/.config/omarchy/plugins/syncwatch` rejects the
  link ("symlinks are not allowed inside a plugin folder"); validate
  `plugin/`, or the link with a trailing slash.
- Never link `plugin/` from the repo by hand before installing the matching
  binary: an older `syncwatch` has no `status` command and opens its window
  instead of answering, every poll, until the 45 s watchdog stops it.
- Any file written under `~/.config/omarchy/plugins/` hot-reloads the plugin;
  for a few seconds the icon shows the unknown state. Every reload also logs
  "ShellIpc ... another handler is registered"; IPC keeps answering.
- The IPC function cannot be named `sync`: it collides with the
  `DriveSyncService` id. It is `syncNow()`, and it checks `canSyncNow` first.
- `syncwatch status --json` output is parsed in
  `StdioCollector.onStreamFinished`, never in `onExited`: the two have no
  fixed order. A binary that fails to start ends the process without ending
  the stream or emitting `exited`, so `onRunningChanged` applies an empty
  answer when nothing streamed, and a 45 s watchdog stops a poll that never
  ends: both show unknown, never the last good snapshot. Only a source-level
  test covers this; the live check is opening the panel with `syncwatch`
  missing.
- Every QML `Text` sets `textFormat: Text.PlainText` (a test enforces it):
  `causeDetail` comes from the log, which is untrusted, and `cleanText` caps
  it at 200 characters on one line.
- `open-history.sh` finds the window by exact class `syncwatch`, because
  `omarchy-launch-or-focus` also matches titles.

## Deploy

Local only: `bin/install` installs the package with pacman, which puts
`/usr/bin/syncwatch`, the `.desktop` entry, the icon and the bar widget in
place, so it shows up in the Omarchy launcher. The DIY repository records it in
`docs/omarchy.md` (packages built from a local repository). Bump `pkgver` in
`pkgbuild/PKGBUILD` and `app.setApplicationVersion` in `src/main.cpp`
together, after closing the changelog.
