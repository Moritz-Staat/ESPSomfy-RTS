# What this fork changes

A fork of [rstrouse/ESPSomfy-RTS](https://github.com/rstrouse/ESPSomfy-RTS), based on
`eb75868` (upstream's last commit, 19 August 2024). Upstream is dormant; this fork exists
to make one thing possible: **reaching a controller that sits in someone else's network,
without putting any extra hardware into that network.**

Four changes, nothing else.

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

The web UI already has the selector — `MQTT` / `MQTTS` next to the host field, which
upstream shipped without a consumer. Because the comparison is case-insensitive, the value
`MQTTS` it stores engages TLS directly. Since v2.4.8 the selector also proposes the
matching port when you switch.

For scripted setup, `PUT /connectmqtt` on **port 80** accepts a partial object — `fromJSON`
checks each key with `containsKey`, so anything you leave out keeps its current value:

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

## 3. Discovery topics carry the controller id

The discovery configuration was published to `<prefix>/cover/<shadeId>/config` — the
controller is nowhere in that path. Two ESPSomfy controllers on one broker therefore
publish shade 1 to the very same topic and silently overwrite each other's configuration;
only one of the two ever appears in Home Assistant. The `unique_id` inside the payload
differs, but that does not help — the retained message at that topic is what HA reads.

Home Assistant's discovery topic accepts an optional node id:
`<prefix>/<component>/[<node_id>/]<object_id>/config`. The controller's `serverId` (six hex
characters, so within the allowed character set) is now published as that node id, in all
six places that build such a topic — publish, unpublish, and the cleanup path for deleted
shades:

```
homeassistant/cover/a1b2c3/1/config
```

**Upgrading an existing installation can leave orphans.** `unpublishDisco()` and the
cleanup path for deleted shades clear the pre-v2.4.8 topic as well, so unpublishing or
deleting a shade tidies up after itself. This does **not** happen at boot on purpose: if a
second controller on the same broker still runs the old build, clearing that topic would
wipe its live configuration.

If a plain upgrade left a duplicate device behind, clear it once by hand and delete the
leftover device in Home Assistant:

```bash
mosquitto_pub -h <broker> -t 'homeassistant/cover/1/config' -r -n
```

---

## 4. OTA points at this fork

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

### Flash budget

Two builds, two numbers. **The release build is the one that matters** — that is what gets
flashed. `ci.yaml` pins esp32 core 2.0.10, `release.yaml` pins 2.0.17, and the difference is
substantial: on the classic ESP32 the newer core costs another 43 KB.

App partition (`default` scheme) is 0x140000 = **1,310,720 bytes**.

| Board | Release build (core 2.0.17) | | CI build (core 2.0.10) |
|---|---:|---:|---:|
| **ESP32** | 1,299,429 B — **99 %**, ~11 KB free | | 1,256,409 B — 95 % |
| ESP32-C3 | 1,216,946 B — 92 %, ~92 KB free | | 1,191,098 B — 90 % |
| ESP32-S2 | 1,184,326 B — 90 %, ~124 KB free | | 1,163,978 B — 88 % |
| ESP32-S3 | 1,175,649 B — 89 %, ~132 KB free | | 1,172,609 B — 89 % |

Static RAM on the ESP32 release build: globals 95,096 bytes (29 %), leaving 232,584 bytes
for local variables and the heap. A TLS session holds roughly 20–35 KB of that while open.

**What this patch costs:** the v2.4.8 `esp32.bin` asset is 1,306,000 bytes against upstream
v2.4.6's 1,305,536 — a difference of about **464 bytes**. The TLS stack was already linked
in for the OTA client, so `mqtts://` really is close to free. The 99 % is upstream's
baseline on this core, not something this fork introduced.

**But 11 KB is tight.** Read the `Sketch uses ... bytes` line in the release log after every
change. If a future change overflows, the fix is not to shrink the code but to rebalance the
partitions: LittleFS gets 1.44 MB while `data/` only needs 442 KB, so several hundred
kilobytes can move to the app partitions with a custom partition table. That changes the
flash layout and therefore requires one flash over USB rather than an OTA step.

Do not judge the budget by the size of a published `.bin` asset either way — the compiler's
own line is the number that counts.
