# GitHub Actions

Un seul workflow : [`.github/workflows/build.yml`](.github/workflows/build.yml).

## Déclencheurs
- push sur `main` / `master`, pull request vers `main`, lancement manuel (`workflow_dispatch`) ;
- tag `v*` : compile puis **publie une release**.

## Jobs

| Job | Plateforme | Détail |
|-----|-----------|--------|
| `build-linux` | Ubuntu x86_64 | compile, **test de fumée**, archive `polar_doctor-linux-x64.tar.gz` |
| `build-linux-arm64` | ARM64 (conteneur QEMU) | idem ; lent, **toléré en échec** pour ne pas bloquer la release |
| `build-windows` | Windows x64 (MSYS2 MINGW64) | compile, test de fumée, exe + DLL, vérifie que l'exe démarre avec ses seules DLL |
| `create-release` | — | sur tag uniquement : archives, sommes SHA-256, release GitHub |

Le **test de fumée** démarre `polar_doctor_web` sur un port de test et interroge `/api/polar`
et `/` : un binaire qui compile mais ne sert rien fait échouer le job.

Chaque archive contient le binaire, le service systemd et son fichier de réglages
(`polar_doctor_web.service`, `polar_doctor_web.default`), le README et la licence.

## Publier une version

```bash
git tag -a v2.0.0 -m "Polar Doctor 2.0.0"
git push origin v2.0.0
```

Lancer le workflow sans tag (par exemple pour valider une branche) :
```bash
gh workflow run build.yml --ref <branche>
```
