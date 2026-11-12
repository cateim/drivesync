.pragma library

// Times, durations and sizes in the Omarchy language. Callers pass `now`
// so relative times move with the window's clock, and read
// translator.language in the binding so a language switch re-renders them.
// Clock times take clockFormat.timeFormat, the 12 or 24 hours of the bar
// clock, read in the binding too so a change on the bar re-renders them.

function valid(date) {
    return !!date && !isNaN(date.getTime())
}

function clock(date, timeFormat) {
    return date.toLocaleTimeString(Qt.locale(), timeFormat || Qt.locale().timeFormat(1))
}

// The clock time alone today, the day and month before it otherwise.
function when(date, now, timeFormat) {
    const today = new Date(now)
    if (date.toDateString() === today.toDateString())
        return clock(date, timeFormat)
    return qsTr("%1, %2").arg(date.toLocaleDateString(Qt.locale(), "d MMM")).arg(clock(date, timeFormat))
}

function ago(date, now) {
    const seconds = Math.max(0, Math.round((now - date.getTime()) / 1000))
    if (seconds < 60)
        return qsTr("just now")
    if (seconds < 3600)
        return qsTr("%n min ago", "", Math.floor(seconds / 60))
    if (seconds < 86400)
        return qsTr("%n h ago", "", Math.floor(seconds / 3600))
    return qsTr("%n days ago", "", Math.floor(seconds / 86400))
}

function duration(seconds) {
    seconds = Math.max(0, Math.round(seconds))
    if (seconds < 60)
        return qsTr("%1 s").arg(seconds)
    const minutes = Math.floor(seconds / 60)
    if (minutes < 60)
        return seconds % 60 ? qsTr("%1 min %2 s").arg(minutes).arg(seconds % 60) : qsTr("%1 min").arg(minutes)
    return qsTr("%1 h %2 min").arg(Math.floor(minutes / 60)).arg(minutes % 60)
}

function size(bytes) {
    return Qt.locale().formattedDataSize(bytes, 1)
}

// "~/Drive" reads better than "/home/someone/Drive".
function home(path) {
    return path.replace(/^\/home\/[^/]+/, "~")
}
