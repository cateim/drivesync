.pragma library

// Pure logic of the SyncWatch bar widget. The state itself is decided by the
// app (`syncwatch status --json`); this file only checks what the app printed
// and turns it into what the bar shows. No QML here, so node:test covers it.

var MAX_ERROR_LENGTH = 200
var EPOCH_MAX = 99999999999

// Every status the app can print (Backend::summarize), and its tones.
var STATUSES = ["ok", "running", "resync", "failed", "interrupted", "skipped", "timerOff",
                "stale", "noDrive", "noLog", "notLoaded", "unavailable", "conflicts"]
var TONES = ["ok", "busy", "warning", "error"]
var OUTCOMES = ["success", "failed", "interrupted"]

// One glyph per bar state, Material Design Icons from the Nerd Font.
var GLYPHS = {
  ok: 0xF0160,        // cloud-check
  syncing: 0xF063F,   // cloud-sync
  resync: 0xF063F,    // cloud-sync
  warning: 0xF09E0,   // cloud-alert
  failing: 0xF09E0,   // cloud-alert
  broken: 0xF09E0,    // cloud-alert
  timerOff: 0xF0164,  // cloud-off-outline
  unknown: 0xF0164    // cloud-off-outline
}

var URGENT_STATES = ["failing", "broken", "timerOff"]
var DIMMED_STATES = ["syncing", "resync", "unknown"]

// Actions in the panel, in cursor order.
var ACTIONS = ["sync", "log", "folder"]

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

// What `syncwatch status --json` printed, checked field by field. Output that
// is not version 1 of the contract, or has no clock, is not a snapshot.
function parseSnapshot(raw) {
  var data = null
  try {
    data = JSON.parse(String(raw === undefined || raw === null ? "" : raw))
  } catch (e) {
    data = null
  }
  var ok = isObject(data) && data.version === 1 && toEpoch(data.now) > 0
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
               ? Math.min(Math.floor(d.conflicts), 999999) : 0
  }
}

// The icon state on the bar, from the app's status and tone.
function barState(snapshot) {
  if (!snapshot || !snapshot.ok) return "unknown"
  if (snapshot.status === "timerOff") return "timerOff"
  switch (snapshot.tone) {
    case "ok": return "ok"
    case "busy": return snapshot.status === "resync" ? "resync" : "syncing"
    case "warning": return "warning"
    case "error": return snapshot.status === "failed" ? "failing" : "broken"
  }
  return "unknown"
}

// The app knows whether a manual run makes sense; the widget just asks.
function canSyncNow(snapshot) {
  return !!snapshot && snapshot.ok && snapshot.canSyncNow
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
