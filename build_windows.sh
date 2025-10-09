#!/bin/bash
# Script de cross-compilation pour Windows depuis Linux
# Nécessite mingw-w64

set -e

echo "=== Polar Doctor - Cross-compilation pour Windows ==="
echo ""

# Vérifier que mingw-w64 est installé
if ! command -v x86_64-w64-mingw32-gcc &> /dev/null; then
    echo "❌ Erreur: mingw-w64 n'est pas installé"
    echo ""
    echo "Installation:"
    echo "  Debian/Ubuntu: sudo apt-get install mingw-w64"
    echo "  Fedora:        sudo dnf install mingw64-gcc"
    echo "  Arch:          sudo pacman -S mingw-w64-gcc"
    exit 1
fi

echo "✓ Compilateur MinGW-w64 trouvé"

# Variables
MINGW_PREFIX="/usr/x86_64-w64-mingw32"
TARGET="polar_doctor.exe"
SRC="polar_doctor.c"

# Vérifier les bibliothèques GTK pour Windows
if [ ! -d "gtk-win" ]; then
    echo ""
    echo "⚠️  Les bibliothèques GTK+ pour Windows ne sont pas trouvées"
    echo ""
    echo "Téléchargement recommandé:"
    echo "  1. Aller sur https://github.com/tschoonj/GTK-for-Windows-Runtime-Environment-Installer"
    echo "  2. Télécharger l'installeur"
    echo "  3. Extraire dans le dossier 'gtk-win'"
    echo ""
    echo "Ou utiliser MSYS2 directement sur Windows (méthode recommandée)"
    exit 1
fi

echo "✓ Bibliothèques GTK trouvées"

# Compilation
echo ""
echo "Compilation en cours..."

GTK_PATH="./gtk-win"

x86_64-w64-mingw32-gcc \
    -o "$TARGET" \
    "$SRC" \
    -I"$GTK_PATH/include/gtk-3.0" \
    -I"$GTK_PATH/include/glib-2.0" \
    -I"$GTK_PATH/lib/glib-2.0/include" \
    -I"$GTK_PATH/include/pango-1.0" \
    -I"$GTK_PATH/include/cairo" \
    -I"$GTK_PATH/include/gdk-pixbuf-2.0" \
    -I"$GTK_PATH/include/atk-1.0" \
    -L"$GTK_PATH/lib" \
    -lgtk-3 -lgdk-3 -lglib-2.0 -lgobject-2.0 \
    -lgio-2.0 -lpango-1.0 -lcairo -lgdk_pixbuf-2.0 \
    -lsqlite3 \
    -mwindows \
    -Wall -O2

if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Compilation réussie!"
    echo "✓ Exécutable créé: $TARGET"
    echo ""
    echo "Pour tester sur Windows:"
    echo "  1. Copier $TARGET vers Windows"
    echo "  2. Copier les DLLs depuis gtk-win/bin/"
    echo "  3. Lancer $TARGET"
else
    echo ""
    echo "❌ Erreur de compilation"
    exit 1
fi
