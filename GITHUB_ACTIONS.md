# GitHub Actions - Guide de compilation automatique

Ce projet utilise **GitHub Actions** pour compiler automatiquement Polar Doctor sur Linux, Windows et macOS à chaque commit et créer des releases.

## 🚀 Configuration initiale

### 1. Créer un repository GitHub

```bash
# Initialiser git (si pas déjà fait)
git init

# Ajouter tous les fichiers
git add .
git commit -m "Initial commit - Polar Doctor"

# Créer un repository sur GitHub, puis:
git remote add origin https://github.com/VOTRE_USERNAME/polar_doctor.git
git branch -M main
git push -u origin main
```

### 2. Activer GitHub Actions

**C'est automatique !** Dès que vous pushez le code avec le dossier `.github/workflows/`, GitHub Actions s'active.

## 📋 Workflows disponibles

### `build.yml` - Compilation automatique

**Déclenché sur :**
- Push sur `main` ou `master`
- Pull requests
- Manuellement via l'interface GitHub

**Ce qu'il fait :**
1. ✅ Compile sur Linux (Ubuntu)
2. ✅ Compile sur Windows (MSYS2)
3. ✅ Compile sur macOS (Homebrew)
4. ✅ Crée des packages pour chaque plateforme
5. ✅ Upload les artifacts (disponibles 30 jours)

**Résultat :**
- `polar_doctor-linux-x64.tar.gz`
- `polar_doctor-windows-x64.zip`
- `polar_doctor-macos-x64.tar.gz`

### `release.yml` - Création de releases

**Déclenché sur :**
- Push d'un tag version (ex: `v1.0.0`)

**Ce qu'il fait :**
1. ✅ Compile toutes les plateformes
2. ✅ Crée une release GitHub
3. ✅ Attache les binaires à la release

## 🏷️ Créer une release

### Méthode simple

```bash
# Créer un tag version
git tag v1.0.0
git push origin v1.0.0

# GitHub Actions compile automatiquement et crée la release !
```

### Méthode complète avec notes

```bash
# Mettre à jour VERSION dans le code
# Commit les changements
git add .
git commit -m "Release v1.0.0"

# Créer le tag avec message
git tag -a v1.0.0 -m "Version 1.0.0 - First stable release"

# Pusher
git push origin main
git push origin v1.0.0

# Attendre quelques minutes
# La release apparaît sur: https://github.com/VOTRE_USERNAME/polar_doctor/releases
```

## 📥 Télécharger les artifacts de build

### Via l'interface web

1. Aller sur https://github.com/VOTRE_USERNAME/polar_doctor/actions
2. Cliquer sur un workflow réussi (✅)
3. Descendre dans la section "Artifacts"
4. Télécharger le package souhaité

### Via GitHub CLI

```bash
# Installer GitHub CLI
# https://cli.github.com/

# Lister les artifacts
gh run list

# Télécharger un artifact spécifique
gh run download <RUN_ID> -n polar_doctor-linux-x64
```

## 🔍 Voir les logs de compilation

1. Aller sur l'onglet **Actions** du repository
2. Cliquer sur un workflow
3. Cliquer sur un job (ex: "Build on Windows")
4. Voir les logs détaillés

## 🐛 Debugging

### Build qui échoue ?

**Linux:**
```yaml
# Vérifier les dépendances dans .github/workflows/build.yml
- name: Install dependencies
  run: |
    sudo apt-get update
    sudo apt-get install -y build-essential libgtk-3-dev libsqlite3-dev
```

**Windows:**
```yaml
# Vérifier la liste des packages MSYS2
install: >-
  mingw-w64-x86_64-gcc
  mingw-w64-x86_64-gtk3
  mingw-w64-x86_64-sqlite3
```

**macOS:**
```yaml
# Vérifier Homebrew
run: |
  brew install gtk+3 sqlite3
```

### Erreur "DLL not found" sur Windows

Le workflow copie automatiquement les DLLs avec:
```bash
ldd polar_doctor.exe | grep mingw64 | awk '{print $3}' | xargs -I {} cp {} polar_doctor_win/
```

Si des DLLs manquent, ajoutez-les manuellement dans le workflow.

## 📊 Badge de status

Ajoutez un badge dans votre README.md :

```markdown
![Build Status](https://github.com/VOTRE_USERNAME/polar_doctor/workflows/Build%20Polar%20Doctor/badge.svg)
```

Résultat:
![Build Status](https://github.com/VOTRE_USERNAME/polar_doctor/workflows/Build%20Polar%20Doctor/badge.svg)

## 🔐 Secrets et tokens

GitHub Actions utilise automatiquement `GITHUB_TOKEN` pour:
- Créer des releases
- Upload des artifacts
- Commenter sur les PRs

**Aucune configuration nécessaire !**

## ⚙️ Personnalisation

### Changer les versions des OS

```yaml
# build.yml
runs-on: ubuntu-22.04  # ou ubuntu-20.04
runs-on: windows-2022  # ou windows-2019
runs-on: macos-12      # ou macos-11
```

### Ajouter des tests

```yaml
- name: Run tests
  run: |
    ./polar_doctor --version
    ./polar_doctor --help
```

### Compiler en mode debug

```yaml
- name: Build (Debug)
  run: |
    gcc -o polar_doctor polar_doctor.c \
      `pkg-config --cflags --libs gtk+-3.0` \
      -lm -lsqlite3 -g -DDEBUG
```

## 🕒 Temps de compilation

**Estimations :**
- Linux: ~2-3 minutes
- Windows: ~5-7 minutes (MSYS2 setup)
- macOS: ~4-5 minutes (Homebrew install)

**Total:** ~12-15 minutes pour les 3 plateformes

## 💰 Coûts

GitHub Actions est **GRATUIT** pour les repositories publics :
- ✅ Minutes illimitées
- ✅ Stockage illimité (artifacts 30 jours)
- ✅ Tous les workflows

Pour les repos privés :
- 2000 minutes/mois gratuites
- Au-delà: ~$0.008/minute

## 📚 Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [MSYS2 Setup Action](https://github.com/msys2/setup-msys2)
- [Upload Release Asset](https://github.com/actions/upload-release-asset)

## 🎯 Exemples de workflows réussis

Voir d'autres projets GTK avec GitHub Actions:
- [GIMP](https://github.com/GNOME/gimp)
- [Inkscape](https://github.com/inkscape/inkscape)
- [Audacity](https://github.com/audacity/audacity)

## ✅ Checklist avant le premier push

- [ ] Fichiers `.github/workflows/*.yml` présents
- [ ] Repository créé sur GitHub
- [ ] `git remote add origin` configuré
- [ ] Actions activées dans les settings du repo
- [ ] README.md avec badge de status
- [ ] Code compile localement sur votre machine

## 🚀 Première release

```bash
# 1. Vérifier que tout compile localement
make clean && make

# 2. Commit final
git add .
git commit -m "Prepare v1.0.0 release"
git push origin main

# 3. Créer le tag
git tag -a v1.0.0 -m "First stable release"
git push origin v1.0.0

# 4. Attendre 15 minutes
# 5. Vérifier: https://github.com/VOTRE_USERNAME/polar_doctor/releases

# 6. Célébrer ! 🎉
```

---

**Bon build ! 🚀**
