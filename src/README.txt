Running it: from the project folder

./bin/vybrant

To rebuild after changes: qmake6 Vybrant.pro && make -j$(nproc). If you want it available everywhere (menu launcher + vybrant on the command line without the path), install it into your home directory — no root needed:

make install

That copies the binary to ~/.local/bin/vybrant and adds a desktop entry (~/.local/share/applications/vybrant.desktop), so it shows up in your app launcher. You can also open a project directly with vybrant song.mtp.
