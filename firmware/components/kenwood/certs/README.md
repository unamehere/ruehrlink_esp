# Zertifikat für die Maschinen-Seite

Die Kenwood Cooking Chef XL prüft das TLS-Zertifikat ihres "Cloud"-Servers nicht.
Dieses Schlüsselpaar ist deshalb **absichtlich öffentlich** und für alle Brücken gleich –
es schützt nichts und muss nicht geheim gehalten werden.

Für die App-API (später TLS) erzeugt jede Brücke ein eigenes, privates Schlüsselpaar
(siehe docs/architektur.md). Echte Geheimnisse (OTA-Signaturschlüssel) gehören nie ins Repo.

Neu erzeugen:

    openssl req -x509 -newkey rsa:2048 -nodes -keyout machine_key.pem -out machine_cert.pem \
      -days 10950 -subj "/CN=*.getdrop.io/O=Freikoch Bruecke (oeffentlich, kein Geheimnis)"
