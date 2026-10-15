# Argus K10 attendance and intercom

`argus-k10` is a separate board identity based on DFRobot UNIHIKER K10 hardware.
It keeps the stock `df-k10` firmware untouched so Argus OTA updates cannot be
mistaken for stock Xiaozhi releases.

## Current firmware scope

- Standard Xiaozhi Wi-Fi provisioning, audio, display, buttons, and camera support.
- Local ESP-DL face detection/recognition with a persistent `argus` SPIFFS database.
- A three-minute duplicate-attendance cooldown.
- Attendance display feedback and LAN upload to the lightweight Argus Bridge.
- Volcano ConversationalAI Embedded Kit 2.0 official low-load WebSocket intercom.
- MCP tools:
  - `argus.attendance.enroll`
  - `argus.attendance.status`
  - `argus.attendance.clear`
  - `argus.bridge.set_config` (user-only)
  - `argus.voice.start`
  - `argus.voice.stop`

Button A retains Xiaozhi's single-click wake/chat behavior. Button B retains
short/long volume behavior. **Double-click either A or B** starts or stops the
Volcano intercom, avoiding dependence on the K10 silkscreen/button-channel
naming.

The Volcano transport runs half-duplex on this first-generation build: while
the cloud agent is speaking, microphone upload is paused to avoid the device
hearing its own speaker. Attendance scanning and its face models are also
released while the intercom is active, then reloaded after the intercom stops.

## Build

```bash
source /path/to/esp-idf/export.sh
python3 scripts/build.py argus/k10 --name argus-k10
```

The build includes the official ConversationalAI Embedded Kit 2.0 submodule and
uses its low-load WebSocket transport with 16 kHz PCM16 audio. It does not link
the high-quality RTC Lite library. The Argus partition table keeps large OTA
slots for Xiaozhi assets and the face models.

The canonical build command also writes a generated board marker for ESP-IDF 6's
early dependency-discovery phase. Volcano credential values are carried between
ignored local `sdkconfig` generations, so rebuilding or switching back from a
stock board does not silently clear them.

## Validation status

- Argus K10 and a representative stock ESP32-S3 camera board both build with
  ESP-IDF 6.1.
- The stock build contains no Argus/Volcano symbols or credentials.
- Final K10 startup completes camera, face-database, audio, MCP tool, and Wi-Fi
  initialization with about 76 KB internal SRAM free while idle.
- End-to-end Volcano speech requires the device to see a saved 2.4 GHz AP. If no
  AP is visible, it remains in normal Xiaozhi provisioning/connection behavior.

## Experimental RTC branch

`argus/k10-v2.5.2-rtc-test` links Volcano RTC Lite with G.711A and an
experimental 16 MB partition table. RTC Lite adds about 205 KiB to each OTA
slot, so the test layout reduces the local face-database partition from 1.3 MiB
to 832 KiB. It is not production-ready.

On ESP-IDF 6.1, initial runtime tests reached device registration and RTC config
fetch, then triggered a TLS heap assertion in the prebuilt RTC Lite network
stack. The branch retains the failed experiment for comparison and debugging;
the main `argus/k10-v2.5.2` branch remains on the stable low-load WebSocket
transport.

## Wi-Fi

Do not hard-code Wi-Fi credentials. On first boot, the device starts normal
Xiaozhi hotspot provisioning. Press A while starting to re-enter that flow. Use a
2.4 GHz network.

## Volcano credentials

Configure these in `idf.py menuconfig` under **Argus Volcano voice**:

- `VOLC_INSTANCE_ID`
- `VOLC_PRODUCT_KEY`
- `VOLC_PRODUCT_SECRET`
- `VOLC_DEVICE_NAME` (first device: `CN1510340039`)
- `VOLC_BOT_ID`

This is the ConversationalAI Embedded Kit 2.0 low-load model: the device
dynamically registers with ProductKey/ProductSecret and exchanges 16 kHz PCM16
audio with the configured Bot over WebSocket. The earlier RTC AppKey plus
Volcano AccessKey/Secret bridge design is no longer used.

Keep ProductSecret out of public repositories. Production firmware should place
per-device credentials in protected provisioning/NVS storage rather than commit
a populated `sdkconfig`.

## Attendance bridge

```bash
cd tools/argus-bridge
cp .env.example .env
python3 argus_bridge.py --check
python3 argus_bridge.py
```

The bridge only receives JSON attendance events. It does not proxy audio and does
not hold Volcano credentials. Configure the K10 with `argus.bridge.set_config`
and the Mac LAN address, for example `http://192.168.1.10:8787`.

No face image or feature vector is uploaded; only person ID, confidence, device
ID, event ID, and timestamp are sent.

## Production caveats

Local face recognition is not liveness-proof; photos can defeat it. Production
attendance should add liveness detection or use a cloud face service with
anti-spoofing. Person management, reports, and audit storage should move to a
server.
