# Changelog - Polar Doctor

Toutes les modifications notables du projet seront documentées dans ce fichier.

## [1.1.0] - 2026-06-11

### Ajouté
- ✅ **Mode dynamique** du diagramme (façon qtVlm) : case à cocher activant l'analyse interactive
  - Champ TWS libre (boutons ± de 1 kn, décimales saisissables au clavier, ex. 9,85)
  - Affichage de la seule **courbe interpolée** (bilinéaire) pour la TWS saisie
  - **Clic gauche maintenu / glissé** sur le diagramme : ligne bleue suivant le curseur (TWA au degré entier)
  - Lecture en direct **TWA / AWA / AWS / BS / VMG** pendant le glissé
  - Au relâchement : résumé **vitesse max de la courbe** (+ TWA) et **vitesse max absolue** de la polaire (+ TWS/TWA)
- ✅ **Coloration des zones VMG** sur les courbes (modes dynamique et multi-courbes)
  - Vert (ou couleur de la TWS) sur la plage VMG utile, **rouge** hors plage (près trop serré / portant trop bas)
  - Transition placée exactement aux angles VMG optimaux, cohérente avec le tableau VMG
- ✅ **Couleurs distinctes par TWS** + **légende colorée** en mode multi-courbes
- ✅ **Curseur de percentile** dans la barre d'outils (P85–P95) pour régler l'agrégation
- ✅ **Filtre moteur** à l'import VDR : exclusion automatique des points avec RPM moteur en route
  (détection de la colonne RPM ; sans perturbateur, contrairement à la gîte)

### Modifié
- 🔄 **Agrégation par percentile** (P90 par défaut) au lieu de la moyenne tronquée :
  une polaire vise la performance atteignable, on cesse de rejeter le haut de distribution
  (gain mesuré ~6–7 % en moyenne, jusqu'à ~20 % sur les cellules bien documentées)
- 🔄 Les polaires générées depuis des données incluent désormais toujours la colonne TWS 0 (conformité .pol/.csv)

### Corrigé
- 🐛 Index par défaut du sélecteur TWS « à » hors limites pour les polaires de ≤ 6 vitesses réelles

## [1.0.1] - 2025-10-09

### Corrigé
- 🐛 **[Windows]** Crash de l'application lors de l'ouverture des dialogues de fichiers
  - Ajout de configuration GTK spécifique Windows dans le code
  - Désactivation des portails GTK qui causaient des conflits
  - Force l'utilisation du backend win32 pour GDK
  - Ajout du script de lancement `polar_doctor_win.bat` comme solution de secours

### Ajouté
- ✅ Script `polar_doctor_win.bat` pour lancement sécurisé sur Windows
- ✅ Documentation détaillée du bug fix Windows (WINDOWS_FIX.md)
- ✅ Instructions de dépannage améliorées dans LISEZMOI.txt

## [1.0.0] - 2025-10-09

### Ajouté
- 🎉 Première version stable de Polar Doctor
- ✅ Éditeur et générateur complet de polaires de voilier
- ✅ Support des fichiers NMEA (.txt, .nmea, .log)
- ✅ Support des bases VDR SQLite (.db) de qtVlm
- ✅ Interface GTK+3 complète et moderne
- ✅ Interface multilingue (Français/English)
- ✅ Calcul automatique des angles VMG (près et portant)
- ✅ Export PDF avec mise en page professionnelle
- ✅ Édition graphique interactive du tableau de données
- ✅ Visualisation polaire en temps réel avec interpolation Catmull-Rom
- ✅ Mode mise à jour (conserve les meilleures performances)
- ✅ Icône professionnelle (SVG + PNG)
- ✅ Fichier .desktop pour intégration Linux

### Infrastructure
- ✅ GitHub Actions pour compilation automatique multi-plateformes
- ✅ Workflows pour Linux, Windows (MSYS2), macOS (Homebrew)
- ✅ Génération automatique de releases avec binaires
- ✅ Checksums SHA256 pour vérification d'intégrité
- ✅ Cache Homebrew pour builds macOS plus rapides
- ✅ Tests automatiques des binaires
- ✅ Makefile universel avec détection de plateforme

### Documentation
- ✅ README.md complet avec guide utilisateur
- ✅ BUILD.md avec instructions de compilation détaillées
- ✅ GITHUB_ACTIONS.md avec guide CI/CD
- ✅ CLAUDE.md avec documentation technique du projet
- ✅ Aide intégrée dans l'application (FR/EN)

### Fichiers de test inclus
- Horta-SantaCruz.db - Passage Açores → Canaries
- Mindelo-LeMarin.db - Traversée Cap-Vert → Martinique
- LR-Horta.db, Panama.db, Perlas-Galapagos.db, etc.
- Fichiers polaires : CM50.pol, figaro3.pol

### Fonctionnalités techniques
- Agrégation statistique avec moyenne tronquée (20%)
- Filtrage des données aberrantes
- Validation des sentences NMEA avec checksums
- Buckets TWA (5°) et TWS (2 kn)
- Support de plages TWS personnalisées pour affichage
- Format .pol compatible standard

## [Unreleased]

### À venir
- Support de formats de fichiers additionnels
- Export vers d'autres formats (CSV, JSON)
- Graphiques de performance supplémentaires
- Mode comparaison de polaires
- Intégration avec d'autres logiciels de navigation

---

## Format du changelog

Ce changelog suit les conventions de [Keep a Changelog](https://keepachangelog.com/fr/1.0.0/),
et ce projet adhère au [Semantic Versioning](https://semver.org/lang/fr/).

### Types de changements
- **Ajouté** pour les nouvelles fonctionnalités
- **Modifié** pour les changements aux fonctionnalités existantes
- **Déprécié** pour les fonctionnalités qui seront bientôt supprimées
- **Supprimé** pour les fonctionnalités supprimées
- **Corrigé** pour les corrections de bugs
- **Sécurité** pour les vulnérabilités corrigées
