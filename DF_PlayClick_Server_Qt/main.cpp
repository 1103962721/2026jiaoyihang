#include "MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QString>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/AppIcon.ico")));

    MainWindow window;
    window.show();
    return app.exec();
}
