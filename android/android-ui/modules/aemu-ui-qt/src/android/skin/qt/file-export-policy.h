// Shared-file export policy. No device or GUI dependencies.
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace FileExport {
struct Entry {
    QString name;
    bool directory;
};
QString shellQuote(const QString& value);
bool guestComponent(const QString& name);
bool hostFileName(const QString& name);
// Shared storage roots the transfer dialog may read and write.
QStringList sharedRoots();
bool sharedPath(const QString& path);
// Strict NUL-framed (type, name) records; reject the entire listing on corruption.
bool parseListing(const QByteArray& bytes, QVector<Entry>* entries, QString* error);
QString listScript(const QString& path);
// Preflight a regular shared file; rejects symlink components and returns size\0.
// This is a trusted-local export, not a hostile guest containment boundary:
// concurrent guest mutation between preflight and adb pull remains possible.
QString preflightScript(const QString& path);
bool parseSize(const QByteArray& bytes, qint64* size);
// Upload precheck: exit 0 only when the target does not exist (also for broken
// links). exit 61 means the name is taken, exit 40 an unsupported path.
QString existsScript(const QString& path);
// Atomic same-volume, no-replace commit. Never falls back to copying on the UI thread.
bool commitNoReplace(const QString& temporary, const QString& destination, QString* error);
}  // namespace FileExport
