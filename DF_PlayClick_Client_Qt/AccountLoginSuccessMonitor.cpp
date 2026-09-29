#include "AccountLoginSuccessMonitor.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QStandardPaths>
#include <QThread>
#include <QDateTime>
#include <QRegularExpression>
#include <QScreen>
#include <QGuiApplication>
#include <QPixmap>

#include "UmiOcrRunner.h"
#include "Win32WindowBinder.h"

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
QImage capture(HWND hwnd, QRect region) {
    RECT cr{}; if (!GetClientRect(hwnd, &cr)) return {};
    const int cw = cr.right, ch = cr.bottom;
    region = QRect(region.x() * cw / 2560, region.y() * ch / 1440,
                   region.width() * cw / 2560, region.height() * ch / 1440);
    POINT p{region.x(), region.y()}; if (!ClientToScreen(hwnd, &p)) return {};
    QScreen* screen = QGuiApplication::primaryScreen();
    return screen ? screen->grabWindow(0, p.x, p.y, region.width(), region.height()).toImage() : QImage{};
}
}

AccountLoginSuccessMonitor::AccountLoginSuccessMonitor(QObject* parent) : QObject(parent) {}
AccountLoginSuccessMonitor::~AccountLoginSuccessMonitor() { stop(); }
void AccountLoginSuccessMonitor::start(HWND hwnd, const QString& exe, int port) {
    stop(); stopRequested_ = false; running_ = true;
    worker_ = std::thread([this, hwnd, exe, port] { run(hwnd, exe, port); running_ = false; });
}
void AccountLoginSuccessMonitor::stop() { stopRequested_ = true; if (worker_.joinable()) worker_.join(); running_ = false; }
bool AccountLoginSuccessMonitor::isRunning() const { return running_.load(); }

void AccountLoginSuccessMonitor::run(HWND hwnd, QString exe, int port) {
    UmiOcrRunner ocr;
    const QString temp = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("df_playclick_login_ready.png"));
    auto recognize = [&](QRect r, const QString& key) {
        const QImage image = capture(hwnd, r); if (image.isNull() || !image.save(temp, "PNG")) return false;
        QString error; const QString text = ocr.recognizeImage(temp, exe, port, error); QFile::remove(temp); return has(text, key);
    };
    emit monitorStatus(QStringLiteral("登录成功 OCR 已开始监测"));
    enum class Stage { Resident, Tab, Prepare } stage = Stage::Resident;
    qint64 lastSpace = 0, stageDeadline = 0;
    while (!stopRequested_ && gameProcessAlive(hwnd)) {
        if (stage == Stage::Resident) {
            if (recognize(QRect(262,144,520,60), QStringLiteral("常驻"))) {
                emit residentFound();
                emit monitorStatus(QStringLiteral("已识别常驻，进入游戏流程"));
                stage = Stage::Tab;
                stageDeadline = QDateTime::currentMSecsSinceEpoch() + 5000;
                continue;
            }
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastSpace >= 5000) { lastSpace = now; emit retrySpace(); emit monitorStatus(QStringLiteral("未识别常驻，已执行空格重试")); }
        } else if (stage == Stage::Tab) {
            recognize(QRect(1084,849,400,80), QStringLiteral("进入游戏"));
            if (recognize(QRect(994,1145,500,255), QStringLiteral("TAB开始游戏"))) {
                emit tabFound(); emit monitorStatus(QStringLiteral("已识别 TAB 开始游戏")); stage = Stage::Prepare; stageDeadline = QDateTime::currentMSecsSinceEpoch() + 5000; continue;
            }
            if (QDateTime::currentMSecsSinceEpoch() >= stageDeadline) stage = Stage::Resident;
        } else {
            if (recognize(QRect(1946,1262,500,56), QStringLiteral("行前备战"))) { emit loginReady(); break; }
            if (QDateTime::currentMSecsSinceEpoch() >= stageDeadline) stage = Stage::Tab;
        }
        for (int i=0; i<10 && !stopRequested_; ++i) QThread::msleep(500);
    }
    QFile::remove(temp);
}
