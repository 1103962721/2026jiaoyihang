#include "MainWindow.h"
#include "ClientConfig.h"

#include <windows.h>
#include <tlhelp32.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace
{
void showStartupMessage(const QString& text, bool error)
{
    QMessageBox messageBox;
    messageBox.setWindowTitle(QStringLiteral("提示"));
    messageBox.setText(text);
    messageBox.setIcon(error ? QMessageBox::Warning : QMessageBox::Information);
    messageBox.setMinimumWidth(360);
    messageBox.setStandardButtons(QMessageBox::Ok);
    messageBox.setButtonText(QMessageBox::Ok, QStringLiteral("确定"));
    if (QAbstractButton* button = messageBox.button(QMessageBox::Ok))
    {
        button->setMinimumWidth(96);
        button->setMinimumHeight(34);
    }
    messageBox.exec();
}

bool isRunningAsAdministrator()
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

bool isDesktopScale100Percent()
{
    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
    {
        return true;
    }

    const int dpiX = GetDeviceCaps(screenDc, LOGPIXELSX);
    ReleaseDC(nullptr, screenDc);
    return dpiX == 96;
}

bool isUmiOcrProcessRunning()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (QString::fromWCharArray(entry.szExeFile).compare(QStringLiteral("Umi-OCR.exe"), Qt::CaseInsensitive) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool ensureUmiOcrProcess(QString& errorText)
{
    if (isUmiOcrProcessRunning())
    {
        return true;
    }

    // 优先查找客户端 EXE 所在文件夹和当前工作文件夹，再使用已有 OCR 配置中的路径。
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    const QDir workingDir(QDir::currentPath());
    ClientConfig config;
    const QStringList candidates{
        applicationDir.filePath(QStringLiteral("Umi-OCR.exe")),
        workingDir.filePath(QStringLiteral("Umi-OCR.exe")),
        config.loadUmiOcrPath()
    };

    QString executablePath;
    for (const QString& candidate : candidates)
    {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile())
        {
            executablePath = info.absoluteFilePath();
            break;
        }
    }
    if (executablePath.isEmpty())
    {
        errorText = QStringLiteral("未找到 Umi-OCR.exe，请将其放在客户端文件夹中");
        return false;
    }

    const QFileInfo executableInfo(executablePath);
    if (!QProcess::startDetached(executableInfo.absoluteFilePath(), QStringList(), executableInfo.absolutePath()))
    {
        errorText = QStringLiteral("启动 Umi-OCR.exe 失败：%1").arg(executableInfo.absoluteFilePath());
        return false;
    }
    return true;
}
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/AppIcon.ico")));

    if (!isRunningAsAdministrator())
    {
        showStartupMessage(QStringLiteral("请以管理员身份运行客户端"), true);
        return 0;
    }

    if (!isDesktopScale100Percent())
    {
        showStartupMessage(QStringLiteral("请将 Windows 显示缩放设置为 100% 后再运行"), true);
        return 0;
    }

    QString umiOcrError;
    if (!ensureUmiOcrProcess(umiOcrError))
    {
        // OCR 进程缺失不阻止客户端启动，执行 OCR 时仍会按原有配置再次尝试启动。
        showStartupMessage(umiOcrError, true);
    }

    MainWindow window;
    window.show();
    return app.exec();
}
