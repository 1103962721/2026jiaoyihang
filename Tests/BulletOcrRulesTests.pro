TEMPLATE = app
TARGET = BulletOcrRulesTests
QT = core
CONFIG += console c++17
CONFIG -= app_bundle
SOURCES += BulletOcrRulesTests.cpp ../DF_PlayClick_Client_Qt/BulletOcrRules.cpp
HEADERS += ../DF_PlayClick_Client_Qt/BulletOcrRules.h
