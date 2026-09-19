# Polar Doctor — serveur web (polaires, édition, capture live)
#
#   make                  compile polar_doctor_web (Linux, ou Windows sous MSYS2 MINGW64)
#   sudo make install     installe le binaire et le service systemd (Linux)
#   make help
#
# Dépendances : glib-2.0, sqlite3, pkg-config. Aucun toolkit graphique :
# l'interface est servie au navigateur.

CC      = gcc
TARGET  = polar_doctor_web
SRC     = web/server.c polar_data.c import.c boat_config.c libpolar.c
HDR     = libpolar.h
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -D_GNU_SOURCE -Wall -I. $(shell pkg-config --cflags glib-2.0 sqlite3)
LDLIBS  = $(shell pkg-config --libs glib-2.0 sqlite3) -lm
PREFIX ?= /usr/local

ifeq ($(OS),Windows_NT)
    TARGET  = polar_doctor_web.exe
    LDLIBS += -lws2_32 -lbcrypt
endif

.PHONY: all web test install install-web uninstall uninstall-web clean help

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
	@echo "✓ $(TARGET) : ./$(TARGET) [dossier-bateau|fichier.pol] --port 8081"

web: all   # ancien nom de cible, conservé

# Tests unitaires du cœur (sans le serveur), lancés depuis la racine du dépôt.
CORE = polar_data.c import.c boat_config.c libpolar.c
TESTS = $(wildcard tests/test_*.c)
test: $(TESTS) $(CORE) $(HDR)
	@set -e; for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o tests/run_$$(basename $$t .c) $$t $(CORE) $(LDLIBS); \
	    echo "== $$t"; ./tests/run_$$(basename $$t .c); \
	done

# --- Service systemd (Linux) : make && sudo make install ---
# Ne recompile PAS (sous sudo, le binaire du dépôt deviendrait propriété de root).
# Le service tourne sous l'utilisateur qui a lancé sudo (SERVICE_USER).
SERVICE_USER ?= $(SUDO_USER)
install: install-web
install-web:
ifeq ($(OS),Windows_NT)
	@echo "Pas de service systemd sous Windows : lancer $(TARGET) directement."
else
	@[ -x $(TARGET) ] || { echo "Lancer d'abord (sans sudo) : make"; exit 1; }
	@[ -n "$(SERVICE_USER)" ] || { echo "SERVICE_USER vide : lancer via sudo, ou SERVICE_USER=<utilisateur>"; exit 1; }
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -d $(DESTDIR)/etc/systemd/system
	sed 's/@USER@/$(SERVICE_USER)/' web/polar_doctor_web.service > $(DESTDIR)/etc/systemd/system/polar_doctor_web.service
	chmod 644 $(DESTDIR)/etc/systemd/system/polar_doctor_web.service
	install -Dm644 web/polar_doctor_web.default $(DESTDIR)/etc/default/polar_doctor_web.example
	@[ -e $(DESTDIR)/etc/default/polar_doctor_web ] || install -Dm600 web/polar_doctor_web.default $(DESTDIR)/etc/default/polar_doctor_web
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null; then systemctl daemon-reload; fi
	@echo "✓ Service installé pour $(SERVICE_USER)."
	@echo "  1) Poser WEB_AUTH=user:pass dans /etc/default/polar_doctor_web"
	@echo "  2) sudo systemctl enable --now polar_doctor_web"
endif

uninstall: uninstall-web
uninstall-web:
	-systemctl disable --now polar_doctor_web 2>/dev/null
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET) $(DESTDIR)/etc/systemd/system/polar_doctor_web.service $(DESTDIR)/etc/default/polar_doctor_web.example
	-systemctl daemon-reload 2>/dev/null
	@echo "(/etc/default/polar_doctor_web conservé : il contient le mot de passe)"

clean:
	rm -f polar_doctor_web polar_doctor_web.exe polar_doctor tests/run_*
	rm -rf dist/

help:
	@echo "Polar Doctor — serveur web"
	@echo "  make                 compiler $(TARGET)"
	@echo "  sudo make install    installer binaire + service systemd (Linux)"
	@echo "  sudo make uninstall  désinstaller (garde /etc/default/polar_doctor_web)"
	@echo "  make test            tests unitaires du cœur"
	@echo "  make clean           supprimer les binaires"
