TEMPLATE = app
TARGET = AndironsAdbDemo
QT += widgets
CONFIG += c++17 windows

SOURCES += main.cpp
HEADERS += WindowClientCapture.h
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
LIBS += -luser32 -lgdi32
