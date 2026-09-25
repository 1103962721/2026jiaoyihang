#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

#include "../Shared/ClientRole.h"

struct OcrRequestRegion
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int referenceWidth = 2560;
    int referenceHeight = 1440;
};

struct ClientListItem
{
    quint64 id = 0;
    QString ip;
    bool ready = false;
    QString remark;
    ClientRole role = ClientRoleUtil::DefaultRole;
    QString teamReadyText;
    int teamReadyState = -1;
    int teamReadyCurrent = 0;
    int teamReadyTotal = 0;
    qint64 teamReadyUpdatedAtMs = 0;
    QString teamCode;
    QString runningAction;
    QString listingStatus = QStringLiteral("待执行");
    int listingQuantityX = -1;
    QString currentAmount = QStringLiteral("-");
    QString expectedAmount = QStringLiteral("-");
    qint64 cumulativeIncome = 0;
    int lowPriceCount = 0;
    int highPriceCount = 0;
    int consecutiveHighPriceCount = 0;
    bool hasAmountBaseline = false;
    bool actionRunning = false;
    bool connected = true;
};

enum class ServerStatusTone
{
    Neutral,
    Success,
    Error
};
