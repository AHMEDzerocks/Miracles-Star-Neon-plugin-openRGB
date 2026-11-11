QT += core gui widgets bluetooth
TEMPLATE = lib
CONFIG += plugin c++17
TARGET = MiraclesStarNeon

OPENRGB_ROOT = D:/OpenRGB-Source
INCLUDEPATH += $$OPENRGB_ROOT $$OPENRGB_ROOT/RGBController $$OPENRGB_ROOT/dependencies/json

SOURCES += MiraclesStarNeonPlugin.cpp \
           NeonController.cpp

HEADERS += MiraclesStarNeonPlugin.h \
           NeonController.h

DEFINES += MIRACLESSTARNEON_LIBRARY _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN

DESTDIR = build
CONFIG(release, debug|release): DESTDIR = build/release
CONFIG(debug, debug|release): DESTDIR = build/debug
