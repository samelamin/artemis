QT += core testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_logscrubber

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    tst_logscrubber.cpp \
    $$PWD/../../app/backend/logscrubber.cpp

HEADERS += \
    $$PWD/../../app/backend/logscrubber.h

target.path = /app/libexec
INSTALLS += target
