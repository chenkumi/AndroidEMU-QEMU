#pragma once

#include <QDialog>
#include <QPointer>
#include <QStringList>
#include <memory>

class QComboBox;
class QLabel;
class QListWidget;
class QProcess;
class QProgressBar;
class QPushButton;
class QTemporaryDir;
class QTimer;

// No moc required. Each instance captures one window's exact adb executable and
// serial; it never enumerates devices or consults the global adb interface.
// The dialog moves files both ways: guest shared storage -> Windows folder, and
// Windows files -> the folder currently open on the device.
class FileExportDialog final : public QDialog {
public:
    FileExportDialog(QString adbPath, QString serial, QWidget* parent);
    ~FileExportDialog() override;
    void reject() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class State { Idle, Listing, Validating, Pulling, Checking, Uploading, Verifying };
    QProcess* startProcess(const QStringList& arguments);
    void stopProcesses();
    void setBusy(State state);
    void browse(const QString& path);
    void chooseDestination();
    void nextFile();
    void pullFile();
    void uploadFiles();
    void startUpload(const QStringList& files);
    void uploadNext();
    void pushFile(const QString& target);
    void verifyUpload(const QString& target, qint64 expected);
    void finishUpload();
    void fail(const QString& error);
    void cancel();
    void progress();
    void refreshSelection();
    void showSelection();
    QString command(const QString& script) const;
    void setLocation(const QString& path);
    bool selectedFiles(QStringList* names) const;

    const QString mAdbPath;
    const QString mSerial;
    QString mPath;
    QString mDestination;
    QStringList mQueue;
    QStringList mUploadQueue;
    QString mCurrent;
    QString mCurrentHost;
    QString mSummary;
    QString mProbedPath;
    int mCompleted = 0;
    int mTotal = 0;
    int mSkipped = 0;
    qint64 mExpectedSize = 0;
    qint64 mTotalBytes = 0;
    QByteArray mOutput;
    QByteArray mProbeOutput;
    QByteArray mErrors;
    State mState = State::Idle;
    QPointer<QProcess> mCommand;
    QPointer<QProcess> mProbe;
    std::shared_ptr<QTemporaryDir> mTemporary;
    QComboBox* mRoots;
    QLabel* mHeading;
    QLabel* mLocation;
    QLabel* mSelection;
    QListWidget* mFiles;
    QPushButton* mUp;
    QPushButton* mRefresh;
    QPushButton* mExport;
    QPushButton* mUpload;
    QPushButton* mCancel;
    QPushButton* mOpen;
    QProgressBar* mProgress;
    QLabel* mStatus;
    QTimer* mWatchdog;
    QTimer* mProgressTimer;
    QTimer* mProbeTimer;
};
