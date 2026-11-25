#!/usr/bin/env python3
"""Firmware-Release abschließen: von GitHub gebautes Image lokal signieren und veröffentlichen.

Ablauf (Details: docs/updates.md, lokal):
  1. VERSION + CHANGELOG.md setzen, committen, pushen.
  2. git tag fw-v<version> && git push origin fw-v<version>
     → GitHub baut die Firmware und legt einen Release-Entwurf mit dem unsignierten Image an (~10 min).
  3. python tools/release.py            (Beta, als Pre-Release)
     python tools/release.py --stable   (stabil, als „Latest“)

Das Skript lädt das unsignierte Image, signiert es mit ~/.ruehrlink/firmware-signing-key.pem zur
.rlfw (tools/firmware_sign.py), lädt die .rlfw hoch, entfernt das unsignierte Image und veröffentlicht
den Entwurf. Eine Kopie landet in ~/Downloads. Der Schlüssel verlässt den Rechner nie.

Braucht die GitHub CLI (gh, angemeldet) und die Pakete von firmware_sign.py.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GH_WINDOWS = Path(r"C:\Program Files\GitHub CLI\gh.exe")


def gh_exe() -> str:
    exe = shutil.which("gh") or (str(GH_WINDOWS) if GH_WINDOWS.exists() else None)
    if not exe:
        sys.exit("GitHub CLI (gh) nicht gefunden.")
    return exe


def gh(*args: str, capture: bool = False) -> str:
    r = subprocess.run([gh_exe(), *args], cwd=REPO, text=True, encoding="utf-8",
                       capture_output=capture)
    if r.returncode:
        sys.exit(f"gh {' '.join(args)} fehlgeschlagen" + (f":\n{r.stderr}" if capture else ""))
    return r.stdout if capture else ""


def changelog_section(version: str) -> str:
    lines, inside = [], False
    for line in (REPO / "CHANGELOG.md").read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            if inside:
                break
            inside = line.startswith(f"## {version} ")
            continue
        if inside:
            lines.append(line)
    text = "\n".join(lines).strip()
    if not text:
        sys.exit(f"Kein Abschnitt '## {version} …' in CHANGELOG.md.")
    return text


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=(REPO / "VERSION").read_text().strip(),
                    help="Firmware-Version (Standard: Datei VERSION)")
    ap.add_argument("--min-app", default="0.4.0", help="älteste App, die mit dieser Firmware zurechtkommt")
    ap.add_argument("--stable", action="store_true", help="stabiles Release statt Beta/Pre-Release")
    ap.add_argument("--security", action="store_true", help="Sicherheitsupdate")
    a = ap.parse_args()

    v, tag = a.version, f"fw-v{a.version}"
    info = gh("release", "view", tag, "--json", "assets", "--jq", ".assets[].name", capture=True).split()
    unsigned = f"firmware-{v}-unsigniert.bin"
    if unsigned not in info:
        sys.exit(f"Release {tag} hat kein {unsigned} – ist der GitHub-Lauf fertig? "
                 f"(gh run list --workflow release.yml)")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        gh("release", "download", tag, "-p", unsigned, "-D", str(tmp))
        notes = tmp / "notes.md"
        notes.write_text(changelog_section(v), encoding="utf-8")
        cmd = [sys.executable, str(REPO / "tools" / "firmware_sign.py"), "pack",
               "--bin", str(tmp / unsigned), "--out", str(tmp / "out"),
               "--min-app", a.min_app, "--notes-file", str(notes),
               "--channel", "stable" if a.stable else "beta"]
        if a.security:
            cmd.append("--security")
        subprocess.run(cmd, check=True)
        rlfw = tmp / "out" / f"ruehrlink-{v}.rlfw"
        gh("release", "upload", tag, str(rlfw), "--clobber")
        gh("release", "delete-asset", tag, unsigned, "--yes")
        flags = ["--prerelease=false", "--latest"] if a.stable else ["--prerelease"]
        gh("release", "edit", tag, "--draft=false", *flags)
        dl = Path(os.path.expanduser("~")) / "Downloads" / rlfw.name
        shutil.copy2(rlfw, dl)

    url = gh("release", "view", tag, "--json", "url", "--jq", ".url", capture=True).strip()
    print(f"Veröffentlicht: {url}\nKopie: {dl}")
    print(f"Für die App: FIRMWARE_VERSION in ruehrlink_app auf {v} setzen (die App-Pipeline holt die .rlfw von hier).")


if __name__ == "__main__":
    main()
