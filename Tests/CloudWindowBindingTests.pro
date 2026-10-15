TEMPLATE = app
TARGET = CloudWindowBindingTests
QT += widgets testlib
CONFIG += console c++17
DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
LIBS += -lUser32 -lGdi32
SOURCES += CloudWindowBindingTests.cpp ../DF_PlayClick_Client_Qt/CloudWindowPicker.cpp ../DF_PlayClick_Client_Qt/Win32WindowBinder.cpp ../DF_PlayClick_Client_Qt/BulletOcrRules.cpp
HEADERS += ../DF_PlayClick_Client_Qt/CloudWindowPicker.h
