QT += core gui quick quickcontrols2 testlib dbus
CONFIG += c++17 testcase
TARGET = syncwatch_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/conflictcache.h ../src/pluginsetup.h ../src/snapshot.h ../src/statuscommand.h ../src/synclog.h ../src/theme.h ../src/translator.h
SOURCES += syncwatch_tests.cpp ../src/backend.cpp ../src/conflictcache.cpp ../src/pluginsetup.cpp ../src/snapshot.cpp ../src/statuscommand.cpp ../src/synclog.cpp ../src/theme.cpp ../src/translator.cpp

# The tests read the shipped catalogs straight from the source tree.
DEFINES += SRCDIR=\\\"$$PWD\\\"
