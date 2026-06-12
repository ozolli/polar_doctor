# Polar Doctor ⛵

[![Build Status](https://github.com/ozolli/polar_doctor/workflows/Build%20Polar%20Doctor/badge.svg)](https://github.com/ozolli/polar_doctor/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-blue)](https://github.com/ozolli/polar_doctor)

**Polar Doctor** est un éditeur et générateur de diagrammes polaires pour voiliers. Il permet de créer, éditer, analyser et exporter des polaires de performance à partir de données NMEA et VDR.

![Polar Doctor](polar_doctor.png)

## ⚓ C'est quoi une polaire (et à quoi sert ce programme) ?

La **polaire** d'un voilier est un tableau — et un diagramme — qui donne la vitesse
du bateau pour chaque combinaison d'**angle** et de **force** du vent. C'est la carte
d'identité de performance du bateau, utilisée par les logiciels de routage et pour
comparer/régler vos navigations.

Polar Doctor fabrique cette polaire à partir de vos données **réelles** : fichiers de
log NMEA0183, bases VDR de qtVlm, ou **en direct** pendant que vous naviguez. Plus vous
accumulez de données dans des conditions variées, plus la polaire est fidèle.

**Le vocabulaire en 30 secondes :**

| Sigle | Signification |
|-------|---------------|
| **TWA** | *True Wind Angle* — angle du vent réel par rapport à l'axe du bateau, de 0° (vent debout) à 180° (vent arrière) |
| **TWS** | *True Wind Speed* — force du vent réel (nœuds) |
| **STW / BSP** | *Speed Through Water* — vitesse du bateau dans l'eau (loch) ; c'est ce que la polaire représente |
| **SOG** | *Speed Over Ground* — vitesse par rapport au fond (GPS) ; sert à fiabiliser le STW |
| **VMG** | *Velocity Made Good* — composante de la vitesse dans l'axe du vent ; efficacité au près et au portant |
| **AWA / AWS** | vent **apparent** ressenti à bord (vent réel combiné à la vitesse du bateau) |

**Prise en main en 4 étapes :** ① ouvrir ou créer un bateau → ② (option) régler son
inventaire (voiles, états de mer, nombre de polaires) → ③ **Créer** depuis des fichiers
de log ou **Capture live** en navigation → ④ lire (onglets Données / Polaire / VMG),
corriger, **Export PDF**.

## 🌟 Fonctionnalités

### Génération de polaires
- ✅ Import de fichiers **NMEA** (.txt, .nmea, .log)
- ✅ Import de bases **VDR SQLite** (.db) de qtVlm
- ✅ Sélection multiple de fichiers
- ✅ Agrégation par **percentile configurable** (P85–P95, défaut P90) — vise la performance atteignable
- ✅ **Lissage glissant** du STW à l'import NMEA (anti-bruit du loch)
- ✅ **Débruitage du STW par le SOG** : les sauts du loch (coque déjaugée, roue à aube bloquée)
  sont rejetés, un courant lent est préservé
- ✅ **Filtre moteur** : points moteur (RPM > 0) exclus, **sauf charge batteries** (commentaire `Charge` du VDR, moteur débrayé)
- ✅ Mode mise à jour : ré-agrège l'existant + les nouvelles données au percentile (la polaire peut monter ou baisser)

### Mode bateau (multi-polaires)
- ⛵ Un **bateau = un dossier** (config `boat.cfg` ou `<nom>.cfg` + ses polaires `.pol`)
- ✅ Bouton **Ouvrir ▾** : liste des **bateaux récents** + parcourir un bateau / ouvrir une polaire seule
- ✅ Bouton **Nouveau bateau** ; bouton **Bateau…** pour éditer l'inventaire
- ✅ **Inventaire** par bateau : grand-voile, voiles d'avant, **états de mer (échelle Douglas)**, mots-clés moteur
- ✅ **Polaires multiples** : définir N polaires et leurs critères (voiles × états de mer) par **cases à cocher**
- ✅ **Routage automatique** à l'import : chaque point va dans les polaires dont les critères correspondent
  (état voile/mer suivi via les commentaires VDR) → un `.pol` par polaire
- ✅ **Sélecteur de polaire** pour basculer entre les polaires du bateau
- ✅ Filtre moteur configurable (`Moteur`/`Charge`) ; commentaires tolérants casse/accents et accolés (`GVJ1`)

### Capture live (temps réel)
- 📡 Alimente les polaires du bateau **en direct** depuis 3 sources :
  - **VDR qtVlm** : suit le fichier `vdr.db` en lecture seule (mode WAL)
  - **NMEA TCP** : client vers une passerelle (ex. `hôte:10110`)
  - **NMEA UDP** : écoute d'un port de diffusion
- ✅ État **voile / moteur / mer** piloté en direct par **listes déroulantes + bouton Moteur**
  (les commentaires VDR sont ignorés en live)
- ✅ Chaque point est **routé** vers les polaires correspondantes
- ✅ **Visualisation temps réel** : nuage de points bruts + point courant, échelle et plage TWS qui suivent les données
- ✅ Grilles ré-ensemencées depuis les `.pol` au démarrage, enregistrées à l'arrêt

### Édition graphique
- ✅ Tableau de données éditable (double-clic)
- ✅ Ajout/suppression de lignes TWA (angles de vent)
- ✅ Ajout/suppression de colonnes TWS (vitesses de vent)
- ✅ Visualisation polaire en temps réel
- ✅ Interpolation Catmull-Rom pour courbes lisses
- ✅ Sélection de plage TWS pour affichage
- ✅ Couleur distincte par TWS + légende en mode multi-courbes

### Analyse VMG
- ✅ Calcul automatique des meilleurs angles VMG
- ✅ VMG upwind (près) et downwind (portant)
- ✅ Tableau récapitulatif par vitesse de vent
- ✅ **Zones VMG matérialisées sur le diagramme** : vert = plage utile, rouge = près trop serré / portant trop bas

### Mode dynamique (analyse interactive)
- ✅ Courbe **interpolée** pour une TWS quelconque saisie au clavier (ex. 9,85 kn)
- ✅ **Clic maintenu / glissé** sur le diagramme : ligne bleue qui suit le curseur (TWA au degré entier)
- ✅ Lecture en direct **TWA / AWA / AWS / BS / VMG**
- ✅ Au relâchement : vitesse max de la courbe et vitesse max absolue de la polaire

### Export et impression
- ✅ **Export PDF** (données + diagramme + VMG)
- ✅ Nom de fichier automatique (nom de la polaire)
- ✅ Format .pol compatible
- ✅ Mise en page professionnelle
- ✅ Taille de texte ajustable (variable POLAR_PRINT_SCALE)

### Interface multilingue
- 🇫🇷 **Français**
- 🇬🇧 **English**
- ✅ Changement de langue à la volée
- ✅ Traduction complète (interface + aide + export)

## 📦 Installation

### Installation rapide (Linux)

```bash
# Cloner le dépôt
git clone https://github.com/ozolli/polar_doctor.git
cd polar_doctor

# Compiler et installer
make
sudo make install

# Lancer
polar_doctor
```

### Binaires pré-compilés

Téléchargez depuis [GitHub Releases](https://github.com/ozolli/polar_doctor/releases) :
- 🐧 **Linux x86_64** (PC standard)
- 🐧 **Linux ARM64** (Raspberry Pi, serveurs ARM)
- 🍎 **macOS arm64** (Apple Silicon ; Mac Intel : compiler depuis les sources)
- 🪟 **Windows x64** (package portable avec DLLs)

### Compilation depuis les sources

Consultez [BUILD.md](BUILD.md) pour les instructions détaillées pour :
- 🐧 Linux (Debian, Ubuntu, Fedora, Arch)
- 🪟 Windows (MSYS2, MinGW)
- 🍎 macOS (Homebrew)
- 🐳 Docker

## 🚀 Utilisation rapide

### 1. Créer une nouvelle polaire

1. Cliquer sur **"Créer"**
2. Sélectionner un ou plusieurs fichiers NMEA/VDR
3. La polaire est générée automatiquement

### 2. Mettre à jour une polaire existante

1. Ouvrir une polaire (.pol)
2. Cliquer sur **"Mettre à jour"**
3. Sélectionner de nouveaux fichiers de données
4. L'existant et les nouvelles données sont ré-agrégés au percentile (la polaire s'ajuste au réel, à la hausse comme à la baisse)

### 3. Éditer manuellement

**Modifier une valeur :**
- Double-cliquer sur une cellule
- Saisir la nouvelle valeur

**Ajouter un angle TWA :**
- Cliquer sur "Ajout TWA"
- Saisir l'angle (0-180°)

**Ajouter une vitesse TWS :**
- Cliquer sur "Ajout TWS"
- Saisir la vitesse en nœuds

**Supprimer ligne/colonne :**
- Cliquer sur "Suppression"
- Cliquer sur l'en-tête à supprimer
- Confirmer

### 4. Visualiser et analyser

**Onglet Polaire :**
- Visualisation graphique du diagramme
- Sélectionner la plage TWS à afficher

**Onglet VMG :**
- Angles optimaux pour le près et le portant
- Tableau récapitulatif par vitesse de vent

### 5. Exporter en PDF

- Cliquer sur **"Export PDF"**
- Choisir l'emplacement de sauvegarde
- Le fichier PDF sera nommé automatiquement d'après la polaire
- **Windows :** Utilise des dialogues natifs Windows pour plus de stabilité
- **Ajuster la taille du texte :** Définir `POLAR_PRINT_SCALE` (0.5 à 3.0, défaut 1.0)
  ```bash
  # Linux/macOS
  export POLAR_PRINT_SCALE=1.5
  ./polar_doctor

  # Windows
  set POLAR_PRINT_SCALE=1.5
  polar_doctor.exe
  ```

### 6. Capturer en direct (live)

1. Ouvrir le **bateau** à alimenter
2. Dans la colonne à droite du diagramme, choisir la **source** : VDR qtVlm, NMEA TCP ou NMEA UDP
   (renseigner le chemin du `vdr.db` ou l'adresse `hôte:port`)
3. Régler l'état courant via les **listes déroulantes** (grand-voile / voile d'avant / état de mer)
   et le bouton **Moteur**
4. Cliquer sur **Démarrer** : les points arrivent en temps réel, routés vers les polaires correspondantes
5. **Arrêter** : les polaires sont enregistrées

## 📊 Format des fichiers

### Fichiers NMEA

Sentences supportées :
- **MWV** - Vent apparent **ou** vrai (réf. `R`/`T`) — *vent*
- **MWD** - Vent **vrai** (direction + vitesse) — *vent*, alternative à MWV
- **VHW** - Vitesse surface / STW — *requise*
- **HDT / HDG** - Cap vrai — nécessaire pour calculer le TWA à partir de MWD (`TWA = TWD − cap`)
- **RMC, VTG, VBW, RMA, OSD** - Vitesse fond (SOG), lue si présente pour débruiter le STW

Unités de vent reconnues : **nœuds (`N`)**, **m/s (`M`)**, **km/h (`K`)**.

Exemple :
```
$IIMWV,045.2,T,12.3,N,A*XX
$WIMWD,135.0,T,,,12.3,N,6.3,M*XX
$IIHDT,090.0,T*XX
$IIVHW,,,,,05.8,N,,*XX
$GPRMC,123519,A,4807.038,N,01131.000,E,5.8,084.4,230394,,,A*XX
```

> Le découpage des trames préserve les champs vides (positions fixes) : un VHW sans cap vrai
> (`$IIVHW,,T,25.0,M,5.9,N,…`) est correctement interprété.

### Fichiers VDR (qtVlm)

Base SQLite avec table `VDR` contenant :
- `TWA` - True Wind Angle (°)
- `TWS` - True Wind Speed (kn)
- `STW` - Speed Through Water (kn)
- `SOG` - Speed Over Ground (kn), optionnelle — sert à débruiter le STW
- `RPM` - régime moteur, optionnelle — filtre moteur (voir ci-dessous)
- `COMMENT` - commentaire libre, optionnel — mot-clé `Charge` = moteur débrayé (données conservées)

### Fichiers polaires (.pol)

Format CSV avec point-virgule :
```
TWA\TWS;6;8;10;12;14
30;4.2;5.1;5.8;6.2;6.5
45;4.8;5.9;6.7;7.1;7.4
60;5.2;6.4;7.3;7.8;8.1
...
```

## 🎨 Raccourcis clavier

| Raccourci | Action |
|-----------|--------|
| `Ctrl+O` | Ouvrir |
| `Ctrl+S` | Enregistrer |
| `Ctrl+N` | Créer nouvelle polaire |
| `F1` | Aide |
| `Ctrl+Q` | Quitter |

## 🛠️ Compilation

### Prérequis

- GCC ou Clang
- GTK+ 3.0+
- SQLite3
- pkg-config

### Commande simple

```bash
gcc -o polar_doctor *.c \
    `pkg-config --cflags --libs gtk+-3.0` \
    -lm -lsqlite3
```

### Avec le Makefile

```bash
make              # Compiler
make clean        # Nettoyer
make install      # Installer (Linux/macOS)
make dist         # Créer un package
make help         # Aide
```

## 📚 Documentation

- **[BUILD.md](BUILD.md)** - Guide de compilation multi-plateformes
- **[CLAUDE.md](CLAUDE.md)** - Documentation technique du projet
- **Aide intégrée** - Appuyez sur le bouton "Aide" dans l'application

## 🔧 Architecture

Code découpé en modules (un header commun + compilation de tous les `.c`) :

```
polar_doctor.h     # Déclarations communes : types, constantes, globals, prototypes
boat_config.c      # Config bateau : inventaire voiles/mer, lecture/écriture INI
import.c           # NMEA (champs préservés + SOG RMC/VTG/VBW/RMA/OSD) + VDR
                   #   (filtre moteur RPM/Charge, débruitage STW/SOG) + agrégation P90
polar_data.c       # Modèle PolarData : .pol load/save, interpolation, VMG, palette
diagram.c          # Dessin du diagramme polaire + mode dynamique + événements + overlay live
gui_tabs.c         # Onglets Données/Diagramme, table VMG, légende, ouverture/sauvegarde
gui_window.c       # Fenêtre, toolbar, création/MAJ, édition, langue, dialogues Aide/Bateau
live.c             # Capture live : sources VDR/NMEA TCP/UDP, routage, boutons d'état temps réel
export_pdf.c       # Export / impression PDF
win32_dialogs.c    # Dialogues de fichiers natifs Windows (vide hors Windows)
main.c             # Point d'entrée + globals
```

## 🧪 Tests

Fichiers de test inclus :
- `Horta-SantaCruz.db` - Passage Açores → Canaries
- `Mindelo-LeMarin.db` - Traversée Cap-Vert → Martinique
- Autres bases VDR de passages océaniques

## 🐛 Signalement de bugs

Ouvrir une issue sur GitHub avec :
- Système d'exploitation et version
- Étapes pour reproduire
- Messages d'erreur
- Fichiers de test si possible

## 🤝 Contribution

Les contributions sont bienvenues !

1. Fork le projet
2. Créer une branche (`git checkout -b feature/nouvelle-fonctionnalite`)
3. Commit (`git commit -am 'Ajout nouvelle fonctionnalité'`)
4. Push (`git push origin feature/nouvelle-fonctionnalite`)
5. Créer une Pull Request

## 📄 Licence

Ce projet est sous licence MIT. Voir le fichier LICENSE pour plus de détails.

## 👨‍💻 Auteur

Développé avec ❤️ pour la communauté nautique

## 🙏 Remerciements

- **GTK Project** - Toolkit graphique
- **SQLite** - Base de données
- **qtVlm** - Format VDR
- Tous les navigateurs qui ont contribué des données de test

## 📈 Statistiques

- **Lignes de code :** ~6200 (10 modules + header commun)
- **Fonctions :** 90+
- **Formats supportés :** 3 (NMEA, VDR, POL)
- **Langues :** 2 (FR, EN)
- **Plateformes :** Linux, Windows, macOS

---

**Bon vent ! ⛵**
