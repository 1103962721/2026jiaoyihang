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
    stop();
    stopRequested_ = false;
    stage_ = 0;
    running_ = true;
    worker_ = std::thread([this, hwnd, exe, port] { run(hwnd, exe, port); running_ = false; });
}
void AccountLoginSuccessMonitor::stop() {
    stopRequested_ = true;
    if (worker_.joinable()) worker_.join();
    running_ = false;
}
void AccountLoginSuccessMonitor::setStage(int stage) { stage_.store(qBound(0, stage, 3)); }
bool AccountLoginSuccessMonitor::isRunning() const { return running_.load(); }

void AccountLoginSuccessMonitor::run(HWND hwnd, QString exe, int port) {
    Q_UNUSED(exe); Q_UNUSED(port);
    emit monitorStatus(QStringLiteral("登录成功 OCR 已开始监测"));
    qint64 lastSpace = 0;
    int phaseStage = stage_.load();
    while (!stopRequested_ && gameProcessAlive(hwnd)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const int requestedStage = stage_.load();
        if (requestedStage >= 0 && requestedStage <= 3) {
            phaseStage = requestedStage;
        }
        if (phaseStage == 0 && now - lastSpace >= 5000) {
            lastSpace = now;
            emit retrySpace();
        }
        // 每个阶段只发起一次 OCR，保持 2 秒后再切换，避免常驻和封禁 OCR 并发。
        emit ocrRequested(phaseStage);
        for (int i = 0; i < 20 && !stopRequested_; ++i) {
            QThread::msleep(100);
        }
        if (stopRequested_ || !gameProcessAlive(hwnd)) break;

        const int changedStage = stage_.load();
        if (changedStage >= 0 && changedStage <= 3 && changedStage != phaseStage) {
            phaseStage = changedStage;
            continue;
        }
        if (phaseStage != 3)
        {
            phaseStage = 3;
        }
        stage_.store(phaseStage);
    }
}
