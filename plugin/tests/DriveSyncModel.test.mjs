// Run from the plugin directory: node --test tests/*.test.mjs
import { test } from "node:test"
import assert from "node:assert/strict"
import { readFileSync, readdirSync } from "node:fs"
import { execFileSync } from "node:child_process"
import vm from "node:vm"

// The model is a QML `.pragma library` script; drop the pragma and load it
// into a sandbox so node sees the same functions QML does.
const source = readFileSync(new URL("../DriveSyncModel.js", import.meta.url), "utf8")
const model = {}
vm.runInNewContext(source.replace(/^\.pragma library\s*$/m, ""), model)

const STATUSES = ["ok", "running", "resync", "paused", "failed", "interrupted", "skipped", "timerOff",
                  "stale", "noDrive", "noRuns", "noLog", "notLoaded", "unavailable", "conflicts",
                  "fixing", "fixWaiting", "fixFailed"]

// What `drive-sync status --json` prints with drive-sync in sync (contract version 2).
const HEALTHY = {
  version: 2, now: 1794076940, status: "ok", tone: "ok",
  causeKind: "", causeDetail: "", warnings: [],
  running: false, resync: false, canSyncNow: true,
  lastSuccess: 1794076584, lastRun: { start: 1794076098, end: 1794076584, outcome: "success" },
  currentRun: { start: 0, phase: "" }, nextRun: 1794076998,
  failingSince: 0, typicalSeconds: 480, conflicts: 0,
  managed: true, paused: false, pausedUntil: 0
}
const json = (overrides = {}) => JSON.stringify({ ...HEALTHY, ...overrides })
const snapshot = (overrides = {}) => model.parseSnapshot(json(overrides))

test("parseSnapshot reads a healthy snapshot", () => {
  const s = model.parseSnapshot(json())
  assert.equal(s.ok, true)
  assert.equal(s.status, "ok")
  assert.equal(s.tone, "ok")
  assert.equal(s.lastSuccess, 1794076584)
  assert.equal(s.lastRunStart, 1794076098)
  assert.equal(s.lastRunEnd, 1794076584)
  assert.equal(s.lastRunOutcome, "success")
  assert.equal(s.nextRun, 1794076998)
  assert.equal(s.canSyncNow, true)
  assert.equal(s.conflicts, 0)
  assert.equal(s.causeDetail, "")
})

test("parseSnapshot rejects non-JSON, other versions and a missing clock", () => {
  assert.equal(model.parseSnapshot("now=1794076940").ok, false)
  // Version 1 has no pause: an older drive-sync must not look like one that is never paused.
  assert.equal(model.parseSnapshot(json({ version: 1 })).ok, false)
  assert.equal(model.parseSnapshot(json({ version: 3 })).ok, false)
  assert.equal(model.parseSnapshot(json({ now: 0 })).ok, false)
  assert.equal(model.parseSnapshot("[1, 2]").ok, false)
  assert.equal(model.parseSnapshot(null).ok, false)
})

test("parseSnapshot never trusts field shapes", () => {
  const s = snapshot({
    lastSuccess: "1794076584", nextRun: -5, failingSince: 12.5,
    status: { evil: true }, tone: "rainbow", warnings: "timerOff",
    running: "true", canSyncNow: 1, conflicts: "7", lastRun: "yesterday"
  })
  assert.equal(s.lastSuccess, 0)
  assert.equal(s.nextRun, 0)
  assert.equal(s.failingSince, 0)
  assert.equal(s.status, "unknown")
  assert.equal(s.tone, "unknown")
  assert.deepEqual([...s.warnings], [])
  assert.equal(s.running, false)
  assert.equal(s.canSyncNow, false)
  assert.equal(s.conflicts, 0)
  assert.equal(s.lastRunStart, 0)
  assert.equal(snapshot({ warnings: ["timerDisabled", "<b>x</b>", 3] }).warnings.length, 1)
})

test("causeDetail keeps markup inert on one bounded line", () => {
  const markup = '<img src="https://example.invalid/tracker"> <b>Path1</b>'
  // The QML sinks are PlainText; the model must not drop or alter the characters.
  assert.equal(snapshot({ causeDetail: markup }).causeDetail, markup)
  assert.equal(snapshot({ causeDetail: "x".repeat(500) }).causeDetail.length, 200)
  assert.equal(model.cleanText("a\u0000b\u001b[31mc\r\nd"), "a b [31mc d")
})

test("barState maps every status", () => {
  const expected = {
    ok: "ok", running: "syncing", resync: "resync", paused: "paused", failed: "failing",
    interrupted: "warning", skipped: "warning", stale: "warning", noRuns: "warning", conflicts: "warning",
    timerOff: "timerOff", noDrive: "broken", noLog: "broken", notLoaded: "broken", unavailable: "broken",
    fixing: "resync", fixWaiting: "warning", fixFailed: "broken"
  }
  const tones = {
    ok: "ok", running: "busy", resync: "busy", paused: "warning", failed: "error", interrupted: "warning",
    skipped: "warning", stale: "warning", noRuns: "warning", conflicts: "warning", timerOff: "error",
    noDrive: "error", noLog: "error", notLoaded: "error", unavailable: "error",
    fixing: "busy", fixWaiting: "warning", fixFailed: "error"
  }
  for (const status of STATUSES) {
    assert.equal(model.barState(snapshot({ status, tone: tones[status] })), expected[status], status)
  }
  assert.equal(model.barState(snapshot({ status: "", tone: "" })), "unknown")
})

test("a missing or old drive-sync shows unknown", () => {
  assert.equal(model.barState(model.parseSnapshot("")), "unknown")
  assert.equal(model.barState(model.parseSnapshot("usage: drive-sync [open]")), "unknown")
})

test("canSyncNow follows the app", () => {
  assert.equal(model.canSyncNow(snapshot()), true)
  assert.equal(model.canSyncNow(snapshot({ canSyncNow: false })), false)
  assert.equal(model.canSyncNow(model.parseSnapshot("")), false)
})

// Quickshell ends a process that failed to start without ending its output
// stream, and an old drive-sync would open a window instead of answering. Both
// must end as "unknown", never as the last good snapshot.
test("a poll that ends without output or hangs shows unknown", () => {
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.match(qml, /onRunningChanged:[^}]*applyStatus\(""\)/)
  const watchdog = qml.match(/id:\s*watchdog[\s\S]*?interval:\s*(\d+)/)
  assert.ok(watchdog, "no watchdog timer on the status poll")
  assert.ok(Number(watchdog[1]) > 35000, "the watchdog must outlast a cold find plus systemctl")
})

test("lastRunSeconds only for a finished run", () => {
  assert.equal(model.lastRunSeconds(snapshot()), 486)
  assert.equal(model.lastRunSeconds(snapshot({ lastRun: { start: 1794076098, end: 0, outcome: "interrupted" } })), 0)
})

// The late rule lives in the app now; the widget must not keep a copy.
test("the widget leaves the late rule to the app", () => {
  const manifest = JSON.parse(readFileSync(new URL("../manifest.json", import.meta.url), "utf8"))
  assert.equal(manifest.id, "drive-sync")
  assert.equal("staleAfterMin" in manifest.barWidget.defaults, false)
  assert.equal(manifest.barWidget.schema.some((s) => s.key === "staleAfterMin"), false)
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  assert.doesNotMatch(qml, /staleAfter/)
})

test("helper scripts parse in bash", () => {
  execFileSync("bash", ["-n", new URL("../open-history.sh", import.meta.url).pathname])
})

test("glyphs map to the Nerd Font cloud icons", () => {
  assert.equal(model.glyph("ok").codePointAt(0), 0xF0160)
  assert.equal(model.glyph("syncing").codePointAt(0), 0xF063F)
  assert.equal(model.glyph("warning").codePointAt(0), 0xF09E0)
  assert.equal(model.glyph("failing").codePointAt(0), 0xF09E0)
  assert.equal(model.glyph("nonsense").codePointAt(0), 0xF0164)
  assert.equal(model.isUrgent("failing"), true)
  assert.equal(model.isUrgent("warning"), false)
  assert.equal(model.isDimmed("syncing"), true)
})

test("ageParts picks the coarsest readable unit", () => {
  const HOUR = 3600
  assert.deepEqual({ ...model.ageParts(30) }, { unit: "now", value: 0 })
  assert.deepEqual({ ...model.ageParts(125) }, { unit: "minutes", value: 2 })
  assert.deepEqual({ ...model.ageParts(3 * HOUR + 59) }, { unit: "hours", value: 3 })
  assert.deepEqual({ ...model.ageParts(50 * HOUR) }, { unit: "days", value: 2 })
  assert.deepEqual({ ...model.ageParts(-10) }, { unit: "now", value: 0 })
})

test("the action cursor and clampInt", () => {
  assert.deepEqual([...model.ACTIONS], ["sync", "pause", "log", "folder"])
  assert.equal(model.stepAction(0, -1), 3)
  assert.equal(model.stepAction(3, 1), 0)
  assert.equal(model.clampInt("abc", 30, 10, 600), 30)
  assert.equal(model.clampInt("5", 30, 10, 600), 10)
})

// Every Text in the plugin shows log or plugin text, so none may fall back to
// AutoText, where markup-shaped log content would render as rich text.
test("every QML Text sets textFormat: Text.PlainText", () => {
  const dir = new URL("../", import.meta.url)
  for (const name of readdirSync(dir).filter((f) => f.endsWith(".qml"))) {
    const qml = readFileSync(new URL(name, dir), "utf8")
    const blocks = qml.split(/\bText\s*\{/).slice(1)
    for (const block of blocks) {
      const body = block.slice(0, block.indexOf("}"))
      assert.match(body, /textFormat:\s*Text\.PlainText/, `${name}: Text without PlainText`)
    }
  }
})

test("every key used in QML exists in en.json and pt-BR.json", () => {
  const dir = new URL("../", import.meta.url)
  const en = JSON.parse(readFileSync(new URL("i18n/en.json", dir), "utf8"))
  const pt = JSON.parse(readFileSync(new URL("i18n/pt-BR.json", dir), "utf8"))
  for (const name of readdirSync(dir).filter((f) => f.endsWith(".qml"))) {
    const qml = readFileSync(new URL(name, dir), "utf8")
    for (const [, key] of qml.matchAll(/strings\.tn?\("([^"]+)"/g)) {
      // "status." + status and "age." + unit are built at runtime; checked below.
      if (key.endsWith(".")) continue
      assert.ok(key in en, `${name}: "${key}" missing in en.json`)
      assert.ok(key in pt, `${name}: "${key}" missing in pt-BR.json`)
    }
  }
  for (const status of [...STATUSES, "unknown"]) {
    for (const catalog of [en, pt]) assert.ok(`status.${status}` in catalog, `status.${status} missing`)
  }
  for (const unit of ["minutes", "hours", "days"]) {
    for (const catalog of [en, pt]) {
      const entry = catalog[`age.${unit}`]
      assert.ok(entry && entry.one && entry.other, `age.${unit} needs one and other`)
    }
  }
})

test("pause and resume follow the app", () => {
  const paused = snapshot({ status: "paused", tone: "warning", paused: true, pausedUntil: 1794080540, canSyncNow: false })
  assert.equal(paused.paused, true)
  assert.equal(paused.pausedUntil, 1794080540)
  assert.equal(model.barState(paused), "paused")
  assert.equal(model.glyph("paused").codePointAt(0), 0xF18F6)
  assert.equal(model.isUrgent("paused"), false)
  assert.equal(model.canResume(paused), true)
  assert.equal(model.canPause(paused), false)
  assert.equal(model.canSyncNow(paused), false)

  assert.equal(model.canPause(snapshot()), true)
  assert.equal(model.canResume(snapshot()), false)
  // The old units never look at the mark: no pause before a takeover.
  assert.equal(model.canPause(snapshot({ managed: false })), false)
  assert.equal(model.canPause(snapshot({ resync: true })), false)
  assert.equal(model.canPause(model.parseSnapshot("")), false)
  assert.equal(snapshot({ paused: "yes", pausedUntil: "soon" }).paused, false)
  assert.equal(snapshot({ pausedUntil: "soon" }).pausedUntil, 0)
})

test("the widget changes drive-sync only through drive-sync", () => {
  const qml = readFileSync(new URL("../DriveSyncService.qml", import.meta.url), "utf8")
  assert.match(qml, /\["drive-sync", "sync"\]/)
  assert.match(qml, /\["drive-sync", "pause", "--minutes", "60"\]/)
  assert.match(qml, /\["drive-sync", "resume"\]/)
  assert.doesNotMatch(qml, /"systemctl"/)
})

test("hoursInFormat reads the hour tokens outside quoted text", () => {
  assert.equal(model.hoursInFormat("ddd d MMM HH:mm"), "24")
  assert.equal(model.hoursInFormat("dddd h:mm:ss ap"), "12")
  assert.equal(model.hoursInFormat("h:mm AP"), "12")
  assert.equal(model.hoursInFormat("h:mm"), "24")  // Qt's h is 0 to 23 without AM/PM
  assert.equal(model.hoursInFormat("'at' HH:mm"), "24")
  assert.equal(model.hoursInFormat("'It''s' h:mm a"), "12")
  assert.equal(model.hoursInFormat("d MMMM 'W'ww yyyy"), "")
  assert.equal(model.hoursInFormat(undefined), "")
})

test("barClockHours follows the clock on the bar", () => {
  const user = JSON.stringify({ bar: { position: "top", layout: { center: [
    { id: "omarchy.clock", format: "ddd d MMM h:mm AP", verticalFormat: "HH\nmm" }] } } })
  assert.equal(model.barClockHours([user], false), "12")
  assert.equal(model.barClockHours([user], true), "24")  // a side bar shows the vertical format
  assert.equal(model.barClockHours([JSON.stringify({ bar: { layout: { right: [{ id: "omarchy.clock" }] } } })], false), "24")
  assert.equal(model.barClockHours([JSON.stringify({ bar: { layout: { left: [{ id: "omarchy.workspaces" }] } } })], false), "")
  assert.equal(model.barClockHours(["not json", ""], false), "")
  // A user file without a layout leaves it to Omarchy's defaults.
  const defaults = JSON.stringify({ bar: { layout: { center: [{ id: "omarchy.clock", format: "h:mm AP" }] } } })
  assert.equal(model.barClockHours([JSON.stringify({ idle: {} }), defaults], false), "12")
  assert.equal(model.barClockHours([user, defaults.replace("h:mm AP", "HH:mm")], false), "12")
})

test("clockTimeFormat falls back when the bar says nothing", () => {
  assert.equal(model.clockTimeFormat("24", "h:mm AP"), "HH:mm")
  assert.equal(model.clockTimeFormat("12", "HH:mm"), "h:mm AP")
  assert.equal(model.clockTimeFormat("", "HH:mm"), "HH:mm")
})

test("warnings get the window's orange on the bar and in the panel", () => {
  assert.equal(model.isWarning("warning"), true)
  for (const state of ["ok", "syncing", "paused", "failing", "broken", "timerOff", "unknown"])
    assert.equal(model.isWarning(state), false, state)
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  const window = readFileSync(new URL("../../src/StatusPanel.qml", import.meta.url), "utf8")
  const orange = qml.match(/readonly property color warning: "(#[0-9a-f]{6})"/)
  assert.ok(orange, "the widget names its warning color")
  assert.match(window, new RegExp(`"warning" \\? "${orange[1]}"`))
  assert.match(qml, /activeColor: root\.stateColor/)
  assert.match(qml, /color: root\.stateColor/)
})

test("the failing line has a sentence for every cause the app names", () => {
  const qml = readFileSync(new URL("../DriveSync.qml", import.meta.url), "utf8")
  for (const kind of ["lostListings", "outOfSync", "timeout", "systemd", "filtersChanged", "fixInterrupted", "maxDelete"])
    assert.match(qml, new RegExp(`case "${kind}": return strings\\.t\\("cause\\.${kind}"\\)`), kind)
  // A fix that stopped says why too, not only a failed run.
  assert.match(qml, /root\.syncState === "failing" \|\| root\.status\.status === "fixFailed"/)
  // An "error" with no line, or a kind this widget does not know, still says something.
  assert.match(qml, /status\.causeDetail !== "" \? status\.causeDetail : strings\.t\("cause\.unknown"\)/)
})

test("every status the app prints has a headline in both catalogs", () => {
  // The headline key is built at run time ("status." + status), so the
  // literal-key test above cannot see a missing one.
  for (const lang of ["en", "pt-BR"]) {
    const catalog = JSON.parse(readFileSync(new URL(`../i18n/${lang}.json`, import.meta.url), "utf8"))
    for (const status of STATUSES) assert.ok(`status.${status}` in catalog, `${lang}: status.${status}`)
  }
})
