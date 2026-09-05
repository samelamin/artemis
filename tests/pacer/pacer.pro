QT += core qml testlib
CONFIG += console c++11 testcase link_pkgconfig
CONFIG -= app_bundle
PKGCONFIG += sdl2 libavcodec libavutil

INCLUDEPATH += \
    $$PWD/../../app \
    $$PWD/../../moonlight-common-c/moonlight-common-c/src

TEMPLATE = app
TARGET = tst_pacer

SOURCES += \
    tst_pacer.cpp \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp

HEADERS += \
    $$PWD/../../app/streaming/video/ffmpeg-renderers/pacer/pacer.h

target.path = /app/libexec
INSTALLS += target
