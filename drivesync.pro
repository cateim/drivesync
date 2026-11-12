QT += core gui qml quick quickcontrols2 dbus

CONFIG += c++17 release
TARGET = drivesync
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/clockformat.h \
    src/conflictcache.h \
    src/controlcommands.h \
    src/legacymigration.h \
    src/notifier.h \
    src/pausestate.h \
    src/pluginsetup.h \
    src/snapshot.h \
    src/statuscommand.h \
    src/syncconfig.h \
    src/synclog.h \
    src/takeover.h \
    src/takeoversetup.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/clockformat.cpp \
    src/conflictcache.cpp \
    src/controlcommands.cpp \
    src/legacymigration.cpp \
    src/notifier.cpp \
    src/pausestate.cpp \
    src/pluginsetup.cpp \
    src/snapshot.cpp \
    src/statuscommand.cpp \
    src/syncconfig.cpp \
    src/synclog.cpp \
    src/takeover.cpp \
    src/takeoversetup.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
