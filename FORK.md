# What this fork changes

A fork of [rstrouse/ESPSomfy-RTS](https://github.com/rstrouse/ESPSomfy-RTS), based on
`eb75868` (upstream's last commit, 19 August 2024). Upstream is dormant; this fork exists
to make one thing possible: **reaching a controller that sits in someone else's network,
without putting any extra hardware into that network.**

Three changes, nothing else.

---

## 1. MQTT over TLS (`mqtts://`)

`MQTT.cpp` only ever used a plain `WiFiClient`, so the broker had to live on the local
network — sending credentials in the clear across the internet is not an option. The
`protocol` setting already existed (`char protocol[10] = "mqtt://"` in `ConfigSettings.h`)
and `connect()` even checked it for length, but nothing ever read its value.

Now `connect()` picks the transport from it: anything starting with `mqtts` gets a
`WiFiClientSecure`. Two details worth knowing:

- **No extra flash.** `WiFiClientSecure` is already linked in — `GitOTA.cpp` has used it
  since forever to fetch releases from GitHub. This change adds the call sites, not the
  TLS stack.
- **Allocated on demand.** The secure client is created the first time an `mqtts://`
  broker is configured, so plain-MQTT installations pay neither heap nor handshake.

### Switching a device over

There is no protocol selector in the web UI, but `PUT /connectmqtt` on **port 80** accepts
a partial object — `fromJSON` checks each key with `containsKey`, so anything you leave out
keeps its current value:

```bash
curl -X PUT http://192.168.1.50/connectmqtt \
  -H "Content-Type: application/json" \
  -d '{"enabled":true,
       "protocol":"mqtts://",
       "hostname":"your-broker.example.com",
       "port":8883,
       "username":"shades",
       "password":"...",
       "rootTopic":"espsomfy",
       "pubDisco":true,
       "discoTopic":"homeassistant"}'
```

The route saves and reconnects immediately. `GET /mqttsettings` reads the state back.

### The security trade, stated plainly

The device stores no CA certificate, so `setInsecure()` is used and **the broker's
identity is not verified**. The session is still encrypted, which is what keeps the
credentials off the wire; an active man-in-the-middle, however, is not ruled out. This is
the same trade the OTA client already makes with GitHub. Pair it with per-device broker
credentials and topic ACLs that only permit that device's own root topic.

### Watchdog

The TLS handshake runs inside `PubSubClient::connect()` and takes a second or more on an
ESP32. The connect call is now bracketed by `esp_task_wdt_delete(NULL)` /
`esp_task_wdt_add(NULL)` — the same idiom `Web::handleStreamFile()` uses — so an
unreachable broker cannot reboot the controller.

### Publish chunk size

`publishBuffer()` streamed in 128-byte pieces. Over TLS every `write()` becomes its own
record with roughly 29 bytes of overhead, so the ~2 KB discovery payloads turned into
sixteen records. The chunk is now 512 bytes. `PubSubClient::write()` passes straight
through to the client during a `beginPublish()` stream, so nothing else had to change.

---

## 2. Home Assistant discovery fix

`SomfyShade::publishDisco()` published `via_device` carrying **the same identifier as the
device itself** — the line that would have made it different was commented out. Home
Assistant **2026.9.0** rejects that with *"A device can not be its own via device"*, and
the shades then never appear at all. This is upstream issue
[#684](https://github.com/rstrouse/ESPSomfy-RTS/issues/684).

`via_device` is now simply absent. Every shade belongs to the one controller device, so
there was never a parent to name. Existing installations keep working because their
devices were registered before the Home Assistant upgrade; a fresh setup hits the bug.

---

## 3. OTA points at this fork

`GitOTA.cpp` had the upstream repository hard-coded in five places. They now point here, so
the update check looks at this fork's releases instead of upstream's — otherwise a device
running this build could be talked into "updating" to upstream v2.4.6 and losing the patch.

Until this fork publishes a release, the update check simply finds nothing, which is the
correct answer.

---

## Building

No local toolchain needed. `.github/workflows/ci.yaml` compiles all four board variants on
every push, and `release.yaml` builds the flashable images when a release is published:

| Artifact | What it is |
|---|---|
| `SomfyController.ino.esp32.bin` | firmware only, for OTA |
| `SomfyController.littlefs.bin` | the web UI filesystem |
| `SomfyController.onboard.esp32.bin.zip` | merged image for a first flash over USB |

Pinned by the workflow: esp32 core 2.0.17, ArduinoJson 6.21.5, PubSubClient 2.8.0,
SmartRC-CC1101-Driver-Lib 2.5.7, WebSockets 2.4.0.

### Watch the flash budget

The `default` partition scheme gives the app **0x140000 = 1,310,720 bytes**, and upstream's
v2.4.6 build already used 1,305,536 of them. That is **about 5 KB of headroom**, so read
the `Sketch uses ... bytes (xx%) of program storage space` line in the CI log after every
change.

If it overflows, the fix is not to shrink the code but to rebalance the partitions: the
LittleFS partition is 1.44 MB while `data/` only needs 442 KB, so several hundred kilobytes
can move to the app partitions with a custom partition table. That changes the flash layout
and therefore requires one flash over USB rather than an OTA step.
