# The offscreen harness: bin/harness builds it into build/harness.
QT += core gui qml quick quickcontrols2 dbus testlib
CONFIG += c++17
TARGET = harness
TEMPLATE = app

SRC = ../../src
INCLUDEPATH += $$SRC
HEADERS += $$SRC/backend.h $$SRC/blockeddeletes.h $$SRC/clockformat.h $$SRC/conflictcache.h $$SRC/controlcommands.h $$SRC/fixcommand.h $$SRC/fixstate.h \
           $$SRC/listingdiff.h $$SRC/notifier.h \
           $$SRC/pausestate.h $$SRC/pluginsetup.h $$SRC/snapshot.h $$SRC/syncconfig.h $$SRC/synclog.h \
           $$SRC/takeoversetup.h $$SRC/theme.h $$SRC/translator.h
SOURCES += harness.cpp $$SRC/backend.cpp $$SRC/blockeddeletes.cpp $$SRC/clockformat.cpp $$SRC/conflictcache.cpp $$SRC/controlcommands.cpp $$SRC/fixcommand.cpp $$SRC/fixstate.cpp \
           $$SRC/listingdiff.cpp $$SRC/notifier.cpp \
           $$SRC/pausestate.cpp $$SRC/pluginsetup.cpp $$SRC/snapshot.cpp $$SRC/syncconfig.cpp $$SRC/synclog.cpp \
           $$SRC/takeoversetup.cpp $$SRC/theme.cpp $$SRC/translator.cpp
RESOURCES += $$SRC/resources.qrc
