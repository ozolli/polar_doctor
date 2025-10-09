#!/bin/bash
# Script d'initialisation du repository GitHub pour Polar Doctor

set -e

echo "=========================================="
echo "  Polar Doctor - Setup GitHub"
echo "=========================================="
echo ""

# Vérifier si git est installé
if ! command -v git &> /dev/null; then
    echo "❌ Git n'est pas installé"
    echo "   Installation: sudo apt-get install git"
    exit 1
fi

echo "✓ Git est installé"

# Vérifier si on est déjà dans un repo git
if [ -d ".git" ]; then
    echo "✓ Repository Git déjà initialisé"
else
    echo "📦 Initialisation du repository Git..."
    git init
    echo "✓ Repository Git créé"
fi

# Vérifier si .gitignore existe
if [ -f ".gitignore" ]; then
    echo "✓ .gitignore présent"
else
    echo "⚠️  .gitignore manquant - veuillez le créer"
fi

# Vérifier les workflows GitHub Actions
if [ -d ".github/workflows" ]; then
    echo "✓ Workflows GitHub Actions présents"
    ls -1 .github/workflows/*.yml | while read file; do
        echo "  - $(basename $file)"
    done
else
    echo "⚠️  Workflows GitHub Actions manquants"
fi

# Ajouter tous les fichiers
echo ""
echo "📝 Ajout des fichiers au repository..."
git add .

# Vérifier le statut
echo ""
echo "📊 Statut du repository:"
git status --short

# Proposer de créer le commit initial
echo ""
read -p "Créer le commit initial ? (o/N) " -n 1 -r
echo
if [[ $REPLY =~ ^[OoYy]$ ]]; then
    git commit -m "Initial commit - Polar Doctor

- Application complète d'édition de polaires
- Support NMEA et VDR (SQLite)
- Interface GTK+3 multilingue (FR/EN)
- Calcul VMG automatique
- Impression et export PDF
- Compilation multi-plateformes (Linux/Windows/macOS)
- GitHub Actions pour builds automatiques"

    echo "✓ Commit initial créé"
else
    echo "⏭️  Commit initial ignoré"
fi

# Instructions pour créer le repository GitHub
echo ""
echo "=========================================="
echo "  Étapes suivantes"
echo "=========================================="
echo ""
echo "1. Créer un repository sur GitHub:"
echo "   https://github.com/new"
echo ""
echo "2. Configurer le remote:"
echo "   git remote add origin https://github.com/VOTRE_USERNAME/polar_doctor.git"
echo ""
echo "3. Définir la branche principale:"
echo "   git branch -M main"
echo ""
echo "4. Pousser le code:"
echo "   git push -u origin main"
echo ""
echo "5. Créer la première release:"
echo "   git tag -a v1.0.0 -m 'First release'"
echo "   git push origin v1.0.0"
echo ""
echo "6. GitHub Actions compilera automatiquement pour:"
echo "   🐧 Linux (Ubuntu)"
echo "   🪟 Windows (MSYS2)"
echo "   🍎 macOS (Homebrew)"
echo ""
echo "7. Les binaires seront disponibles dans:"
echo "   - Actions > Artifacts (builds de développement)"
echo "   - Releases (versions officielles)"
echo ""
echo "=========================================="
echo ""
echo "📚 Documentation:"
echo "   - README.md          - Guide utilisateur"
echo "   - BUILD.md           - Guide compilation"
echo "   - GITHUB_ACTIONS.md  - Guide CI/CD"
echo ""
echo "✅ Setup terminé !"
echo ""
