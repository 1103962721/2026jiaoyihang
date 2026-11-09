#pragma once

#include <QPoint>
#include <QString>
#include <windows.h>

namespace WindowBackgroundInput
{
enum class Mode { Post, SendTimeout };

bool click(HWND hwnd, QPoint clientPoint, Mode mode, QString& error);
bool key(HWND hwnd, WORD virtualKey, Mode mode, QString& error);
bool text(HWND hwnd, const QString& value, Mode mode, QString& error);
}
