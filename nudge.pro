QT += core gui qml quick quickcontrols2 dbus sql network

CONFIG += c++20 release
TARGET = nudge
TEMPLATE = app

VERSION = $$cat($$PWD/VERSION)
DEFINES += NUDGE_VERSION=\\\"$$VERSION\\\"

# make follows neither VERSION nor a changed define on its own: a new VERSION
# reruns qmake, and recompiles main.cpp, which builds the version in.
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/VERSION
versioned.target = main.o
versioned.depends = $$PWD/VERSION
QMAKE_EXTRA_TARGETS += versioned

HEADERS += \
    src/alerts.h \
    src/backend.h \
    src/cli.h \
    src/dates.h \
    src/look.h \
    src/recurrence.h \
    src/remindermodel.h \
    src/sidebarmodel.h \
    src/store.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/alerts.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/dates.cpp \
    src/look.cpp \
    src/recurrence.cpp \
    src/remindermodel.cpp \
    src/sidebarmodel.cpp \
    src/store.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
