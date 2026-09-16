Running it: from the project folder

./bin/musicthingy

To rebuild after changes: qmake6 MusicThingy.pro && make -j$(nproc). If you want it available everywhere (menu launcher + musicthingy on the command line without the path), install it into your home directory — no root needed:

make install

That copies the binary to ~/.local/bin/musicthingy and adds a desktop entry (~/.local/share/applications/musicthingy.desktop), so it shows up in your app launcher. You can also open a project directly with musicthingy song.mtp.
