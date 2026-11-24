"""Bluetooth-Mini-Thermodrucker der „MX…/Cat-Printer“-Familie (z. B. Callstel, MX10) direkt ansteuern.

58-mm-Papier, 384 Punkte pro Zeile, 203 dpi. Protokoll (aus offenen Projekten, z. B. rbaron/catprinter,
NaitLee/Cat-Printer): GATT-Dienst AE30, Befehle auf AE01 (write without response), Rückmeldungen auf AE02.
Paket: 51 78 <Befehl> 00 <Länge> 00 <Daten …> <CRC8 über Daten> FF

    python tools/bt_drucker.py etikett.png        # druckt ein Bild (wird auf 384 Punkte skaliert)
"""
import asyncio
import sys

from PIL import Image

WIDTH = 384
SERVICE = "0000ae30-0000-1000-8000-00805f9b34fb"
TX = "0000ae01-0000-1000-8000-00805f9b34fb"
RX = "0000ae02-0000-1000-8000-00805f9b34fb"
NAME_PREFIXES = ("MX", "GB0", "GT0", "YT0", "MXW")

CMD_FEED = 0xA1
CMD_BITMAP = 0xA2
CMD_STATE = 0xA3
CMD_QUALITY = 0xA4
CMD_LATTICE = 0xA6
CMD_UPDATE = 0xA9
CMD_FLOW = 0xAE
CMD_ENERGY = 0xAF
CMD_SPEED = 0xBD
CMD_APPLY_ENERGY = 0xBE

LATTICE_START = bytes([0xAA, 0x55, 0x17, 0x38, 0x44, 0x5F, 0x5F, 0x5F, 0x44, 0x38, 0x2C])
LATTICE_END = bytes([0xAA, 0x55, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17])


def _crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def packet(cmd: int, data: bytes = b"\x00") -> bytes:
    return bytes([0x51, 0x78, cmd, 0x00, len(data), 0x00]) + data + bytes([_crc8(data), 0xFF])


def image_rows(img: Image.Image) -> list:
    """Bild → Zeilen à 48 Byte, 1 = schwarz, niedrigstes Bit = linkes Pixel."""
    if img.width != WIDTH:
        img = img.resize((WIDTH, round(img.height * WIDTH / img.width)), Image.Resampling.LANCZOS)
    img = img.convert("L").point(lambda v: 0 if v < 128 else 255, mode="1")
    px = img.load()
    rows = []
    for y in range(img.height):
        row = bytearray(WIDTH // 8)
        for x in range(WIDTH):
            if px[x, y] == 0:
                row[x // 8] |= 1 << (x % 8)
        rows.append(bytes(row))
    return rows


def job(img: Image.Image, energy=0x3000, feed=80) -> list:
    """Komplette Paketfolge für ein Bild."""
    pk = [packet(CMD_STATE), packet(CMD_QUALITY, b"\x33"), packet(CMD_LATTICE, LATTICE_START),
          packet(CMD_ENERGY, energy.to_bytes(2, "little")), packet(CMD_APPLY_ENERGY, b"\x01"),
          packet(CMD_SPEED, b"\x23"), packet(CMD_UPDATE, b"\x00")]
    blank = 0
    for r in image_rows(img):
        if not any(r):          # Leerzeilen als Vorschub – schneller
            blank += 1
            continue
        if blank:
            pk.append(packet(CMD_FEED, blank.to_bytes(2, "little")))
            blank = 0
        pk.append(packet(CMD_BITMAP, r))
    pk += [packet(CMD_FEED, (blank + feed).to_bytes(2, "little")), packet(CMD_LATTICE, LATTICE_END),
           packet(CMD_STATE)]
    return pk


async def find_printer(timeout=6.0):
    from bleak import BleakScanner
    devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for d, adv in devs.values():
        if (d.name or "").upper().startswith(NAME_PREFIXES) or SERVICE in (adv.service_uuids or []):
            return d
    return None


async def print_image_async(img: Image.Image, address=None, log=print):
    from bleak import BleakClient
    if address is None:
        log("Suche Bluetooth-Drucker …")
        d = await find_printer()
        if not d:
            raise RuntimeError("Kein Bluetooth-Drucker gefunden – eingeschaltet und in der Nähe?")
        address = d
        log(f"Drucker: {d.name} ({d.address})")
    paused = asyncio.Event()
    paused.set()   # gesetzt = darf senden

    def on_rx(_, data: bytearray):
        # Flusssteuerung: 51 78 AE 00 01 00 10 … = Pause, … 00 … = weiter
        if len(data) >= 7 and data[2] == CMD_FLOW:
            if data[6] == 0x10:
                paused.clear()
            else:
                paused.set()

    async with BleakClient(address, timeout=15) as c:
        await c.start_notify(RX, on_rx)
        pk = job(img)
        for i, p in enumerate(pk):
            await paused.wait()
            await c.write_gatt_char(TX, p, response=False)
            if i % 4 == 3:
                await asyncio.sleep(0.02)   # Puffer des Druckers nicht überfahren
        await asyncio.sleep(1.5)            # letzte Pakete ausdrucken lassen
        await c.stop_notify(RX)
    log(f"{len(pk)} Pakete an den Bluetooth-Drucker gesendet")


def print_image(img: Image.Image, address=None, log=print):
    asyncio.run(print_image_async(img, address, log))


if __name__ == "__main__":
    print_image(Image.open(sys.argv[1]))
