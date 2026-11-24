#!/usr/bin/env python3
"""Entwicklung: Firmware vom Laptop per WLAN auf die Brücke spielen (gleicher Weg wie die App).

    PY=~/.platformio/penv/.espidf-6.1.0/Scripts/python.exe

    # einmalig: Laptop wie ein Handy koppeln – Anfrage in der App bestätigen (oder BOOT-Taste im Entwicklermodus)
    $PY tools/ota.py pair --host 192.168.178.50

    # nach `pio run`: aktuellen Build signieren und hochladen
    $PY tools/ota.py upload --host 192.168.178.50

Voraussetzung: Die Brücke läuft schon mit einer OTA-fähigen Firmware (einmal per USB flashen).
Der Token landet in ~/.ruehrlink/bridges.json – nie im Repo. Signiert wird mit dem Schlüssel aus
firmware_sign.py (~/.ruehrlink/firmware-signing-key.pem bzw. RUEHRLINK_SIGNING_KEY).
"""
import argparse
import hashlib
import http.client
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import firmware_sign  # noqa: E402

TOKENS = Path.home() / ".ruehrlink" / "bridges.json"


def request(host, method, path, body=None, headers=None, timeout=10):
    c = http.client.HTTPConnection(host, 80, timeout=timeout)
    data = json.dumps(body).encode() if isinstance(body, dict) else body
    h = {"Content-Type": "application/json", **(headers or {})}
    c.request(method, "/api/v1/" + path, body=data, headers=h)
    r = c.getresponse()
    text = r.read().decode("utf-8", "replace")
    c.close()
    try:
        return r.status, json.loads(text) if text else {}
    except json.JSONDecodeError:
        return r.status, {"raw": text}


def load_tokens():
    return json.loads(TOKENS.read_text()) if TOKENS.exists() else {}


def token_for(host):
    t = load_tokens().get(host)
    if not t:
        sys.exit(f"Kein Token für {host} – erst: ota.py pair --host {host}")
    return t


def cmd_pair(a):
    status, res = request(a.host, "POST", "pair", {"name": "Laptop (Entwicklung)"})
    if status == 200 and "token" in res:
        token = res["token"]
    else:
        rid = res.get("id")
        if not rid:
            sys.exit(f"Kopplung nicht möglich: {status} {res}")
        print("Anfrage gestellt – jetzt in der App bestätigen (60 s) …")
        token = None
        for _ in range(30):
            time.sleep(2)
            status, res = request(a.host, "POST", "pair", {"id": rid})
            if status == 200 and "token" in res:
                token = res["token"]
                break
            if status >= 400:
                sys.exit(f"Abgelehnt/abgelaufen: {status} {res}")
        if not token:
            sys.exit("Nicht bestätigt.")
    tokens = load_tokens()
    tokens[a.host] = token
    TOKENS.parent.mkdir(parents=True, exist_ok=True)
    TOKENS.write_text(json.dumps(tokens, indent=2))
    print(f"Gekoppelt, Token gespeichert in {TOKENS}")


def cmd_upload(a):
    auth = {"Authorization": "Bearer " + token_for(a.host)}
    status, info = request(a.host, "GET", "info", headers=auth)
    if status != 200:
        sys.exit(f"Brücke antwortet nicht wie erwartet: {status} {info}")
    image = Path(a.bin).read_bytes()
    sig = firmware_sign.sign_image(firmware_sign.load_key(), image)
    version = firmware_sign.check_version(image)
    print(f"Brücke {info.get('id')}: fw {info.get('fw')} → {version} ({len(image)} Byte)")

    t0 = time.time()
    status, res = request(a.host, "POST", "ota", image, timeout=120, headers={
        **auth,
        "Content-Type": "application/octet-stream",
        "X-Sha256": hashlib.sha256(image).hexdigest(),
        "X-Signature": sig.hex(),
    })
    if status != 202:
        sys.exit(f"Update abgelehnt: {status} {res}")
    print(f"Übertragen in {time.time() - t0:.0f} s, Brücke startet neu …")

    # Erste angemeldete Anfrage bestätigt die neue Firmware (sonst Rollback nach 5 min).
    time.sleep(4)
    for _ in range(45):
        try:
            status, info = request(a.host, "GET", "info", headers=auth, timeout=3)
            if status == 200:
                fw = info.get("fw")
                print(f"Läuft wieder mit fw {fw}" + ("" if fw == version else "  ← ROLLBACK?"))
                return
        except OSError:
            pass
        time.sleep(2)
    sys.exit("Brücke meldet sich nicht (Rollback nach 5 min, falls die neue Firmware nicht hochkommt).")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(required=True)
    p = sub.add_parser("pair")
    p.add_argument("--host", required=True, help="IP oder Name der Brücke im Heim-WLAN")
    p.set_defaults(f=cmd_pair)
    u = sub.add_parser("upload")
    u.add_argument("--host", required=True)
    u.add_argument("--bin", default=str(firmware_sign.DEFAULT_BIN))
    u.set_defaults(f=cmd_upload)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
