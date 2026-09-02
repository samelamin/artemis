QT += core testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_virtualdisplay

INCLUDEPATH += $$PWD/../../app

DEFINES += SESSION_CPP_SOURCE_PATH=\"\\\"$$absolute_path(../../app/streaming/session.cpp)\\\"\"

SOURCES += \
    tst_virtualdisplay.cpp \
    ../../app/streaming/virtualdisplaylaunch.cpp

HEADERS += \
    ../../app/streaming/virtualdisplaylaunch.h

target.path = /app/libexec
INSTALLS += target
