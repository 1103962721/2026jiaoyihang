#pragma once

#include <QString>

struct TeamReadyResult
{
    int state = -1;
    int current = 0;
    int total = 0;
    QString displayText = QStringLiteral("未识别");
};

class TeamReadyParser
{
public:
    TeamReadyResult parse(const QString& text) const;
};
