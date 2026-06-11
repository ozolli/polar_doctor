# Polar Doctor - Makefile multi-plateformes

# Détection automatique de la plateforme
UNAME_S := $(shell uname -s)

# Variables communes
CC = gcc
TARGET = polar_doctor
SRC = $(wildcard *.c)
HDR = $(wildcard *.h)
LIBS = -lm -lsqlite3
CFLAGS_COMMON = -Wall -O2

# Configuration selon la plateforme
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    CFLAGS = $(CFLAGS_COMMON) `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS)
    INSTALL_PREFIX = /usr/local
endif

ifeq ($(UNAME_S),Darwin)
    PLATFORM = macos
    CFLAGS = $(CFLAGS_COMMON) `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS)
    INSTALL_PREFIX = /usr/local
endif

ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    TARGET = polar_doctor.exe
    CFLAGS = $(CFLAGS_COMMON) `pkg-config --cflags gtk+-3.0`
    LDFLAGS = `pkg-config --libs gtk+-3.0` $(LIBS) -mwindows
endif

# Règles de compilation
.PHONY: all clean install uninstall help dist

all: $(TARGET)
	@echo "✓ Compilation terminée pour $(PLATFORM)"
	@echo "✓ Exécutable: $(TARGET)"

$(TARGET): $(SRC) $(HDR)
	@echo "Compilation de Polar Doctor pour $(PLATFORM)..."
	$(CC) -o $(TARGET) $(SRC) $(CFLAGS) $(LDFLAGS)

clean:
	@echo "Nettoyage..."
	rm -f $(TARGET) polar_doctor.exe
	rm -rf build/ dist/ *.o
	@echo "✓ Nettoyage terminé"

# Installation (Linux/macOS uniquement)
install: $(TARGET)
ifeq ($(UNAME_S),Linux)
	@echo "Installation de Polar Doctor..."
	install -D -m 755 $(TARGET) $(INSTALL_PREFIX)/bin/$(TARGET)
	install -D -m 644 polar_doctor.png $(INSTALL_PREFIX)/share/pixmaps/polar_doctor.png
	install -D -m 644 polar_doctor.desktop $(INSTALL_PREFIX)/share/applications/polar_doctor.desktop
	update-desktop-database $(INSTALL_PREFIX)/share/applications/ 2>/dev/null || true
	@echo "✓ Installation terminée dans $(INSTALL_PREFIX)"
	@echo "  Lancez: polar_doctor"
else ifeq ($(UNAME_S),Darwin)
	@echo "Installation de Polar Doctor..."
	install -m 755 $(TARGET) $(INSTALL_PREFIX)/bin/$(TARGET)
	install -m 644 polar_doctor.png $(INSTALL_PREFIX)/share/pixmaps/polar_doctor.png
	@echo "✓ Installation terminée dans $(INSTALL_PREFIX)"
else
	@echo "Installation non supportée sur Windows"
	@echo "Utilisez 'make dist' pour créer un package portable"
endif

# Désinstallation (Linux/macOS uniquement)
uninstall:
ifeq ($(UNAME_S),Linux)
	@echo "Désinstallation de Polar Doctor..."
	rm -f $(INSTALL_PREFIX)/bin/$(TARGET)
	rm -f $(INSTALL_PREFIX)/share/pixmaps/polar_doctor.png
	rm -f $(INSTALL_PREFIX)/share/applications/polar_doctor.desktop
	update-desktop-database $(INSTALL_PREFIX)/share/applications/ 2>/dev/null || true
	@echo "✓ Désinstallation terminée"
else ifeq ($(UNAME_S),Darwin)
	@echo "Désinstallation de Polar Doctor..."
	rm -f $(INSTALL_PREFIX)/bin/$(TARGET)
	rm -f $(INSTALL_PREFIX)/share/pixmaps/polar_doctor.png
	@echo "✓ Désinstallation terminée"
else
	@echo "Désinstallation non applicable sur Windows"
endif

# Créer un package de distribution
dist: $(TARGET)
	@echo "Création du package de distribution pour $(PLATFORM)..."
	mkdir -p dist
ifeq ($(UNAME_S),Linux)
	tar -czf dist/polar_doctor-$(PLATFORM)-x64.tar.gz $(TARGET) polar_doctor.png polar_doctor.desktop polar_doctor.svg README.md BUILD.md
	@echo "✓ Package créé: dist/polar_doctor-$(PLATFORM)-x64.tar.gz"
else ifeq ($(UNAME_S),Darwin)
	tar -czf dist/polar_doctor-$(PLATFORM)-x64.tar.gz $(TARGET) polar_doctor.png polar_doctor.svg README.md BUILD.md
	@echo "✓ Package créé: dist/polar_doctor-$(PLATFORM)-x64.tar.gz"
else
	# Windows: créer un dossier avec toutes les DLLs
	mkdir -p dist/polar_doctor_win
	cp $(TARGET) polar_doctor.png polar_doctor.svg README.md BUILD.md dist/polar_doctor_win/
	@echo "Copie des DLLs nécessaires..."
	ldd $(TARGET) | grep mingw64 | awk '{print $$3}' | xargs -I {} cp {} dist/polar_doctor_win/ 2>/dev/null || true
	cd dist && zip -r polar_doctor-windows-x64.zip polar_doctor_win/
	@echo "✓ Package créé: dist/polar_doctor-windows-x64.zip"
endif

# Règle de test rapide
test: $(TARGET)
	@echo "Lancement de Polar Doctor en mode test..."
	./$(TARGET) &
	@sleep 3
	@pkill -f $(TARGET) || true
	@echo "✓ Test terminé"

# Aide
help:
	@echo "Polar Doctor - Makefile"
	@echo ""
	@echo "Utilisation:"
	@echo "  make              - Compiler Polar Doctor"
	@echo "  make clean        - Supprimer les fichiers compilés"
	@echo "  make install      - Installer sur le système (Linux/macOS, nécessite sudo)"
	@echo "  make uninstall    - Désinstaller (Linux/macOS, nécessite sudo)"
	@echo "  make dist         - Créer un package de distribution"
	@echo "  make test         - Compiler et tester rapidement"
	@echo "  make help         - Afficher cette aide"
	@echo ""
	@echo "Plateforme détectée: $(PLATFORM)"
	@echo "Compilateur: $(CC)"
	@echo ""
	@echo "Exemples:"
	@echo "  make && ./$(TARGET)     - Compiler et lancer"
	@echo "  sudo make install       - Installer globalement"
	@echo "  make dist               - Créer un package portable"
