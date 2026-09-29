#include "AccountBanMonitor.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QDateTime>

#include <cwchar>

#include "UmiOcrRunner.h"
#include "../Shared/ClientBindingRules.h"

namespace
{
bool IsDeltaForceProcessRunning(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
    {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId);
    if (!process)
    {
        return false;
    }

    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(_countof(path));
    const bool nameRead = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    if (!nameRead || !running)
    {
        return false;
    }

    const wchar_t* separator = wcsrchr(path, L'\\');
    const wchar_t* processName = separator ? separator + 1 : path;
    return IsDeltaForceGameBodyProcessName(processName);
}

QImage CaptureClientArea(HWND hwnd)
{
    RECT clientRect{};
    if (!GetClientRect(hwnd, &clientRect))
    {
        return {};
    }

    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0)
    {
        return {};
    }

    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin))
    {
        return {};
    }

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = (screen && memory) ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (!screen || !memory || !bitmap)
    {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return {};
    }

    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);

    QImage image;
    if (copied)
    {
        image = QImage(width, height, QImage::Format_RGB32);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(memory, bitmap, 0, static_cast<UINT>(height), image.bits(), &info, DIB_RGB_COLORS) == 0)
        {
            image = {};
        }
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return image;
}

QString NormalizeBanText(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}]")));
    return text;
}

bool IsBanText(const QString& recognizedText)
{
    const QString text = NormalizeBanText(recognizedText);
    const bool abnormalEnvironment = text.contains(QStringLiteral("游戏环境及行为数据异常"), Qt::CaseInsensitive)
        || (text.contains(QStringLiteral("游戏环境"), Qt::CaseInsensitive)
            && text.contains(QStringLiteral("行为数据异常"), Qt::CaseInsensitive));
    const bool accountFrozen = text.contains(QStringLiteral("账号进行冻结"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("账号冻结"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("冻结"), Qt::CaseInsensitive);
    return abnormalEnvironment && accountFrozen;
}

bool IsStartupFailureText(const QString& recognizedText)
{
    const QString text = NormalizeBanText(recognizedText);
    return text.contains(QStringLiteral("启动器登录组件报错"), Qt::CaseInsensitive)
        && text.contains(QStringLiteral("游戏被强制关闭"), Qt::CaseInsensitive);
}
}

std::atomic_bool AccountBanMonitor::loginOcrInFlight{false};

AccountBanMonitor::AccountBanMonitor(QObject* parent)
    : QObject(parent)
{
}

AccountBanMonitor::~AccountBanMonitor()
{
    stop();
}

void AccountBanMonitor::start(HWND gameWindow, const QString& ocrExecutable, int ocrPort)
{
    stop();
    const StopToken token = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeToken_ = token;
    }
    running_ = true;
    worker_ = std::thread([this, gameWindow, ocrExecutable, ocrPort, token]() {
        run(gameWindow, ocrExecutable, ocrPort, token);
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            current = activeToken_ == token;
        }
        if (current)
        {
            running_ = false;
            if (!token->load()) emit monitorStopped();
        }
    });
}

void AccountBanMonitor::stop()
{
    StopToken token;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        token = activeToken_;
    }
    if (token)
    {
        token->store(true);
    }
    if (worker_.joinable())
    {
        worker_.detach();
    }
    running_ = false;
}

bool AccountBanMonitor::isRunning() const
{
    return running_.load();
}

void AccountBanMonitor::run(HWND gameWindow, QString ocrExecutable, int ocrPort, const StopToken& token)
{
    UmiOcrRunner ocrRunner;
    const QString tempPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("df_playclick_account_ban_%1.png").arg(reinterpret_cast<quintptr>(gameWindow)));

    while (!token->load() && (!IsWindow(gameWindow) || !IsDeltaForceProcessRunning(gameWindow)))
    {
        if (!IsWindow(gameWindow))
        {
            return;
        }
        QThread::msleep(250);
    }
    if (!token->load())
    {
        emit monitorStatus(QStringLiteral("封禁 OCR 已开始监测"));
    }

    while (!token->load() && IsDeltaForceProcessRunning(gameWindow))
    {
        // 登录成功 OCR 与封禁 OCR 共用 UmiOCR 服务，登录阶段避免并发请求导致界面和 OCR 服务拥塞。
        while (!token->load() && IsDeltaForceProcessRunning(gameWindow)
            && loginOcrInFlight.load())
        {
            QThread::msleep(100);
        }
        if (token->load() || !IsDeltaForceProcessRunning(gameWindow)) break;
        const QImage image = CaptureClientArea(gameWindow);
        if (!image.isNull() && image.save(tempPath, "PNG"))
        {
            QString errorText;
            const QString recognizedText = ocrRunner.recognizeImage(tempPath, ocrExecutable, ocrPort, errorText);
            QFile::remove(tempPath);
            if (!token->load() && IsStartupFailureText(recognizedText))
            {
                emit startupFailedDetected(recognizedText);
                return;
            }
            if (!token->load() && IsBanText(recognizedText))
            {
                emit bannedDetected(recognizedText);
                return;
            }
        }

        // 封禁监测是独立的低频后台任务，避免持续 OCR 占用机器资源。
        for (int elapsed = 0; elapsed < 3000 && !token->load(); elapsed += 100)
        {
            QThread::msleep(100);
        }
    }
    QFile::remove(tempPath);
}
