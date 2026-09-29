#include "AccountLoginSuccessMonitor.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QThread>
#include <QDateTime>
#include <QRegularExpression>


namespace {
bool gameProcessAlive(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!p) return false;
    DWORD code = 0; const bool ok = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p); return ok;
}
QString normalize(QString s) { s.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}]"))); return s; }
bool has(const QString& text, const QString& keyword) { return normalize(text).contains(normalize(keyword), Qt::CaseInsensitive); }
}

AccountLoginSuccessMonitor::AccountLoginSuccessMonitor(QObject* parent) : QObject(parent) {}
AccountLoginSuccessMonitor::~AccountLoginSuccessMonitor() { stop(); }
void AccountLoginSuccessMonitor::start(HWND hwnd, const QString& exe, int port) {
    stop(); stopRequested_ = false; running_ = true;
    worker_ = std::thread([this, hwnd, exe, port] { run(hwnd, exe, port); running_ = false; });
}
void AccountLoginSuccessMonitor::stop() { stopRequested_ = true; if (worker_.joinable()) worker_.detach(); running_ = false; }
bool AccountLoginSuccessMonitor::isRunning() const { return running_.load(); }

void AccountLoginSuccessMonitor::run(HWND hwnd, QString exe, int port) {
    Q_UNUSED(exe); Q_UNUSED(port);
    emit monitorStatus(QStringLiteral("登录成功 OCR 已开始监测"));
    enum class Stage { Resident, Tab, Prepare } stage = Stage::Resident;
    qint64 lastSpace = 0;
    while (!stopRequested_ && gameProcessAlive(hwnd)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (stage == Stage::Resident && now - lastSpace >= 5000) { lastSpace = now; emit retrySpace(); }
        emit ocrRequested(static_cast<int>(stage));
        for (int i=0; i<50 && !stopRequested_; ++i) QThread::msleep(100);
    }
}
