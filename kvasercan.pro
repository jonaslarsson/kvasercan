TARGET = qtkvasercanbus

QT = core serialbus
QT -= gui

TEMPLATE = lib
CONFIG += plugin
DESTDIR = $$(QTDIR)/plugins/canbus

HEADERS += \
    kvasercanbackend.h \
    kvasercan_symbols_p.h \
    kvasercanbackend_p.h

SOURCES += \
    main.cpp \
    kvasercanbackend.cpp

DISTFILES = plugin.json
