# Vybrant DAW - qmake project (Linux native)
TEMPLATE = app
TARGET = vybrant
QT += core gui widgets network
CONFIG += c++20 link_pkgconfig
CONFIG -= debug_and_release
QMAKE_CXXFLAGS += -O2 -Wall -Wno-unused-parameter -Wno-unused-function -fno-strict-aliasing
QMAKE_LFLAGS += -rdynamic
PKGCONFIG += portaudio-2.0 alsa sndfile samplerate lame jack lilv-0 libcrypto
LIBS += -ldl -lpthread -lX11
DEFINES += QT_NO_KEYWORDS_NOT_USED
# qmake6 Vybrant.pro VYBRANT_VERSION=0.1.4 (the release workflow, from the tag): without it a development build, which never updates
!isEmpty(VYBRANT_VERSION): DEFINES += VYBRANT_VERSION=\\\"$$VYBRANT_VERSION\\\"
INCLUDEPATH += src third_party
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
DESTDIR = bin

SOURCES += $$files(src/*.cpp, true)
SOURCES -= src/midi/MidiEngineWin.cpp
HEADERS += $$files(src/*.h, true)
RESOURCES += resources/app.qrc

# `make install` puts the binary and a launcher into the user's home (no root needed)
target.path = $$(HOME)/.local/bin
# the launcher gets the full path: not every desktop's launcher has ~/.local/bin on its PATH
desktop.path = $$(HOME)/.local/share/applications
desktop.extra = sed \"s|^Exec=vybrant|Exec=$$(HOME)/.local/bin/vybrant|\" $$PWD/resources/vybrant.desktop > $(INSTALL_ROOT)$$desktop.path/vybrant.desktop
icon.files = resources/vybrant.png
icon.path = $$(HOME)/.local/share/icons/hicolor/512x512/apps
INSTALLS += target desktop icon
