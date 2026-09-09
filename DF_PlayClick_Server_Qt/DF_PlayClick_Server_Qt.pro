TEMPLATE = app
TARGET = DF_PlayClick_Server_Qt
QT += widgets
CONFIG += c++17
DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
INCLUDEPATH += ../Shared ../DF_PlayClick_Server
LIBS += -lWs2_32 -lUser32 -lGdi32 -lAdvapi32 -lShell32

win32:RC_ICONS = AppIcon.ico

RESOURCES += AppIcon.qrc

SOURCES += \
    ../Shared/FramelessTitleBar.cpp \
    main.cpp \
    MainWindow.cpp \
    ServerController.cpp \
    ServerConnectionManager.cpp \
    SponsorDialog.cpp

HEADERS += \
    ../Shared/FramelessTitleBar.h \
    MainWindow.h \
    ServerController.h \
    ServerConnectionManager.h \
    ServerModels.h \
    SponsorDialog.h
