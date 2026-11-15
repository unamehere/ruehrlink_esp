#!/usr/bin/env python3
"""Firmware der Brücke signieren (OTA über die App, siehe docs/updates.md).

Braucht das Paket `cryptography` – am einfachsten mit dem Python von ESP-IDF/PlatformIO:

    PY=~/.platformio/penv/.espidf-6.1.0/Scripts/python.exe

    # einmalig: Schlüsselpaar erzeugen (privat bleibt AUSSERHALB des Repos)
    $PY tools/firmware_sign.py new-key
    # öffentlichen Schlüssel erneut in die Firmware schreiben
    $PY tools/firmware_sign.py pubkey

    # nach dem Build: Image + manifest.json für die App erzeugen
    $PY tools/firmware_sign.py sign --out ../ruehrlink_app/assets/firmware

Privater Schlüssel: Umgebungsvariable RUEHRLINK_SIGNING_KEY oder ~/.ruehrlink/firmware-signing-key.pem.
Wer ihn verliert, kann ausgelieferte Brücken nur noch per USB aktualisieren – also sichern!
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

REPO = Path(__file__).resolve().parent.parent
PUBKEY_H = REPO / "firmware/components/update/signing_pubkey.h"
DEFAULT_BIN = REPO / "firmware/.pio/build/bruecke-devkit/firmware.bin"
API_VERSION = 1  # wie "api" in GET /api/v1/info


def key_path() -> Path:
    env = os.environ.get("RUEHRLINK_SIGNING_KEY")
    return Path(env) if env else Path.home() / ".ruehrlink" / "firmware-signing-key.pem"


def load_key() -> ec.EllipticCurvePrivateKey:
    p = key_path()
    if not p.exists():
        sys.exit(f"Kein privater Schlüssel unter {p} (erst 'new-key' oder RUEHRLINK_SIGNING_KEY setzen)")
    return serialization.load_pem_private_key(p.read_bytes(), password=None)


def write_pubkey(key: ec.EllipticCurvePrivateKey) -> None:
    raw = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    lines = ",\n".join("    " + ", ".join(f"0x{b:02x}" for b in raw[i:i + 13]) for i in range(0, len(raw), 13))
    PUBKEY_H.write_text(
        "// Öffentlicher Schlüssel für Firmware-Updates (ECDSA P-256, unkomprimierter Punkt).\n"
        "// Erzeugt von tools/firmware_sign.py – nicht von Hand ändern. Der private Schlüssel liegt nie im Repo.\n"
        "#pragma once\n\n#include <stdint.h>\n\n"
        f"static const uint8_t SIGNING_PUBKEY[{len(raw)}] = {{\n{lines}\n}};\n",
        encoding="utf-8", newline="\n")
    print(f"öffentlicher Schlüssel → {PUBKEY_H.relative_to(REPO)}")


def image_version(image: bytes) -> str:
    """Version aus esp_app_desc_t im Image (nach Image- und Segment-Header, Magic 0xABCD5432)."""
    if int.from_bytes(image[32:36], "little") != 0xABCD5432:
        sys.exit("Kein ESP-App-Image (esp_app_desc fehlt)")
    return image[48:80].split(bytes(1))[0].decode()


def check_version(image: bytes) -> str:
    """Build muss zur VERSION-Datei passen – sonst ist er veraltet (pio hat nicht neu konfiguriert)."""
    want = (REPO / "VERSION").read_text().strip()
    got = image_version(image)
    if got != want:
        sys.exit(f"Build hat Version {got}, VERSION sagt {want} – bitte neu bauen")
    return want


def sign_image(key: ec.EllipticCurvePrivateKey, image: bytes) -> bytes:
    """ECDSA P-256 über SHA-256 des Images, als r||s (64 Byte) – so prüft es die Brücke."""
    r, s = decode_dss_signature(key.sign(image, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def cmd_new_key(_):
    p = key_path()
    if p.exists():
        sys.exit(f"{p} existiert schon – wird nicht überschrieben")
    key = ec.generate_private_key(ec.SECP256R1())
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                    serialization.NoEncryption()))
    print(f"privater Schlüssel → {p}  (sichern! nie ins Repo)")
    write_pubkey(key)


def cmd_pubkey(_):
    write_pubkey(load_key())


def cmd_sign(a):
    key = load_key()
    image = Path(a.bin).read_bytes()
    version = check_version(image)
    sig = sign_image(key, image)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    name = f"ruehrlink-{version}.bin"
    for old in [*out.glob("ruehrlink-*.bin"), *out.glob("bruecke-*.bin")]:  # die App trägt genau eine Firmware
        old.unlink()
    (out / name).write_bytes(image)
    manifest = {
        "version": version,
        "api": API_VERSION,
        "file": name,
        "size": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "sig": sig.hex(),
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{name} ({len(image)} Byte) + manifest.json → {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(required=True)
    sub.add_parser("new-key").set_defaults(f=cmd_new_key)
    sub.add_parser("pubkey").set_defaults(f=cmd_pubkey)
    s = sub.add_parser("sign")
    s.add_argument("--bin", default=str(DEFAULT_BIN))
    s.add_argument("--out", required=True)
    s.set_defaults(f=cmd_sign)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
