TEMPLATE = app
TARGET = DF_PlayClick_Client_Qt
QT += widgets
CONFIG += c++17
DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
INCLUDEPATH += ../Shared
LIBS += -lWs2_32 -lUser32 -lGdi32 -lAdvapi32 -lShell32 -luiautomationcore -lole32

win32:RC_ICONS = AppIcon.ico

RESOURCES += AppIcon.qrc

SOURCES += \
    ../Shared/FramelessTitleBar.cpp \
    main.cpp \
    MainWindow.cpp \
    ClientController.cpp \
    ClientConnection.cpp \
    AccountLoginAutomation.cpp \
    AccountBanMonitor.cpp \
    AccountLoginSuccessMonitor.cpp \
    Win32WindowBinder.cpp \
    Win32InputDispatcher.cpp \
    HotkeyManager.cpp \
    ClientConfig.cpp \
    SponsorDialog.cpp \
    TeamReadyParser.cpp \
    UmiOcrRunner.cpp \
    OcrNumberParser.cpp \
    BulletOcrRules.cpp

HEADERS += \
    ../Shared/FramelessTitleBar.h \
    MainWindow.h \
    ClientController.h \
    ClientConnection.h \
    AccountLoginAutomation.h \
    AccountBanMonitor.h \
    AccountLoginSuccessMonitor.h \
    Win32WindowBinder.h \
    Win32InputDispatcher.h \
    HotkeyManager.h \
    ClientConfig.h \
    SponsorDialog.h \
    TeamReadyParser.h \
    UmiOcrRunner.h \
    OcrNumberParser.h \
    BulletOcrRules.h
