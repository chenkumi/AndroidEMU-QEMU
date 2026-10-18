#include "android/skin/qt/file-export-policy.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QStringList>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace FileExport {
QString shellQuote(const QString& value) {
    QString result = value;
    result.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + result + QLatin1Char('\'');
}

bool guestComponent(const QString& name) {
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/')) return false;
    for (QChar c : name) {
        if (c.unicode() < 32 || c.unicode() == 127 || c == QChar::ReplacementCharacter)
            return false;
    }
    return true;
}

bool hostFileName(const QString& name) {
    if (!guestComponent(name) || name.size() > 200 || name.endsWith('.') || name.endsWith(' '))
        return false;
    for (QChar c : QStringLiteral("<>:\"\\|?*")) if (name.contains(c)) return false;
    const QString stem = name.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) &&
        QStringLiteral("123456789\u00b9\u00b2\u00b3").contains(stem.at(3))) return false;
    return true;
}

QStringList sharedRoots() {
    return {QStringLiteral("Download"), QStringLiteral("Documents"), QStringLiteral("Music"),
            QStringLiteral("Movies"), QStringLiteral("Pictures"), QStringLiteral("DCIM")};
}

bool sharedPath(const QString& path) {
    const QString prefix = QStringLiteral("/storage/emulated/0/");
    if (!path.startsWith(prefix)) return false;
    const auto parts = path.mid(prefix.size()).split('/');
    if (parts.isEmpty() || !sharedRoots().contains(parts[0]))
        return false;
    for (const auto& part : parts) if (!guestComponent(part)) return false;
    return true;
}

bool parseListing(const QByteArray& bytes, QVector<Entry>* entries, QString* error) {
    entries->clear();
    auto fail = [&] { *error = QStringLiteral("Invalid or oversized directory listing."); entries->clear(); return false; };
    if (bytes.size() > 4 * 1024 * 1024) return fail();
    if (bytes.isEmpty()) return true;
    if (!bytes.endsWith('\0')) return fail();
    auto fields = bytes.split('\0');
    fields.removeLast();
    if (fields.size() % 2 || fields.size() > 20000) return fail();
    QSet<QString> seen;
    for (int i = 0; i < fields.size(); i += 2) {
        const QString name = QString::fromUtf8(fields[i + 1]);
        if ((fields[i] != "d" && fields[i] != "f") || name.toUtf8() != fields[i + 1] ||
            name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
            seen.contains(name)) return fail();
        seen.insert(name);
        // Validly framed but unusable names must not hide unrelated siblings.
        // Never pass control-containing names to a path or GUI operation.
        if (!guestComponent(name)) continue;
        entries->push_back({name, fields[i] == "d"});
    }
    std::sort(entries->begin(), entries->end(), [](const Entry& a, const Entry& b) {
        return a.directory != b.directory ? a.directory : a.name < b.name;
    });
    return true;
}

// cd pins the directory inode. Checking pwd -P after cd rejects symlinked
// components, including a root which has been replaced by a link.
static QString enterDirectory(const QString& path) {
    return "export LC_ALL=C; cd " + shellQuote(path) +
           " || exit 41; [ \"$(pwd -P)\" = " + shellQuote(path) + " ] || exit 42; ";
}

QString listScript(const QString& path) {
    if (!sharedPath(path)) return "exit 40";
    return enterDirectory(path) +
           "for p in ./* ./.[!.]* ./..?*; do "
           "[ -L \"$p\" ] && continue; "
           "if [ -d \"$p\" ]; then printf 'd\\000%s\\000' \"${p#./}\"; "
           "elif [ -f \"$p\" ]; then printf 'f\\000%s\\000' \"${p#./}\"; fi; done";
}

QString preflightScript(const QString& path) {
    if (!sharedPath(path)) return "exit 40";
    const int slash = path.lastIndexOf('/');
    if (!sharedPath(path.left(slash))) return "exit 40";
    return enterDirectory(path.left(slash)) + "p=" + shellQuote("./" + path.mid(slash + 1)) + "; "
           "[ ! -L \"$p\" ] && [ -f \"$p\" ] || exit 43; "
           "[ \"$(readlink -f \"$p\")\" = " + shellQuote(path) + " ] || exit 44; "
           "s=$(stat -Lc %s \"$p\") || exit 45; printf '%s\\000' \"$s\"";
}

// Upload precheck. -e covers regular files and directories, -L also covers a
// broken link, so a taken name is never silently replaced by a push.
QString existsScript(const QString& path) {
    if (!sharedPath(path)) return "exit 40";
    const int slash = path.lastIndexOf('/');
    if (!sharedPath(path.left(slash))) return "exit 40";
    return enterDirectory(path.left(slash)) + "p=" + shellQuote("./" + path.mid(slash + 1)) +
           "; [ ! -e \"$p\" ] && [ ! -L \"$p\" ] || exit 61";
}

bool parseSize(const QByteArray& bytes, qint64* size) {
    if (bytes.size() < 2 || bytes.size() > 21 || !bytes.endsWith('\0')) return false;
    const auto number = bytes.left(bytes.size() - 1);
    for (char c : number) if (c < '0' || c > '9') return false;
    bool ok;
    *size = number.toLongLong(&ok);
    return ok && *size >= 0;
}

bool commitNoReplace(const QString& temporary, const QString& destination, QString* error) {
#ifdef _WIN32
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()),
                    reinterpret_cast<LPCWSTR>(destination.utf16()), 0)) return true;
    *error = QStringLiteral("Cannot commit file without replacing an existing file (Windows error %1).")
                     .arg(GetLastError());
#else
    // link is atomic and fails with EEXIST, unlike POSIX rename.
    if (::link(QFile::encodeName(temporary).constData(), QFile::encodeName(destination).constData()) == 0) {
        ::unlink(QFile::encodeName(temporary).constData());
        return true;
    }
    *error = QString::fromLocal8Bit(std::strerror(errno));
#endif
    return false;
}
}  // namespace FileExport
