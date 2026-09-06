# Einrichtung: ESPSomfy im fremden Haushalt, Sprachsteuerung über Google Home

Diese Anleitung richtet einen ESPSomfy-RTS-Controller so ein, dass er **in einem anderen
Haushalt** steht, dort **keine zusätzliche Hardware** braucht und die Rollos über **Google
Home per Sprache** bedienbar sind. Sie setzt die Firmware aus diesem Fork voraus
(siehe [FORK.md](FORK.md)).

Rollen in dieser Anleitung: **du** betreibst den Server, **sie** hat die Rollos.

---

## Das Bild am Ende

```
   ihr Haushalt                 Internet                  dein Server
 ┌──────────────┐          ┌──────────────┐        ┌────────────────────────┐
 │  ihr ESP32   │─mqtts───►│  MQTT-Broker │◄──────►│  Home Assistant        │
 │  + CC1101    │◄─────────│  (kostenlos) │        │  nur für ihr Gerät     │
 └──────┬───────┘  beide   └──────────────┘        └───────────┬────────────┘
        │          Seiten                                      │
        │          wählen                                      │ google_assistant
        │          nach außen                                  ▼
        │                                          ihr Google-Konto
        └─ lokal weiter bedienbar:                      │
           Web-UI am Gerät, Handsender                   ▼
                                              „Hey Google, öffne die Rollos"
```

Der Kern: **beide Seiten verbinden sich nach außen.** Weder in ihrem Router noch in deinem
muss ein Port geöffnet werden. Das ist der Grund, warum bei ihr nichts stehen muss.

---

## Zutaten

| Was | Wo | Kosten |
|---|---|---|
| ESPSomfy-Controller (ESP32 + CC1101) | bei ihr | hast du gebaut |
| Firmware aus diesem Fork | Release dieses Repos | – |
| MQTT-Broker mit TLS | gehostet, z. B. HiveMQ Cloud Serverless | 0 € |
| Home Assistant | Container auf deinem Server | 0 € |
| Öffentliche HTTPS-Adresse für dieses HA | dein Reverse-Proxy | 0 € |
| Google-Cloud-Projekt | ihr oder dein Google-Konto | 0 € |
| Ein Google-Lautsprecher oder -Display | bei ihr | hat sie |

**Nicht** nötig: zusätzliche Hardware in ihrem Haushalt, eine offene Portweiterleitung —
weder bei ihr noch bei dir — und keine App, die installiert werden müsste.

---

## Schritt 1 — Broker anlegen

Ein gehosteter Broker ist hier besser als ein eigener: beide Seiten verbinden sich
ausgehend, also muss nirgends ein Port auf. Der kostenlose Tarif von HiveMQ Cloud
(100 Verbindungen, 10 GB/Monat, TLS) ist für zwei Rollos maßlos überdimensioniert.

1. Konto anlegen, **Serverless-Cluster** erstellen.
2. Hostname und Port notieren — der Port für TLS ist **8883**.
3. Unter *Access Management* zwei Zugangsdatensätze anlegen:
   - einen für **ihr Gerät**, z. B. `shade-anna`
   - einen für **Home Assistant**, z. B. `ha-anna`
4. Falls der Broker Themenrechte anbietet: dem Gerät nur seinen eigenen Wurzelpfad
   erlauben (`espsomfy/anna/#`) und den Discovery-Pfad (`homeassistant/#`).

> **Warum nicht dein eigener Mosquitto?** Weil ein Cloudflare-Tunnel MQTT nicht tragen
> kann — das ist rohes TCP, kein HTTP. Ein eigener Broker bräuchte also eine echte
> Portweiterleitung auf Port 8883.

---

## Schritt 2 — Firmware flashen

Die Firmware aus diesem Fork muss **einmal über USB** aufs Gerät. Ein OTA-Update von einer
Upstream-Installation aus geht nicht, weil das Upstream-Repo diesen Build nicht kennt.

1. Vom [Release dieses Repos](../../releases) die passende Datei ziehen:
   - **erste Installation:** `SomfyController.onboard.esp32.bin.zip` — enthält Bootloader,
     Partitionstabelle, Firmware und Dateisystem in einem Bild
   - späteres Update über die Weboberfläche: `SomfyController.ino.esp32.bin` und
     `SomfyController.littlefs.bin`
2. Entpacken und flashen:

```bash
pip install esptool
python -m esptool --chip esp32 --port COM5 --baud 921600 \
  write_flash 0x0 SomfyController.onboard.esp32.bin
```

Bei einem S2, S3 oder C3 ist `--chip` entsprechend zu setzen; die Adresse `0x0` gilt für
das zusammengesetzte Bild.

3. Nach dem Neustart öffnet das Gerät einen Access Point. Dort das WLAN **ihres**
   Haushalts eintragen.

---

## Schritt 3 — Grundeinrichtung am Gerät

Alles im Web-UI unter `http://<ip-des-geraets>`.

### 3.1 Feste Adresse

Im Router eine DHCP-Reservierung setzen oder im Gerät eine statische Adresse. Notieren.

### 3.2 Rollos anlernen

Wie gewohnt über *Shades* — Motor in den Programmiermodus, Adresse vergeben, prüfen.

### 3.3 `repeats` auf 3 setzen — nicht überspringen

Steht bei einem Rollo `repeats = 0`, sendet die Firmware genau **einen** Funkframe. Der
RTS-Empfänger schläft im Duty-Cycle: der Weckimpuls kommt an, der eine Datenframe aber zu
früh und verpufft. Im Alltag heißt das *„Google reagiert erst beim zweiten Mal."*

```bash
curl -X PUT http://<ip>/saveShade \
  -H "Content-Type: application/json" \
  -d '{"shadeId":1,"repeats":3}'
```

Für jedes Rollo einzeln. Läuft auf **Port 80**, nicht 8081. Wirkt sofort und bleibt
gespeichert. Ein Wiederholungsschritt kostet rund 65 ms Sendezeit.

### 3.4 Namen kurz halten

Der Name wird zum Sprachbefehl. Kurz und **ohne** Raumangabe, den Raum kennt Google selbst:
`Wohnzimmer`, `Küche`, `Schlafzimmer`. Grenze der Firmware: **20 Zeichen**.

### 3.5 Räume vergeben

Im Web-UI Räume anlegen und zuordnen. Erspart Handarbeit in Google Home.

---

## Schritt 4 — MQTT auf TLS umschalten

Im Web-UI steht die Auswahl **MQTT / MQTTS** direkt neben dem Hostfeld; beim Umschalten
schlägt sie den passenden Port vor. Damit, Zugangsdaten und Wurzelthema eintragen,
speichern — fertig.

Wer es geskriptet mag: `PUT /connectmqtt` auf **Port 80** speichert und verbindet sofort
neu. Fehlende Felder behalten ihren Wert, weil die Firmware jeden Schlüssel einzeln prüft.

```bash
curl -X PUT http://<ip>/connectmqtt \
  -H "Content-Type: application/json" \
  -d '{"enabled":true,
       "protocol":"mqtts://",
       "hostname":"xxxxxxxx.s1.eu.hivemq.cloud",
       "port":8883,
       "username":"shade-anna",
       "password":"...",
       "rootTopic":"espsomfy/anna",
       "pubDisco":true,
       "discoTopic":"homeassistant"}'
```

Zwei Felder verdienen Aufmerksamkeit:

- **`rootTopic` muss je Gerät eindeutig sein.** Sonst mischen sich zwei Controller am
  gleichen Broker.
- **`discoTopic`** bleibt `homeassistant`. Der Controller hängt seine eigene Kennung selbst
  in den Pfad (`homeassistant/cover/<serverId>/<shadeId>/config`), deshalb kollidieren
  mehrere Geräte nicht mehr.

Prüfen:

```bash
curl http://<ip>/mqttsettings          # liest den Zustand zurueck
```

Am Broker sollte jetzt unter `espsomfy/anna/status` der Wert `online` stehen.

> Der Broker wird **nicht** verifiziert (`setInsecure`), die Verbindung ist aber
> verschlüsselt. Zugangsdaten gehen also nicht im Klartext über die Leitung, ein aktiver
> Man-in-the-Middle ist damit nicht ausgeschlossen. Deshalb eigene Zugangsdaten je Gerät.

---

## Schritt 5 — Home Assistant für ihr Gerät

**Nimm eine eigene HA-Instanz für sie, nicht deine.** Der Grund ist nicht Technik, sondern
Zugriff: für die Google-Verknüpfung muss sie sich einmal an dieser HA-Instanz anmelden, und
HA kennt keine belastbare Rechtetrennung je Entität. Mit einer eigenen Instanz sieht ihr
Konto ausschließlich ihre Rollos.

`~/stacks/ha-anna/docker-compose.yml`:

```yaml
services:
  homeassistant:
    image: ghcr.io/home-assistant/home-assistant:stable
    container_name: ha-anna
    restart: unless-stopped
    volumes:
      - ./config:/config
    environment:
      - TZ=Europe/Berlin
    networks:
      - core_default

networks:
  core_default:
    external: true
```

Kein `ports:` — der Reverse-Proxy erreicht den Container über seinen Namen.

`config/configuration.yaml`:

```yaml
default_config:

http:
  use_x_forwarded_for: true
  trusted_proxies:
    - 172.16.0.0/12        # Docker-Netz des Proxys
```

Dann im Container die **MQTT-Integration** hinzufügen (Einstellungen → Geräte & Dienste):
Broker-Hostname, Port **8883**, Zugangsdaten `ha-anna`, **TLS aktivieren**. Sobald sie
steht, erscheinen ihre Rollos von selbst als `cover`-Entitäten — die Firmware
veröffentlicht die Discovery-Konfiguration selbst.

Zum Schluss eine Subdomain im Reverse-Proxy auf `ha-anna:8123` zeigen lassen. Diese
Adresse braucht der nächste Schritt.

---

## Schritt 6 — Google-Verknüpfung

Google nennt das **Cloud-to-Cloud**, eingerichtet wird es in der **Google Home Developer
Console**. Der Ablauf ist der von Home Assistant dokumentierte; hier nur die Punkte, an
denen man stolpert.

1. Projekt anlegen, **Cloud-to-Cloud-Integration** hinzufügen, Icon 144 × 144 px.
2. Eintragen:
   - **OAuth-Client-ID / Secret**: frei wählbar, muss unten in der YAML wieder auftauchen
   - **Authorization URL**: `https://ha-anna.deine-domain/auth/authorize`
   - **Token URL**: `https://ha-anna.deine-domain/auth/token`
   - **Fulfillment URL**: `https://ha-anna.deine-domain/api/google_assistant`
   - **Scopes**: `email`, `name`
3. In der Google Cloud Console die **HomeGraph-API** aktivieren.
4. **Service-Account** mit der Rolle *Service Account Token Creator* anlegen, Schlüssel als
   JSON herunterladen, als `config/SERVICE_ACCOUNT.json` ablegen.
5. In `configuration.yaml`:

```yaml
google_assistant:
  project_id: <projekt-id>
  service_account: !include SERVICE_ACCOUNT.json
  report_state: true
  expose_by_default: false        # nichts geht ungefragt nach draussen
  entity_config:
    cover.espsomfyrts_wohnzimmer:
      name: Wohnzimmer
      expose: true
      room: Wohnzimmer
    cover.espsomfyrts_schlafzimmer:
      name: Schlafzimmer
      expose: true
      room: Schlafzimmer
```

`expose_by_default: false` ist wichtig: so landet ausschließlich, was hier ausdrücklich
steht, in ihrem Google-Konto.

6. HA neu starten. Dann **auf ihrem Handy**: Google-Home-App → *Geräte* → *Hinzufügen* →
   *Mit Google Home verwenden*. Das Projekt steht dort mit dem Vermerk `[test]`. Antippen,
   an HA anmelden, Rollos erscheinen.

> Der Test-Modus reicht dauerhaft. Eine Zertifizierung bräuchte es nur, um die Integration
> öffentlich in Google Home zu listen.

---

## Schritt 7 — In Google Home aufräumen

- **Räume zuweisen.** Ohne Raum muss sie jedes Mal den vollen Gerätenamen sagen; mit Raum
  funktionieren Sammelbefehle wie *„Öffne die Rollos im Wohnzimmer."*
- **Spitznamen vergeben** — *Rollo*, *Rolladen*, *Jalousie*. Dann versteht Google alle.
- **Routinen**: „Guten Morgen" um 7:00 alle auf 100 %, „Gute Nacht" alle zu.

### Prozentzählung

Google zählt **0 % = geschlossen, 100 % = offen**. Der ESPSomfy zählt intern umgekehrt, und
Home Assistant rechnet es um, weil die Firmware in der Discovery `position_open: 0` und
`position_closed: 100` mitschickt. Sie sagt also immer *„100 Prozent"* für ganz offen.

Reagiert ein einzelnes Rollo verkehrt, ist an ihm `flipPosition` gesetzt — das gehört am
Gerät korrigiert, nicht durch Umdenken kompensiert.

---

## Abnahme

Erst fertig, wenn das alles am echten Gerät gilt:

| # | Prüfung | Erwartung |
|---|---|---|
| 1 | `espsomfy/anna/status` am Broker | `online` |
| 2 | Rollos in ihrem HA | erscheinen von selbst als `cover` |
| 3 | Regler in HA auf 40 % | Rollo fährt |
| 4 | Rollo per Handsender fahren | HA zieht in Sekunden nach |
| 5 | *„Öffne das Rollo im Wohnzimmer"* | fährt ganz auf |
| 6 | *„Stelle das Rollo auf 40 Prozent"* | fährt auf 40 |
| 7 | *„Schließe alle Rollos"* | alle zu |
| 8 | *„Wie weit ist das Rollo offen?"* | nennt den Wert |
| 9 | **Erster Befehl nach zwei Stunden Ruhe** | fährt **sofort** — prüft `repeats` |
| 10 | Gerät stromlos, wieder an | verbindet sich von selbst zum Broker |
| 11 | Freien Heap über 24 h beobachten | bleibt stabil, kein Abwärtstrend |
| 12 | In ihrem Google-Konto | **nur** ihre Rollos, nichts von dir |

Punkt 11 ist der wichtigste Langzeittest: eine offene TLS-Sitzung hält dauerhaft 20–35 KB
Heap. Der Diagnose-Bildschirm der Handy-App zeigt den Wert, ebenso
`GET http://<ip>:8081/discovery` im Feld `memory`.

---

## Fehlersuche

| Symptom | Ursache | Lösung |
|---|---|---|
| Erster Befehl wirkt nicht, der zweite schon | `repeats = 0` | Schritt 3.3 |
| `status` bleibt leer, Gerät verbindet nicht | falscher Port oder `protocol` noch `mqtt://` | `GET /mqttsettings` prüfen, Port muss 8883 sein |
| Gerät startet beim Verbindungsversuch neu | Watchdog beim TLS-Handshake | sollte dieser Build verhindern; sonst Serial-Log mitlesen |
| Rollos erscheinen nicht in HA | `pubDisco` aus, oder falscher `discoTopic` | beides in Schritt 4 |
| Rollos doppelt in HA | alte Discovery-Konfiguration vom Vorgänger-Build liegt retained am Broker | einmal leeren: `mosquitto_pub -t 'homeassistant/cover/1/config' -r -n` |
| Zwei Controller, nur einer sichtbar | ein Gerät läuft noch auf dem alten Build ohne Kennung im Discovery-Pfad | beide auf diesen Build bringen |
| Position in HA veraltet | MQTT-Verbindung abgerissen | Broker-Log; das Gerät verbindet alle 10 s neu |
| Google findet die Geräte nicht | `expose_by_default: false` ohne `expose: true` je Entität | Schritt 6.5 |
| Google reagiert langsam | `report_state: false` | auf `true` |
| Sprachbefehl unverstanden | Name zu lang | Schritt 3.4, Spitznamen in Google Home |

---

## Was danach dauerhaft gilt

**Sie muss nichts können.** Ihre Rollos hängen an Google Home; das Web-UI am Gerät und der
Handsender funktionieren unabhängig davon immer.

**Du bist ihre Infrastruktur.** Fällt dein Server, der Broker oder ihr Internet aus, ist die
Sprachsteuerung weg — die Rollos selbst bleiben von Hand und über das Gerät bedienbar. Das
ist der Preis dafür, dass bei ihr keine zusätzliche Kiste steht.
