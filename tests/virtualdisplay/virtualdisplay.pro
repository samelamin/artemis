QT += core testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

TEMPLATE = app
TARGET = tst_virtualdisplay

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    tst_virtualdisplay.cpp \
    ../../app/backend/steamdecksession.cpp \
    ../../app/settings/refreshrateparser.cpp \
    ../../app/streaming/virtualdisplaylaunch.cpp

HEADERS += \
    ../../app/backend/steamdecksession.h \
    ../../app/settings/refreshrateparser.h \
    ../../app/streaming/virtualdisplaylaunch.h

target.path = /app/libexec
INSTALLS += target
