#include <cassert>

#include "../DF_PlayClick_Server_Qt/AutoStartOcrRules.h"

int main()
{
    assert(AutoStartOcrRules::ContainsCancelActionMarker(QStringLiteral("取消行动")));
    assert(AutoStartOcrRules::ContainsCancelActionMarker(QStringLiteral("取消 行动")));
    assert(AutoStartOcrRules::ContainsCancelActionMarker(QStringLiteral("取 消 行 动")));
    assert(!AutoStartOcrRules::ContainsCancelActionMarker(QStringLiteral("取消")));
    assert(!AutoStartOcrRules::ContainsCancelActionMarker(QStringLiteral("出发")));

    assert(AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("配装")));
    assert(AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("配\n装")));
    assert(AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("出发")));
    assert(AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("出\t发")));
    assert(AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("取消行动")));
    assert(!AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("正在加载")));
    assert(!AutoStartOcrRules::ContainsActionRegionWaitingMarker(QStringLiteral("加载界面")));
    assert(!AutoStartOcrRules::ContainsActionRegionWaitingMarker(QString()));

    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("机密")));
    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("机\n密")));
    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("普通")));
    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("普 通")));
    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("绝密")));
    assert(AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("绝\t密")));
    assert(!AutoStartOcrRules::ContainsMapTypeMarker(QStringLiteral("匹配中")));

    return 0;
}
