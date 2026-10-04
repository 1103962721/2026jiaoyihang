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
void AccountLoginSuccessMonitor::setStage(int stage) { stage_.store(qBound(0, stage, 7)); }
bool AccountLoginSuccessMonitor::isRunning() const { return running_.load(); }

void AccountLoginSuccessMonitor::run(HWND hwnd, QString exe, int port) {
    Q_UNUSED(exe); Q_UNUSED(port);
    emit monitorStatus(QStringLiteral("登录成功 OCR 已开始监测"));
    qint64 lastSpace = 0;
    qint64 tabStageStartedAt = 0;
    qint64 readyStageStartedAt = 0;
    qint64 continueStageStartedAt = 0;
    qint64 enterGameStageStartedAt = 0;
    int phaseStage = stage_.load();
    while (!stopRequested_ && gameProcessAlive(hwnd)) {
        const qint64 cycleStartedAt = QDateTime::currentMSecsSinceEpoch();
        const int requestedStage = stage_.load();
        if (requestedStage >= 0 && requestedStage <= 7) {
            if (requestedStage != phaseStage && requestedStage == 1) {
                tabStageStartedAt = cycleStartedAt;
            } else if (requestedStage != phaseStage && requestedStage == 4) {
                continueStageStartedAt = cycleStartedAt;
            } else if (requestedStage != 1) {
                tabStageStartedAt = 0;
            }
            if (requestedStage == 7 && requestedStage != phaseStage) {
                enterGameStageStartedAt = cycleStartedAt;
            } else if (requestedStage != 7) {
                enterGameStageStartedAt = 0;
            }
            if (requestedStage == 2 && requestedStage != phaseStage) {
                readyStageStartedAt = cycleStartedAt;
            } else if (requestedStage != 2) {
                readyStageStartedAt = 0;
            }
            if (requestedStage != 4 && requestedStage != 5) {
                continueStageStartedAt = 0;
            }
            phaseStage = requestedStage;
        }
        if (phaseStage == 1 && tabStageStartedAt == 0) {
            tabStageStartedAt = cycleStartedAt;
        }
        if ((phaseStage == 4 || phaseStage == 5) && continueStageStartedAt == 0) {
            continueStageStartedAt = cycleStartedAt;
        }
        if (phaseStage == 2 && readyStageStartedAt == 0) {
            readyStageStartedAt = cycleStartedAt;
        }
        if (phaseStage == 2 && cycleStartedAt - readyStageStartedAt >= 10000) {
            emit monitorStatus(QStringLiteral("10 秒内未识别到“行前备战”，返回常驻检测"));
            phaseStage = 0;
            stage_.store(phaseStage);
            readyStageStartedAt = 0;
            continue;
        }
        if (phaseStage == 0 && cycleStartedAt - lastSpace >= 5000) {
            lastSpace = cycleStartedAt;
            emit retrySpace();
        }
        // 常驻命中后先等待 5 秒，再开始“继续/参加/跳转”确认 OCR。
        // 每个阶段只发起一次 OCR，避免多个 OCR 请求并发。
        const bool continueWaiting = phaseStage == 4 && cycleStartedAt - continueStageStartedAt < 5000;
        if (phaseStage != 5 && phaseStage != 6 && !continueWaiting) {
            emit ocrRequested(phaseStage);
        }
        const int sleepSlices = phaseStage == 5 ? 2 : 20;
        for (int i = 0; i < sleepSlices && !stopRequested_; ++i) {
            QThread::msleep(100);
        }
        if (stopRequested_ || !gameProcessAlive(hwnd)) break;

        const qint64 cycleFinishedAt = QDateTime::currentMSecsSinceEpoch();
        const int changedStage = stage_.load();
        if (changedStage >= 0 && changedStage <= 7 && changedStage != phaseStage) {
            phaseStage = changedStage;
            if (phaseStage == 1) {
                tabStageStartedAt = cycleFinishedAt;
            } else {
                tabStageStartedAt = 0;
            }
            if (phaseStage == 7) {
                enterGameStageStartedAt = cycleFinishedAt;
            } else {
                enterGameStageStartedAt = 0;
            }
            if (phaseStage == 2) {
                readyStageStartedAt = cycleFinishedAt;
            } else {
                readyStageStartedAt = 0;
            }
            if (phaseStage == 4 || phaseStage == 5) {
                continueStageStartedAt = cycleFinishedAt;
            } else {
                continueStageStartedAt = 0;
            }
            continue;
        }
        if (phaseStage == 5) {
            // 空格按下后等待 1.5 秒，再开始菜单识别。
            if (cycleFinishedAt - continueStageStartedAt >= 1500) {
                phaseStage = 1;
                stage_.store(phaseStage);
                tabStageStartedAt = cycleFinishedAt;
                continueStageStartedAt = 0;
            }
            continue;
        }
        if (phaseStage == 6) {
            // 菜单命中后由 UI 线程等待 2 秒再按 Tab；等待期间暂停 OCR。
            continue;
        }
        if (phaseStage == 7) {
            // “进入游戏”是可选提示：最多识别 5 秒，命中或超时都继续后续流程。
            if (enterGameStageStartedAt == 0) {
                enterGameStageStartedAt = cycleFinishedAt;
            }
            if (cycleFinishedAt - enterGameStageStartedAt >= 5000) {
                phaseStage = 4;
                stage_.store(phaseStage);
                continueStageStartedAt = cycleFinishedAt;
                enterGameStageStartedAt = 0;
            }
            continue;
        }
        if (phaseStage == 4) {
            // 等待 5 秒后最多识别 15 秒，超时仍然进入菜单识别。
            if (cycleFinishedAt - continueStageStartedAt >= 20000) {
                phaseStage = 1;
                stage_.store(phaseStage);
                tabStageStartedAt = cycleFinishedAt;
                continueStageStartedAt = 0;
            }
            continue;
        }
        if (phaseStage == 1) {
            if (cycleFinishedAt - tabStageStartedAt < 10000) {
                continue;
            }
            // Tab 阶段连续识别 10 秒仍未命中，才回到常驻阶段。
            phaseStage = 0;
            tabStageStartedAt = 0;
            stage_.store(phaseStage);
            continue;
        }
        // 阶段 3 曾用于封禁 OCR，封禁监测现已由 AccountBanMonitor 独立负责。
        // 常驻阶段未命中时必须保持阶段 0，继续 OCR 并执行空格保底，不能落入废弃阶段。
        stage_.store(phaseStage);
    }
    emit monitorFinished();
}
