#include "listingdiff.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <algorithm>

namespace {

constexpr qint64 kNsPerSecond = 1000000000LL;
// Google Drive keeps times to the millisecond.
constexpr qint64 kEqualWindowNs = 1000000LL;
// QDate::toJulianDay() of 1970-01-01.
constexpr qint64 kUnixEpochJulianDay = 2440588;

// The value of digits hex digits at quoted[from], or false.
bool hexValue(const QString &quoted, qsizetype from, int digits, char32_t *value) {
    if (from + digits > quoted.size())
        return false;
    *value = 0;
    // By hand: QString::toUInt would also take a sign or spaces.
    for (qsizetype i = from; i < from + digits; ++i) {
        const char16_t c = quoted.at(i).unicode();
        const int digit = c >= '0' && c <= '9' ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0)
            return false;
        *value = *value * 16 + char32_t(digit);
    }
    return true;
}

bool octalValue(const QString &quoted, qsizetype from, char32_t *value) {
    if (from + 3 > quoted.size())
        return false;
    *value = 0;
    for (qsizetype i = from; i < from + 3; ++i) {
        const char16_t c = quoted.at(i).unicode();
        if (c < '0' || c > '7')
            return false;
        *value = *value * 8 + (c - '0');
    }
    return *value <= 0xFF;
}

void appendCodePoint(QByteArray &bytes, char32_t codePoint) { bytes += QString::fromUcs4(&codePoint, 1).toUtf8(); }

// "2026-11-11T12:00:00.500000000+0000" as Unix nanoseconds. Built by hand
// with the fixed offset: a local-time QDateTime per line costs about 0.2 ms
// on this machine, and a listing has tens of thousands of lines.
bool parseListingTime(const QString &text, qint64 *ns) {
    static const QRegularExpression pattern(
        R"(^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)(?:\.(\d{1,9}))?(Z|[+-]\d{4})$)");
    const QRegularExpressionMatch m = pattern.match(text);
    if (!m.hasMatch())
        return false;
    const QDate date(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    if (!date.isValid())
        return false;
    qint64 seconds = (date.toJulianDay() - kUnixEpochJulianDay) * 86400 + m.captured(4).toInt() * 3600
                     + m.captured(5).toInt() * 60 + m.captured(6).toInt();
    const QString zone = m.captured(8);
    if (zone != "Z") {
        const int offset = zone.mid(1, 2).toInt() * 3600 + zone.mid(3, 2).toInt() * 60;
        seconds -= zone.startsWith('+') ? offset : -offset;
    }
    *ns = seconds * kNsPerSecond + m.captured(7).leftJustified(9, '0').toLongLong();
    return true;
}

}  // namespace

QString unquoteGo(const QString &quoted, bool *ok, bool *notUtf8) {
    QByteArray bytes;
    *ok = false;
    *notUtf8 = false;
    const qsizetype n = quoted.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = quoted.at(i);
        if (c != '\\') {
            const qsizetype length = c.isHighSurrogate() && i + 1 < n ? 2 : 1;
            bytes += quoted.mid(i, length).toUtf8();
            i += length;
            continue;
        }
        if (i + 1 >= n)
            return QString();
        const char16_t escape = quoted.at(i + 1).unicode();
        i += 2;
        char32_t value = 0;
        switch (escape) {
        case 'a': bytes += '\a'; break;
        case 'b': bytes += '\b'; break;
        case 'f': bytes += '\f'; break;
        case 'n': bytes += '\n'; break;
        case 'r': bytes += '\r'; break;
        case 't': bytes += '\t'; break;
        case 'v': bytes += '\v'; break;
        case '\\': bytes += '\\'; break;
        case '"': bytes += '"'; break;
        case 'x':
            // One byte, which may be half of a UTF-8 sequence.
            if (!hexValue(quoted, i, 2, &value))
                return QString();
            bytes += char(value);
            i += 2;
            break;
        case 'u':
        case 'U': {
            const int digits = escape == 'u' ? 4 : 8;
            if (!hexValue(quoted, i, digits, &value) || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                return QString();
            appendCodePoint(bytes, value);
            i += digits;
            break;
        }
        default:
            if (escape < '0' || escape > '7' || !octalValue(quoted, i - 1, &value))
                return QString();
            bytes += char(value);
            i += 2;
            break;
        }
    }
    const QString text = QString::fromUtf8(bytes);
    *notUtf8 = text.toUtf8() != bytes;
    *ok = true;
    return text;
}

std::optional<QHash<QString, ListingEntry>> readListing(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QString("%1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    static const QRegularExpression linePattern(R"re(^(\S+) +(-?\d+) \S+ \S+ (\S+) "(.*)"$)re");
    QHash<QString, ListingEntry> files;
    int number = 0;
    while (!file.atEnd()) {
        QByteArray raw = file.readLine();
        ++number;
        if (raw.endsWith('\n'))
            raw.chop(1);
        if (raw.isEmpty() || raw.startsWith('#'))
            continue;
        const QString text = QString::fromUtf8(raw);
        const auto fail = [&](const QString &why) {
            *error = QString("%1:%2: %3: %4").arg(path).arg(number).arg(why, text.left(120));
            return std::nullopt;
        };
        const QRegularExpressionMatch m = linePattern.match(text);
        if (!m.hasMatch())
            return fail("not a bisync listing line");
        if (m.captured(1) != "-")
            continue;
        ListingEntry entry;
        entry.size = m.captured(2).toLongLong();
        if (!parseListingTime(m.captured(3), &entry.mtimeNs))
            return fail("the time is not in the bisync format");
        bool ok = false;
        const QString name = unquoteGo(m.captured(4), &ok, &entry.notUtf8);
        if (!ok)
            return fail("the name has a malformed escape");
        // Names that are not UTF-8 keep their quoted text, which is unique
        // and printable; decoding would fold them into replacement marks.
        files.insert(entry.notUtf8 ? m.captured(4) : name, entry);
    }
    return files;
}

QList<Divergence> divergences(const QHash<QString, ListingEntry> &cloud, const QHash<QString, ListingEntry> &notebook) {
    QStringList names = cloud.keys();
    for (auto it = notebook.cbegin(); it != notebook.cend(); ++it)
        if (!cloud.contains(it.key()))
            names << it.key();
    std::sort(names.begin(), names.end());

    QList<Divergence> found;
    for (const QString &name : std::as_const(names)) {
        const auto inCloud = cloud.constFind(name);
        const auto inNotebook = notebook.constFind(name);
        Divergence d;
        d.path = name;
        if (inCloud != cloud.cend())
            d.cloud = *inCloud;
        if (inNotebook != notebook.cend())
            d.notebook = *inNotebook;
        if (inCloud == cloud.cend()) {
            d.kind = "onlyNotebook";
            d.newer = "notebook";
        } else if (inNotebook == notebook.cend()) {
            d.kind = "onlyCloud";
            d.newer = "cloud";
        } else {
            const bool sameSize = d.cloud.size < 0 || d.notebook.size < 0 || d.cloud.size == d.notebook.size;
            const bool sameTime = qAbs(d.cloud.mtimeNs - d.notebook.mtimeNs) < kEqualWindowNs;
            if (sameSize && sameTime && !d.cloud.notUtf8 && !d.notebook.notUtf8)
                continue;
            d.kind = "differs";
            d.newer = d.notebook.mtimeNs - d.cloud.mtimeNs >= kEqualWindowNs ? "notebook" : "cloud";
        }
        if (d.cloud.notUtf8 || d.notebook.notUtf8)
            d.kind = "notUtf8";
        found << d;
    }
    return found;
}

QPair<QString, QString> currentListings(const QString &workDir) {
    const QDir dir(workDir);
    const QStringList cloud = dir.entryList({"*.path1.lst"}, QDir::Files | QDir::Hidden);
    const QStringList notebook = dir.entryList({"*.path2.lst"}, QDir::Files | QDir::Hidden);
    if (cloud.size() != 1 || notebook.size() != 1)
        return {};
    return {dir.filePath(cloud.first()), dir.filePath(notebook.first())};
}
