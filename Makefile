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
.PHONY: all web install-web uninstall-web clean install uninstall help dist

all: $(TARGET)
	@echo "✓ Compilation terminée pour $(PLATFORM)"
	@echo "✓ Exécutable: $(TARGET)"

$(TARGET): $(SRC) $(HDR)
	@echo "Compilation de Polar Doctor pour $(PLATFORM)..."
	$(CC) -o $(TARGET) $(SRC) $(CFLAGS) $(LDFLAGS)

# --- Interface web : serveur HTTP embarqué + SPA (diagramme, édition, live) ---
# Liée au cœur libpolar (polar_data/import/boat_config) : glib + sqlite seulement,
# ni GTK ni Cairo (vérifiable via ldd).
WEB_TARGET = polar_doctor_web
WEB_SRC    = web/server.c polar_data.c import.c boat_config.c libpolar.c
WEB_CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -O2 -I. `pkg-config --cflags glib-2.0`
WEB_LIBS   = `pkg-config --libs glib-2.0` -lsqlite3 -lm

web: $(WEB_TARGET)

$(WEB_TARGET): $(WEB_SRC) $(HDR)
	@echo "Compilation de polar_doctor_web (cœur libpolar, sans GTK)..."
	$(CC) -o $(WEB_TARGET) $(WEB_SRC) $(WEB_CFLAGS) $(WEB_LIBS)
	@echo "✓ Web: ./$(WEB_TARGET) [fichier.pol|dossier] --port 8081 --bind 0.0.0.0"

# --- Service systemd de l'interface web : make web && sudo make install-web ---
# Ne recompile PAS (sous sudo, le binaire du dépôt deviendrait propriété de root).
# Le service tourne sous l'utilisateur qui a lancé sudo (SERVICE_USER).
SERVICE_USER ?= $(SUDO_USER)
install-web:
	@[ -x $(WEB_TARGET) ] || { echo "Lancer d'abord (sans sudo) : make web"; exit 1; }
	@[ -n "$(SERVICE_USER)" ] || { echo "SERVICE_USER vide : lancer via sudo, ou SERVICE_USER=<utilisateur>"; exit 1; }
	install -Dm755 $(WEB_TARGET) $(DESTDIR)$(INSTALL_PREFIX)/bin/$(WEB_TARGET)
	install -d $(DESTDIR)/etc/systemd/system
	sed 's/@USER@/$(SERVICE_USER)/' web/polar_doctor_web.service > $(DESTDIR)/etc/systemd/system/polar_doctor_web.service
	chmod 644 $(DESTDIR)/etc/systemd/system/polar_doctor_web.service
	install -Dm644 web/polar_doctor_web.default $(DESTDIR)/etc/default/polar_doctor_web.example
	@[ -e $(DESTDIR)/etc/default/polar_doctor_web ] || install -Dm600 web/polar_doctor_web.default $(DESTDIR)/etc/default/polar_doctor_web
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null; then systemctl daemon-reload; fi
	@echo "✓ Service installé pour $(SERVICE_USER)."
	@echo "  1) Poser WEB_AUTH=user:pass dans /etc/default/polar_doctor_web"
	@echo "  2) sudo systemctl enable --now polar_doctor_web"

uninstall-web:
	-systemctl disable --now polar_doctor_web 2>/dev/null
	rm -f $(DESTDIR)$(INSTALL_PREFIX)/bin/$(WEB_TARGET) $(DESTDIR)/etc/systemd/system/polar_doctor_web.service $(DESTDIR)/etc/default/polar_doctor_web.example
	-systemctl daemon-reload 2>/dev/null
	@echo "(/etc/default/polar_doctor_web conservé : il contient le mot de passe)"

clean:
	@echo "Nettoyage..."
	rm -f $(TARGET) polar_doctor.exe $(WEB_TARGET)
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
	@echo "  make              - Compiler Polar Doctor (GTK)"
	@echo "  make web          - Compiler l'interface web (polar_doctor_web)"
	@echo "  sudo make install-web - Installer le service systemd polar_doctor_web"
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
