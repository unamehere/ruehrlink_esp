# Arbeitsweise mit Git

Gilt für alle Repos des Projekts (`freikoch_esp`, `freikoch_app`, `freikoch_hardware`). Stand 13.11.2026.

## Branches

| Branch | Zweck | entsteht aus | geht zurück in |
|---|---|---|---|
| `main` (Hardware: `master`) | getesteter Stand – das, was auf Beta-Geräten läuft | – | – |
| `develop` | Integration: alles Fertige, noch nicht an der Maschine getestet | `main` | `main` (Release) |
| `feature/<kurz>` | neue Funktion, z. B. `feature/sequencer` | `develop` | `develop` |
| `fix/<kurz>` | Fehler aus der Entwicklung, z. B. `fix/schuessel-status` | `develop` | `develop` |
| `hotfix/<kurz>` | dringender Fehler im ausgelieferten Stand | `main` | `main` **und** `develop` |

- Zusammenführen immer mit `git merge --no-ff`, damit der Branch in der Historie sichtbar bleibt.
- Kleine reine Doku-Änderungen dürfen direkt auf `develop`.

## Commit-Nachrichten

[Conventional Commits](https://www.conventionalcommits.org/de/), Text auf Deutsch:

```
<typ>(<bereich>): <was, im Imperativ oder als Ergebnis>

<warum, Befund, Bezug auf die Fehlerliste (F2) oder den Testplan>
```

Typen: `feat` (Funktion) · `fix` (Fehler) · `docs` · `style` (Formatierung) · `refactor` · `test` ·
`build` (Bauen, Fertigungsdaten) · `chore` (Sonstiges).

## Release

1. Testplan an der echten Maschine durchlaufen (`freikoch_esp/docs/testplan.md`), Ergebnis eintragen.
2. `develop` → `main` mergen, Tag `vX.Y.Z` setzen (App-Version = Firmware-Version, sobald OTA steht).
3. Fehler aus Tests kommen in die Fehlerliste (`Freikoch/user_feedback/fehlerliste.md`, lokal, nicht im Repo).

Keine Zugangsdaten (WLAN, Tokens, Passwörter) in Dateien im Repo – auch nicht in ignorierten.
