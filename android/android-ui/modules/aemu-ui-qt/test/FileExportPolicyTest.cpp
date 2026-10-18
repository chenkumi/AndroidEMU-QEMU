// Standalone tests: no gtest, moc, emulator, adb server or live device required.
#include "android/skin/qt/file-export-policy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; \
} } while (false)

static QByteArray record(char type, const QByteArray& name) {
    QByteArray result(1, type);
    result.append('\0'); result.append(name); result.append('\0');
    return result;
}

static QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace FileExport;
    CHECK(shellQuote("a b") == "'a b'");
    CHECK(shellQuote("a'b") == "'a'\\''b'");
    CHECK(shellQuote("$(touch /x); `id`") == "'$(touch /x); `id`'");
    CHECK(shellQuote("") == "''");
    CHECK(hostFileName(QString::fromUtf8("照片 01.png")));
    CHECK(hostFileName("it's $(not a shell command).txt"));
    CHECK(hostFileName("-leading-dash.txt"));
    CHECK(hostFileName("com10.txt"));
    for (const auto& value : QStringList{"", ".", "..", "a/b", "a\\b", "a:b", "NUL", "con.txt",
                                        "LPT1.log", "COM9", "CONOUT$", "COM\u00b9.txt", "a.", "a ",
                                        "a\nb", "a\rb", "a\tb", "a?b", "a*b", "a|b", "a<b", "a>b", "a\"b"}) {
        CHECK(!hostFileName(value));
    }
    CHECK(!hostFileName(QString(201, 'a')));
    CHECK(!guestComponent(QString(QChar(0))));
    CHECK(sharedPath("/storage/emulated/0/Download"));
    CHECK(sharedPath("/storage/emulated/0/DCIM/Camera/a.jpg"));
    CHECK(sharedPath("/storage/emulated/0/Pictures/a'b.jpg"));
    CHECK(sharedPath("/storage/emulated/0/Documents/file.txt"));
    CHECK(sharedPath("/storage/emulated/0/Music/album/song.mp3"));
    CHECK(sharedPath("/storage/emulated/0/Movies/clip.mp4"));
    const QStringList expectedRoots{"Download", "Documents", "Music", "Movies", "Pictures", "DCIM"};
    CHECK(sharedRoots() == expectedRoots);
    for (const auto& path : QStringList{"/data/data/private", "/sdcard/Download", "/storage/emulated/0/Android/data",
                                       "/storage/emulated/0/Android/media/com.x/a.mp3",
                                       "/storage/emulated/0/Ringtones/a.ogg", "/storage/emulated/0/Musicx/a.mp3",
                                       "/storage/emulated/0/Download/../Android/data", "/storage/emulated/0/Download//a",
                                       "/storage/emulated/0/Download/", "/storage/emulated/0/Downloads/a"}) {
        CHECK(!sharedPath(path));
        CHECK(listScript(path) == "exit 40");
        CHECK(preflightScript(path) == "exit 40");
        CHECK(existsScript(path) == "exit 40");
    }
    CHECK(existsScript("/storage/emulated/0/Music") == "exit 40");
    CHECK(existsScript("/storage/emulated/0/Music/a'b.mp3").contains(shellQuote("./a'b.mp3")));
    CHECK(existsScript("/storage/emulated/0/Music/a.mp3").contains("[ ! -e"));
    CHECK(existsScript("/storage/emulated/0/Music/a.mp3").contains("[ ! -L"));
    CHECK(existsScript("/storage/emulated/0/Music/a.mp3").contains("exit 61"));
    CHECK(existsScript("/storage/emulated/0/Music/a.mp3").contains("pwd -P"));
    CHECK(listScript("/storage/emulated/0/Music/a.mp3").contains("for p in"));
    CHECK(preflightScript("/storage/emulated/0/Download") == "exit 40");
    CHECK(preflightScript("/storage/emulated/0/Download/a'b").contains(shellQuote("./a'b")));
    CHECK(preflightScript("/storage/emulated/0/Download/a").contains("[ ! -L"));
    CHECK(preflightScript("/storage/emulated/0/Download/a").contains("readlink -f"));
    CHECK(listScript("/storage/emulated/0/Download").contains("pwd -P"));
    CHECK(listScript("/storage/emulated/0/Download").contains("[ -L"));

    QVector<Entry> entries;
    QString error;
    CHECK(parseListing(record('f', "a'b; $(echo BAD).txt") + record('d', "Camera") +
                       record('f', QString::fromUtf8("照片.png").toUtf8()), &entries, &error));
    CHECK(entries.size() == 3);
    if (entries.size() == 3) {
        CHECK(entries[0].directory && entries[0].name == "Camera");
        CHECK(entries[1].name == "a'b; $(echo BAD).txt");
    }
    CHECK(parseListing({}, &entries, &error) && entries.isEmpty());
    CHECK(!parseListing(record('f', "same") + record('d', "same"), &entries, &error));
    CHECK(entries.isEmpty());
    CHECK(!parseListing(record('l', "link"), &entries, &error));
    CHECK(!parseListing(record('f', "../private"), &entries, &error));
    CHECK(parseListing(record('f', "good.txt") + record('f', "line\nbreak") +
                       record('d', "tab\tname"), &entries, &error));
    CHECK(entries.size() == 1 && entries[0].name == "good.txt");
    CHECK(!parseListing(record('f', QByteArray("\xff", 1)), &entries, &error));
    CHECK(!parseListing(QByteArray("f\0missing", 9), &entries, &error));
    CHECK(!parseListing(QByteArray(4 * 1024 * 1024 + 1, 'x'), &entries, &error));
    qint64 size = -1;
    CHECK(parseSize(QByteArray("0\0", 2), &size) && size == 0);
    CHECK(parseSize(QByteArray("4294967296\0", 11), &size) && size == 4294967296LL);
    CHECK(!parseSize(QByteArray("-1\0", 3), &size));
    CHECK(!parseSize(QByteArray("1\0extra\0", 8), &size));
    CHECK(!parseSize(QByteArray("1\n\0", 3), &size));
    CHECK(!parseSize("1", &size));
    CHECK(!parseSize(QByteArray("9999999999999999999\0", 20), &size));

    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    if (temporary.isValid()) {
        const QString staged = temporary.filePath("unique.part");
        const QString target = temporary.filePath("existing.txt");
        { QFile file(staged); CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write("new") == 3); }
        { QFile file(target); CHECK(file.open(QIODevice::WriteOnly)); CHECK(file.write("old") == 3); }
        CHECK(!commitNoReplace(staged, target, &error));
        CHECK(readFile(target) == "old");
        CHECK(readFile(staged) == "new");
        const QString fresh = temporary.filePath("fresh.txt");
        CHECK(commitNoReplace(staged, fresh, &error));
        CHECK(readFile(fresh) == "new");
        CHECK(!QFile::exists(staged));
        CHECK(!commitNoReplace(staged, temporary.filePath("absent.txt"), &error));
        CHECK(!QFile::exists(temporary.filePath("absent.txt")));
    }
    std::cout << "FileExportPolicyTest: " << (failures ? "FAILED" : "PASS") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
