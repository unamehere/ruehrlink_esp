#!/usr/bin/env python3
"""Rührlink-Werkbank: Box per USB einrichten und Etikett mit QR-Code erzeugen.

Ablauf pro Box (ein Klick in der GUI):
  1. MAC über USB auslesen (esptool) → Name Ruehrlink-XXXX, Seriennummer RL-<MAC>
  2. Einrichtungs-Code = HMAC-SHA256(Werkstattschlüssel, MAC) → 10 Zeichen ohne Verwechsler
  3. optional Firmware flashen, dann Werksdaten (Code, Seriennummer) in die Partition "fctry" schreiben
  4. Box neu starten und prüfen, dass sie den Code aus den Werksdaten übernimmt
  5. Etikett (QR + Name + Code + Seriennummer) als PNG speichern und optional drucken

Der Werkstattschlüssel liegt nur auf diesem Rechner (~/.ruehrlink/werkstatt-schluessel.bin, beim ersten
Start erzeugt – SICHERN!). Damit lässt sich jedes Etikett aus der MAC jederzeit neu erzeugen.
Etiketten und Geräteliste liegen in ~/.ruehrlink/ (nie im Repo).

    python tools/werkbank.py                               # GUI
    python tools/werkbank.py einrichten --port COM18 [--flash] [--loeschen] [--drucker "Name"]
    python tools/werkbank.py etikett --mac 84:c7:bb:2d:a1:e8 [--drucker "Name"]

Benötigt (venv): segno, esptool<5, pyserial, pillow, esp-idf-nvs-partition-gen, pywin32 (nur zum Drucken).
"""
import argparse
import csv
import datetime as dt
import hashlib
import hmac
import json
import os
import queue
import secrets
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import segno
import serial
import serial.tools.list_ports
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "firmware" / ".pio" / "build" / "ruehrlink"
HOME = Path.home() / ".ruehrlink"
KEY_FILE = HOME / "werkstatt-schluessel.bin"
LABEL_DIR = HOME / "etiketten"
DEVICE_LIST = HOME / "geraete.csv"

# Muss zur Firmware passen (components/provision/provision.c, partitions_8mb.csv)
POP_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"
POP_LEN = 10
SRP_USER = "ruehrlink"
FCTRY_OFFSET = 0x12000
FCTRY_SIZE = 0x3000
ESPRESSIF_VID = 0x303A

# Firmware-Abbilder der Platine (Offsets für ESP32-C6, siehe pio-Build)
FLASH_FILES = [(0x0, "bootloader.bin"), (0x8000, "partitions.bin"), (0xF000, "ota_data_initial.bin"),
               (0x20000, "firmware.bin")]


# ---------------------------------------------------------------------------
# Ableitung aus der MAC

def load_key(create=True) -> bytes:
    if KEY_FILE.exists():
        k = KEY_FILE.read_bytes()
        if len(k) != 32:
            raise SystemExit(f"{KEY_FILE} ist beschädigt (erwartet 32 Byte)")
        return k
    if not create:
        raise SystemExit(f"Werkstattschlüssel fehlt: {KEY_FILE}")
    HOME.mkdir(parents=True, exist_ok=True)
    k = secrets.token_bytes(32)
    KEY_FILE.write_bytes(k)
    print(f"Neuer Werkstattschlüssel → {KEY_FILE}  (SICHERN! Ohne ihn keine Nachdrucke für bestehende Boxen)")
    return k


def norm_mac(mac: str) -> str:
    h = "".join(c for c in mac.lower() if c in "0123456789abcdef")
    if len(h) == 16 and h[6:10] == "fffe":   # EUI-64 aus esptool (84:c7:bb:ff:fe:2d:a1:e8)
        h = h[:6] + h[10:]
    if len(h) != 12:
        raise ValueError(f"keine gültige MAC: {mac}")
    return h


def derive(key: bytes, mac: str) -> dict:
    m = norm_mac(mac)
    digest = hmac.new(key, f"ruehrlink-pop-v1:{m}".encode(), hashlib.sha256).digest()
    n = int.from_bytes(digest, "big")
    pop = ""
    for _ in range(POP_LEN):
        n, r = divmod(n, len(POP_ALPHABET))
        pop += POP_ALPHABET[r]
    name = f"Ruehrlink-{m[8:].upper()}"
    return {"mac": ":".join(m[i:i + 2] for i in range(0, 12, 2)), "name": name, "serial": f"RL-{m.upper()}",
            "pop": pop}


def qr_payload(d: dict) -> str:
    return json.dumps({"ver": "v1", "name": d["name"], "pop": d["pop"], "transport": "ble", "security": 2,
                       "username": SRP_USER}, separators=(",", ":"))


def pop_display(pop: str) -> str:
    return f"{pop[:5]}-{pop[5:]}"


# ---------------------------------------------------------------------------
# Etikett (Bondrucker: 58 mm ≈ 384 Punkte, 80 mm ≈ 576 Punkte bei 203 dpi)

def _font(size, bold=False):
    for name in (("segoeuib.ttf", "arialbd.ttf") if bold else ("segoeui.ttf", "arial.ttf")):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def make_label(d: dict, width_px=384) -> Image.Image:
    pad = width_px // 24
    qr = segno.make(qr_payload(d), error="m")
    modules = qr.symbol_size(border=2)[0]
    scale = max(1, (width_px - 2 * pad) // modules)
    import io
    buf = io.BytesIO()
    qr.save(buf, kind="png", scale=scale, border=2)
    qimg = Image.open(io.BytesIO(buf.getvalue())).convert("L")

    lines = [("Rührlink", _font(width_px // 9, True)),
             (d["name"], _font(width_px // 14, True)),
             ("Einrichtungs-Code", _font(width_px // 20)),
             (pop_display(d["pop"]), _font(width_px // 10, True)),
             (d["serial"], _font(width_px // 22))]
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    heights = [probe.textbbox((0, 0), t, font=f)[3] + pad // 2 for t, f in lines]
    h = pad + heights[0] + qimg.height + sum(heights[1:]) + pad
    img = Image.new("L", (width_px, h), 255)
    dr = ImageDraw.Draw(img)
    y = pad

    def center(text, font, y):
        w = dr.textbbox((0, 0), text, font=font)[2]
        dr.text(((width_px - w) // 2, y), text, font=font, fill=0)

    center(*lines[0], y)
    y += heights[0]
    img.paste(qimg, ((width_px - qimg.width) // 2, y))
    y += qimg.height
    for (t, f), hh in zip(lines[1:], heights[1:]):
        center(t, f, y)
        y += hh
    return img.convert("1", dither=Image.Dither.NONE)


def save_label(d: dict, img: Image.Image) -> Path:
    LABEL_DIR.mkdir(parents=True, exist_ok=True)
    p = LABEL_DIR / f"{d['serial']}.png"
    img.save(p, dpi=(203, 203))
    return p


BT_PRINTER = "Bluetooth-Thermodrucker (MX…)"


def list_printers() -> list:
    """Bluetooth-Mini-Drucker zuerst (Callstel/MX10), dann die Windows-Drucker."""
    return [BT_PRINTER] + _windows_printers()


def _windows_printers() -> list:
    try:
        import win32print
        return [p[2] for p in win32print.EnumPrinters(win32print.PRINTER_ENUM_LOCAL | win32print.PRINTER_ENUM_CONNECTIONS)]
    except Exception:
        return []


def print_label(img: Image.Image, printer: str, log=print):
    """Druckt das Etikett in voller druckbarer Breite (Bondrucker: Breite = Papier, Länge = Bild)."""
    if printer == BT_PRINTER:
        import bt_drucker
        bt_drucker.print_image(img, log=log)
        return
    import win32con
    import win32ui
    from PIL import ImageWin
    dc = win32ui.CreateDC()
    dc.CreatePrinterDC(printer)
    pw = dc.GetDeviceCaps(win32con.HORZRES)
    ph = int(img.height * pw / img.width)
    dc.StartDoc("Rührlink-Etikett")
    dc.StartPage()
    ImageWin.Dib(img.convert("RGB")).draw(dc.GetHandleOutput(), (0, 0, pw, ph))
    dc.EndPage()
    dc.EndDoc()
    dc.DeleteDC()


def record(d: dict):
    HOME.mkdir(parents=True, exist_ok=True)
    new = not DEVICE_LIST.exists()
    with open(DEVICE_LIST, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["datum", "seriennummer", "name", "mac"])   # Code steht bewusst nicht drin (ableitbar)
        w.writerow([dt.datetime.now().isoformat(timespec="seconds"), d["serial"], d["name"], d["mac"]])


# ---------------------------------------------------------------------------
# Box über USB

def find_ports() -> list:
    return [p.device for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_VID]


def esptool(port: str, *args, log=print) -> str:
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", port, *args]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        log(r.stdout[-1500:] + r.stderr[-1500:])
        raise RuntimeError(f"esptool {' '.join(args[:3])} fehlgeschlagen")
    return r.stdout


def read_mac(port: str, log=print) -> str:
    out = esptool(port, "--after", "no_reset", "read_mac", log=log)
    for line in out.splitlines():
        if line.startswith("BASE MAC:"):
            return line.split(":", 1)[1].strip()
    for line in out.splitlines():
        if line.startswith("MAC:"):
            return line.split(":", 1)[1].strip()
    raise RuntimeError("MAC nicht gefunden")


def check_partition_table(port: str, log=print):
    """Werksdaten nur schreiben, wenn auf der Box eine Tabelle mit "fctry" an der erwarteten Stelle liegt."""
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "pt.bin")
        esptool(port, "--after", "no_reset", "read_flash", "0x8000", "0xC00", p, log=log)
        data = Path(p).read_bytes()
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        off, size = int.from_bytes(e[4:8], "little"), int.from_bytes(e[8:12], "little")
        label = e[12:28].split(b"\0")[0].decode(errors="replace")
        if label == "fctry":
            if (off, size) != (FCTRY_OFFSET, FCTRY_SIZE):
                raise RuntimeError(f"fctry liegt bei {off:#x}/{size:#x}, erwartet {FCTRY_OFFSET:#x}/{FCTRY_SIZE:#x}")
            return
    raise RuntimeError("Auf der Box fehlt die Partition fctry – zuerst Firmware für die Platine flashen (--flash)")


def firmware_version() -> str | None:
    """Version aus esp_app_desc_t des letzten Builds (wie tools/firmware_sign.py)."""
    p = BUILD / "firmware.bin"
    if not p.exists():
        return None
    img = p.read_bytes()[:128]
    if int.from_bytes(img[32:36], "little") != 0xABCD5432:
        return "?"
    return img[48:80].split(bytes(1))[0].decode(errors="replace")


def erase_all(port: str, log=print):
    log("Lösche den kompletten Flash …")
    esptool(port, "--after", "no_reset", "erase_flash", log=log)


def flash_firmware(port: str, log=print):
    files = []
    for off, name in FLASH_FILES:
        p = BUILD / name
        if not p.exists():
            raise RuntimeError(f"{p} fehlt – zuerst 'pio run' im firmware-Ordner")
        files += [hex(off), str(p)]
    log(f"Flashe Firmware {firmware_version()} …")
    esptool(port, "--after", "no_reset", "write_flash", *files, log=log)


def write_factory(port: str, d: dict, log=print):
    rows = [("key", "type", "encoding", "value"), ("fctry", "namespace", "", ""),
            ("pop", "data", "string", d["pop"]), ("serial", "data", "string", d["serial"])]
    with tempfile.TemporaryDirectory() as tmp:
        c, b = os.path.join(tmp, "f.csv"), os.path.join(tmp, "f.bin")
        with open(c, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(rows)
        r = subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", c, b, hex(FCTRY_SIZE)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            log(r.stdout + r.stderr)
            raise RuntimeError("Werksdaten-Abbild konnte nicht erzeugt werden")
        esptool(port, "--after", "no_reset", "write_flash", hex(FCTRY_OFFSET), b, log=log)


def _open(port, wait=10.0):
    end = time.time() + wait
    while True:
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 0.1
            s.dtr = False   # DTR=1 beim Reset → Download-Modus (USB-Serial/JTAG)
            s.rts = False
            s.open()
            return s
        except serial.SerialException:
            if time.time() > end:
                raise
            time.sleep(0.05)


def reset_and_check(port: str, d: dict, log=print, timeout=20.0) -> bool:
    """Neustart (RTS-Puls mit DTR=0; Windows schickt RTS erst mit einer DTR-Änderung) und Log prüfen."""
    s = _open(port)
    s.setDTR(False)
    s.setRTS(True)
    s.setDTR(False)
    time.sleep(0.2)
    s.setRTS(False)
    s.setDTR(False)
    s.close()
    time.sleep(0.2)
    s = _open(port)
    buf, end = "", time.time() + timeout
    want_serial = f"Werksdaten: Seriennummer {d['serial']}"
    want_code = "Einrichtungs-Code aus den Werksdaten"
    try:
        while time.time() < end:
            try:
                buf += s.read(4096).decode("utf-8", "replace")
            except serial.SerialException:
                s.close()
                s = _open(port)
            if want_serial in buf and want_code in buf and f"aktiv als {d['name']}" in buf:
                return True
    finally:
        s.close()
    log("Box hat die Werksdaten nicht bestätigt. Letzte Ausgabe:\n" + buf[-1500:])
    return False


def setup_box(port: str, flash: bool, printer: str | None, width_px: int, log=print, erase=False):
    """Kompletter Ablauf für eine Box – auch eine fabrikneue mit leerem Flash."""
    key = load_key()
    log(f"Lese MAC von {port} …")
    d = derive(key, read_mac(port, log))
    log(f"{d['name']}  {d['serial']}")
    if erase:
        erase_all(port, log)
    if flash or erase:
        flash_firmware(port, log)
    try:
        check_partition_table(port, log)
    except RuntimeError:
        if flash or erase:
            raise
        log("Keine Rührlink-Firmware auf der Box – flashe sie jetzt.")
        flash_firmware(port, log)
        check_partition_table(port, log)
    log("Schreibe Werksdaten …")
    write_factory(port, d, log)
    log("Starte Box neu und prüfe …")
    ok = reset_and_check(port, d, log)
    img = make_label(d, width_px)
    p = save_label(d, img)
    log(f"Etikett gespeichert: {p}")
    if ok:
        record(d)
        log("✔ Box eingerichtet")
        if printer:
            print_label(img, printer, log)
            log(f"Gedruckt auf {printer}")
    else:
        log("✘ Prüfung fehlgeschlagen – Etikett nicht gedruckt")
    return d, img, ok


# ---------------------------------------------------------------------------
# GUI

def gui():
    import tkinter as tk
    from tkinter import ttk, messagebox
    from PIL import ImageTk

    root = tk.Tk()
    root.title("Rührlink-Werkbank")
    root.geometry("860x620")
    q: queue.Queue = queue.Queue()

    def log(msg):
        q.put(("log", str(msg)))

    left = ttk.Frame(root, padding=12)
    left.pack(side="left", fill="both", expand=True)
    right = ttk.Frame(root, padding=12)
    right.pack(side="right", fill="y")

    ttk.Label(left, text="Rührlink-Werkbank", font=("Segoe UI", 16, "bold")).pack(anchor="w")
    ttk.Label(left, text="Box per USB anstecken → Einrichten. Code wird aus der MAC berechnet.").pack(anchor="w")

    f = ttk.Frame(left)
    f.pack(fill="x", pady=8)
    ttk.Label(f, text="Port").grid(row=0, column=0, sticky="w")
    port = ttk.Combobox(f, width=12, state="readonly")
    port.grid(row=0, column=1, sticky="w", padx=4)

    def refresh():
        ports = find_ports()
        port["values"] = ports
        if ports and port.get() not in ports:
            port.set(ports[0])
        if not ports:
            port.set("")
    ttk.Button(f, text="↻", width=3, command=refresh).grid(row=0, column=2)

    ttk.Label(f, text="Drucker").grid(row=1, column=0, sticky="w", pady=4)
    printers = ["(nicht drucken)"] + list_printers()
    printer = ttk.Combobox(f, values=printers, width=36, state="readonly")
    printer.set(BT_PRINTER)
    printer.grid(row=1, column=1, columnspan=3, sticky="w", padx=4)

    ttk.Label(f, text="Papier").grid(row=2, column=0, sticky="w")
    paper = ttk.Combobox(f, values=["58 mm", "80 mm"], width=8, state="readonly")
    paper.set("58 mm")
    paper.grid(row=2, column=1, sticky="w", padx=4)

    fw = firmware_version()
    flash = tk.BooleanVar(value=fw is not None)
    ttk.Checkbutton(f, text=f"Firmware flashen ({fw or 'kein Build gefunden'})", variable=flash).grid(
        row=3, column=0, columnspan=4, sticky="w", pady=(4, 0))
    erase = tk.BooleanVar(value=False)
    ttk.Checkbutton(f, text="Vorher alles löschen (gebrauchte Box: WLAN, Kopplungen, Rezepte im Flash)",
                    variable=erase).grid(row=4, column=0, columnspan=4, sticky="w")
    auto = tk.BooleanVar(value=False)
    ttk.Checkbutton(f, text="Automatisch starten, sobald eine Box angesteckt wird", variable=auto).grid(
        row=5, column=0, columnspan=4, sticky="w")
    ttk.Label(f, foreground="#666", text="Leere Box wird nicht erkannt? BOOT-Taste halten, USB anstecken, loslassen.").grid(
        row=6, column=0, columnspan=4, sticky="w", pady=(2, 0))

    def width_px():
        return 576 if paper.get().startswith("80") else 384

    def chosen_printer():
        p = printer.get()
        return None if p.startswith("(") else p

    btns = ttk.Frame(left)
    btns.pack(fill="x")
    go = ttk.Button(btns, text="Einrichten")
    go.pack(side="left", ipadx=20, ipady=8)

    reprint_var = tk.StringVar()
    ttk.Label(btns, text="   Nachdruck (MAC):").pack(side="left")
    ttk.Entry(btns, textvariable=reprint_var, width=20).pack(side="left", padx=4)

    out = tk.Text(left, height=18, wrap="word", font=("Consolas", 9))
    out.pack(fill="both", expand=True, pady=8)

    preview = ttk.Label(right)
    preview.pack()
    current = {"img": None, "label": None, "name": "etikett"}

    def export():
        from tkinter import filedialog
        if current["label"] is None:
            messagebox.showinfo("Rührlink", "Noch kein Etikett – erst einrichten oder Nachdruck erzeugen.")
            return
        p = filedialog.asksaveasfilename(
            title="Etikett speichern", initialfile=current["name"], defaultextension=".png",
            filetypes=[("PNG-Bild", "*.png"), ("PDF", "*.pdf")])
        if not p:
            return
        im = current["label"]
        if p.lower().endswith(".pdf"):
            im.convert("RGB").save(p, resolution=203)
        else:
            im.save(p, dpi=(203, 203))
        log(f"Exportiert: {p}")

    def print_current():
        pr = chosen_printer()
        if current["label"] is None or not pr:
            messagebox.showinfo("Rührlink", "Etikett und Drucker wählen.")
            return
        im = current["label"]
        run(lambda: (print_label(im, pr, log), log(f"Gedruckt auf {pr}")))

    ex = ttk.Frame(right)
    ex.pack(fill="x", pady=8)
    ttk.Button(ex, text="Speichern unter …", command=export).pack(fill="x")
    ttk.Button(ex, text="Drucken", command=print_current).pack(fill="x", pady=4)
    ttk.Button(ex, text="Etiketten-Ordner öffnen",
               command=lambda: (LABEL_DIR.mkdir(parents=True, exist_ok=True), os.startfile(LABEL_DIR))).pack(fill="x")

    def run(job):
        go.state(["disabled"])
        threading.Thread(target=lambda: _job(job), daemon=True).start()

    def _job(job):
        try:
            job()
        except Exception as e:  # noqa: BLE001 – Fehler in der GUI anzeigen
            log(f"Fehler: {e}")
        q.put(("done", None))

    def do_setup():
        p = port.get()
        if not p:
            messagebox.showwarning("Rührlink", "Keine Box gefunden – USB-Kabel prüfen und ↻ drücken.")
            return
        out.delete("1.0", "end")
        pr, w, fl, er = chosen_printer(), width_px(), flash.get(), erase.get()

        def job():
            d, img, ok = setup_box(p, fl, pr, w, log, erase=er)
            q.put(("img", (img, d["serial"])))
        run(job)

    def do_reprint():
        mac = reprint_var.get().strip()
        if not mac:
            return
        pr, w = chosen_printer(), width_px()

        def job():
            d = derive(load_key(create=False), mac)
            img = make_label(d, w)
            log(f"Etikett {d['name']} gespeichert: {save_label(d, img)}")
            if pr:
                print_label(img, pr, log)
                log(f"Gedruckt auf {pr}")
            q.put(("img", (img, d["serial"])))
        run(job)

    go.configure(command=do_setup)
    ttk.Button(btns, text="Etikett", command=do_reprint).pack(side="left")

    def poll():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    out.insert("end", val + "\n")
                    out.see("end")
                elif kind == "img":
                    current["label"], current["name"] = val
                    im = val[0].convert("L")
                    im.thumbnail((300, 520))
                    current["img"] = ImageTk.PhotoImage(im)
                    preview.configure(image=current["img"])
                elif kind == "done":
                    go.state(["!disabled"])
        except queue.Empty:
            pass
        root.after(100, poll)

    known = {"ports": set(find_ports())}

    def auto_refresh():
        if "disabled" not in go.state():
            refresh()
            now = set(port["values"] or ())
            new = now - known["ports"]
            known["ports"] = now
            if auto.get() and new:
                port.set(sorted(new)[0])
                log(f"Neue Box an {port.get()} – starte …")
                root.after(1500, do_setup)   # USB erst zur Ruhe kommen lassen
        root.after(1000, auto_refresh)

    load_key()   # beim ersten Start anlegen (Hinweis auf der Konsole)
    refresh()
    poll()
    auto_refresh()
    root.mainloop()


def main():
    for stream in (sys.stdout, sys.stderr):   # Windows-Konsole: Umlaute/Pfeile nicht abstürzen lassen
        if stream and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    a = sub.add_parser("einrichten", help="Box an --port einrichten")
    a.add_argument("--port", help="COM-Port (Standard: erste Rührlink-Box)")
    a.add_argument("--flash", action="store_true", help="vorher Firmware aus dem pio-Build flashen")
    a.add_argument("--loeschen", action="store_true", help="vorher den kompletten Flash löschen")
    a.add_argument("--drucker")
    a.add_argument("--papier", type=int, choices=[58, 80], default=58)
    e = sub.add_parser("etikett", help="Etikett aus der MAC neu erzeugen")
    e.add_argument("--mac", required=True)
    e.add_argument("--drucker")
    e.add_argument("--papier", type=int, choices=[58, 80], default=58)
    args = ap.parse_args()
    if args.cmd is None:
        return gui()
    w = 576 if args.papier == 80 else 384
    if args.cmd == "einrichten":
        p = args.port or (find_ports() or [None])[0]
        if not p:
            raise SystemExit("Keine Rührlink-Box an USB gefunden")
        _, _, ok = setup_box(p, args.flash, args.drucker, w, erase=args.loeschen)
        return 0 if ok else 1
    d = derive(load_key(create=False), args.mac)
    img = make_label(d, w)
    print(f"{d['name']} {d['serial']} → {save_label(d, img)}")
    if args.drucker:
        print_label(img, args.drucker)
    return 0


if __name__ == "__main__":
    sys.exit(main())
