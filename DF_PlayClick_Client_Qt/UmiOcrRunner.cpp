#include "UmiOcrRunner.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QThread>

#include <mutex>

#include "../Shared/SocketUtil.h"

namespace
{
constexpr int OcrReadyWaitRounds = 20;
constexpr int OcrReadyWaitMs = 250;
// OCR 请求在后台线程执行；较短的网络超时可以避免停止监测时长时间占用 OCR 调度。
constexpr int OcrHttpTimeoutMs = 5000;
std::mutex g_ocrRequestMutex;

bool ConnectLocalSocket(SOCKET& socketHandle, int port)
{
    socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        return false;
    }

    const DWORD timeoutMs = OcrHttpTimeoutMs;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socketHandle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
        return false;
    }

    return true;
}

QString NormalizeOcrMatchText(const QString& value)
{
    QString normalized;
    normalized.reserve(value.size());
    for (const QChar character : value)
    {
        if (character.isSpace())
        {
            continue;
        }

        // Umi-OCR 常把“霰”识别成“霞”或“散”，这些字符在物品名称中按同义误识别处理。
        if (character == QChar(u'霞') || character == QChar(u'散'))
        {
            normalized.append(QChar(u'霰'));
        }
        else
        {
            normalized.append(character);
        }
    }
    return normalized.toCaseFolded();
}
}

bool UmiOcrRunner::requestOcr(const QString& imagePath, const QString& executablePath, int port, const QString& dataFormat,
    QJsonObject& responseObject, QString& errorText) const
{
    // UmiOCR 本地接口在并发截图请求时会明显增加延迟，登录成功和封禁检测统一排队。
    std::unique_lock<std::mutex> requestLock(g_ocrRequestMutex);
    errorText.clear();
    QFile imageFile(imagePath);
    if (!imageFile.open(QIODevice::ReadOnly))
    {
        errorText = QStringLiteral("无法读取OCR截图文件");
        return false;
    }

    if (!ensureServerRunning(executablePath, port, errorText))
    {
        return false;
    }

    const QByteArray imageBase64 = imageFile.readAll().toBase64();
    QJsonObject options;
    options.insert(QStringLiteral("ocr.angle"), false);
    options.insert(QStringLiteral("ocr.language"), QStringLiteral("models/config_chinese.txt"));
    // 保持当前 OCR 截图的原始尺寸，避免 UmiOCR 缩放后返回的文字框坐标
    // 与客户端截图坐标不一致，导致后续点击偏移到分隔线或空白区域。
    options.insert(QStringLiteral("ocr.maxSideLen"), 2048);
    options.insert(QStringLiteral("ocr.cpuThreads"), 12);
    options.insert(QStringLiteral("tbpu.parser"), QStringLiteral("single_line"));
    options.insert(QStringLiteral("data.format"), dataFormat);

    QJsonObject request;
    request.insert(QStringLiteral("base64"), QString::fromLatin1(imageBase64));
    request.insert(QStringLiteral("options"), options);
    const QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);

    SocketUtil::WsaSession session;
    if (!session.Ok())
    {
        errorText = QStringLiteral("Winsock 初始化失败");
        return false;
    }

    SOCKET socketHandle = INVALID_SOCKET;
    if (!ConnectLocalSocket(socketHandle, port))
    {
        errorText = QStringLiteral("连接 UmiOCR 本地接口失败");
        return false;
    }

    const QByteArray header = "POST /api/ocr HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    const QByteArray requestBytes = header + body;
    if (!SocketUtil::SendAll(socketHandle, requestBytes.constData(), requestBytes.size()))
    {
        closesocket(socketHandle);
        errorText = QStringLiteral("发送 UmiOCR 请求失败");
        return false;
    }

    shutdown(socketHandle, SD_SEND);
    QByteArray response;
    char buffer[4096]{};
    while (true)
    {
        const int received = recv(socketHandle, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            closesocket(socketHandle);
            errorText = QStringLiteral("读取 UmiOCR 响应失败");
            return false;
        }
        response.append(buffer, received);
    }
    closesocket(socketHandle);

    const int bodyIndex = response.indexOf("\r\n\r\n");
    if (bodyIndex < 0)
    {
        errorText = QStringLiteral("UmiOCR 返回格式异常");
        return false;
    }

    const QByteArray responseBody = response.mid(bodyIndex + 4);
    const QJsonDocument json = QJsonDocument::fromJson(responseBody);
    if (!json.isObject())
    {
        errorText = QStringLiteral("UmiOCR 返回 JSON 解析失败");
        return false;
    }

    responseObject = json.object();
    if (responseObject.value(QStringLiteral("code")).toInt() != 100)
    {
        errorText = QStringLiteral("UmiOCR 识别失败：%1").arg(responseObject.value(QStringLiteral("data")).toVariant().toString());
        return false;
    }
    return true;
}

QString UmiOcrRunner::recognizeImage(const QString& imagePath, const QString& executablePath, int port, QString& errorText) const
{
    QJsonObject responseObject;
    if (!requestOcr(imagePath, executablePath, port, QStringLiteral("text"), responseObject, errorText))
    {
        return {};
    }
    return responseObject.value(QStringLiteral("data")).toString();
}

bool UmiOcrRunner::findTextBounds(const QString& imagePath, const QString& executablePath, int port, const QString& keyword,
    QRect& bounds, QString& errorText, bool exactMatch, QString* recognizedText) const
{
    bounds = {};
    QJsonObject responseObject;
    if (!requestOcr(imagePath, executablePath, port, QStringLiteral("json"), responseObject, errorText))
    {
        return false;
    }

    const QJsonValue dataValue = responseObject.value(QStringLiteral("data"));
    if (!dataValue.isArray())
    {
        errorText = QStringLiteral("UmiOCR 未返回文字坐标");
        return false;
    }

    QStringList allTexts;
    const QString normalizedKeyword = NormalizeOcrMatchText(keyword);
    const QJsonArray blocks = dataValue.toArray();
    for (const QJsonValue blockValue : blocks)
    {
        const QJsonObject block = blockValue.toObject();
        const QString text = block.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty())
        {
            allTexts.append(text);
        }
        const QString normalizedText = NormalizeOcrMatchText(text);
        if (exactMatch ? normalizedText != normalizedKeyword : !normalizedText.contains(normalizedKeyword))
        {
            continue;
        }

        const QJsonArray box = block.value(QStringLiteral("box")).toArray();
        bool hasPoint = false;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        for (const QJsonValue pointValue : box)
        {
            const QJsonArray point = pointValue.toArray();
            if (point.size() < 2 || !point.at(0).isDouble() || !point.at(1).isDouble())
            {
                continue;
            }
            const int x = point.at(0).toInt();
            const int y = point.at(1).toInt();
            if (!hasPoint)
            {
                minX = maxX = x;
                minY = maxY = y;
                hasPoint = true;
            }
            else
            {
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
        if (hasPoint && maxX > minX && maxY > minY)
        {
            if (recognizedText)
            {
                *recognizedText = allTexts.join(QLatin1Char('\n'));
            }
            bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY)).normalized();
            return true;
        }
    }

    const QString recognizedTextValue = allTexts.join(QLatin1Char('\n'));
    if (recognizedText)
    {
        *recognizedText = recognizedTextValue;
    }
    errorText = recognizedTextValue.isEmpty()
        ? QStringLiteral("OCR未识别到文字，未找到“%1”").arg(keyword)
        : QStringLiteral("OCR未找到“%1”，原文：%2").arg(keyword, recognizedTextValue);
    return false;
}

bool UmiOcrRunner::findBulletTextBounds(const QString& imagePath, const QString& executablePath, int port,
    const QString& keyword, QRect& bounds, QString& errorText, QString* recognizedText) const
{
    bounds = {};
    QJsonObject responseObject;
    if (!requestOcr(imagePath, executablePath, port, QStringLiteral("json"), responseObject, errorText))
        return false;

    const QJsonValue dataValue = responseObject.value(QStringLiteral("data"));
    if (!dataValue.isArray())
    {
        errorText = QStringLiteral("UmiOCR 未返回文字坐标");
        return false;
    }

    const QString normalizedKeyword = NormalizeOcrMatchText(keyword);
    QStringList allTexts;
    const auto matches = [](QString expected, QString actual) {
        // 只允许明确的 OCR 差异：.45 的前导句点缺失，以及字母 I 被识别成数字 1。
        if (expected.startsWith(QLatin1Char('.')) && !actual.startsWith(QLatin1Char('.')))
            expected.remove(0, 1);
        if (expected.size() != actual.size())
            return false;
        for (int index = 0; index < expected.size(); ++index)
        {
            const QChar expectedChar = expected.at(index);
            const QChar actualChar = actual.at(index);
            if (expectedChar == actualChar)
                continue;
            if (expectedChar == QLatin1Char('i') && actualChar == QLatin1Char('1'))
                continue;
            return false;
        }
        return true;
    };

    for (const QJsonValue blockValue : dataValue.toArray())
    {
        const QJsonObject block = blockValue.toObject();
        const QString text = block.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty())
            allTexts.append(text);
        if (!matches(normalizedKeyword, NormalizeOcrMatchText(text)))
            continue;

        const QJsonArray box = block.value(QStringLiteral("box")).toArray();
        bool hasPoint = false;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        for (const QJsonValue pointValue : box)
        {
            const QJsonArray point = pointValue.toArray();
            if (point.size() < 2 || !point.at(0).isDouble() || !point.at(1).isDouble())
                continue;
            const int x = point.at(0).toInt();
            const int y = point.at(1).toInt();
            if (!hasPoint)
            {
                minX = maxX = x;
                minY = maxY = y;
                hasPoint = true;
            }
            else
            {
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
        if (hasPoint && maxX > minX && maxY > minY)
        {
            if (recognizedText)
                *recognizedText = allTexts.join(QLatin1Char('\n'));
            bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY)).normalized();
            return true;
        }
    }

    const QString recognizedTextValue = allTexts.join(QLatin1Char('\n'));
    if (recognizedText)
        *recognizedText = recognizedTextValue;
    errorText = recognizedTextValue.isEmpty()
        ? QStringLiteral("OCR未识别到文字，未找到“%1”").arg(keyword)
        : QStringLiteral("OCR未找到“%1”，原文：%2").arg(keyword, recognizedTextValue);
    return false;
}

bool UmiOcrRunner::ensureServerRunning(const QString& executablePath, int port, QString& errorText) const
{
    if (isServerReady(port))
    {
        return true;
    }

    const QFileInfo exeInfo(executablePath);
    if (!exeInfo.exists())
    {
        errorText = QStringLiteral("UmiOCR路径不存在：%1").arg(executablePath);
        return false;
    }

    // UmiOCR 本体负责启动本地 HTTP 接口；已运行时会复用同一个服务进程。
    if (!QProcess::startDetached(executablePath, QStringList(), exeInfo.absolutePath()))
    {
        errorText = QStringLiteral("启动 UmiOCR 失败");
        return false;
    }

    for (int round = 0; round < OcrReadyWaitRounds; ++round)
    {
        QThread::msleep(OcrReadyWaitMs);
        QCoreApplication::processEvents();
        if (isServerReady(port))
        {
            return true;
        }
    }

    errorText = QStringLiteral("等待 UmiOCR 本地接口超时");
    return false;
}

bool UmiOcrRunner::isServerReady(int port) const
{
    SocketUtil::WsaSession session;
    if (!session.Ok())
    {
        return false;
    }

    SOCKET socketHandle = INVALID_SOCKET;
    const bool connected = ConnectLocalSocket(socketHandle, port);
    if (socketHandle != INVALID_SOCKET)
    {
        closesocket(socketHandle);
    }
    return connected;
}
