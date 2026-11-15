#!/usr/bin/env python3
"""Entwicklung: Rezept- und Foto-Endpunkte einer laufenden Brücke durchprüfen (docs/api.md, Testplan 6).

    PY=~/.platformio/penv/.espidf-6.1.0/Scripts/python.exe
    $PY tools/ota.py pair --host 192.168.178.50      # einmalig, Token landet in ~/.ruehrlink/bridges.json
    $PY tools/recipes_check.py --host 192.168.178.50

Legt ein Testrezept an, ändert und löscht es wieder (bleibt als Tombstone stehen) und lädt ein
winziges Test-Foto hoch. Gibt pro Prüfung eine Zeile aus und endet mit Fehlercode, wenn etwas nicht passt.
"""
import argparse
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ota import request, token_for  # noqa: E402

failed = 0


def check(name, ok, detail=""):
    global failed
    print(("ok    " if ok else "FEHLER") + "  " + name + ("" if ok else f"  → {detail}"))
    if not ok:
        failed += 1


def fake_webp(payload: bytes) -> bytes:
    # Nur die Kennung muss stimmen – die Brücke dekodiert keine Bilder.
    body = b"WEBP" + payload
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    a = ap.parse_args()
    auth = {"Authorization": "Bearer " + token_for(a.host)}

    def req(method, path, body=None, headers=None):
        return request(a.host, method, path, body, headers={**auth, **(headers or {})}, timeout=20)

    s, lib = req("GET", "recipes")
    check("GET /recipes", s == 200 and "rev" in lib and isinstance(lib.get("changed"), list), (s, lib))
    rev0 = lib.get("rev", 0)

    rid = str(uuid.uuid4())
    recipe = {"title": "Testrezept", "servings": 2, "photo": None, "tags": ["Test"],
              "ingredients": [{"amount": 250, "unit": "g", "name": "Mehl"}],
              "steps": [{"text": "Mehl wiegen.", "weigh": {"target": 250, "unit": "g"}},
                        {"text": "Rühren.", "machine": {"temperature": 0, "time": 30, "speed": 5}}],
              "zukunft": {"bleibt": True}}
    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": "0"})
    check("PUT neu (If-Match 0)", s == 200 and r.get("rev", 0) > rev0, (s, r))
    rev1 = r.get("rev")

    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": "0"})
    check("PUT neu, gibt es schon → 412", s == 412 and r.get("rev") == rev1, (s, r))

    s, r = req("GET", f"recipes/{rid}")
    check("GET Rezept", s == 200 and r.get("id") == rid and r.get("rev") == rev1 and r.get("v") == 1
          and r.get("zukunft") == {"bleibt": True}, (s, r))

    recipe["title"] = "Testrezept geändert"
    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": f'"{rev1}"'})
    check("PUT ändern (If-Match rev)", s == 200 and r.get("rev") == rev1 + 1, (s, r))
    rev2 = r.get("rev")
    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": str(rev1)})
    check("PUT mit alter Revision → 412", s == 412 and r.get("rev") == rev2, (s, r))

    s, lib = req("GET", f"recipes?since={rev0}")
    mine = [x for x in lib.get("changed", []) if x.get("id") == rid]
    check("since: geändertes Rezept dabei", s == 200 and len(mine) == 1 and mine[0].get("rev") == rev2, (s, lib))

    s, r = req("PUT", f"recipes/{rid}", {"servings": 2}, {"If-Match": str(rev2)})
    check("PUT ohne Titel → 400 title", s == 400 and r.get("error") == "title", (s, r))
    s, r = req("PUT", f"recipes/{rid}", b"{kaputt", {"If-Match": str(rev2)})
    check("PUT kaputtes JSON → 400 json", s == 400 and r.get("error") == "json", (s, r))
    big = dict(recipe, title="x", notes="y" * 17000)
    s, r = req("PUT", f"recipes/{rid}", big, {"If-Match": str(rev2)})
    check("PUT > 16 KB → 413", s == 413, (s, r))
    s, r = req("PUT", "recipes/GROSS", recipe)
    check("PUT ungültige id → 400 id", s == 400 and r.get("error") == "id", (s, r))

    # Fotos
    photo = fake_webp(uuid.uuid4().bytes * 3000)  # ~48 KB
    sha = hashlib.sha256(photo).hexdigest()
    s, r = req("PUT", f"media/{sha}", photo, {"Content-Type": "image/webp"})
    check("PUT Foto → 201", s == 201, (s, r))
    s, r = req("PUT", f"media/{sha}", photo, {"Content-Type": "image/webp"})
    check("PUT Foto nochmal → 200", s == 200, (s, r))
    s, r = req("PUT", f"media/{'0' * 64}", photo, {"Content-Type": "image/webp"})
    check("PUT Foto falsche Prüfsumme → 400 sha256", s == 400 and r.get("error") == "sha256", (s, r))
    png = b"\x89PNG" + b"x" * 100
    s, r = req("PUT", f"media/{hashlib.sha256(png).hexdigest()}", png, {"Content-Type": "image/webp"})
    check("PUT kein WebP → 400 format", s == 400 and r.get("error") == "format", (s, r))
    huge = fake_webp(b"z" * (1024 * 1024))
    s, r = req("PUT", f"media/{hashlib.sha256(huge).hexdigest()}", huge, {"Content-Type": "image/webp"})
    check("PUT Foto > 1 MB → 413", s == 413, (s, r))
    import http.client
    c = http.client.HTTPConnection(a.host, 80, timeout=20)
    c.request("GET", f"/api/v1/media/{sha}", headers=auth)
    resp = c.getresponse()
    data = resp.read()
    c.close()
    check("GET Foto = hochgeladene Bytes", resp.status == 200 and data == photo, resp.status)

    recipe["photo"] = sha
    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": str(rev2)})
    check("PUT mit Foto", s == 200, (s, r))
    rev3 = r.get("rev")

    # Löschen
    s, r = req("DELETE", f"recipes/{rid}", headers={"If-Match": str(rev2)})
    check("DELETE mit alter Revision → 412", s == 412 and r.get("rev") == rev3, (s, r))
    s, r = req("DELETE", f"recipes/{rid}", headers={"If-Match": str(rev3)})
    check("DELETE", s == 200 and r.get("deleted") is True, (s, r))
    rev4 = r.get("rev")
    s, r = req("GET", f"recipes/{rid}")
    check("GET gelöscht → 404 deleted", s == 404 and r.get("error") == "deleted", (s, r))
    s, lib = req("GET", f"recipes?since={rev3}")
    check("since: Löschung dabei", s == 200 and rid in lib.get("deleted", []) and lib.get("rev") == rev4, (s, lib))
    s, lib = req("GET", "recipes")
    check("ohne since: keine Löschungen", s == 200 and lib.get("deleted") == [], (s, lib))
    s, r = req("PUT", f"recipes/{rid}", recipe, {"If-Match": str(rev3)})
    check("PUT auf gelöschtes → 412 deleted", s == 412 and r.get("deleted") is True, (s, r))

    print(json.dumps({"fehler": failed}))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
