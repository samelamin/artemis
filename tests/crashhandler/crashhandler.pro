QT += core testlib
CONFIG += console c++11 testcase
CONFIG -= app_bundle

linux-g++* {
    message(Crash handler test enabled)

    DEFINES += VERSION_STR=\\\"0.6.7-test\\\"
    DEFINES += VIBERTEMIS_BUILD_COMMIT=\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\"

    TEMPLATE = app
    TARGET = tst_crashhandler

    INCLUDEPATH += $$PWD/../../app

    SOURCES += \
        tst_crashhandler.cpp \
        $$PWD/../../app/backend/crashhandler.cpp \
        $$PWD/../../app/backend/crashringbuffer.cpp \
        $$PWD/../../app/path.cpp

    HEADERS += \
        $$PWD/../../app/backend/crashhandler.h \
        $$PWD/../../app/backend/crashringbuffer.h \
        $$PWD/../../app/path.h

    target.path = /app/libexec
    INSTALLS += target
}
