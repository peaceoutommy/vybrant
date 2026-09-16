# MusicThingy DAW - qmake project (Linux native)
TEMPLATE = app
TARGET = musicthingy
QT += core gui widgets
CONFIG += c++20 link_pkgconfig
CONFIG -= debug_and_release
QMAKE_CXXFLAGS += -O2 -Wall -Wno-unused-parameter -Wno-unused-function -fno-strict-aliasing
QMAKE_LFLAGS += -rdynamic
PKGCONFIG += portaudio-2.0 alsa sndfile samplerate rubberband lame fftw3f
LIBS += -ldl -lpthread -lX11
DEFINES += QT_NO_KEYWORDS_NOT_USED
INCLUDEPATH += src
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
DESTDIR = bin

SOURCES += $$files(src/*.cpp, true)
HEADERS += $$files(src/*.h, true)

# `make install` puts the binary and a launcher into the user's home (no root needed)
target.path = $$(HOME)/.local/bin
desktop.files = resources/musicthingy.desktop
desktop.path = $$(HOME)/.local/share/applications
INSTALLS += target desktop
