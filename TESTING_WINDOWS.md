# Guide de test - Correction Windows

## 🎯 Objectif

Vérifier que le bug des dialogues de fichiers Windows a été corrigé.

## 📋 Pré-requis

- Machine Windows 10 ou 11 (64-bit)
- Build récent de Polar Doctor (après commit 10314e7)

## 📥 Obtenir le binaire de test

### Option 1 : Depuis GitHub Actions (builds de développement)

1. Aller sur https://github.com/ozolli/polar_doctor/actions
2. Cliquer sur le workflow "Build Polar Doctor" le plus récent avec ✅
3. Descendre à la section "Artifacts"
4. Télécharger `polar_doctor-windows-x64`
5. Extraire le ZIP

### Option 2 : Depuis une release (versions officielles)

1. Aller sur https://github.com/ozolli/polar_doctor/releases
2. Télécharger `polar_doctor-windows-x64.zip` de la version v1.0.1+
3. Extraire le ZIP

## 🧪 Procédure de test

### Test 1 : Lancement de base

1. Ouvrir le dossier `polar_doctor_win/`
2. Double-cliquer sur `polar_doctor.exe`
3. **Vérifier** : L'application démarre sans erreur

**Résultat attendu** : ✅ Fenêtre principale s'affiche

### Test 2 : Dialogue "Ouvrir"

1. Dans l'application, cliquer sur le bouton **"Ouvrir"**
2. **Vérifier** : Un dialogue de sélection de fichier s'ouvre

**Résultat attendu** :
- ✅ Dialogue s'ouvre normalement
- ✅ Possibilité de naviguer dans les dossiers
- ✅ Possibilité de sélectionner un fichier .pol
- ✅ Cliquer "Annuler" ne cause pas de crash
- ✅ L'application reste ouverte

**Comportement avant le fix** : 💥 Crash immédiat

### Test 3 : Dialogue "Enregistrer"

1. Cliquer sur le bouton **"Enregistrer"**
2. **Vérifier** : Un dialogue "Enregistrer sous" s'ouvre

**Résultat attendu** :
- ✅ Dialogue s'ouvre normalement
- ✅ Possibilité de choisir un emplacement
- ✅ Possibilité de saisir un nom de fichier
- ✅ Cliquer "Annuler" ne cause pas de crash

**Comportement avant le fix** : 💥 Crash immédiat

### Test 4 : Dialogue "Créer"

1. Cliquer sur le bouton **"Créer"**
2. **Vérifier** : Un dialogue de sélection de fichiers NMEA/VDR s'ouvre

**Résultat attendu** :
- ✅ Dialogue s'ouvre normalement
- ✅ Sélection multiple possible
- ✅ Filtres de fichiers fonctionnels
- ✅ Cliquer "Annuler" ne cause pas de crash

**Comportement avant le fix** : 💥 Crash immédiat

### Test 5 : Dialogue "Mettre à jour"

1. Ouvrir un fichier .pol existant
2. Cliquer sur le bouton **"Mettre à jour"**
3. **Vérifier** : Un dialogue de sélection s'ouvre

**Résultat attendu** :
- ✅ Dialogue s'ouvre normalement
- ✅ Sélection multiple possible

### Test 6 : Script de lancement alternatif

Si l'un des tests ci-dessus échoue :

1. Fermer l'application
2. Double-cliquer sur `polar_doctor_win.bat` au lieu de `polar_doctor.exe`
3. Refaire les tests 2-5

**Résultat attendu** :
- ✅ Le script devrait résoudre les problèmes si le fix dans le code ne suffit pas

## 📝 Rapport de test

### Modèle de rapport

```markdown
## Test Polar Doctor Windows - [DATE]

**Version testée** : [hash du commit ou numéro de version]
**Système** : Windows [10/11] [Pro/Home] 64-bit

### Résultats

| Test | Statut | Commentaire |
|------|--------|-------------|
| Lancement | ✅/❌ | |
| Ouvrir | ✅/❌ | |
| Enregistrer | ✅/❌ | |
| Créer | ✅/❌ | |
| Mettre à jour | ✅/❌ | |
| Script .bat | ✅/❌/N/A | |

### Détails

[Description des problèmes rencontrés, screenshots, messages d'erreur...]

### Logs (si applicable)

```
[Copier les messages de la console si lancé depuis cmd]
```

### Configuration

- Résolution écran : [ex: 1920x1080]
- Langue Windows : [FR/EN]
- Antivirus : [nom]
- Autres logiciels GTK installés : [OUI/NON, lesquels]
```

## 🐛 En cas de problème persistant

### Étape 1 : Lancer avec debug

Créer un fichier `test_debug.bat` dans le dossier :

```batch
@echo off
echo ========================================
echo Polar Doctor - Test Debug
echo ========================================
echo.

set GTK_USE_PORTAL=0
set GDK_BACKEND=win32
set GTK_FILE_CHOOSER_BACKEND=gtk
set GDK_DEBUG=all
set GTK_DEBUG=all

echo Configuration GTK:
echo GTK_USE_PORTAL=%GTK_USE_PORTAL%
echo GDK_BACKEND=%GDK_BACKEND%
echo GTK_FILE_CHOOSER_BACKEND=%GTK_FILE_CHOOSER_BACKEND%
echo.

echo Lancement de Polar Doctor...
echo.

polar_doctor.exe

echo.
echo ========================================
echo Appuyez sur une touche pour fermer...
pause > nul
```

Lancer ce script et copier TOUS les messages affichés.

### Étape 2 : Vérifier les DLLs

Dans une invite de commande :

```cmd
cd polar_doctor_win
dir *.dll > liste_dlls.txt
notepad liste_dlls.txt
```

Vérifier la présence de :
- `libgtk-3-0.dll`
- `libgdk-3-0.dll`
- `libgio-2.0-0.dll`
- `libglib-2.0-0.dll`
- `libgobject-2.0-0.dll`
- `libcairo-2.dll`
- Et environ 30-40 autres DLLs

### Étape 3 : Test de compatibilité

Vérifier s'il y a des conflits avec d'autres installations GTK :

```cmd
where libgtk-3-0.dll
```

Si plusieurs chemins s'affichent, il peut y avoir un conflit.

### Étape 4 : Rapport de bug

Si le problème persiste après toutes ces étapes :

1. Créer une issue sur GitHub : https://github.com/ozolli/polar_doctor/issues/new
2. Titre : `[Windows] File dialog crash after fix`
3. Inclure :
   - Version de Windows
   - Contenu de `liste_dlls.txt`
   - Logs du `test_debug.bat`
   - Screenshots si possible

## ✅ Validation finale

Le fix est considéré comme validé si :

- ✅ Tous les tests 1-5 passent avec succès
- ✅ Aucun crash lors de l'utilisation normale
- ✅ Les dialogues s'ouvrent et fonctionnent correctement
- ✅ L'application reste stable après fermeture des dialogues

## 📊 Comparaison Avant/Après

| Action | Avant fix v1.0.0 | Après fix v1.0.1 |
|--------|------------------|------------------|
| Clic "Ouvrir" | 💥 Crash | ✅ Dialogue s'ouvre |
| Clic "Enregistrer" | 💥 Crash | ✅ Dialogue s'ouvre |
| Clic "Créer" | 💥 Crash | ✅ Dialogue s'ouvre |
| Stabilité générale | ❌ Instable | ✅ Stable |

## 🎯 Critères de succès

Le fix sera considéré comme réussi avec un taux de succès > 95% sur :
- Windows 10 (64-bit)
- Windows 11 (64-bit)
- Différentes configurations (Home/Pro)
- Avec/sans autres applications GTK installées

---

**Dernière mise à jour** : 2025-10-09
**Commit du fix** : 10314e7
**Version cible** : v1.0.1
