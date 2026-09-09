TEMPLATE = app
TARGET = OcrNumberParserTests
QT = core
CONFIG += console c++17
CONFIG -= app_bundle
SOURCES += OcrNumberParserTests.cpp ../DF_PlayClick_Client_Qt/OcrNumberParser.cpp
HEADERS += ../DF_PlayClick_Client_Qt/OcrNumberParser.h
