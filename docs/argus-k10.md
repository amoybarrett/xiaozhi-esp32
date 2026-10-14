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

Button A retains Xiaozhi's wake/chat behavior. Button B retains short/long
volume behavior; **double-click B** starts or stops the Volcano intercom.

## Build

```bash
source /path/to/esp-idf/export.sh
python3 scripts/build.py argus/k10 --name argus-k10
```

The build includes the official ConversationalAI Embedded Kit 2.0 submodule and
uses its low-load WebSocket transport with 16 kHz PCM16 audio. It does not link
the high-quality RTC Lite library. The Argus partition table keeps large OTA
slots for Xiaozhi assets and the face models.

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
