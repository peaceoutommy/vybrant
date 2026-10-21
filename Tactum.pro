# Tactum DAW - qmake project (Linux native)
TEMPLATE = app
TARGET = tactum
QT += core gui widgets network dbus
CONFIG += c++20 link_pkgconfig
CONFIG -= debug_and_release
QMAKE_CXXFLAGS += -O2 -Wall -Wno-unused-parameter -Wno-unused-function -fno-strict-aliasing
QMAKE_LFLAGS += -rdynamic
PKGCONFIG += portaudio-2.0 alsa sndfile samplerate rubberband lame fftw3f jack lilv-0
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
# the launcher gets the full path: not every desktop's launcher has ~/.local/bin on its PATH
desktop.path = $$(HOME)/.local/share/applications
desktop.extra = sed \"s|^Exec=tactum|Exec=$$(HOME)/.local/bin/tactum|\" $$PWD/resources/tactum.desktop > $(INSTALL_ROOT)$$desktop.path/tactum.desktop
icon.files = resources/tactum.svg
icon.path = $$(HOME)/.local/share/icons/hicolor/scalable/apps
INSTALLS += target desktop icon
