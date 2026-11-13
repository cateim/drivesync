# CLAUDE.md - drivesync

Single source of truth for agents and humans working on this repository.
`AGENTS.md` only points here.

## Overview

DriveSync is an Omarchy desktop app (Qt 6, C++ and QML) that shows the state
of `drive-sync`, the rclone bisync of `~/Drive` with Google Drive that runs
from the `drive-sync.timer` user unit every 15 minutes, in a window, on the
Omarchy bar (its widget ships in the package) and through
`drivesync status --json`. It reads the rclone log, asks systemd, and looks
for conflict copies. It writes its own state (`~/.local/state/drivesync/`),
the link to its bar widget, and, when the user accepts, the bar layout through
`omarchy plugin`. Since step 2 it can take over a drive-sync made by
`drive-sync.sh` (`drivesync takeover`): its packaged user units then run the
same rclone command from `~/.config/drivesync/config.json`, and the window,
the bar and the command line can sync now, pause and resume. Since step 3 it
fixes a broken base (`F`, `drivesync fix`): a resync with the folder in use,
run by `drive-sync-fix.service`, which only starts after the user confirms a
dry run and touches single synced files only as the fix's comparison or the
user's answers say. It never starts a resync or touches a synced file
otherwise. Since step 5b, after `--max-delete` stopped a run, it lists what
that run would delete (`D`, `drivesync deletes`) and lets one run through
with the guard raised once the user types how many files that is. The plan for growing it into the
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
drivesync.pro           qmake project
bin/build               qmake6 + make into build/drivesync
bin/test                builds and runs build/tests/drivesync_tests offscreen
bin/install             bin/build, then makepkg -fsi with BUILDDIR/PKGDEST under build/
bin/harness             builds tools/harness into build/harness and runs it with the given options
src/main.cpp            commands run headless (status, sync, pause, resume, takeover, fix, and run-condition, run, notify, watchdog, fix-run for the units); otherwise the window
src/synclog.{h,cpp}     pure parser of the rclone bisync log into SyncRun records
src/snapshot.{h,cpp}    buildSnapshot(): the one place the state is decided; tone; the JSON contract
src/backend.{h,cpp}     the window's state: log watch, systemctl calls, conflict scan, headline rules (summarize)
src/clockformat.{h,cpp}  12 or 24 hours, read from the Omarchy bar clock in shell.json
src/conflictcache.{h,cpp}  conflict count shared through ~/.local/state/drivesync/conflicts.json
src/statuscommand.{h,cpp}  `drivesync status [--json]`: same sources, read once, synchronously
src/syncconfig.{h,cpp}  the config a takeover writes; rclone arguments; filtersUsable (the md5 check)
src/pausestate.{h,cpp}  the pause mark (~/.local/state/drivesync/paused.json) and "next 08:00"
src/controlcommands.{h,cpp}  sync, pause, resume, run-condition, run (execs rclone); runProcess
src/fixstate.{h,cpp}    the fix's state machine in ~/.local/state/drivesync/fix.json (steps, questions)
src/fixcommand.{h,cpp}  `drivesync fix` and `fix-run` (drive-sync-fix.service): dry run, base run, questions, final run
src/listingdiff.{h,cpp}  reads bisync's .lst listings (Go %q names) and compares both sides, as drive-sync.sh did
src/blockeddeletes.{h,cpp}  a max-delete stop: its deletions from the log, the one-run mark (allow-deletes.json), the raised guard, `drivesync deletes`
src/legacymigration.{h,cpp}  moves SyncWatch's folders to the DriveSync names; first thing main() does
src/notifier.{h,cpp}    notify (ExecStopPost) and watchdog: the port of drive-sync-aviso
src/takeover.{h,cpp}    detect, apply with rollback, undo; `drivesync takeover`
src/takeoversetup.{h,cpp}  the window's side of a takeover, through a child process
src/pluginsetup.{h,cpp} links the shipped bar widget and enables it through `omarchy plugin`
src/theme.{h,cpp}       Omarchy accent and Hyprland rounding (from the skill template)
src/translator.{h,cpp}  Omarchy language, JSON catalogs (from the skill template)
src/Main.qml            window, shortcuts, layout
src/StatusPanel.qml     headline, details, progress, warning chips
src/RunsPage.qml        run list; RunDetail.qml shows the picked run
src/ConflictsPage.qml   conflict copies
src/HelpOverlay.qml     "?" overlay
src/BarBanner.qml       offer to put the widget on the bar (B), or why it could not
src/ControlsRow.qml     sync now (S), pause (P), resume (U), open ~/Drive (O)
src/PauseMenu.qml       the three pause lengths (1, 2, 3)
src/TakeoverBanner.qml  offer to take over (T), or what blocks it; TakeoverDialog.qml confirms
src/FixBanner.qml       offer to fix (F) after lost listings, out of sync or changed filters
src/FixDialog.qml       the fix's start, the dry run to confirm (Enter) and a stopped step (retry; X cancels)
src/FixQuestions.qml    the files that changed in the cloud during the base run: C cloud, L notebook, Enter sends
src/DeleteBanner.qml    offer to review the deletions (D) after a max-delete stop
src/DeleteDialog.qml    those deletions per side, and the typed count that lets one run through (Enter)
src/format.js           relative times, durations, sizes
src/i18n/*.json         en (plural forms only) and pt-BR
tests/                  Qt Test suite
tools/harness/          offscreen harness: the real Main.qml over a made-up drive-sync, saved as a PNG
plugin/                 Omarchy bar widget (id drivesync), installed to /usr/share/drivesync/omarchy-plugin; reads `drivesync status --json`
docs/superpowers/       git-ignored, local only: design (specs/) and step plans (plans/)
pkgbuild/               PKGBUILD, .desktop, units/ (drive-sync, drive-sync-watchdog and drive-sync-fix user units, /usr/lib/systemd/user)
assets/icons/           launcher icon, installed by the PKGBUILD
assets/images/          README pictures: window-*.png from bin/harness over a made-up log; bar-*.png are real screens with no file names
```

## Commands

```sh
bin/build            # about 30 s from clean
bin/test             # 193 cases, about 14 s (four run real rclone on temp folders)
bin/install          # needs sudo (pacman -U); run it in a real terminal
bin/harness --scenario running --clock "h:mm AP" --out build/shot.png   # the window offscreen, made-up data; options atop tools/harness/harness.cpp
makepkg -f           # from pkgbuild/, with BUILDDIR and PKGDEST set as in bin/install, to check packaging without sudo
build/drivesync status --json   # the state as the bar sees it (0.13 s with a fresh conflict cache)
(cd plugin && node --test tests/*.test.mjs)   # bar widget, 24 cases; the glob is required on Node 22
build/drivesync takeover --check              # read-only: what a takeover would move, or what blocks it
omarchy plugin validate plugin                # needs the folder path, not the plugin id
```

## How the state is decided

`buildSnapshot()` (`src/snapshot.cpp`, pure, tested) is the only place: the
window's `Backend`, `drivesync status` and, through its JSON, the bar widget
all show what it decides. It finds the running run, the last success, the
last run, `failingSince`, `canSyncNow` and the `tone` (`ok`, `busy`,
`warning`, `error`), and calls `Backend::summarize`, which picks one
headline, most urgent first:
`unavailable` (systemctl failed), `notLoaded`, `fixing`, `fixWaiting`, `fixFailed`,
`running`, `resync`, `paused`, `noLog`,
`failed`, `interrupted`, `skipped` (ExecCondition: emergency home), `timerOff`,
`stale` (no success for 45 min), `noDrive`, `noRuns` (a log with no run:
new or rotated), `conflicts`, `ok`. What the
headline leaves out goes to `warnings`: `timerOff`, `timerDisabled`,
`conflicts`.

- A run is "running" when systemd says the service is active, when the resync
  pause mark holds a live `drive-sync.sh` pid, or when the open run wrote to
  the log in the last 150 s (the log is watched, systemd is only polled).
- systemd's `Result` counts only when no log run started after the service's
  `ExecMainStartTimestamp`: `drive-sync.sh resync` runs rclone outside the unit.
- Failure causes match `drive-sync-aviso`: lost listings, out of sync, else the
  first error that is not the generic "Bisync aborted" line.
- The JSON (`snapshotToJson`, version 2) always has the same 20 keys (version
  1 plus `managed`, `paused`, `pausedUntil`); times are Unix seconds, `0` when
  unknown. The widget rejects any other version, so a change to the keys means
  a new version and a widget that reads it, installed together.
- A pause is a mark file, not a stopped timer: `run-condition` skips each tick
  while it holds and clears it at the first tick after its end, so "paused"
  lasts until a run really starts and survives a reboot.
- A fix is a mark file too: `fix.json` is written before anything else and
  `run-condition` skips every tick while it exists, so the timer, `S`, `U` and
  the bar's sync wait for the whole fix (spec section 5). Its steps:
  `preparing` (wait for a run, dry run into `fix-dry-run.log`), `confirm`
  (the user), `base` (the resync, into the main log), `questions` (the
  user), `finishing` (answers, then a normal run through `runCommandPlan`),
  or `failed`. `fix-run` loops until a step waits for the user, fails or ends,
  and sends a card at once when it does (the watchdog repeats it hourly). It
  checks the config's `runOnlyWhen` before every step, `fix --start` also
  refuses after a run that `--max-delete` stopped (`causeKind` `maxDelete`,
  spec 5b), and a final run that failed and took the listings with it sends
  the retry back to `base`.
- A fix shows as `fixing` (busy; the JSON's `resync` is true), `fixWaiting`
  (confirm or questions) or `fixFailed`. A working step with
  `drive-sync-fix.service` inactive is interrupted (`causeKind`
  `fixInterrupted`), but only 60 s after `stepStarted`: `fix` writes the step
  before systemd starts the unit. The `systemctl show` call describes three
  units (service, timer, fix unit), and every reader requires all three.
- Every writer of `fix.json` (the `fix` command, `fix-run`, the watchdog's
  reminder) takes `FixLock` (an flock on `fix.lock`) around its read and
  write, never across a long step. The fix and the takeover find the bisync
  listings by `bisyncSessionName(remote, local)`, the name rclone gives them
  (checked against rclone 1.75.2), never "the only pair in the work dir".
- Runs refused for the filters' md5 have `causeKind` `filtersChanged` (the
  refusal text ends in `kFiltersChangedReason`): a normal card says "press F"
  instead of the critical "stopped syncing" one.
- A run `--max-delete` stopped is `causeKind` `maxDelete` (spec 5b). Its
  deletions come from its own log lines (`blockedDeletes`, only when it is
  the newest run), so the headline, the card and `D` count both sides.
  `drivesync deletes --allow <count>` (the count of that list, typed by the
  user) writes `allow-deletes.json`; the next `drivesync run` (or the fix's
  final run, when that is what stopped) takes it, removes it, and dry-runs
  the plan with `--max-delete` raised to `ceil(100 * larger side / total)`
  into `allowed-dry-run.log`. The mark names the stopped run's start, and it
  is dropped unless that run is still the newest one; the real run gets the
  raised guard only when the dry run ends well and every deletion it finds,
  by side and path, is in that run's list. Otherwise the config's own guard
  stops it again with the new list. A mark older than 30 minutes is dropped.
  Never `--force`. `maxDeleteRefusal` (the fix) looks past records with no
  phase, which never reached bisync (a refusal), to the stop before them.
- Every reader of the log uses `SyncLog::readRecent`: the tail, extended back
  (at most 64 MB) until it holds a "Synching Path1" line, because a stop of
  tens of thousands of deletions writes more than the 3 MB tail, and its
  start would be cut off.

## Conventions

- C++ owns logic and anything that can fail; QML owns layout and keys. Every
  shortcut is a `Shortcut` with `Qt.ApplicationShortcut`.
- Every visible string goes through `qsTr()`/`tr()` with `%1` placeholders and
  `%n`/`%Ln` plurals; add each new one to `src/i18n/pt-BR.json` (and to
  `en.json` when it is plural). Bindings that format times read
  `translator.language`.
- Clock times show 12 or 24 hours like the clock on the Omarchy bar, never
  by the language: the `omarchy.clock` entry of `~/.config/omarchy/shell.json`
  (`format`, or `verticalFormat` on a side bar), with Omarchy's defaults file
  when the user's has no layout and the system locale when the bar has no
  clock. The window watches the file (`ClockFormat`, passed to `format.js` as
  `clockFormat.timeFormat`), the notifications read it once, and the widget
  reads it with a `FileView`. The rule exists twice, `src/clockformat.cpp` and
  `plugin/DriveSyncModel.js`: change both together.
- Corners use `theme.radius`; small elements `Math.min(4, theme.radius)`.
- Comments explain why, in full sentences, in English.
- No em or en dashes anywhere (house rule of the owner).

## Gotchas

- **The app was SyncWatch until 2026-11-11.** `main()` first calls
  `migrateFromSyncWatch()`, which renames `~/.config/syncwatch`,
  `~/.local/state/syncwatch` and `~/.local/share/syncwatch` to `drivesync`
  and rewrites the paths inside `config.json` and every
  `taken-over/*/manifest.json`. So running `build/drivesync` with any argument
  migrates the real home: never do it while the installed binary is still the
  old `syncwatch`, which would then lose its config. `PluginSetup` swaps the
  `syncwatch` widget for `drivesync` on the bar by itself (`enable drivesync
  --after syncwatch`, then `disable syncwatch`, then it removes the dangling
  link), and the PKGBUILD `conflicts`/`replaces` `syncwatch`. Keep the old
  names only in those places and in `takeoverLockHeld`.

- **A bisync run with the filters' `.md5` missing or stale destroys its base.**
  Tried with rclone 1.75 on 2026-11-08: the run stops with a critical error and
  renames the listings to `-err`, and from then on even the right filters fail
  until a resync. So `drivesync run` checks `filtersUsable` before it execs
  rclone, the takeover copies the filters with their `.md5`, and no byte of the
  filters (comments included) may change outside a flow that resyncs. The fix
  is that flow: edit `filters.txt`, then `F`; runs are refused until it ends.
- **A bisync lock written without `--max-lock` never expires.** Tried with
  rclone 1.75.2 on 2026-11-11: a base run killed with SIGKILL and no
  `--max-lock` leaves `Expires at 0001-01-01`, so every later run fails with
  "prior lock file found". The fix's dry run and base run pass
  `--max-lock 2m` (the final run keeps the config's own), and a lock
  line in what a run just wrote (never an older line of the log) is waited
  out once, 130 s.
- **A user unit beats a packaged one of the same name.** The package ships
  `drive-sync.service` and `.timer`; they stay inert while the old ones exist in
  `~/.config/systemd/user`, which is why the takeover moves those aside and then
  checks `FragmentPath`. Rollback and undo disable one set before enabling the
  other, so there is never a second scheduler.
- `unix` is a predefined macro with GCC on Linux: a variable or lambda named
  `unix` fails to compile.
- moc reads a `'` inside a raw string literal as a char literal: a test with
  `R"(q'uote)"` made moc generate nothing ("No relevant classes found") and
  the link fail on the vtable. Use a plain escaped string there.
- **The max-delete guard, as rclone 1.75.2 does it** (tried 2026-11-12):
  each side is checked against its own count in the last listing, Path1
  first, after both sides' "File was deleted" lines are logged; the
  percentage is compared as a real number (11 of 21 stops at 52 and passes at
  53; 11 of 20 passes at 55); an abort leaves the listings as they were (no
  `-err`); a `--dry-run` writes the deletion lines as NOTICE and leaves the
  `.lst` files alone (it writes `-dry` ones). A name with `"` or `\` is
  written Go-quoted in those lines.

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
- **The bar widget depends on two names.** It runs `drivesync status --json`
  from the PATH, and its `W` key (`plugin/open-history.sh`) focuses the window
  whose Hyprland class is exactly `drivesync`. Renaming the binary or changing
  `setDesktopFileName` breaks it, so change the widget in the same commit.
- `QString::arg` reads `%1000` as placeholder `%10`: build microsecond values
  as numbers, never by appending zeros after `%1`.
- clangd shows "file not found" for every Qt header because there is no
  `compile_commands.json`; the qmake build is the truth.
- Looking at the window without touching the desktop: `bin/harness` loads
  `qrc:/Main.qml` offscreen with fake `BackendPaths` (a made-up log, Drive,
  state dir, a fake `systemctl`), a fake `omarchy`, a fake `drivesync` for
  the takeover check and a fake shell.json clock, presses `--keys`, and
  saves `window->grabWindow()`; QML warnings go to stderr. It is how the
  README window pictures are made, so it must never read this machine:
  `--real` exists for local checks only. A new context property in
  `main.cpp` needs the same line in `tools/harness/harness.cpp`, and a new
  source file the window uses needs `tools/harness/harness.pro`.
- The Material `Button` background (an elevation effect) does not render in
  the offscreen harness; `BarBanner.qml` draws its button's background itself.

### Bar plugin (`plugin/`)

- Omarchy loads third-party plugins only from `~/.config/omarchy/plugins/<id>/`
  (`PluginRegistry.qml` scans `"$dir"/*/`, which follows a symlink). A pacman
  package cannot install there, so `PluginSetup` links
  `~/.config/omarchy/plugins/drivesync` to the package's copy and, after the
  user agrees (`B`), runs `omarchy plugin enable drivesync`. A folder or a
  foreign link at that path is never touched.
  `N` says no and is remembered in `~/.local/state/drivesync/bar-declined`:
  no offer and no new link afterwards, so a removal stays removed. `N` also
  hides the "blocked" notice (someone else's folder or link at that path)
  the same way. A failed `omarchy plugin list` before the user pressed `B`
  is state `unchecked`, with no banner; after `B` it is `failed`.
- A package upgrade reaches the bar only after a shell reload
  (`omarchy restart shell`): the plugin watcher's inotify does not follow the
  link into `/usr/share`.
- `omarchy plugin validate ~/.config/omarchy/plugins/drivesync` rejects the
  link ("symlinks are not allowed inside a plugin folder"); validate
  `plugin/`, or the link with a trailing slash.
- Never link `plugin/` from the repo by hand before installing the matching
  binary: an older `drivesync` has no `status` command and opens its window
  instead of answering, every poll, until the 45 s watchdog stops it.
- Any file written under `~/.config/omarchy/plugins/` hot-reloads the plugin;
  for a few seconds the icon shows the unknown state. Every reload also logs
  "ShellIpc ... another handler is registered"; IPC keeps answering.
- The IPC function cannot be named `sync`: it collides with the
  `DriveSyncService` id. It is `syncNow()`, and it checks `canSyncNow` first.
- `drivesync status --json` output is parsed in
  `StdioCollector.onStreamFinished`, never in `onExited`: the two have no
  fixed order. A binary that fails to start ends the process without ending
  the stream or emitting `exited`, so `onRunningChanged` applies an empty
  answer when nothing streamed, and a 45 s watchdog stops a poll that never
  ends: both show unknown, never the last good snapshot. Only a source-level
  test covers this; the live check is opening the panel with `drivesync`
  missing.
- Every QML `Text` sets `textFormat: Text.PlainText` (a test enforces it):
  `causeDetail` comes from the log, which is untrusted, and `cleanText` caps
  it at 200 characters on one line.
- `open-history.sh` finds the window by exact class `drivesync`, because
  `omarchy-launch-or-focus` also matches titles.

## Deploy

Local only: `bin/install` installs the package with pacman, which puts
`/usr/bin/drivesync`, the `.desktop` entry, the icon and the bar widget in
place, so it shows up in the Omarchy launcher. The DIY repository records it in
`docs/omarchy.md` (packages built from a local repository). Bump `pkgver` in
`pkgbuild/PKGBUILD` and `app.setApplicationVersion` in `src/main.cpp`
together, after closing the changelog.
