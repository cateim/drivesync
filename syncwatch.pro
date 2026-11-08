QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = syncwatch
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/conflictcache.h \
    src/pluginsetup.h \
    src/snapshot.h \
    src/statuscommand.h \
    src/synclog.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/conflictcache.cpp \
    src/pluginsetup.cpp \
    src/snapshot.cpp \
    src/statuscommand.cpp \
    src/synclog.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
