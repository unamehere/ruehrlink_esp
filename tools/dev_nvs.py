#!/usr/bin/env python3
"""Entwicklung: WLAN-Zugangsdaten direkt in den NVS-Speicher der Brücke schreiben.

Die Daten landen nur auf dem Gerät – nie in einer Datei im Repo. Das NVS-Abbild entsteht
in einem temporären Ordner und wird nach dem Flashen gelöscht. Wird überflüssig, sobald
die Bluetooth-Einrichtung fertig ist.

    python tools/dev_nvs.py --port COM3            # fragt die Werte ab
    python tools/dev_nvs.py --port COM3 --stdin    # liest JSON von stdin:
        {"home_ssid": "...", "home_pass": "...", "ap_ssid": "...", "ap_pass": "..."}

ACHTUNG: überschreibt den kompletten NVS (gespeichertes Heim-WLAN, AP-Passwort, Token).

ap_ssid/ap_pass sind optional: nur nötig, wenn die Maschine schon mit einem anderen
Netz gekoppelt ist (z. B. dem alten Prototyp) und ohne neue Kopplung verbinden soll.
"""
import argparse
import csv
import getpass
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

NVS_OFFSET = "0x9000"   # siehe firmware/partitions.csv
NVS_SIZE = "0x6000"
NAMESPACE = "net"

PIO = Path.home() / ".platformio"
ESPTOOL = PIO / "packages" / "tool-esptoolpy" / "esptool.py"


def idf_python() -> str:
    """Python der ESP-IDF-Umgebung von PlatformIO (enthält esp_idf_nvs_partition_gen)."""
    for env in sorted(PIO.glob("penv/.espidf-*"), reverse=True):
        for exe in (env / "Scripts" / "python.exe", env / "bin" / "python"):
            if exe.exists():
                return str(exe)
    sys.exit("ESP-IDF-Python nicht gefunden - einmal 'pio run' im firmware-Ordner ausfuehren.")

def ask() -> dict:
    v = {"home_ssid": input("Heim-WLAN SSID: ").strip()}
    v["home_pass"] = getpass.getpass("Heim-WLAN Passwort: ")
    v["ap_ssid"] = input("Maschinen-WLAN SSID ueberschreiben (leer = Freikoch-XXXX): ").strip()
    if v["ap_ssid"]:
        v["ap_pass"] = getpass.getpass("Maschinen-WLAN Passwort: ")
    return v


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--stdin", action="store_true", help="Werte als JSON von stdin lesen")
    args = ap.parse_args()

    v = json.load(sys.stdin) if args.stdin else ask()
    rows = [("key", "type", "encoding", "value"), (NAMESPACE, "namespace", "", "")]
    for key in ("home_ssid", "home_pass", "ap_ssid", "ap_pass"):
        if v.get(key):
            rows.append(("dev_" + key, "data", "string", v[key]))

    with tempfile.TemporaryDirectory() as tmp:
        csv_path = os.path.join(tmp, "nvs.csv")
        bin_path = os.path.join(tmp, "nvs.bin")
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(rows)
        subprocess.run([idf_python(), "-m", "esp_idf_nvs_partition_gen", "generate", csv_path, bin_path, NVS_SIZE],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(ESPTOOL), "--chip", "esp32s3", "--port", args.port,
                        "write_flash", NVS_OFFSET, bin_path], check=True)
    print("NVS geschrieben (%d Eintraege). Temporaere Dateien geloescht." % (len(rows) - 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
