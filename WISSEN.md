# Wissensstand

Was dieser Fork löst, wie der Aufbau funktioniert und welche Eigenheiten der Firmware dabei
zu beachten sind. Alle Befunde sind am Quelltext oder am laufenden Gerät geprüft, mit
Fundstellen.

**Stand:** 6. September 2026 · Fork-Basis `eb75868` · Release `v2.4.9`

---

## 1. Die Aufgabe und die Randbedingungen

Es gibt zwei ESPSomfy-Controller in zwei Haushalten.

| | Haushalt A | Haushalt B |
|---|---|---|
| Bedienung | Android-App aus `Moritz-Staat/ESPSomfy`, dazu Home Assistant | iPhone, Google Home, **kein** Home Assistant |
| Aufgabe | läuft | Rollos per Sprache über Google Home, **ohne zusätzliche Hardware dort** |

### Drei Wände, die den Aufbau bestimmen

Der Controller war ein Gerät, das man **besuchen** muss. Drei unabhängige Gründe:

1. **Die HTTP-API sendet keine CORS-Header.** `Web::sendCORSHeaders()` ist eine leere Hülle
   — alle sechs `sendHeader`-Zeilen sind auskommentiert (`Web.cpp:52-59`) — und wird
   trotzdem an **63** Stellen aufgerufen. Eine Webseite von einem fremden Ursprung darf die
   Antworten des Geräts deshalb nicht lesen, und ein `PUT` mit `application/json` verlangt
   einen Preflight, den das Gerät mit HTTP 200 aber **ohne** Header beantwortet. Nur das
   mitgelieferte Web-UI funktioniert, weil es vom Gerät selbst kommt und damit same-origin
   ist.
2. **Mixed Content.** Eine über HTTPS geladene Seite darf nichts von `http://192.168.x.x`
   holen, auch kein `ws://`. Eine gehostete Web-App scheitert also selbst dann, wenn man
   Wand 1 beseitigt.
3. **Der MQTT-Client konnte kein TLS.** `WiFiClient tcpClient;` — kein `WiFiClientSecure`,
   kein Zertifikat. Ein Broker außerhalb des LAN hätte bedeutet, Zugangsdaten im Klartext
   durchs Internet zu schicken.

**Wand 3 ist die einzige, die sich in der Firmware wegnehmen lässt** — und genau das ist der
Inhalt dieses Forks. Wand 1 und 2 bleiben; deshalb redet kein Browser-Client von außen
direkt mit dem Gerät.

Dazu eine vierte Randbedingung, die nicht im Gerät steckt: **ein Cloudflare-Tunnel kann MQTT
nicht tragen.** Das ist rohes TCP, kein HTTP. Ein selbst betriebener Broker bräuchte eine
echte Portweiterleitung; ein gehosteter Broker verlangt nirgends einen offenen Port, weil
beide Seiten ausgehend wählen.

---

## 2. Wie der Aufbau funktioniert

```
   ihr Haushalt                Internet                  dein Server
 ┌──────────────┐         ┌──────────────┐        ┌──────────────────────┐
 │  ESP32       │─mqtts──►│  MQTT-Broker │◄──────►│  HA-Instanz nur      │
 │  + CC1101    │◄────────│  (gehostet)  │        │  fuer ihr Geraet     │
 └──────┬───────┘  beide  └──────────────┘        └──────────┬───────────┘
        │          waehlen                                   │ google_assistant
        │          nach                                      ▼
        │          aussen                          ihr Google-Konto
        └─ lokal unabhaengig:                            │
           Web-UI, Handsender                             ▼
                                            „Hey Google, oeffne die Rollos"
```

**Beide Seiten verbinden sich ausgehend** und treffen sich beim Broker. Deshalb muss nirgends
ein Port geöffnet werden, und deshalb steht in ihrem Haushalt keine zusätzliche Kiste.

### Die Teile und ihre Rollen

| Teil | Rolle | Für wen |
|---|---|---|
| **ESP-Firmware** | funkt Somfy, rechnet Positionen, meldet sich verschlüsselt am Broker an | beide Geräte |
| **MQTT-Broker** | Briefkasten in der Mitte, gehostet, kostenlos | beide Seiten |
| **Home Assistant** | **nur Übersetzer** nach Google. Eigene Instanz je Haushalt, damit ihr Login nicht deine Wohnung sieht | für sie |
| **Android-App** | direkter Draht im eigenen WLAN, kein MQTT | für dich |

Die Android-App spricht ausschließlich die HTTP- und WebSocket-Schnittstelle eines Geräts
**im selben WLAN** an. Sie hat keinen MQTT-Teil und kann den Broker nicht benutzen.

### Warum eine eigene HA-Instanz je Haushalt

Nicht aus technischen Gründen, sondern wegen des Zugriffs: für die Google-Verknüpfung muss
sie sich einmal an dieser HA-Instanz anmelden, und Home Assistant kennt keine belastbare
Rechtetrennung je Entität — mit einem Konto in deiner Instanz könnte sie über die API deine
Wohnung steuern. Eine eigene Instanz löst das sauber. Details in
[ANLEITUNG.md](ANLEITUNG.md).

### Ausfallverhalten

| Fällt aus | Sprachsteuerung | Rollos vor Ort |
|---|---|---|
| dein Server oder HA | weg | gehen — Web-UI am Gerät läuft |
| der Broker | weg | gehen |
| ihr Internet | weg | gehen |
| ihr WLAN oder Strom | weg | Handsender geht immer |

Der Preis dafür, dass bei ihr nichts steht: **du bist ihre Infrastruktur.** Die Rollos selbst
bleiben immer bedienbar, es fällt nur der Komfort aus.

---

## 3. Was dieser Fork ändert

Vier Änderungen, Begründung und Details in [FORK.md](FORK.md).

1. **MQTT über TLS** — `mqtts://` schaltet auf `WiFiClientSecure`, der erst bei Bedarf
   angelegt wird. Der Handshake ist aus dem Task-Watchdog ausgeklammert. `publishBuffer`
   schreibt in 512er statt 128er Häppchen.
2. **`via_device` entfernt** — war mit derselben Kennung wie das Gerät selbst belegt, was
   Home Assistant 2026.9.0 ablehnt („A device can not be its own via device", Upstream-Issue
   #684). Ohne den Fix erscheinen die Rollos gar nicht.
3. **Discovery-Topics tragen die `serverId`** — sonst überschreiben sich zwei Controller am
   selben Broker gegenseitig.
4. **OTA zeigt auf diesen Fork** — sonst ließe sich ein Gerät zum Downgrade überreden.

### Zwei Vorarbeiten lagen schon im Quelltext

Das ist der Grund, warum der Patch so klein ausfiel — rund **464 Byte** im Binärabbild:

- **`WiFiClientSecure` war längst eingebunden.** `GitOTA.cpp` nutzt es an drei Stellen mit
  `setInsecure()` (`GitOTA.cpp:2,94,354,475`), um Releases von GitHub zu holen. TLS läuft auf
  diesem Gerät also nachweislich; der Patch fügt Aufrufstellen hinzu, nicht den Stack.
- **Das Konfigurationsfeld existierte und wurde nie ausgewertet.**
  `char protocol[10] = "mqtt://"` (`ConfigSettings.h:156`), und `connect()` prüfte es nur auf
  Länge (`MQTT.cpp:200`). Sogar **die Auswahl im Web-UI war da** — ein `<select>` mit
  `MQTT` / `MQTTS` neben dem Hostfeld, ohne Abnehmer im Code. Weil der Vergleich
  groß-klein-unabhängig ist, greift der Wert `MQTTS` aus der Oberfläche direkt.

---

## 4. Die Firmware von innen

### Drei Ports, zwei Webserver

Die Firmware betreibt `apiServer` auf **8081** und `server` auf **80** (`Web.cpp:41-42`),
dazu einen WebSocket auf **8080**. Sie bedienen **unterschiedliche** Routen.

| Port | Inhalt |
|---|---|
| **8081** | `/discovery`, `/shades`, `/rooms`, `/groups`, `/controller`, `/login`, `/shadeCommand`, `/groupCommand`, `/tiltCommand`, `/repeatCommand`, `/setPositions`, `/setSensor`, `/backup`, `/reboot` |
| **80** | **sämtliche Verwaltung**: `/saveShade`, `/saveRoom`, `/saveGroup`, `/addShade`, `/addRoom`, `/addGroup`, `/delete*`, `/linkToGroup`, `/unlinkFromGroup`, `/*SortOrder`, `/setMyPosition`, `/connectmqtt`, `/mqttsettings` |
| **8080** | WebSocket, zusätzlich als mDNS-Dienst `_espsomfy_rts._tcp` beworben (`Network.cpp:353-356`) mit TXT-Feldern `serverId`, `model`, `version` |

Verwaltungsrouten sind auf 8081 **nicht** registriert — ein `PUT` dorthin läuft ins Leere.
`/mqttsettings` ist **nur lesend**; gespeichert wird über `PUT /connectmqtt`.

**`GET /discovery` ist der wichtigste Endpunkt.** Er liefert in einem Request `serverId`,
`version`, `latest`, `model`, `hostname`, `authType`, `permissions`, `chipModel`, `connType`,
`memory{max,free,min,total}` und die vollständigen Arrays `rooms`, `shades`, `groups`.

### WebSocket-Eigenheiten

- Die Frames sind **kein gültiges JSON**: `42[shadeState,{"shadeId":1,…}]` — der Event-Name
  steht **unquotiert** im Array. Ein normaler `JSON.parse` scheitert.
- Direkt nach dem Verbinden kommt der Klartext-String `Connected`, kein Frame.
- **Kein Heartbeat.** Im Leerlauf kommen minutenlang keine Frames (60 s im Mitschnitt
  bestätigt). Ein Watchdog „keine Daten in 60 s ⇒ tot" reißt gesunde Verbindungen ab —
  stattdessen WebSocket-Protokoll-Pings nutzen, deren Pong RFC-Pflicht ist.
- **Maximal 5 Clients.** Web-UI-Tabs, Handy-App und Bridges zählen alle mit.
- `shadeType` heißt im Socket-Event `type`. Tilt-Felder fehlen dort bei `tiltType: 0` — beim
  Zusammenführen nie auf `undefined` zurücksetzen.

### Die MQTT-Oberfläche

Veröffentlicht je Rollo (`Somfy.cpp:1448ff`): `position`, `direction`, `target`, `mypos`,
`myTiltPos`, `lastRollingCode`, bei Lamellen `tiltPosition`, `tiltDirection`, `tiltTarget`,
dazu `sunFlag`, `sunny`, `windy`. Auf Geräteebene `status` (LWT `online`/`offline`),
`ipAddress`, `host`, `firmware`, `serverId`, `mac`.

Abonniert (`MQTT.cpp:216-229`): `shades/+/target/set`, `direction/set`, `tiltTarget/set`,
`mypos/set`, `myTiltPos/set`, `position/set`, `tiltPosition/set`, `sunFlag/set`, `sunny/set`,
`windy/set` sowie `groups/+/direction/set`, `sunFlag/set`, `sunny/set`, `windy/set`.

Damit ist die MQTT-Schnittstelle für Steuerung **und** Zustand vollständig — sie war der
Grund, diesen Weg zu wählen statt einer neuen Protokollschicht.

`setBufferSize` wird **nirgends** aufgerufen, der PubSubClient-Default liegt bei 256 Byte.
Die Discovery-Nachrichten von bis zu 2 KB gehen trotzdem durch, weil `publishBuffer` mit
`beginPublish` und gestückelten `write()`-Aufrufen streamt (`MQTT.cpp:329-345`). Für
empfangene Nachrichten ist die Grenze ohne Belang, die Befehlsnutzlasten sind Zahlen.

### Discovery-Topics

```
<discoTopic>/cover/<serverId>/<shadeId>/config
```

Home Assistant erlaubt die node_id ausdrücklich:
`<discovery_prefix>/<component>/[<node_id>/]<object_id>/config`, zulässige Zeichen
`[a-zA-Z0-9_-]`. Die `serverId` ist sechsstelliger Hex und damit gültig.

### Die Positionsfalle — drei Konventionen, zwei Richtungen

| System | Wert 0 heißt | Höchstwert heißt |
|---|---|---|
| **ESPSomfy** `position`/`target` | **offen** | 100 = **geschlossen** |
| **Matter** `CurrentPositionLiftPercent100ths` | **offen** | 10000 = **geschlossen** |
| **Google** `openPercent` | **geschlossen** | 100 = **offen** |

Belegt in `Somfy.cpp:1497-1499`: die Discovery meldet `position_open = 0` und
`position_closed = 100`. Zwei Sonderfälle gelten **je Rollo**:

- **`flipPosition`** dreht die Skala.
- **`shadeType = awning`** (Markise) ist per Default schon gedreht (`Somfy.cpp:1534-1535`).

```
Matter:  lift100ths  = flip ? (100 - pos) * 100 : pos * 100
Google:  openPercent = flip ? pos : 100 - pos
         flip = flipPosition XOR (shadeType == awning)
```

Home Assistant übernimmt das automatisch, weil die Firmware `position_open` und
`position_closed` in der Discovery mitschickt. Wer selbst übersetzt, kapselt es in **eine**
Funktion mit Tests.

Fahrtrichtung im MQTT-Vokabular: `1` schließt, `-1` öffnet, `0` steht.

### Betriebsfallen

| Befund | Fundstelle |
|---|---|
| **`repeats = 0` lässt den ersten Befehl verpuffen.** `sendFrame` sendet dann genau einen Frame; der RTS-Empfänger tastet im Duty-Cycle ab, wird vom Weckimpuls geweckt, der Datenframe kommt aber zu früh. Symptom: „wirkt erst beim zweiten Mal". Fix: `repeats: 3` per `PUT /saveShade` auf **Port 80**. Der Code-Default in `clear()` wäre 1, gespeichert war trotzdem 0 — bei neu angelegten Rollos also immer prüfen. | `Somfy.cpp:4011`, `Somfy.cpp:703` |
| **`Web::isAuthenticated()` wird an keiner Route aufgerufen** — deklariert und implementiert, aber tot. Alle Routen sind ohne `apikey` erreichbar, unabhängig von `authType`. Das Gerät gehört deshalb nie ins offene Internet. | `Web.h:43`, `Web.cpp:79` |
| **Fehler kommen teils mit 2xx.** Bei falscher HTTP-Methode antworten Routen mit HTTP **201** und `{"status":"ERROR"}`. `/reboot` verlangt PUT oder POST; auf GET kommt 201 und das Gerät startet nicht neu. Clients müssen bei **jeder** Antwort das `status`-Feld prüfen. | |
| **`myPos`/`myTiltPos` sind `-1`, nicht 255**, wenn kein Favorit gesetzt ist. Alles außerhalb 0–100 heißt „kein Favorit". | |
| **`setMyPosition` programmiert den Favoriten im Motor**, nicht in der Datenbank — ein Befehl mit drei Wirkungen: hinfahren, setzen, löschen (löschen = denselben Wert erneut senden). Bleibt `tilt` weg, setzt die Firmware `tilt = myPos`, also den Wert der Fahrachse. Bei Lamellen immer beides senden. | `Web.cpp:1550` |
| **`/setPositions` schreibt nur die ESP-Datenbank**, der Motor erfährt nichts. Kein Ersatz für `setMyPosition`. | |
| **Die `*SortOrder`-Routen erwarten ein nacktes JSON-Array** (`[3,1,2]`) und rufen kein `save()` — die Reihenfolge steht zunächst nur im RAM. | |
| **`/backup` streamt Text**, kein JSON-Objekt; auf 8081 fehlt der `Content-Disposition`-Header. | |
| **Telemetrie ist ereignisgetrieben:** `wifiStrength` nur bei Änderung > 1 dBm, `memStatus` nur bei Sprüngen > 1500 Byte, spätestens alle 15 s. `strength: -100` mit leerer SSID heißt „keine Verbindung", nicht „sehr schwach". | |
| **Namensgrenze `char[21]`** = 20 Zeichen, für Rollos, Räume und Gruppen. | |
| **`linkToGroup` funkt keinen Prog-Befehl** und behandelt `shadeId 0` als „nicht angegeben". `deleteShade` antwortet mit HTTP 400, wenn das Rollo in einer Gruppe ist. | |
| **Kein generischer Datei-Handler.** Jede ausgelieferte Datei ist eine fest verdrahtete Route (`Web.cpp:1094-1190`), unbekannte Pfade gehen in `onNotFound` → 404. Eine zusätzlich hochgeladene HTML-Datei würde nie ausgeliefert — obwohl LittleFS Platz hätte (442 von 1408 KB belegt). | |
| **Das Web-UI ist bereits eine iOS-Web-App.** `data/index.html` enthält `apple-mobile-web-app-capable`, `apple-mobile-web-app-title` und sieben `apple-touch-icon`-Größen. „Zum Home-Bildschirm" liefert also ein Icon ohne Browserleisten — iOS-Safari erlaubt das auch über einfaches HTTP, anders als Chrome. | |

---

## 5. Flash und RAM

App-Partition (`default`) = 0x140000 = **1.310.720 Byte**. Zwei Builds, zwei Zahlen — und
**maßgeblich ist der Release-Build**, denn der wird geflasht. `ci.yaml` pinnt Core 2.0.10,
`release.yaml` pinnt 2.0.17, und auf dem klassischen ESP32 kostet der neuere Core **43 KB
mehr**.

| Board | Release (Core 2.0.17) | CI (Core 2.0.10) |
|---|---|---|
| **ESP32** | 1.299.429 B — **99 %**, ~11 KB frei | 1.256.409 B — 95 % |
| ESP32-C3 | 1.216.946 B — 92 %, ~92 KB frei | 1.191.098 B — 90 % |
| ESP32-S2 | 1.184.326 B — 90 %, ~124 KB frei | 1.163.978 B — 88 % |
| ESP32-S3 | 1.175.649 B — 89 %, ~132 KB frei | 1.172.609 B — 89 % |

Statisches RAM im Release-Build auf dem ESP32: globale Variablen 95.096 Byte (29 %), es
bleiben 232.584 Byte für lokale Variablen und Heap. Eine offene TLS-Sitzung hält davon
dauerhaft rund 20–35 KB.

**Was der Patch kostet:** das `esp32.bin` von v2.4.8 ist 1.306.000 Byte groß, das von
Upstream v2.4.6 war 1.305.536 — Unterschied rund **464 Byte**. Der TLS-Stack war über den
OTA-Client schon eingebunden, `mqtts://` ist also fast gratis. Die 99 % sind Upstreams
Ausgangslage auf diesem Core.

**11 KB sind aber knapp.** Nach jeder Änderung die Größenzeile im **Release**-Log lesen.
Läuft es künftig über, ist die Antwort nicht Code kürzen, sondern Partitionen umverteilen:
LittleFS hat 1,44 MB, `data/` braucht nur 442 KB. Das ändert das Flash-Layout und verlangt
dann einmal USB statt OTA.

Die Dateigröße eines veröffentlichten `.bin`-Assets ist übrigens **kein** Maß für den Sketch
— maßgeblich ist allein die Compiler-Zeile `Sketch uses … bytes (xx%)`.

---

## 6. Build, CI und Release

Kein lokaler Werkzeugkasten nötig. `ci.yaml` kompiliert bei **jedem Push** alle vier Boards,
`release.yaml` baut die flashbaren Bilder beim Veröffentlichen eines Releases.

Gepinnt von `release.yaml`: esp32-Core 2.0.17, ArduinoJson 6.21.5, PubSubClient 2.8.0,
SmartRC-CC1101-Driver-Lib 2.5.7, WebSockets 2.4.0, LittleFS-Image 1.441.792 Byte bei Offset
`0x290000`.

### Eigenheiten, die Zeit kosten

- **Actions sind bei Forks gesperrt.** Bis zum einmaligen Freischalten in der Actions-Ansicht
  listet `gh api repos/…/actions/workflows` nichts und kein Lauf startet. **Issues sind bei
  Forks ebenfalls aus** — einschalten mit
  `gh api -X PATCH repos/OWNER/REPO -F has_issues=true`.
- **Freischalten löst keinen Lauf für bereits gepushte Commits aus.** Es braucht einen neuen
  Push.
- **`actions/upload-artifact@v3` wird automatisch abgewiesen.** Der Lauf scheitert in
  „Set up job", bevor Code angefasst wird. `ci.yaml` hing upstream noch auf v3.
- **`release.yaml` braucht `permissions: contents: write`.** Der Standard für den
  `GITHUB_TOKEN` ist nur noch `read`; das Anhängen der Release-Dateien scheiterte mit
  `unexpected status code: 403`. Upstream deklarierte Rechte allein im `arduino`-Job.
- **Ein `release`-Ereignis lässt sich nicht mit `gh run rerun` wiederholen** — der Lauf
  benutzt die alte Fassung der Workflow-Datei. Nach einer Korrektur Release **und** Tag
  löschen (`gh release delete --cleanup-tag`) und neu anlegen.
- **`gh run list` zeigte die Läufe des Forks nicht**, die REST-API unter
  `repos/…/actions/runs` schon.
- **Job-Logs brauchen `--allow-escape-sequences`**, sonst gibt `gh api` nur eine Fehlermeldung
  zurück:
  ```bash
  gh api repos/OWNER/REPO/actions/jobs/<id>/logs --allow-escape-sequences \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -i "sketch uses"
  ```
- **Forks erben keine Releases und keine Tags.** `gh release list` ohne `-R` greift bei zwei
  Remotes womöglich auf upstream.
- **Lokal bauen scheidet hier aus:** installiert war esp32-Core 3.0.7, gepinnt ist 2.0.x —
  3.x ist für diesen Code ein Breaking Change.

### Release-Konvention

Der Tag muss zu `FW_VERSION` in `ConfigSettings.h` passen, weil `GitOTA` die Download-URL aus
`settings.fwVersion.name` baut. Aktuell **`v2.4.9`**. Für den nächsten Patch beides gemeinsam
hochziehen.

Nicht `v2.4.7` nehmen: Upstream hat diese Version als **Vorabversion** veröffentlicht
(19. August 2024) — gleicher Name bei anderem Inhalt wäre verwirrend. Deshalb liefert die
API bei Upstream auch v2.4.6 als „latest", sie überspringt Vorabversionen.

`appver_t` parst Ziffern vor den Punkten in `major`/`minor`/`build` und hat ein `suffix[4]`;
`name` ist `char[15]`, die Version darf also höchstens 14 Zeichen haben.

---

## 7. Google Home — was für diesen Aufbau gilt

Die Anbindung läuft über die **Google-Assistant-Integration von Home Assistant**, also über
Googles **Cloud-to-Cloud**-Schnittstelle. Eingerichtet wird sie in der **Google Home
Developer Console**, nicht mehr in der alten Actions-Konsole; der Test-Modus reicht dauerhaft
für den eigenen Haushalt.

- Gerätetyp `action.devices.types.BLINDS`, Trait `action.devices.traits.OpenClose` mit
  `openPercent` — **0 = zu, 100 = offen** —, `discreteOnlyOpenClose: false`, dazu
  `OpenCloseRelative` für „öffne 10 Prozent mehr".
- **`expose_by_default: false`** und `expose: true` je Entität. Nur so landet ausschließlich
  das in ihrem Google-Konto, was ausdrücklich dafür bestimmt ist.
- **`report_state: true`**, sonst fragt Google den Zustand nur auf Nachfrage ab und die
  Kachel hinkt.
- Räume und Spitznamen in Google Home entscheiden über die Sprachqualität: kurze Gerätenamen
  ohne Raumangabe, der Raum kommt aus Google.

---

## 8. Offene Punkte

Geführt als [Issues im Repo](../../issues):

| # | Punkt | Stand |
|---|---|---|
| [#1](../../issues/1) | Test am echten Gerät: TLS-Verbindung, Heap über 24 h | **offen** — die einzige echte Unbekannte |
| [#2](../../issues/2) | Broker-Zertifikat wird nicht geprüft (`setInsecure`) | offen, mit Abwägung — entscheidet sich, sobald der Broker feststeht |
| [#3](../../issues/3) | Alte Discovery-Konfigurationen nach dem Umstieg | erledigt, soweit sicher machbar |
| [#4](../../issues/4) | Port beim Umschalten auf MQTTS vorschlagen | erledigt |
| [#5](../../issues/5) | Patches nach upstream einreichen? | offen, Entscheidung des Eigners |

**Wichtigster Punkt bleibt #1.** Der Code kompiliert für alle vier Boards, ist aber auf keiner
Hardware gelaufen. Erst am eigenen Gerät testen, nicht am ausgelieferten.
