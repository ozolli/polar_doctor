# Polar Doctor - Guide de compilation multi-plateformes

## Dépendances

Polar Doctor nécessite :
- GTK+ 3.0 ou supérieur
- SQLite3
- Bibliothèque mathématique (libm)
- Compilateur C (GCC, Clang, MinGW, MSVC)

---

## 🐧 Linux

### Installation des dépendances

**Debian/Ubuntu :**
```bash
sudo apt-get install build-essential libgtk-3-dev libsqlite3-dev
```

**Fedora/RHEL :**
```bash
sudo dnf install gcc gtk3-devel sqlite-devel
```

**Arch Linux :**
```bash
sudo pacman -S base-devel gtk3 sqlite
```

### Compilation
```bash
gcc -o polar_doctor polar_doctor.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lm -lsqlite3
```

### Installation (optionnel)
```bash
# Installer l'application
sudo cp polar_doctor /usr/local/bin/

# Installer l'icône
sudo cp polar_doctor.png /usr/share/pixmaps/

# Installer le fichier .desktop
sudo cp polar_doctor.desktop /usr/share/applications/
sudo update-desktop-database
```

---

## 🪟 Windows

### Méthode 1 : MSYS2/MinGW (Recommandée)

**Installation de MSYS2 :**
1. Télécharger depuis https://www.msys2.org/
2. Installer MSYS2

**Installation des dépendances dans MSYS2 :**
```bash
# Ouvrir MSYS2 MinGW 64-bit
pacman -S mingw-w64-x86_64-gcc \
          mingw-w64-x86_64-gtk3 \
          mingw-w64-x86_64-sqlite3 \
          mingw-w64-x86_64-pkg-config
```

**Compilation :**
```bash
gcc -o polar_doctor.exe polar_doctor.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lm -lsqlite3 \
    -mwindows
```

**Créer un package portable :**
```bash
# Créer un dossier pour l'application
mkdir polar_doctor_win
cp polar_doctor.exe polar_doctor_win/
cp polar_doctor.png polar_doctor_win/

# Copier les DLLs nécessaires
ldd polar_doctor.exe | grep mingw64 | awk '{print $3}' | xargs -I {} cp {} polar_doctor_win/

# Copier les données GTK
mkdir -p polar_doctor_win/share
cp -r /mingw64/share/glib-2.0 polar_doctor_win/share/
cp -r /mingw64/share/icons polar_doctor_win/share/
cp -r /mingw64/share/themes polar_doctor_win/share/
```

### Méthode 2 : Visual Studio

**Prérequis :**
- Visual Studio 2019 ou plus récent
- vcpkg pour les dépendances

**Installation des dépendances avec vcpkg :**
```cmd
vcpkg install gtk:x64-windows sqlite3:x64-windows
vcpkg integrate install
```

**Compilation :**
```cmd
cl /Fe:polar_doctor.exe polar_doctor.c ^
   /I"C:\vcpkg\installed\x64-windows\include" ^
   /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" ^
   gtk-3.lib sqlite3.lib
```

### Méthode 3 : Cross-compilation depuis Linux

```bash
# Installer MinGW cross-compiler
sudo apt-get install mingw-w64

# Télécharger GTK et SQLite pour Windows
# Depuis https://www.gtk.org/docs/installations/windows/

# Compiler
x86_64-w64-mingw32-gcc -o polar_doctor.exe polar_doctor.c \
    -I/path/to/gtk-win/include \
    -L/path/to/gtk-win/lib \
    -lgtk-3 -lgdk-3 -lglib-2.0 -lsqlite3 \
    -mwindows
```

---

## 🍎 macOS

### Installation des dépendances avec Homebrew

```bash
# Installer Homebrew si nécessaire
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Installer les dépendances
brew install gtk+3 sqlite3 pkg-config
```

### Compilation
```bash
gcc -o polar_doctor polar_doctor.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lsqlite3
```

### Créer un bundle macOS (optionnel)
```bash
# Créer la structure du bundle
mkdir -p PolarDoctor.app/Contents/{MacOS,Resources}

# Copier l'exécutable
cp polar_doctor PolarDoctor.app/Contents/MacOS/

# Copier l'icône (convertir en .icns)
cp polar_doctor.png PolarDoctor.app/Contents/Resources/

# Créer Info.plist
cat > PolarDoctor.app/Contents/Info.plist << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>polar_doctor</string>
    <key>CFBundleIconFile</key>
    <string>polar_doctor</string>
    <key>CFBundleIdentifier</key>
    <string>com.polardoctor.app</string>
    <key>CFBundleName</key>
    <string>Polar Doctor</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
</dict>
</plist>
EOF
```

---

## 🐳 Docker (Multi-plateforme)

### Créer un Dockerfile

```dockerfile
FROM ubuntu:22.04

# Installer les dépendances
RUN apt-get update && apt-get install -y \
    build-essential \
    libgtk-3-dev \
    libsqlite3-dev \
    pkg-config

# Copier les sources
COPY polar_doctor.c /app/
COPY polar_doctor.png /app/
WORKDIR /app

# Compiler
RUN gcc -o polar_doctor polar_doctor.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lm -lsqlite3

# Lancer l'application
CMD ["./polar_doctor"]
```

### Builder et exécuter
```bash
docker build -t polar_doctor .
docker run -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix polar_doctor
```

---

## 📦 Makefile universel

Créer un `Makefile` pour faciliter la compilation :

```makefile
# Polar Doctor Makefile

# Détection automatique de la plateforme
UNAME_S := $(shell uname -s)

# Variables communes
CC = gcc
TARGET = polar_doctor
SRC = polar_doctor.c
LIBS = -lm -lsqlite3

# Configuration selon la plateforme
ifeq ($(UNAME_S),Linux)
    CFLAGS = `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS)
endif

ifeq ($(UNAME_S),Darwin)
    CFLAGS = `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS)
endif

ifeq ($(OS),Windows_NT)
    TARGET = polar_doctor.exe
    CFLAGS = `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS) -mwindows
endif

# Règles de compilation
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -o $(TARGET) $(SRC) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET).exe

install:
ifeq ($(UNAME_S),Linux)
	cp $(TARGET) /usr/local/bin/
	cp polar_doctor.png /usr/share/pixmaps/
	cp polar_doctor.desktop /usr/share/applications/
	update-desktop-database
endif

.PHONY: all clean install
```

**Usage :**
```bash
make              # Compiler
make clean        # Nettoyer
make install      # Installer (Linux uniquement)
```

---

## 🚀 CMake (Recommandé pour projets complexes)

### Créer CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(PolarDoctor C)

set(CMAKE_C_STANDARD 11)

# Trouver les dépendances
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
find_package(SQLite3 REQUIRED)

# Inclure les répertoires
include_directories(${GTK3_INCLUDE_DIRS})
link_directories(${GTK3_LIBRARY_DIRS})

# Définir les flags
add_definitions(${GTK3_CFLAGS_OTHER})

# Créer l'exécutable
add_executable(polar_doctor polar_doctor.c)

# Lier les bibliothèques
target_link_libraries(polar_doctor
    ${GTK3_LIBRARIES}
    ${SQLite3_LIBRARIES}
    m
)

# Installation
install(TARGETS polar_doctor DESTINATION bin)
install(FILES polar_doctor.png DESTINATION share/pixmaps)
install(FILES polar_doctor.desktop DESTINATION share/applications)
```

**Compiler avec CMake :**
```bash
mkdir build
cd build
cmake ..
make
sudo make install
```

---

## 📋 Résumé rapide

| Plateforme | Commande rapide |
|------------|-----------------|
| **Linux** | `gcc -o polar_doctor polar_doctor.c $(pkg-config --cflags --libs gtk+-3.0) -lm -lsqlite3` |
| **Windows (MSYS2)** | `gcc -o polar_doctor.exe polar_doctor.c $(pkg-config --cflags --libs gtk+-3.0) -lm -lsqlite3 -mwindows` |
| **macOS** | `gcc -o polar_doctor polar_doctor.c $(pkg-config --cflags --libs gtk+-3.0) -lsqlite3` |

---

## 🐛 Dépannage

### Linux : "gtk+-3.0 not found"
```bash
sudo apt-get install libgtk-3-dev pkg-config
```

### Windows : DLLs manquantes
- Copier toutes les DLLs depuis `/mingw64/bin/` vers le dossier de l'exécutable
- Utiliser `ldd polar_doctor.exe` pour lister les dépendances

### macOS : "library not found"
```bash
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
```

---

## 📦 Distribution

### Créer des archives pour distribution

**Linux :**
```bash
tar -czf polar_doctor-linux-x64.tar.gz polar_doctor polar_doctor.png polar_doctor.desktop
```

**Windows :**
```bash
zip -r polar_doctor-windows-x64.zip polar_doctor_win/
```

**macOS :**
```bash
zip -r polar_doctor-macos.zip PolarDoctor.app
```
