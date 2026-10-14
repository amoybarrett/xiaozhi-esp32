# Argus UNIHIKER K10

`argus-k10` is the Argus attendance/intercom firmware variant for DFRobot
UNIHIKER K10 hardware. It intentionally does not replace the stock
`dfrobot/df-k10` board identity, so Argus OTA releases remain separate.

## Hardware

* SoC: ESP32-S3
* Camera: GC2145
* Display: ILI9341, 240x320
* Buttons: A/B (double-click B toggles Volcano intercom)
* Audio: 24 kHz duplex audio

## Build

```bash
source /path/to/esp-idf/export.sh
python3 scripts/build.py argus/k10 --name argus-k10
```

Wi-Fi follows the standard Xiaozhi provisioning flow. On first boot, or after
button A is pressed while the device is starting, the device starts its
configuration hotspot; join that hotspot with a phone or computer and complete
the web form. Do not hard-code Wi-Fi credentials into the firmware.

## Argus additions

Face attendance uses a dedicated `argus` SPIFFS partition. Volcano intercom uses
the ConversationalAI Embedded Kit 2.0 submodule's official low-load WebSocket
transport and ProductKey/ProductSecret provisioning. See `docs/argus-k10.md`
for configuration and test status.
