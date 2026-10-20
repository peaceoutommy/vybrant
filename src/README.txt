Running it: from the project folder

./bin/tactum

To rebuild after changes: qmake6 Tactum.pro && make -j$(nproc). If you want it available everywhere (menu launcher + tactum on the command line without the path), install it into your home directory — no root needed:

make install

That copies the binary to ~/.local/bin/tactum and adds a desktop entry (~/.local/share/applications/tactum.desktop), so it shows up in your app launcher. You can also open a project directly with tactum song.mtp.
