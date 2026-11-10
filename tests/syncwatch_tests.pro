QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = syncwatch_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/conflictcache.h ../src/controlcommands.h ../src/notifier.h ../src/pausestate.h ../src/pluginsetup.h ../src/snapshot.h ../src/statuscommand.h ../src/syncconfig.h ../src/synclog.h ../src/takeover.h ../src/takeoversetup.h ../src/theme.h ../src/translator.h
SOURCES += syncwatch_tests.cpp ../src/backend.cpp ../src/conflictcache.cpp ../src/controlcommands.cpp ../src/notifier.cpp ../src/pausestate.cpp ../src/pluginsetup.cpp ../src/snapshot.cpp ../src/statuscommand.cpp ../src/syncconfig.cpp ../src/synclog.cpp ../src/takeover.cpp ../src/takeoversetup.cpp ../src/theme.cpp ../src/translator.cpp

# The tests read the shipped catalogs straight from the source tree.
DEFINES += SRCDIR=\\\"$$PWD\\\"
