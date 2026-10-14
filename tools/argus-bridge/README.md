# Argus K10 Attendance Bridge

A small LAN service that receives attendance events from the K10. Volcano voice
is **not** proxied through this bridge: firmware uses ConversationalAI Embedded
Kit 2.0 and Volcano `InstanceID` / `ProductKey` / `ProductSecret` / `BotID`
directly.

## Run

```bash
cd tools/argus-bridge
cp .env.example .env
python3 argus_bridge.py --check
python3 argus_bridge.py
```

Keep the Mac and K10 on the same 2.4 GHz LAN. Configure the K10 with the Mac's
LAN address through the user-only MCP tool `argus.bridge.set_config`.

## API

- `GET /health`
- `POST /attendance/event` with `Authorization: Bearer <ARGUS_BRIDGE_TOKEN>`

Required fields: `event_id`, `person_id`, `device_id`, `timestamp`. Records are
appended to `data/attendance.jsonl`; no face image or feature vector is uploaded.

Volcano credentials belong in Xiaozhi menuconfig under **Argus Volcano voice**,
not in this bridge.
