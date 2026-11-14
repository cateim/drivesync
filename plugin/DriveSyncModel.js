.pragma library

// Pure logic of the DriveSync bar widget. The state itself is decided by the
// app (`drive-sync status --json`); this file only checks what the app printed
// and turns it into what the bar shows. No QML here, so node:test covers it.

var MAX_ERROR_LENGTH = 200
var EPOCH_MAX = 99999999999

// Every status the app can print (Backend::summarize), and its tones.
var STATUSES = ["ok", "running", "resync", "paused", "failed", "interrupted", "skipped", "timerOff",
                "stale", "noDrive", "noRuns", "noLog", "notLoaded", "unavailable", "conflicts",
                "fixing", "fixWaiting", "fixFailed"]
var TONES = ["ok", "busy", "warning", "error"]
var OUTCOMES = ["success", "failed", "interrupted"]

// One glyph per bar state, Material Design Icons from the Nerd Font.
var GLYPHS = {
  ok: 0xF0160,        // cloud-check
  syncing: 0xF063F,   // cloud-sync
  resync: 0xF063F,    // cloud-sync
  paused: 0xF18F6,    // weather-cloudy-clock: no cloud-pause in the font
  warning: 0xF09E0,   // cloud-alert
  failing: 0xF09E0,   // cloud-alert
  broken: 0xF09E0,    // cloud-alert
  timerOff: 0xF0164,  // cloud-off-outline
  unknown: 0xF0164    // cloud-off-outline
}

var URGENT_STATES = ["failing", "broken", "timerOff"]
var DIMMED_STATES = ["syncing", "resync", "unknown"]

// Actions in the panel, in cursor order.
var ACTIONS = ["sync", "pause", "log", "folder"]

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value)
}

// The contract prints times as integers; anything else is "unknown" (0).
function toEpoch(value) {
  return typeof value === "number" && isFinite(value) && Math.floor(value) === value &&
         value > 0 && value <= EPOCH_MAX ? value : 0
}

// Identifiers such as "lostListings" or "listing"; anything else is empty.
function toKey(value) {
  return typeof value === "string" && /^[A-Za-z][A-Za-z-]{0,31}$/.test(value) ? value : ""
}

function oneOf(value, list, fallback) {
  return typeof value === "string" && list.indexOf(value) !== -1 ? value : fallback
}

// Log text for display: no control characters, one line, bounded.
function cleanText(value) {
  var text = String(value === undefined || value === null ? "" : value)
  text = text.replace(/[\u0000-\u001f\u007f]+/g, " ").replace(/\s+/g, " ").trim()
  if (text.length > MAX_ERROR_LENGTH) text = text.substring(0, MAX_ERROR_LENGTH - 1) + "…"
  return text
}

// What `drive-sync status --json` printed, checked field by field. Output that
// is not version 2 of the contract, or has no clock, is not a snapshot.
function parseSnapshot(raw) {
  var data = null
  try {
    data = JSON.parse(String(raw === undefined || raw === null ? "" : raw))
  } catch (e) {
    data = null
  }
  var ok = isObject(data) && data.version === 2 && toEpoch(data.now) > 0
  var d = ok ? data : {}
  var lastRun = isObject(d.lastRun) ? d.lastRun : {}
  var currentRun = isObject(d.currentRun) ? d.currentRun : {}
  var warnings = []
  if (Array.isArray(d.warnings)) {
    for (var i = 0; i < d.warnings.length && warnings.length < 8; i++) {
      var warning = toKey(d.warnings[i])
      if (warning !== "") warnings.push(warning)
    }
  }
  return {
    ok: ok,
    now: toEpoch(d.now),
    status: oneOf(d.status, STATUSES, "unknown"),
    tone: oneOf(d.tone, TONES, "unknown"),
    causeKind: toKey(d.causeKind),
    causeDetail: cleanText(d.causeDetail),
    warnings: warnings,
    running: d.running === true,
    resync: d.resync === true,
    canSyncNow: d.canSyncNow === true,
    lastSuccess: toEpoch(d.lastSuccess),
    lastRunStart: toEpoch(lastRun.start),
    lastRunEnd: toEpoch(lastRun.end),
    lastRunOutcome: oneOf(lastRun.outcome, OUTCOMES, ""),
    currentRunStart: toEpoch(currentRun.start),
    phase: toKey(currentRun.phase),
    nextRun: toEpoch(d.nextRun),
    failingSince: toEpoch(d.failingSince),
    conflicts: typeof d.conflicts === "number" && isFinite(d.conflicts) && d.conflicts > 0
               ? Math.min(Math.floor(d.conflicts), 999999) : 0,
    managed: d.managed === true,
    paused: d.paused === true,
    pausedUntil: toEpoch(d.pausedUntil)
  }
}

// The icon state on the bar, from the app's status and tone.
function barState(snapshot) {
  if (!snapshot || !snapshot.ok) return "unknown"
  if (snapshot.status === "timerOff") return "timerOff"
  if (snapshot.status === "paused") return "paused"
  switch (snapshot.tone) {
    case "ok": return "ok"
    // A fix rebuilds the base, as a resync does.
    case "busy": return snapshot.status === "resync" || snapshot.status === "fixing" ? "resync" : "syncing"
    case "warning": return "warning"
    case "error": return snapshot.status === "failed" ? "failing" : "broken"
  }
  return "unknown"
}

// The app knows whether a manual run makes sense; the widget just asks.
function canSyncNow(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.canSyncNow
}

// A pause needs DriveSync's own units: the old ones never look at the mark.
function canPause(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.managed && !snapshot.paused && !snapshot.resync
}

function canResume(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.paused
}

// Seconds the last finished run took, 0 when it did not finish or is unknown.
function lastRunSeconds(snapshot) {
  if (!snapshot || snapshot.lastRunStart === 0 || snapshot.lastRunEnd < snapshot.lastRunStart) return 0
  return snapshot.lastRunEnd - snapshot.lastRunStart
}

function glyph(state) {
  var code = GLYPHS[state] || GLYPHS.unknown
  return String.fromCodePoint(code)
}

function isUrgent(state) {
  return URGENT_STATES.indexOf(state) !== -1
}

// Conflicts, a late or skipped run: worth a look, not an alarm.
function isWarning(state) {
  return state === "warning"
}

function isDimmed(state) {
  return DIMMED_STATES.indexOf(state) !== -1
}

// Age in the coarsest unit that still reads well: { unit, value }.
function ageParts(seconds) {
  var s = Math.max(0, Math.floor(Number(seconds) || 0))
  if (s < 60) return { unit: "now", value: 0 }
  if (s < 3600) return { unit: "minutes", value: Math.floor(s / 60) }
  if (s < 86400) return { unit: "hours", value: Math.floor(s / 3600) }
  return { unit: "days", value: Math.floor(s / 86400) }
}

function clampInt(value, fallback, min, max) {
  var n = parseInt(String(value), 10)
  if (!isFinite(n)) n = fallback
  return Math.max(min, Math.min(max, n))
}

function stepAction(index, delta) {
  var n = ACTIONS.length
  return ((index + delta) % n + n) % n
}

// Clock times show 12 or 24 hours like the clock on the Omarchy bar, the
// same rule as the app's (src/clockformat.cpp).

// "12" with an AM/PM marker and an "h", "24" with any other hour, "" with no
// hour at all. Quoted text is literal, so "'at' HH:mm" is "24".
function hoursInFormat(format) {
  var text = String(format === undefined || format === null ? "" : format)
  var quoted = false
  var twelveHour = false
  var anyHour = false
  var amPm = false
  for (var i = 0; i < text.length; i++) {
    var c = text.charAt(i)
    if (c === "'") {
      if (text.charAt(i + 1) === "'") i++
      else quoted = !quoted
      continue
    }
    if (quoted) continue
    if (c === "h") {
      twelveHour = true
      anyHour = true
    } else if (c === "H") {
      anyHour = true
    } else if (c === "a" || c === "A") {
      amPm = true
    }
  }
  if (!anyHour) return ""
  return amPm && twelveHour ? "12" : "24"
}

// The first entry with the clock's id anywhere in a bar layout.
function findClock(value) {
  if (!value || typeof value !== "object") return null
  if (!Array.isArray(value) && value.id === "omarchy.clock") return value
  for (var key in value) {
    var found = findClock(value[key])
    if (found) return found
  }
  return null
}

// The hours of the bar clock, from the text of shell.json files in order of
// precedence (the user's, then Omarchy's defaults): the layout comes from the
// first file that has one. `vertical` is the bar's own and picks the vertical
// format. "" when no layout has a clock.
function barClockHours(texts, vertical) {
  var layout = null
  for (var i = 0; i < texts.length; i++) {
    var bar = null
    try {
      var parsed = JSON.parse(String(texts[i]))
      bar = parsed && typeof parsed === "object" ? parsed.bar : null
    } catch (e) {
      continue
    }
    if (bar && typeof bar === "object" && !Array.isArray(bar) && "layout" in bar) {
      layout = bar.layout
      break
    }
  }
  var clock = findClock(layout)
  if (!clock) return ""
  // Omarchy's own defaults, for an entry that names no format, are both 24 hours.
  var format = vertical ? clock.verticalFormat : clock.format
  if (typeof format !== "string") format = vertical ? "HH mm" : "dddd HH:mm"
  return hoursInFormat(format)
}

// The Qt format for a clock time, or the fallback when the bar says nothing.
function clockTimeFormat(hours, fallback) {
  if (hours === "24") return "HH:mm"
  if (hours === "12") return "h:mm AP"
  return fallback
}
