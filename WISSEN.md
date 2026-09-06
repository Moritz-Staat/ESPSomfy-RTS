# Wissensstand

Alles, was bei der Arbeit an diesem Fork herausgekommen ist: warum er existiert, wie das
Ganze jetzt funktioniert, welche Befunde am Quelltext verifiziert sind und welche Wege
begründet verworfen wurden.

**Stand:** 6. September 2026 · Basis `eb75868` (Upstream-HEAD vom 19. August 2024)

---

## 1. Ausgangslage

Es gibt zwei ESPSomfy-Controller.

| | Haushalt A | Haushalt B |
|---|---|---|
| Gerät | ESP32 + CC1101 | ESP32 + CC1101, dort aufgestellt |
| Bedienung | Android-App aus `Moritz-Staat/ESPSomfy`, dazu Home Assistant | iPhone, Google Home, **kein** Home Assistant |
| Zustand | läuft | war ohne Lösung |

Die Aufgabe: Haushalt B soll die Rollos per Sprache über Google Home bedienen können. Die
Android-App fällt weg (iPhone), Home Assistant gibt es dort nicht, und es sollte **keine
zusätzliche Hardware** dorthin.

### Warum das schwerer ist, als es klingt

Der Controller war ein Gerät, das man **besuchen** muss. Drei unabhängige Gründe, alle im
Quelltext geprüft:

1. **Die HTTP-API sendet keine CORS-Header.** `Web::sendCORSHeaders()` ist eine leere Hülle
   — alle sechs `sendHeader`-Zeilen sind auskommentiert (`Web.cpp:52-59`) — und wird
   trotzdem an **63** Stellen aufgerufen. Eine Webseite von einem fremden Ursprung darf die
   Antworten des Geräts deshalb nicht lesen, und ein `PUT` mit `application/json` verlangt
   einen Preflight, den das Gerät mit HTTP 200 aber **ohne** Header beantwortet. Genau
   deshalb funktioniert nur das mitgelieferte Web-UI: es kommt vom Gerät selbst, ist also
   same-origin.
2. **Mixed Content.** Eine über HTTPS geladene Seite darf nichts von `http://192.168.x.x`
   holen, auch kein `ws://`. Eine gehostete Web-App scheitert also selbst dann, wenn man
   Wand 1 beseitigt.
3. **Der MQTT-Client konnte kein TLS.** `WiFiClient tcpClient;` (`MQTT.cpp:11`) — kein
   `WiFiClientSecure`, kein Zertifikat. Ein Broker außerhalb des LAN hätte bedeutet,
   Zugangsdaten im Klartext durchs Internet zu schicken.

**Wand 3 ist die einzige, die sich in der Firmware wegnehmen lässt.** Das ist der ganze
Inhalt dieses Forks.

---

## 2. Wie es jetzt funktioniert

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

Der Trick: **beide Seiten verbinden sich ausgehend** und treffen sich beim Broker. Damit
muss nirgends ein Port geöffnet werden — und das ist der Grund, warum bei ihr keine Kiste
mehr stehen muss.

### Die Teile und ihre Rollen

| Teil | Rolle | Für wen |
|---|---|---|
| **ESP-Firmware** | funkt Somfy, rechnet Positionen, **neu:** meldet sich verschlüsselt am Broker an | beide Geräte |
| **MQTT-Broker** | Briefkasten in der Mitte, gehostet, kostenlos | beide Seiten |
| **Home Assistant** | **nur Übersetzer** nach Google. Eigene Instanz je Haushalt, damit ihr Login nicht deine Wohnung sieht | für sie |
| **Android-App** | direkter Draht im eigenen WLAN, kein MQTT | für dich |
| **Web-App** | optional, dieselbe Codebasis, von deinem Server ausgeliefert | für sie, falls Google Home nicht genügt |

### Warum die Android-App nicht ihre App ist

Sie spricht ausschließlich die HTTP- und WebSocket-Schnittstelle eines Geräts **im selben
WLAN** an und hat keinen MQTT-Teil. Deshalb war die iOS-Portierung für ihren Fall von
Anfang an der falsche Hebel: sie hätte ihr Problem nicht gelöst. Der Plan dafür existiert
(`PLAN-IOS.md` im Projektordner) und bleibt als Option liegen.

### Ausfallverhalten

| Fällt aus | Sprachsteuerung | Rollos vor Ort |
|---|---|---|
| dein Server oder HA | weg | gehen — Web-UI am Gerät läuft |
| der Broker | weg | gehen |
| ihr Internet | weg | gehen |
| ihr WLAN oder Strom | weg | Handsender geht immer |

Der Preis dafür, dass bei ihr nichts steht: **du bist ihre Infrastruktur.** Die Rollos
selbst bleiben immer bedienbar, es fällt nur der Komfort aus.

---

## 3. Was dieser Fork ändert

Vier Änderungen, Begründung und Details in [FORK.md](FORK.md).

1. **MQTT über TLS** — `mqtts://` schaltet auf `WiFiClientSecure`, der erst bei Bedarf
   angelegt wird. Der Handshake ist aus dem Task-Watchdog ausgeklammert. `publishBuffer`
   schreibt in 512er statt 128er Häppchen.
2. **`via_device` entfernt** — war mit derselben Kennung wie das Gerät selbst belegt, was
   Home Assistant 2026.9.0 ablehnt.
3. **Discovery-Topics tragen die `serverId`** — sonst überschreiben sich zwei Controller am
   selben Broker gegenseitig.
4. **OTA zeigt auf diesen Fork** — sonst ließe sich ein Gerät zum Downgrade überreden.

### Zwei Vorarbeiten, die schon im Quelltext lagen

Das ist der Grund, warum der Patch so klein ausfiel:

- **`WiFiClientSecure` war längst eingebunden.** `GitOTA.cpp` nutzt es an drei Stellen mit
  `setInsecure()` (`GitOTA.cpp:2,94,354,475`), um Releases von GitHub zu holen. TLS läuft
  auf diesem Gerät also nachweislich; der Patch fügt Aufrufstellen hinzu, nicht den Stack.
  **Deshalb kostet TLS praktisch kein Flash.**
- **Das Konfigurationsfeld existierte und wurde nie ausgewertet.**
  `char protocol[10] = "mqtt://"` (`ConfigSettings.h:156`), und `connect()` prüfte es nur
  auf Länge (`MQTT.cpp:200`). Zehn Bytes fassen `"mqtts://"`. Der Anschluss war
  vorbereitet und lag brach.

---

## 4. Die Firmware von innen

Alles hier ist am Quelltext oder am echten Gerät geprüft.

### Drei Ports, zwei Webserver

Die Firmware betreibt `apiServer` auf **8081** und `server` auf **80** (`Web.cpp:41-42`),
dazu einen WebSocket auf **8080**. Sie bedienen **unterschiedliche** Routen.

| Port | Inhalt |
|---|---|
| **8081** | `/discovery`, `/shades`, `/rooms`, `/groups`, `/controller`, `/login`, `/shadeCommand`, `/groupCommand`, `/tiltCommand`, `/repeatCommand`, `/setPositions`, `/setSensor`, `/backup`, `/reboot` |
| **80** | **sämtliche Verwaltung**: `/saveShade`, `/saveRoom`, `/saveGroup`, `/addShade`, `/addRoom`, `/addGroup`, `/delete*`, `/linkToGroup`, `/unlinkFromGroup`, `/*SortOrder`, `/setMyPosition`, `/connectmqtt`, `/mqttsettings` |
| **8080** | WebSocket, zusätzlich als mDNS-Dienst `_espsomfy_rts._tcp` beworben (`Network.cpp:353-356`) mit TXT-Feldern `serverId`, `model`, `version` |

Verwaltungsrouten sind auf 8081 **nicht** registriert — ein `PUT` dorthin läuft ins Leere.

### `GET /discovery` ist der wichtigste Endpunkt

Liefert in **einem** Request `serverId`, `version`, `latest`, `model`, `hostname`,
`authType`, `permissions`, `chipModel`, `connType`, `memory{max,free,min,total}` und die
vollständigen Arrays `rooms`, `shades`, `groups`.

### WebSocket-Eigenheiten

- Die Frames sind **kein gültiges JSON**: `42[shadeState,{"shadeId":1,…}]` — der
  Event-Name steht **unquotiert** im Array. Ein normaler `JSON.parse` scheitert.
- Direkt nach dem Verbinden kommt der Klartext-String `Connected`, kein Frame.
- **Kein Heartbeat.** Im Leerlauf kommen minutenlang keine Frames (60 s im Mitschnitt
  bestätigt). Ein Watchdog „keine Daten in 60 s ⇒ tot" reißt gesunde Verbindungen ab —
  stattdessen WebSocket-Protokoll-Pings nutzen, deren Pong RFC-Pflicht ist.
- **Maximal 5 Clients.** Web-UI-Tabs, Handy-App und Bridges zählen alle mit.
- `shadeType` heißt im Socket-Event `type`. Tilt-Felder fehlen dort bei `tiltType: 0` —
  beim Zusammenführen nie auf `undefined` zurücksetzen.

### Die MQTT-Oberfläche

Veröffentlicht je Rollo (`Somfy.cpp:1448ff`): `position`, `direction`, `target`, `mypos`,
`myTiltPos`, `lastRollingCode`, bei Lamellen `tiltPosition`, `tiltDirection`, `tiltTarget`,
dazu `sunFlag`, `sunny`, `windy`. Auf Geräteebene `status` (LWT `online`/`offline`),
`ipAddress`, `host`, `firmware`, `serverId`, `mac`.

Abonniert (`MQTT.cpp:216-229`): `shades/+/target/set`, `direction/set`, `tiltTarget/set`,
`mypos/set`, `myTiltPos/set`, `position/set`, `tiltPosition/set`, `sunFlag/set`,
`sunny/set`, `windy/set` sowie `groups/+/direction/set`, `sunFlag/set`, `sunny/set`,
`windy/set`.

Damit ist die MQTT-Schnittstelle für Steuerung und Zustand **vollständig** — sie war der
Grund, diesen Weg zu wählen statt einer neuen Protokollschicht.

`setBufferSize` wird **nirgends** aufgerufen, der PubSubClient-Default liegt bei 256 Byte.
Die Discovery-Nachrichten von bis zu 2 KB gehen trotzdem durch, weil `publishBuffer`
mit `beginPublish` und gestückelten `write()`-Aufrufen streamt (`MQTT.cpp:329-345`).

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
| **`repeats = 0` lässt den ersten Befehl verpuffen.** `sendFrame` sendet dann genau einen Frame; der RTS-Empfänger tastet im Duty-Cycle ab, wird vom Weckimpuls geweckt, der Datenframe kommt aber zu früh. Symptom: „wirkt erst beim zweiten Mal". Fix: `repeats: 3` per `PUT /saveShade` auf **Port 80**. Der Code-Default in `clear()` wäre 1, gespeichert war trotzdem 0. | `Somfy.cpp:4011`, `Somfy.cpp:703` |
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
| **Kein generischer Datei-Handler.** Jede ausgelieferte Datei ist eine fest verdrahtete Route (`Web.cpp:1094-1190`), unbekannte Pfade gehen in `onNotFound` → 404. Eine zusätzlich hochgeladene HTML-Datei würde also nie ausgeliefert — obwohl LittleFS Platz hätte (442 KB von 1408 KB belegt). | |

---

## 5. Flash und RAM

Gemessen im Lauf `34038430510`, esp32-Core 2.0.10 wie von `ci.yaml` gepinnt. App-Partition
`default` = 0x140000 = **1.310.720 Byte**.

| Board | Sketch | Belegt | Frei |
|---|---:|---:|---:|
| **ESP32** (knappstes Ziel) | 1.256.409 B | **95 %** | ~53 KB |
| ESP32-C3 | 1.191.098 B | 90 % | ~117 KB |
| ESP32-S3 | 1.172.609 B | 89 % | ~135 KB |
| ESP32-S2 | 1.163.978 B | 88 % | ~143 KB |

Statisches RAM auf dem ESP32: globale Variablen 93.936 Byte (28 %), es bleiben 233.744 Byte
für lokale Variablen und Heap. Eine offene TLS-Sitzung hält davon dauerhaft rund 20–35 KB.

**Es ist also Platz, eine eigene Partitionstabelle ist nicht nötig.**

> **Korrektur eines eigenen Fehlers.** Zwischenzeitlich war hier von „rund 5 KB Luft" die
> Rede. Diese Zahl war aus der **Dateigröße** des veröffentlichten v2.4.6-Binaries
> abgeleitet (1.305.536 Byte). Die Dateigröße eines Release-Assets ist **kein** Maß für den
> Sketch. Maßgeblich ist allein die Compiler-Zeile `Sketch uses … bytes (xx%)`.

Zu beachten: `ci.yaml` pinnt Core **2.0.10**, `release.yaml` aber **2.0.17**. Der
Release-Build kann also etwas anders ausfallen — die Größenzeile dort ebenfalls lesen.

---

## 6. Build, CI und Release

Kein lokaler Werkzeugkasten nötig. `ci.yaml` kompiliert bei **jedem Push** alle vier
Boards, `release.yaml` baut die flashbaren Bilder beim Veröffentlichen eines Releases.

Gepinnt von `release.yaml`: esp32-Core 2.0.17, ArduinoJson 6.21.5, PubSubClient 2.8.0,
SmartRC-CC1101-Driver-Lib 2.5.7, WebSockets 2.4.0, LittleFS-Image 1.441.792 Byte bei
Offset `0x290000`.

### Stolpersteine, die Zeit gekostet haben

- **Actions sind bei Forks gesperrt.** Bis zum einmaligen Freischalten in der Actions-Ansicht
  listet `gh api repos/…/actions/workflows` **nichts** und kein Lauf startet.
- **Freischalten löst keinen Lauf für bereits gepushte Commits aus.** Es braucht einen neuen
  Push.
- **`actions/upload-artifact@v3` wird von GitHub automatisch abgewiesen.** Der Lauf scheitert
  in „Set up job", bevor irgendein Code angefasst wird. `ci.yaml` hing upstream noch auf v3,
  `release.yaml` war schon auf v4. Behoben.
- **`gh run list` zeigte die Läufe des Forks nicht**, die REST-API unter
  `repos/…/actions/runs` schon.
- **Job-Logs brauchen `--allow-escape-sequences`**, sonst gibt `gh api` nur eine
  Fehlermeldung zurück:
  ```bash
  gh api repos/OWNER/REPO/actions/jobs/<id>/logs --allow-escape-sequences \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -i "sketch uses"
  ```
- **Lokal bauen scheidet hier aus:** installiert war esp32-Core 3.0.7, gepinnt ist 2.0.x —
  3.x ist für diesen Code ein Breaking Change.

### Release-Konvention dieses Forks

Der Tag muss zu `FW_VERSION` in `ConfigSettings.h` passen, weil `GitOTA` die Download-URL
aus `settings.fwVersion.name` baut. Aktuell `v2.4.7`. Eine Version, die Upstream nie
veröffentlicht hat — jedes Gerät, das v2.4.7 meldet und auf diesen Fork zeigt, ist also ein
Build von hier. Für den nächsten Patch beides gemeinsam hochziehen.

`appver_t` parst Ziffern vor den Punkten in `major`/`minor`/`build` und hat ein
`suffix[4]`; `name` ist `char[15]`, die Version darf also höchstens 14 Zeichen haben.

---

## 7. Google Home — was gilt

- **Fast jedes Google-Gerät ist ein Matter-Hub für WLAN-Geräte:** alle Nest- und
  Google-Home-Lautsprecher (Nest Mini, Nest Audio, Google Home Mini, Google Home Speaker),
  alle Nest-Displays, Nest Wifi Pro und der Google TV Streamer. Die oft zitierte engere
  Liste — Nest Hub 2. Gen, Hub Max, Nest Wifi Pro, Google TV Streamer — betrifft **Thread**,
  nicht Matter über WLAN. *(Eine frühere Fassung dieser Notizen hatte das zu streng
  dargestellt.)*
- **Matter Window Covering wird von Google Home voll unterstützt**, inklusive
  Assistant-Sprachsteuerung; es erscheint als „Blinds".
- **Cloud-to-Cloud:** Gerätetyp `action.devices.types.BLINDS`, Trait
  `action.devices.traits.OpenClose` mit `openPercent` (**0 = zu, 100 = offen**),
  `discreteOnlyOpenClose: false`, dazu `OpenCloseRelative` für „öffne 10 Prozent mehr".
  Eingerichtet wird das in der **Google Home Developer Console**, nicht mehr in der alten
  Actions-Konsole. Der Test-Modus reicht dauerhaft für den eigenen Haushalt.
- **Kopplung von Matter-Bridges scheitert in Google Home auf Android**: die App überträgt
  einen leeren Länder-Code, was die Matter-Spezifikation verbietet. Umweg: Erstkopplung mit
  iPhone oder iPad. Als Fremdfehler geschlossen in Matterbridge-Issue #445. Betrifft diesen
  Aufbau nicht mehr, weil er ohne Matter arbeitet.

---

## 8. iOS — warum es hier keine eigene App gibt

Ausführlich in `PLAN-IOS.md`. Die Kurzfassung:

- **Es gibt keinen Weg, eine Datei aus dem Repo zu laden und mit einem Tap zu installieren.**
  Jede Installation braucht eine Signatur — unsere oder die der Nutzerin.
- **Kostenlos und dauerhaft geht nativ nicht:** eine freie Apple-ID gibt 7-Tage-Zertifikate,
  maximal 3 sideloadete Apps und rund 10 App-IDs pro Woche, jederzeit widerrufbar. AltStore
  erneuert nur mit einem PC im selben WLAN, SideStore nach einmaliger Kopplung auch am Gerät
  allein.
- **Normal anfühlen kostet 99 €/Jahr** (Apple Developer Program, dann TestFlight).
- **Der Hauptblocker im Code wäre die lokale Netzwerkfreigabe** (iOS 14+): wird sie
  verweigert, ist der Status nicht abfragbar und der Fehler sieht aus wie „Gerät nicht
  erreichbar" (`NSURLErrorNotConnectedToInternet`). App Transport Security ist dagegen kein
  Problem — Roh-IPs sind ausgenommen.
- **Eine PWA hilft nicht**, solange sie fremd gehostet ist: Wand 1 und 2 aus Abschnitt 1.
  *(Eine frühere Fassung des Plans schlug „nginx im LAN" vor — das war falsch, denn ein
  zweiter Rechner im selben Netz ist trotzdem ein anderer Ursprung. Es müsste ein
  Reverse-Proxy sein, der App und API unter **einem** Ursprung ausliefert.)*
- **Interessant, aber ungenutzt:** das Web-UI der Firmware ist bereits als iOS-Web-App
  vorbereitet — `data/index.html` enthält `apple-mobile-web-app-capable`,
  `apple-mobile-web-app-title` und sieben `apple-touch-icon`-Größen. „Zum Home-Bildschirm"
  liefert also heute schon ein Icon ohne Browserleisten. iOS-Safari erlaubt das **auch über
  einfaches HTTP**, anders als Chrome.

---

## 9. Verworfene Wege, mit Begründung

| Weg | Warum verworfen |
|---|---|
| **Matter direkt in die Firmware** | Rechnet nicht auf: App 1,28 MB × 2 OTA-Partitionen + 1,44 MB LittleFS füllen einen 4-MB-ESP32 schon aus. esp-matter bringt 1,3–1,6 MB Code dazu. Nur auf ESP32-S3 mit 8/16 MB denkbar, dann als Fork mit gebrochener OTA-Kompatibilität. |
| **Eigenes Matterbridge-Plugin** (`matterbridge-espsomfy-rts`) | Technisch der schönste Weg — 8–12 Tage Arbeit **und** ein Dauerläufer bei ihr. Der MQTT-Patch liefert dasselbe Ergebnis in 3–4 Tagen ohne Hardware dort. Bleibt als Idee: es existiert nichts dergleichen. |
| **Home-Assistant-Kiste bei ihr** (Pi oder HA Green) | Null Zeilen eigener Code, aber 60–120 € Hardware in einem fremden Haushalt, den du dann fernwartest. Ausdrücklich nicht gewollt. |
| **Broker auf dem eigenen Server statt gehostet** | Ein Cloudflare-Tunnel kann MQTT nicht tragen — rohes TCP, kein HTTP. Es bräuchte eine echte Portweiterleitung auf 8883. Ein gehosteter Broker verlangt nirgends einen offenen Port. |
| **Web-App bei dir gehostet, die direkt mit ihrem Gerät redet** | Wand 1 (CORS) und Wand 2 (Mixed Content). Nicht von außen lösbar. |
| **Ihr Login in deiner bestehenden HA-Instanz** | HA hat keine belastbare Rechtetrennung je Entität; sie könnte über die API deine Wohnung steuern. Deshalb eine eigene Instanz je Haushalt. |
| **IFTTT** | Der Google-Assistant-Auslöser ist seit 31. August 2022 auf feste Phrasen beschnitten, und mit der Abschaltung der Conversational Actions am 13. Juni 2023 ist der Rest weggefallen. Prozentwerte gehen darüber nicht. |
| **Automatisierungen / Skript-Editor in Google Home** | Können nur Geräte ansprechen, die schon in Google Home sind. Keine freien HTTP-Aufrufe. |
| **Hue-Bridge-Emulation** | Google Home verlangt Kontoverknüpfung über Signify; eine lokal emulierte Bridge wird nicht mehr gefunden. |
| **SmartThings als Zwischenschicht** | Möglich, tauscht aber nur eine Hub-Abhängigkeit gegen eine andere. |
| **Zusätzliche HTML-Datei aufs Gerät legen** | Platz wäre da (442 von 1408 KB belegt), aber es gibt keinen generischen Datei-Handler; unbekannte Pfade laufen in einen 404. Ginge nur mit gepatchter Firmware und eigenem LittleFS-Image. |

---

## 10. Offene Punkte

1. **Auf echter Hardware ungetestet.** Der Code kompiliert für alle vier Boards. Ob die
   TLS-Verbindung dauerhaft hält, ob der Heap reicht und ob der Watchdog-Kniff greift, zeigt
   erst ein Gerät am Broker. **Erst am eigenen Gerät testen, nicht an ihrem.**
2. **Heap über 24 Stunden beobachten** — der Diagnose-Bildschirm der Handy-App oder das
   Feld `memory` in `GET /discovery`.
3. **Broker-Zertifikat wird nicht geprüft** (`setInsecure`). Ein `setCACert` mit der Wurzel
   des Brokers wäre die Verbesserung; kostet ein Konfigurationsfeld von 1–2 KB.
4. **Kein Protokoll-Schalter im Web-UI.** `mqtts://` lässt sich nur über
   `PUT /connectmqtt` setzen. Eine Auswahl in `data/index.js` wäre nett, kostet aber nur
   LittleFS-Platz — der ist frei.
5. **Alte Discovery-Konfigurationen aufräumen**, wenn ein bestehendes Gerät auf diesen Build
   umgestellt wird: die retained Nachricht am alten Topic bleibt liegen und erzeugt
   Doppel-Entitäten.
6. **Pull Request nach upstream?** Patch 2 behebt ein offenes Issue (#684), Patch 3 einen
   echten Fehler, Patch 1 ist eine sauber begrenzte Erweiterung. Upstream ist seit August
   2024 still, die Aussicht also gering — aber die Patches sind so geschrieben, dass sie
   einreichbar bleiben. Patch 4 (OTA-URLs) gehört ausdrücklich **nicht** dazu.
