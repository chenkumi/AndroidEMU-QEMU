#include "android/skin/qt/file-export-dialog.h"
#include "android/skin/qt/file-export-policy.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <utility>

namespace {
constexpr int kMaxOutput = 4 * 1024 * 1024;
constexpr int kDirectoryRole = Qt::UserRole + 1;
constexpr int kProbeDelayMs = 300;
const char kPrefix[] = "/storage/emulated/0/";

// Human form of a guest path, so the header stays readable for deep folders.
QString breadcrumb(const QString& path) {
    const QString trimmed = path.startsWith(QLatin1String(kPrefix))
                                    ? path.mid(int(sizeof(kPrefix)) - 1)
                                    : path;
    return QString(trimmed).replace('/', QStringLiteral(" \u203a "));
}

QString readableBytes(qint64 bytes) {
    if (bytes < 1024) return QObject::tr("%1 bytes").arg(bytes);
    const double kb = double(bytes) / 1024.0;
    if (kb < 1024) return QObject::tr("%1 KiB").arg(QString::number(kb, 'f', 1));
    return QObject::tr("%1 MiB").arg(QString::number(kb / 1024.0, 'f', 1));
}
}  // namespace

FileExportDialog::FileExportDialog(QString adbPath, QString serial, QWidget* parent)
    : QDialog(parent), mAdbPath(std::move(adbPath)), mSerial(std::move(serial)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("file_export_dialog");
    setWindowTitle(tr("Transfer shared Android files"));
    setMinimumWidth(720);
    resize(760, 580);
    setAcceptDrops(true);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    mHeading = new QLabel(tr("Android shared storage"), this);
    QFont heading = mHeading->font();
    heading.setBold(true);
    heading.setPointSizeF(heading.pointSizeF() + 1.0);
    mHeading->setFont(heading);
    layout->addWidget(mHeading);

    auto navigation = new QHBoxLayout;
    navigation->setSpacing(8);
    mRoots = new QComboBox(this);
    mRoots->setMinimumWidth(190);
    mRoots->setIconSize(QSize(16, 16));
    for (const QString& root : FileExport::sharedRoots())
        mRoots->addItem(style()->standardIcon(QStyle::SP_DirIcon), root);
    mUp = new QPushButton(this);
    mUp->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    mUp->setToolTip(tr("Parent folder (Backspace)"));
    mUp->setFixedSize(32, 32);
    mRefresh = new QPushButton(this);
    mRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    mRefresh->setToolTip(tr("Refresh (F5)"));
    mRefresh->setFixedSize(32, 32);
    navigation->addWidget(mRoots);
    navigation->addWidget(mUp);
    navigation->addWidget(mRefresh);
    navigation->addStretch(1);
    mSelection = new QLabel(this);
    mSelection->setTextFormat(Qt::PlainText);
    mSelection->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    navigation->addWidget(mSelection);
    layout->addLayout(navigation);

    mLocation = new QLabel(this);
    mLocation->setTextFormat(Qt::PlainText);
    mLocation->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mLocation);

    mFiles = new QListWidget(this);
    mFiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mFiles->setAlternatingRowColors(true);
    mFiles->setUniformItemSizes(true);
    mFiles->setIconSize(QSize(18, 18));
    mFiles->setTextElideMode(Qt::ElideMiddle);
    mFiles->setMinimumHeight(260);
    layout->addWidget(mFiles, 1);

    auto hint = new QLabel(tr("Double-click a folder to open it. Export selected files to Windows, or upload "
                              "Windows files into this folder. Existing files are never replaced."),
                           this);
    QFont hintFont = hint->font();
    hintFont.setPointSizeF(qMax(7.0, hintFont.pointSizeF() - 1.0));
    hint->setFont(hintFont);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    mProgress = new QProgressBar(this);
    mProgress->setTextVisible(true);
    mProgress->setVisible(false);
    layout->addWidget(mProgress);

    mStatus = new QLabel(this);
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setWordWrap(true);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(mStatus);

    auto separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    auto actions = new QHBoxLayout;
    actions->setSpacing(8);
    mUpload = new QPushButton(tr("Upload files\u2026"), this);
    mUpload->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    mExport = new QPushButton(tr("Export selected\u2026"), this);
    mExport->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    mCancel = new QPushButton(tr("Cancel operation"), this);
    mOpen = new QPushButton(tr("Open destination folder"), this);
    auto close = new QPushButton(tr("Close"), this);
    actions->addWidget(mUpload);
    actions->addWidget(mExport);
    actions->addStretch(1);
    actions->addWidget(mCancel);
    actions->addWidget(mOpen);
    actions->addWidget(close);
    layout->addLayout(actions);
    mOpen->setEnabled(false);

    mRoots->setObjectName("export_roots");
    mFiles->setObjectName("export_files");
    mExport->setObjectName("export_start");
    mUpload->setObjectName("upload_start");
    mCancel->setObjectName("export_cancel");
    mOpen->setObjectName("export_open");
    mStatus->setObjectName("export_status");
    mProgress->setObjectName("export_progress");
    mUp->setObjectName("export_up");
    mRefresh->setObjectName("export_refresh");
    mSelection->setObjectName("export_selection");
    mLocation->setObjectName("export_location");

    mWatchdog = new QTimer(this);
    mWatchdog->setSingleShot(true);
    mProgressTimer = new QTimer(this);
    mProgressTimer->setInterval(250);
    mProbeTimer = new QTimer(this);
    mProbeTimer->setSingleShot(true);
    mProbeTimer->setInterval(kProbeDelayMs);
    connect(mWatchdog, &QTimer::timeout, this, [this] { fail(tr("ADB operation timed out.")); });
    connect(mProgressTimer, &QTimer::timeout, this, [this] { progress(); });
    connect(mProbeTimer, &QTimer::timeout, this, [this] { showSelection(); });
    connect(mRoots, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) browse(QLatin1String(kPrefix) + mRoots->itemText(index));
    });
    connect(mUp, &QPushButton::clicked, this, [this] { browse(mPath.left(mPath.lastIndexOf('/'))); });
    connect(mRefresh, &QPushButton::clicked, this, [this] { browse(mPath); });
    connect(mFiles, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item->data(kDirectoryRole).toBool()) browse(mPath + '/' + item->text());
        else if (item->flags() & Qt::ItemIsSelectable) chooseDestination();
    });
    connect(mFiles, &QListWidget::itemSelectionChanged, this, [this] {
        QStringList names;
        selectedFiles(&names);
        mExport->setEnabled(mState == State::Idle && !names.isEmpty());
        mProbeTimer->start();
    });
    connect(mExport, &QPushButton::clicked, this, [this] { chooseDestination(); });
    connect(mUpload, &QPushButton::clicked, this, [this] { uploadFiles(); });
    connect(mCancel, &QPushButton::clicked, this, [this] { cancel(); });
    connect(close, &QPushButton::clicked, this, &FileExportDialog::reject);
    connect(mOpen, &QPushButton::clicked, this, [this] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(mDestination)))
            mStatus->setText(tr("Unable to open destination: %1").arg(mDestination));
    });
    setBusy(State::Idle);
    QTimer::singleShot(0, this, [this] {
        if (mAdbPath.isEmpty() || mSerial.isEmpty()) {
            fail(tr("This emulator window has no configured ADB executable or serial."));
            mRoots->setEnabled(false);
            mRefresh->setEnabled(false);
            return;
        }
        if (mRoots->currentIndex() != 0) mRoots->setCurrentIndex(0);
        browse(QLatin1String(kPrefix) + FileExport::sharedRoots().value(0));
    });
}

FileExportDialog::~FileExportDialog() { stopProcesses(); }

void FileExportDialog::reject() {
    stopProcesses();
    QDialog::reject();
}

QString FileExportDialog::command(const QString& script) const {
    // adb shell concatenates arguments into a guest shell command. Quote the
    // complete script as one sh -c operand, as well as each pathname inside it.
    return QStringLiteral("sh -c ") + FileExport::shellQuote(script);
}

QProcess* FileExportDialog::startProcess(const QStringList& arguments) {
    auto process = new QProcess(qApp);
    // Do not make the process a child of the dialog: destruction of a running
    // QProcess waits. On close, kill only these clients and reap asynchronously.
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail(tr("Cannot start ADB: %1").arg(process->errorString()));
    });
    process->setProgram(mAdbPath);
    process->setArguments(QStringList{"-s", mSerial} + arguments);
    // Start on the next event turn, after all consumers have attached signals.
    QTimer::singleShot(0, process, [process] {
        if (!process->property("export_cancelled").toBool()) process->start();
    });
    return process;
}

void FileExportDialog::stopProcesses() {
    mWatchdog->stop();
    mProgressTimer->stop();
    mProbeTimer->stop();
    for (QPointer<QProcess>* slot : {&mCommand, &mProbe}) {
        if (auto process = *slot) {
            process->disconnect(this);
            // Invalidate a queued start, but never delete a signal sender while
            // its FailedToStart/startup stack is still active.
            process->setProperty("export_cancelled", true);
            if (process->state() == QProcess::NotRunning) {
                process->deleteLater();
            } else {
                process->closeWriteChannel();
                process->kill();
            }
        }
        slot->clear();
    }
    mTemporary.reset();
}

void FileExportDialog::setBusy(State state) {
    mState = state;
    const bool idle = state == State::Idle;
    const bool upAllowed = FileExport::sharedPath(mPath.left(mPath.lastIndexOf('/')));
    mRoots->setEnabled(idle);
    mUp->setEnabled(idle && upAllowed);
    mRefresh->setEnabled(idle);
    mFiles->setEnabled(idle);
    mUpload->setEnabled(idle && FileExport::sharedPath(mPath));
    QStringList selection;
    selectedFiles(&selection);
    mExport->setEnabled(idle && !selection.isEmpty());
    mCancel->setEnabled(!idle);
    mProgress->setVisible(!idle);
    if (idle) {
        mProgress->setRange(0, 1000);
        mProgress->setValue(0);
    } else if (state == State::Pulling) {
        mProgress->setRange(0, 1000);
    } else if (state == State::Listing || state == State::Validating) {
        mProgress->setRange(0, 0);
    } else {
        // Upload states report whole files, not bytes.
        mProgress->setRange(0, qMax(1, mTotal));
        mProgress->setValue(mCompleted);
    }
}

bool FileExportDialog::selectedFiles(QStringList* names) const {
    names->clear();
    for (auto item : mFiles->selectedItems()) {
        if (item->data(kDirectoryRole).toBool()) continue;
        if (item->flags() & Qt::ItemIsEnabled) names->append(item->text());
    }
    return !names->isEmpty();
}

void FileExportDialog::setLocation(const QString& path) {
    mLocation->setText(breadcrumb(path));
    mLocation->setToolTip(path);
}

void FileExportDialog::browse(const QString& path) {
    if (mState != State::Idle || !FileExport::sharedPath(path)) return;
    mPath = path;
    setLocation(path);
    mFiles->clear();
    mSelection->clear();
    mProbedPath.clear();
    mOutput.clear();
    mErrors.clear();
    setBusy(State::Listing);
    mStatus->setText(tr("Reading shared folder\u2026"));
    auto process = startProcess({"shell", "-T", command(FileExport::listScript(path))});
    mCommand = process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mOutput += process->readAllStandardOutput();
        if (mOutput.size() > kMaxOutput) fail(tr("Folder is too large to browse (4 MiB listing limit)."));
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        mOutput += process->readAllStandardOutput();
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
        mCommand.clear();
        mWatchdog->stop();
        if (code || status != QProcess::NormalExit) {
            fail(tr("Cannot read folder. It may not exist, be a link, or the device may be offline.\n%1")
                         .arg(QString::fromUtf8(mErrors))); return;
        }
        QVector<FileExport::Entry> entries;
        QString error;
        if (!FileExport::parseListing(mOutput, &entries, &error)) { fail(error); return; }
        for (const auto& entry : entries) {
            auto item = new QListWidgetItem(entry.name, mFiles);
            item->setData(kDirectoryRole, entry.directory);
            item->setIcon(style()->standardIcon(entry.directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
            item->setSizeHint(QSize(0, 30));
            if (entry.directory) {
                item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            } else if (!FileExport::hostFileName(entry.name)) {
                item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
                item->setToolTip(tr("Not a supported Windows filename; export skipped without renaming."));
            }
        }
        setBusy(State::Idle);
        if (entries.isEmpty()) {
            auto empty = new QListWidgetItem(tr("This folder is empty."), mFiles);
            empty->setFlags(Qt::NoItemFlags);
        }
        QString summary = mSummary.isEmpty()
                                  ? tr("%1 item(s). Select files to export, or upload files into this folder.")
                                            .arg(entries.size())
                                  : mSummary;
        mSummary.clear();
        mStatus->setText(summary);
    });
    mWatchdog->start(30000);
}

// One extra ADB round trip per selection change, debounced, only while idle; it
// keeps the header informative without a second listing round trip.
void FileExportDialog::showSelection() {
    QStringList names;
    selectedFiles(&names);
    if (mState != State::Idle || names.size() != 1) {
        mSelection->setText(names.isEmpty() ? QString() : tr("%1 selected").arg(names.size()));
        mProbedPath.clear();
        return;
    }
    const QString path = mPath + '/' + names.first();
    if (path == mProbedPath) return;
    if (mProbe) mProbe->kill();
    mProbedPath = path;
    mProbeOutput.clear();
    mSelection->setText(tr("Checking size\u2026"));
    auto process = startProcess({"shell", "-T", command(FileExport::preflightScript(path))});
    mProbe = process;
    process->setProperty("export_cancelled", false);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mProbeOutput += process->readAllStandardOutput();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, path](int code, QProcess::ExitStatus status) {
        if (mProbe != process) return;
        const QByteArray bytes = mProbeOutput;
        mProbe.clear();
        QStringList now;
        selectedFiles(&now);
        if (mState != State::Idle || code || status != QProcess::NormalExit ||
            mProbedPath != path || now.size() != 1 || (mPath + '/' + now.first()) != path) {
            return;
        }
        qint64 size = 0;
        mSelection->setText(FileExport::parseSize(bytes, &size) ? readableBytes(size)
                                                               : tr("Size unavailable"));
    });
}

void FileExportDialog::chooseDestination() {
    if (mState != State::Idle) return;
    QStringList selection;
    if (!selectedFiles(&selection)) return;
    mQueue.clear();
    for (const QString& name : selection) mQueue.append(name);
    if (mQueue.isEmpty()) return;
    // Asynchronous native directory chooser; no nested blocking exec loop.
    auto chooser = new QFileDialog(this, tr("Choose export destination"), mDestination);
    chooser->setAttribute(Qt::WA_DeleteOnClose);
    chooser->setFileMode(QFileDialog::Directory);
    chooser->setOption(QFileDialog::ShowDirsOnly);
    setBusy(State::Validating); // freeze source selection while the chooser is open
    mCancel->setEnabled(false);
    connect(chooser, &QFileDialog::rejected, this, [this] {
        mQueue.clear();
        setBusy(State::Idle);
    });
    connect(chooser, &QFileDialog::fileSelected, this, [this](const QString& directory) {
        const QFileInfo info(directory);
        mDestination = info.canonicalFilePath();
        if (mDestination.isEmpty() || !info.isDir()) { fail(tr("Invalid destination folder.")); return; }
        mTotal = mQueue.size();
        mCompleted = 0;
        mOpen->setEnabled(true);
        nextFile();
    });
    chooser->open();
}

void FileExportDialog::nextFile() {
    if (mQueue.isEmpty()) {
        stopProcesses();
        setBusy(State::Idle);
        mProgress->setRange(0, 1000);
        mProgress->setValue(1000);
        mStatus->setText(tr("Completed: %1 file(s) exported to %2").arg(mCompleted).arg(mDestination));
        return;
    }
    mCurrent = mQueue.takeFirst();
    const QString target = QDir(mDestination).filePath(mCurrent);
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
        fail(tr("Destination already exists; nothing was overwritten: %1").arg(target)); return;
    }
    // Same-volume unique staging directory: atomic no-replace final commit,
    // no AppRoot scratch files, and no large second copy across volumes.
    mTemporary = std::make_shared<QTemporaryDir>(QDir(mDestination).filePath(".emu-export-XXXXXX.part"));
    if (!mTemporary->isValid()) { fail(tr("Cannot create a private staging directory in the destination.")); return; }
    mOutput.clear();
    mErrors.clear();
    setBusy(State::Validating);
    mStatus->setText(tr("Validating %1 (%2/%3)\u2026").arg(mCurrent).arg(mCompleted + 1).arg(mTotal));
    auto process = startProcess({"shell", "-T", command(FileExport::preflightScript(mPath + '/' + mCurrent))});
    mCommand = process;
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mOutput += process->readAllStandardOutput();
        if (mOutput.size() > 128) fail(tr("Invalid file validation response."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        mOutput += process->readAllStandardOutput();
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
        mCommand.clear();
        if (code || status != QProcess::NormalExit || !FileExport::parseSize(mOutput, &mExpectedSize)) {
            fail(tr("File validation failed: missing, linked, unreadable, or device offline.\n%1")
                         .arg(QString::fromUtf8(mErrors))); return;
        }
        pullFile();
    });
    mWatchdog->start(30000);
}

void FileExportDialog::pullFile() {
    setBusy(State::Pulling);
    // Preflight is intentionally not a security boundary against concurrent
    // guest mutation. Only trusted-local shared-file export is supported.
    auto process = startProcess({"pull", mPath + '/' + mCurrent, mTemporary->filePath("payload.part")});
    mCommand = process;
    // Keep staging alive until this exact client is reaped, even after dialog
    // close/cancel. A still-running adb must never write into a reused pathname.
    auto temporary = mTemporary;
    connect(process, &QObject::destroyed, qApp, [temporary] {});
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardOutput()).right(8192);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        mErrors = (mErrors + process->readAllStandardError() + process->readAllStandardOutput()).right(8192);
        mCommand.clear();
        if (code || status != QProcess::NormalExit) {
            fail(tr("ADB pull failed. Check the device connection and destination space/permissions.\n%1")
                         .arg(QString::fromUtf8(mErrors))); return;
        }
        const QString staged = mTemporary->filePath("payload.part");
        const QFileInfo info(staged);
        if (!info.isFile() || info.isSymLink() || info.size() != mExpectedSize) {
            fail(tr("Export size/type changed or transfer was incomplete; file not committed.")); return;
        }
        QString error;
        if (!FileExport::commitNoReplace(staged, QDir(mDestination).filePath(mCurrent), &error)) {
            fail(error); return;
        }
        ++mCompleted;
        stopProcesses();
        nextFile();
    });
    mProgressTimer->start();
    mWatchdog->start(60 * 60 * 1000); // finite upper bound, not an idle timeout
    progress();
}

void FileExportDialog::uploadFiles() {
    if (mState != State::Idle || !FileExport::sharedPath(mPath)) return;
    // Asynchronous multi-file chooser, same pattern as the directory chooser.
    auto chooser = new QFileDialog(this, tr("Choose files to upload to %1").arg(breadcrumb(mPath)));
    chooser->setAttribute(Qt::WA_DeleteOnClose);
    chooser->setFileMode(QFileDialog::ExistingFiles);
    setBusy(State::Validating);
    mCancel->setEnabled(false);
    connect(chooser, &QFileDialog::rejected, this, [this] { setBusy(State::Idle); });
    connect(chooser, &QFileDialog::filesSelected, this, [this](const QStringList& files) {
        startUpload(files);
    });
    chooser->open();
}

void FileExportDialog::startUpload(const QStringList& files) {
    if (mState != State::Idle && mState != State::Validating) return;
    if (!FileExport::sharedPath(mPath)) return;
    mUploadQueue.clear();
    mSkipped = 0;
    mTotalBytes = 0;
    for (const QString& file : files) {
        const QFileInfo info(file);
        if (!info.isFile() || info.isSymLink() || !FileExport::guestComponent(info.fileName())) {
            ++mSkipped;
            continue;
        }
        mUploadQueue.append(info.absoluteFilePath());
        mTotalBytes += info.size();
    }
    if (mUploadQueue.isEmpty()) {
        setBusy(State::Idle);
        mStatus->setText(tr("Nothing to upload: %1 unusable item(s) (folders, links or unsupported names).")
                                 .arg(mSkipped));
        return;
    }
    mTotal = mUploadQueue.size();
    mCompleted = 0;
    uploadNext();
}

void FileExportDialog::uploadNext() {
    if (mUploadQueue.isEmpty()) { finishUpload(); return; }
    mCurrentHost = mUploadQueue.takeFirst();
    mCurrent = QFileInfo(mCurrentHost).fileName();
    const QString target = mPath + '/' + mCurrent;
    mOutput.clear();
    mErrors.clear();
    setBusy(State::Checking);
    mStatus->setText(tr("Checking %1 (%2/%3)\u2026").arg(mCurrent).arg(mCompleted + 1).arg(mTotal));
    auto process = startProcess({"shell", "-T", command(FileExport::existsScript(target))});
    mCommand = process;
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mOutput += process->readAllStandardOutput();
        if (mOutput.size() > 128) fail(tr("Invalid upload precheck response."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, target](int code, QProcess::ExitStatus status) {
        mCommand.clear();
        if (status != QProcess::NormalExit) {
            fail(tr("Upload precheck failed; the device may be offline.\n%1").arg(QString::fromUtf8(mErrors)));
            return;
        }
        if (code == 61) {
            fail(tr("A file of that name already exists on the device; nothing was overwritten:\n%1").arg(target));
            return;
        }
        if (code) {
            fail(tr("Cannot inspect the device folder (exit %1).\n%2")
                         .arg(code)
                         .arg(QString::fromUtf8(mErrors)));
            return;
        }
        pushFile(target);
    });
    mWatchdog->start(30000);
}

void FileExportDialog::pushFile(const QString& target) {
    setBusy(State::Uploading);
    const qint64 size = QFileInfo(mCurrentHost).size();
    mStatus->setText(tr("Uploading %1 (%2/%3, %4)\u2026")
                             .arg(mCurrent)
                             .arg(mCompleted + 1)
                             .arg(mTotal)
                             .arg(readableBytes(size)));
    auto process = startProcess({"push", mCurrentHost, target});
    mCommand = process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardOutput()).right(8192);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        mErrors = (mErrors + process->readAllStandardError()).right(8192);
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, target, size](int code, QProcess::ExitStatus status) {
        mErrors = (mErrors + process->readAllStandardError() + process->readAllStandardOutput()).right(8192);
        mCommand.clear();
        if (code || status != QProcess::NormalExit) {
            fail(tr("ADB push failed. Check device storage space and permissions.\n%1")
                         .arg(QString::fromUtf8(mErrors)));
            return;
        }
        verifyUpload(target, size);
    });
    mWatchdog->start(60 * 60 * 1000);
}

// Confirms the pushed file is a regular, complete file of the expected size.
// A mismatch is reported with its device path; nothing is deleted automatically.
void FileExportDialog::verifyUpload(const QString& target, qint64 expected) {
    setBusy(State::Verifying);
    mOutput.clear();
    mErrors.clear();
    auto process = startProcess({"shell", "-T", command(FileExport::preflightScript(target))});
    mCommand = process;
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        mOutput += process->readAllStandardOutput();
        if (mOutput.size() > 128) fail(tr("Invalid upload verification response."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, target, expected](int code, QProcess::ExitStatus status) {
        mOutput += process->readAllStandardOutput();
        mCommand.clear();
        if (status != QProcess::NormalExit) {
            fail(tr("Upload verification failed; the device may be offline.")); return;
        }
        qint64 actual = -1;
        if (code || !FileExport::parseSize(mOutput, &actual)) {
            fail(tr("Uploaded file is missing or not a regular file on the device:\n%1").arg(target)); return;
        }
        if (expected >= 0 && actual != expected) {
            fail(tr("Uploaded file has %1 of %2 expected bytes; it was left in place for inspection:\n%3")
                         .arg(actual)
                         .arg(expected)
                         .arg(target));
            return;
        }
        ++mCompleted;
        uploadNext();
    });
    mWatchdog->start(30000);
}

void FileExportDialog::finishUpload() {
    stopProcesses();
    setBusy(State::Idle);
    mProgress->setRange(0, 1000);
    mProgress->setValue(1000);
    mSummary = mSkipped ? tr("Uploaded %1 file(s) (%2) to %3; %4 item(s) skipped.")
                                  .arg(mCompleted)
                                  .arg(readableBytes(mTotalBytes))
                                  .arg(breadcrumb(mPath))
                                  .arg(mSkipped)
                        : tr("Uploaded %1 file(s) (%2) to %3.")
                                  .arg(mCompleted)
                                  .arg(readableBytes(mTotalBytes))
                                  .arg(breadcrumb(mPath));
    browse(mPath); // refresh so the new files are visible and selectable
}

void FileExportDialog::progress() {
    if (mState != State::Pulling || !mTemporary) return;
    const qint64 bytes = QFileInfo(mTemporary->filePath("payload.part")).size();
    const double fraction = mExpectedSize > 0 ? qMin(1.0, double(bytes) / double(mExpectedSize)) : 0.0;
    mProgress->setRange(0, 1000);
    mProgress->setValue(int(1000.0 * (mCompleted + fraction) / qMax(1, mTotal)));
    mStatus->setText(tr("Exporting %1 (%2/%3): %4 / %5 bytes")
                            .arg(mCurrent)
                            .arg(mCompleted + 1)
                            .arg(mTotal)
                            .arg(bytes)
                            .arg(mExpectedSize));
}

void FileExportDialog::fail(const QString& error) {
    stopProcesses();
    mQueue.clear();
    mUploadQueue.clear();
    setBusy(State::Idle);
    mStatus->setText(tr("Transfer stopped: %1\n%2 completed file(s) retained.").arg(error).arg(mCompleted));
}

void FileExportDialog::cancel() {
    stopProcesses();
    mQueue.clear();
    mUploadQueue.clear();
    setBusy(State::Idle);
    mStatus->setText(tr("Cancelled. %1 completed file(s) retained; the partial transfer was discarded.")
                             .arg(mCompleted));
}

void FileExportDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (mState != State::Idle || !FileExport::sharedPath(mPath)) return;
    if (event->mimeData() && event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void FileExportDialog::dropEvent(QDropEvent* event) {
    if (mState != State::Idle || !FileExport::sharedPath(mPath)) return;
    if (!event->mimeData() || !event->mimeData()->hasUrls()) return;
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) files.append(url.toLocalFile());
    }
    if (files.isEmpty()) return;
    event->acceptProposedAction();
    startUpload(files);
}

void FileExportDialog::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Backspace:
        if (mState == State::Idle) browse(mPath.left(mPath.lastIndexOf('/')));
        return;
    case Qt::Key_F5:
        if (mState == State::Idle) browse(mPath);
        return;
    case Qt::Key_A:
        if (mState == State::Idle && event->modifiers().testFlag(Qt::ControlModifier)) {
            mFiles->selectAll();
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (mState != State::Idle) return;
        auto item = mFiles->currentItem();
        if (item && (item->flags() & Qt::ItemIsEnabled) && item->data(kDirectoryRole).toBool()) {
            browse(mPath + '/' + item->text());
        } else {
            chooseDestination();
        }
        return;
    }
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}
