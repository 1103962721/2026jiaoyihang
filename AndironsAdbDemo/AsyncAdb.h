#pragma once
#include <QProcess>
#include <QTimer>
#include <QRegularExpression>
#include <functional>

struct DemoAdbDevice
{
    QString serial, state, description;
    bool usb = false;
};
inline QList<DemoAdbDevice> parseDemoDevices(const QByteArray& output)
{
    QList<DemoAdbDevice> result;
    for (const auto& line : QString::fromUtf8(output).split(QLatin1Char('\n')))
    {
        const auto fields = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 2 || line.startsWith("List of devices") || line.startsWith('*')) continue;
        const bool usb = line.contains(QStringLiteral("usb:"))
            || (!fields[0].contains(QLatin1Char(':')) && !fields[0].startsWith("emulator-")
                && !fields[0].contains("._tcp"));
        result.append({fields[0], fields[1], line.trimmed(), usb});
    }
    return result;
}

// One request at a time. Preview skips busy ticks instead of building a queue.
class AsyncAdb : public QObject
{
public:
    using Completion = std::function<void(bool, QByteArray, QString)>;
    explicit AsyncAdb(QObject* parent = nullptr) : QObject(parent)
    {
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] {
            timedOut_ = true; process_.kill();
        });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(false);
        });
        connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { finish(code == 0 && status == QProcess::NormalExit); });
    }
    ~AsyncAdb() override { callback_ = {}; process_.kill(); process_.waitForFinished(1000); }
    bool busy() const { return process_.state() != QProcess::NotRunning || bool(callback_); }
    bool start(const QString& executable, const QStringList& args, Completion completion, int timeoutMs = 15000)
    {
        if (busy()) return false;
        callback_ = std::move(completion); timedOut_ = false;
        timer_.start(timeoutMs);
        process_.start(executable, args);
        return true;
    }
    void cancel() { callback_ = {}; timer_.stop(); process_.kill(); }
private:
    void finish(bool ok)
    {
        timer_.stop();
        const auto output = process_.readAllStandardOutput();
        QString error = QString::fromLocal8Bit(process_.readAllStandardError()).trimmed();
        if (timedOut_) error = QStringLiteral("ADB 请求超时，请检查设备连接");
        if (!ok && error.isEmpty()) error = process_.errorString();
        auto completion = std::move(callback_); callback_ = {};
        if (completion) completion(ok && !timedOut_, output, error);
    }
    QProcess process_;
    QTimer timer_;
    Completion callback_;
    bool timedOut_ = false;
};
