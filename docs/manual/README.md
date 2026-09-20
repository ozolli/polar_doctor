# Sources des manuels

`docs/manuel-fr.pdf` et `docs/manual-en.pdf` sont produits à partir des fichiers de ce dossier.

## Refabriquer les PDF

Dépendances : `weasyprint` (Debian/Ubuntu : `sudo apt install weasyprint`).

```bash
cd docs/manual
python3 build.py manuel-fr.html ../manuel-fr.pdf fr
python3 build.py manual-en.html ../manual-en.pdf en
```

`build.py` ajoute la couverture, numérote les titres, construit le sommaire paginé
(`target-counter`) et appelle WeasyPrint. La mise en page est dans `style.css` (typographie) et
`print.css` (pages, couverture, sommaire).

## Refaire les captures d'écran

Les images attendues sont dans `docs/manual/img/` (`diagram-fr.png`, `data-en.png`, …). Elles ne
sont pas versionnées : voici comment les régénérer.

1. Créer le bateau de démonstration :

```bash
mkdir -p ~/Bateaux/"Bon Vent"
cp docs/manual/demo-boat.cfg ~/Bateaux/"Bon Vent"/boat.cfg
cp Test/figaro3.pol ~/Bateaux/"Bon Vent"/Pres.pol
cp Test/CM50.pol    ~/Bateaux/"Bon Vent"/Portant.pol
```

2. Lancer le serveur sur le port 18081, puis le harnais de capture :

```bash
./polar_doctor_web ~/Bateaux/"Bon Vent" --port 18081 &
python3 docs/manual/shotserver.py &          # proxy de capture, port 18090
```

`shotserver.py` sert la page de l'application en y injectant un pilote : `?view=` vaut
`diagram`, `dyn`, `data`, `vmg`, `boat`, `help` ou `live`, et `?lang=` vaut `fr` ou `en`.
L'adresse `/slow` retarde l'événement `load` le temps que la page se dessine.

3. Capturer (Firefox en mode sans interface) :

```bash
for lang in fr en; do for v in diagram dyn data vmg boat help live; do
  firefox --headless --window-size 1400,1000 \
    --screenshot ~/pdshots/$v-$lang.png "http://localhost:18090/?view=$v&lang=$lang"
done; done
```

La vue `live` demande une source qui émet : le pilote se connecte en TCP sur
`127.0.0.1:2700`. Adaptez-le à votre installation.

Pour la page de connexion, lancer une seconde instance avec `WEB_AUTH=demo:demo` et faire
pointer le proxy dessus (`SHOT_APP`, `SHOT_PORT`).

> Ne jamais arrêter la capture live (`/api/live/stop`) pendant les captures d'écran : l'arrêt
> enregistre la polaire et écraserait celle du bateau de démonstration.
