QT += core network testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

DEFINES += VERSION_STR=\\\"0.6.7\\\"
DEFINES += VIBERTEMIS_BUILD_COMMIT=\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\"
DEFINES += VBT_DIAG_ENDPOINT=\\\"http://127.0.0.1:1/v1/report\\\"

TEMPLATE = app
TARGET = tst_diagnosticreporter

INCLUDEPATH += $$PWD/../../app

SOURCES += \
    tst_diagnosticreporter.cpp \
    $$PWD/../../app/backend/diagnosticreporter.cpp \
    $$PWD/../../app/backend/logscrubber.cpp

HEADERS += \
    $$PWD/../../app/backend/diagnosticreporter.h \
    $$PWD/../../app/backend/logscrubber.h

target.path = /app/libexec
INSTALLS += target