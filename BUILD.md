# Compiler Polar Doctor

Polar Doctor est un serveur web en C (`polar_doctor_web`) : l'interface est servie au
navigateur. Il ne dépend que de **glib-2.0** et **sqlite3** (plus `pkg-config` et un
compilateur C). Aucun toolkit graphique.

## Linux

### Dépendances

| Distribution | Commande |
|--------------|----------|
| Debian, Ubuntu, Raspberry Pi OS | `sudo apt install build-essential pkg-config libglib2.0-dev libsqlite3-dev` |
| Fedora | `sudo dnf install gcc make pkgconf-pkg-config glib2-devel sqlite-devel` |
| Arch | `sudo pacman -S base-devel pkgconf glib2 sqlite` |

### Compiler et lancer

```bash
make
./polar_doctor_web ~/MonBateau        # http://localhost:8081
```

### Installer comme service systemd

```bash
make                                   # PAS sous sudo (binaire du dépôt appartenant à root sinon)
sudo make install                      # /usr/local/bin + service pour l'utilisateur sudo
sudo nano /etc/default/polar_doctor_web  # WEB_AUTH=utilisateur:motdepasse (fichier root 0600)
sudo systemctl enable --now polar_doctor_web
```

- Le service tourne sous **l'utilisateur** qui a lancé `sudo` (pas root) : il écrit les
  polaires et `boat.cfg` dans son dossier. Autre utilisateur : `sudo make install SERVICE_USER=nom`.
- Réglages dans `/etc/default/polar_doctor_web` : `WEB_AUTH`, `PORT`, `BIND`, `POLAR_DOCTOR_BOAT`.
- Après une modification du code : `make && sudo make install && sudo systemctl restart polar_doctor_web`.
- Désinstaller : `sudo make uninstall` (conserve `/etc/default/polar_doctor_web`).

## Windows (MSYS2)

1. Installer [MSYS2](https://www.msys2.org/) et ouvrir le terminal **MSYS2 MINGW64**.
2. Dépendances :
   ```bash
   pacman -S --needed make mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf \
                      mingw-w64-x86_64-glib2 mingw-w64-x86_64-sqlite3
   ```
3. Compiler : `make` → `polar_doctor_web.exe`
4. Lancer : `./polar_doctor_web.exe /c/chemin/MonBateau` puis http://localhost:8081

Pour distribuer l'exécutable, copier à côté de lui les DLL MinGW qu'il utilise :
```bash
ldd polar_doctor_web.exe | grep -i /mingw64/ | awk '{print $3}' | xargs -I {} cp {} dist/
```

Accès depuis le réseau : poser `WEB_AUTH=utilisateur:motdepasse` et `BIND=0.0.0.0` dans
`%LOCALAPPDATA%\polar_doctor\web.conf` (créé au premier lancement) — voir
[README → Où mettre le mot de passe ?](README.md#où-mettre-le-mot-de-passe-).

## Intégration continue

Voir [GITHUB_ACTIONS.md](GITHUB_ACTIONS.md) : chaque push compile Linux x64, Linux ARM64 et
Windows x64, démarre le serveur et interroge l'API ; un tag `v*` publie une release.
