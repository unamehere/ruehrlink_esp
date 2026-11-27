# Arbeitsweise mit Git

Gilt für alle Repos des Projekts (`ruehrlink_esp`, `ruehrlink_app`, `ruehrlink_hardware`). Stand 13.11.2026.

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

## Changelog

Jede Änderung, die man beim Benutzen merkt, bekommt im selben Commit einen Eintrag in `CHANGELOG.md`
unter **Unveröffentlicht** (Neu / Geändert / Behoben, in Nutzersprache). Reine Umbauten, Tests und Doku nicht.

## Release

1. Testplan an der echten Maschine durchlaufen (`ruehrlink_esp/docs/testplan.md`), Ergebnis eintragen.
2. `develop` → `main` mergen, Tag `vX.Y.Z` (App) bzw. `fw-vX.Y.Z` (Firmware) setzen. App und Firmware haben **dieselbe Mittelzahl** (Firmware 0.6.y ↔ App 0.6.x): Der Patch zählt je Repo unabhängig, die Mittelzahl geht gemeinsam hoch, wenn beide betroffen sind (Funktion/API). Technisch abgesichert bleibt es über `api`, `features` und `minApp` (siehe docs/updates.md).
3. Im `CHANGELOG.md` die Einträge unter „Unveröffentlicht“ in die neue Version verschieben (Datum dazu).
4. Fehler aus Tests kommen in die Fehlerliste (`Ruehrlink/user_feedback/fehlerliste.md`, lokal, nicht im Repo).

### Entwicklungsbuilds

Zwischenstände der Firmware werden nicht veröffentlicht und nie auf GitHub signiert (der Schlüssel liegt nur in
`~/.ruehrlink/`). Entweder per USB flashen (`pio run -d firmware -t upload`) oder lokal zu einer Update-Datei
packen: `pio run -d firmware`, dann
`~/.platformio/penv/Scripts/python.exe tools/firmware_sign.py pack --min-app 0.4.0 --channel beta --out ~/Downloads`
und in der App unter Mehr → Update auswählen. Die CI baut bei jedem Push nur zur Prüfung. RC oder Release gibt
es erst, wenn Niklas es sagt.

### Testversionen (Release Candidates)

Bevor eine Version fertig ist, gibt es Testversionen mit Zusatz: `0.6.1-rc.1`, `0.6.1-rc.2` … Gebaut und
veröffentlicht werden sie wie jede Version über die Pipeline (Tag `v0.6.1-rc.1` bzw. `fw-v0.6.1-rc.1`), aber immer
als Pre-Release mit „Testversion“ im Titel. Gefundene Fehler kommen in die nächste `-rc`. Ist eine Testversion an
der Maschine bestanden, wird **derselbe Stand** ohne Zusatz als `0.6.1` getaggt (Changelog: die `-rc`-Abschnitte zu
`## 0.6.1 – Datum` zusammenfassen). Die App hält `0.6.1-rc.2` für älter als `0.6.1`; Android-Build-Nummer
(`+33`) zählt immer weiter.

Keine Zugangsdaten (WLAN, Tokens, Passwörter) in Dateien im Repo – auch nicht in ignorierten.

## Beiträge und Lizenz

Dieses Repo steht unter der GPLv3 (siehe `LICENSE`), Rechteinhaberin ist die NMX Drive Systems GmbH.
Mit einem Beitrag (Pull Request, Patch) erklären Sie, dass Sie die Rechte daran haben, stellen ihn unter die GPLv3
und räumen der NMX Drive Systems GmbH zusätzlich ein einfaches, unbefristetes Recht ein, ihn auch unter anderen
Lizenzbedingungen zu nutzen und zu vertreiben (z. B. in App-Stores oder auf vorinstallierten Geräten).
