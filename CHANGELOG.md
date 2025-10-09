# Changelog - Polar Doctor

Toutes les modifications notables du projet seront documentées dans ce fichier.

## [1.0.0] - 2025-10-09

### Ajouté
- 🎉 Première version stable de Polar Doctor
- ✅ Éditeur et générateur complet de polaires de voilier
- ✅ Support des fichiers NMEA (.txt, .nmea, .log)
- ✅ Support des bases VDR SQLite (.db) de qtVlm
- ✅ Interface GTK+3 complète et moderne
- ✅ Interface multilingue (Français/English)
- ✅ Calcul automatique des angles VMG (près et portant)
- ✅ Impression et export PDF
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
