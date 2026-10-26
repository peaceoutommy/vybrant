#!/bin/bash
# The portable Windows folder: Vybrant.exe, every DLL it needs from the MinGW prefix, and Qt's Windows platform plugin.
#   packaging/windows-dist.sh <Vybrant.exe> <MinGW prefix, e.g. /ucrt64> <output folder>
# Runs in MSYS2 (CI) and on Linux against a cross prefix (OBJDUMP=llvm-objdump). Zipping the folder is up to the caller.
set -euo pipefail
exe=$1 prefix=$2 out=$3
objdump=${OBJDUMP:-objdump}
rm -rf "$out"
mkdir -p "$out/platforms"
cp "$exe" "$out/Vybrant.exe"
cp "$prefix/share/qt6/plugins/platforms/qwindows.dll" "$out/platforms/"   # Fusion style and PNG are built into Qt; nothing else is loaded
# what they import, and what that imports, from the prefix (DLLs not found there are Windows' own)
todo=("$out/Vybrant.exe" "$out/platforms/qwindows.dll")
while [ ${#todo[@]} -gt 0 ]; do
    f=${todo[0]}
    todo=("${todo[@]:1}")
    for dll in $("$objdump" -p "$f" | sed -n 's/^\s*DLL Name: //p' | tr -d '\r'); do
        src=$(find "$prefix/bin" -maxdepth 1 -iname "$dll" -print -quit)
        [ -n "$src" ] && [ ! -e "$out/$(basename "$src")" ] || continue
        cp "$src" "$out/"
        todo+=("$out/$(basename "$src")")
    done
done
echo "$out: $(ls "$out" | wc -l) files, $(du -sh "$out" | cut -f1)"
