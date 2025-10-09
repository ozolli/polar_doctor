# Fix pour les crashes des dialogues de fichiers sur Windows

## 🐛 Problème

Sur Windows, l'application se fermait brutalement lorsque l'utilisateur cliquait sur :
- Bouton "Ouvrir"
- Bouton "Enregistrer"
- Bouton "Créer"

Tous ces boutons ouvrent des dialogues de sélection de fichiers GTK.

## 🔍 Cause

GTK sur Windows peut avoir des conflits entre :
1. Le backend de fichiers natif Windows (win32)
2. Les portails GTK (GTK_USE_PORTAL)
3. Le sélecteur de fichiers natif vs GTK

Ces conflits causent des crashes lors de l'ouverture des dialogues `GtkFileChooserDialog`.

## ✅ Solution implémentée

### 1. Configuration au niveau du code C

**Fichier : `polar_doctor.c`**

Ajout dans la fonction `main()` avant `gtk_init()` :

```c
#ifdef _WIN32
g_setenv("GTK_USE_PORTAL", "0", TRUE);
g_setenv("GDK_BACKEND", "win32", TRUE);
g_setenv("GTK_FILE_CHOOSER_BACKEND", "gtk", TRUE);
#endif
```

**Explication :**
- `GTK_USE_PORTAL=0` : Désactive les portails GTK qui peuvent être problématiques sur Windows
- `GDK_BACKEND=win32` : Force l'utilisation du backend Windows natif pour GDK
- `GTK_FILE_CHOOSER_BACKEND=gtk` : Force GTK à utiliser son propre dialogue au lieu du natif Windows

### 2. Script de lancement Windows

**Fichier : `polar_doctor_win.bat`**

```batch
@echo off
REM Script de lancement pour Polar Doctor sur Windows
REM Ce script configure l'environnement GTK pour éviter les crashes

REM Force GTK à utiliser le backend GDK natif Windows
set GTK_USE_PORTAL=0
set GDK_BACKEND=win32

REM Lance Polar Doctor
polar_doctor.exe %*
```

**Utilité :**
- Redondance de sécurité si les variables d'environnement dans le code ne suffisent pas
- Permet aux utilisateurs de lancer avec les bonnes variables sans modification du code
- Facilite le débogage (l'utilisateur peut modifier le .bat)

### 3. Documentation utilisateur

**Fichier : `LISEZMOI.txt` (dans le package Windows)**

Ajout des instructions :
```
Pour lancer:
1. Double-cliquer sur polar_doctor.exe
2. Ou utiliser polar_doctor_win.bat (recommandé si problèmes de dialogues)

Si les dialogues de fichiers ferment l'application:
- Utilisez polar_doctor_win.bat au lieu de polar_doctor.exe directement
- Ce script configure l'environnement GTK correctement
```

### 4. Ajout au workflow GitHub Actions

**Fichiers : `build.yml` et `release.yml`**

Modification pour inclure le fichier .bat dans le package Windows :

```yaml
cp polar_doctor_win.bat polar_doctor_win/
```

## 🧪 Test de la solution

### Test local (Linux avec cross-compilation)
```bash
# Compilation
x86_64-w64-mingw32-gcc -o polar_doctor.exe polar_doctor.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lm -lsqlite3 -mwindows
```

### Test sur Windows
1. Extraire le package `polar_doctor-windows-x64.zip`
2. Lancer `polar_doctor.exe`
3. Tester les boutons Ouvrir, Enregistrer, Créer
4. Si problème persiste, utiliser `polar_doctor_win.bat`

## 📊 Résultats attendus

**Avant le fix :**
- ❌ Crash au clic sur Ouvrir/Enregistrer/Créer
- ❌ Application se ferme sans message d'erreur
- ❌ Aucun dialogue de fichier ne s'affiche

**Après le fix :**
- ✅ Les dialogues de fichiers s'ouvrent normalement
- ✅ Possibilité de sélectionner des fichiers
- ✅ L'application reste stable

## 🔧 Alternatives envisagées

### Alternative 1 : Utiliser GtkFileChooserNative (rejeté)
```c
GtkFileChooserNative *native = gtk_file_chooser_native_new(...);
```
**Raison du rejet :** Plus de travail, nécessite de réécrire toutes les fonctions de dialogue

### Alternative 2 : Compiler sans `-mwindows` (rejeté)
**Raison du rejet :** Afficherait une console cmd en permanence, mauvaise UX

### Alternative 3 : Utiliser les dialogues Win32 natifs (rejeté)
**Raison du rejet :** Perte de cohérence visuelle, code spécifique Windows complexe

## 📝 Variables d'environnement GTK pertinentes

| Variable | Valeur | Effet |
|----------|--------|-------|
| `GTK_USE_PORTAL` | 0 | Désactive les portails (freedesktop.org) |
| `GDK_BACKEND` | win32 | Force le backend Windows |
| `GTK_FILE_CHOOSER_BACKEND` | gtk | Force dialogue GTK natif |
| `GTK_THEME` | - | Peut être utilisé pour forcer un thème |
| `GDK_DEBUG` | all | Active le debug (pour dépannage) |

## 🐛 Dépannage supplémentaire

Si le problème persiste après le fix :

### 1. Vérifier les DLLs
```cmd
cd polar_doctor_win
dir *.dll
```

Devrait afficher toutes les DLLs GTK nécessaires.

### 2. Lancer avec debug
Créer `polar_doctor_debug.bat` :
```batch
@echo off
set GTK_USE_PORTAL=0
set GDK_BACKEND=win32
set GDK_DEBUG=all
set GTK_DEBUG=all
polar_doctor.exe
pause
```

### 3. Vérifier les schemas glib
```cmd
dir share\glib-2.0\schemas\gschemas.compiled
```

Si absent, compiler :
```cmd
glib-compile-schemas share\glib-2.0\schemas\
```

## 📚 Références

- [GTK3 Documentation - File Chooser Dialog](https://docs.gtk.org/gtk3/class.FileChooserDialog.html)
- [GDK Backend Selection](https://docs.gtk.org/gdk3/class.Display.html)
- [GTK Environment Variables](https://docs.gtk.org/gtk3/running.html)
- [MSYS2 GTK Documentation](https://www.msys2.org/docs/environments/)

## ✅ Commit associé

**Commit hash :** `10314e7`

**Message :**
```
Fix Windows file dialog crashes

Problem: File dialogs (Open, Save, Create) were causing crashes on Windows

Solution:
- Add Windows-specific GTK environment configuration in main()
- Disable GTK portals that can cause issues on Windows
- Force GDK backend to win32
- Force GTK file chooser to use GTK backend instead of native

Additional improvements:
- Add polar_doctor_win.bat launcher script for Windows
- Update Windows package to include .bat file
- Update LISEZMOI.txt with troubleshooting instructions
- Add Windows-specific headers and defines
```

## 🎯 Version concernée

- **Version avec bug :** v1.0.0
- **Version avec fix :** v1.0.1 (à venir)

## 👥 Contribution

Si vous rencontrez toujours des problèmes après ces correctifs :

1. Ouvrir une issue sur GitHub : https://github.com/ozolli/polar_doctor/issues
2. Inclure :
   - Version de Windows (10/11)
   - Contenu du dossier `polar_doctor_win/`
   - Messages d'erreur (si lancé depuis cmd)
   - Résultat de `polar_doctor_debug.bat`

---

**Fix testé sur :**
- ⏳ Windows 10 64-bit (à tester)
- ⏳ Windows 11 64-bit (à tester)

**Fix compilé avec :**
- ✅ GCC MinGW64 (MSYS2)
- ✅ GTK+ 3.24
- ✅ GitHub Actions (Windows Latest)
