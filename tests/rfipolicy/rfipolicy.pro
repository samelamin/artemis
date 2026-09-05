QT += core testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_rfipolicy

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    tst_rfipolicy.cpp

HEADERS += \
    ../../app/streaming/video/ffmpeg-renderers/rfipolicy.h

target.path = /app/libexec
INSTALLS += target
