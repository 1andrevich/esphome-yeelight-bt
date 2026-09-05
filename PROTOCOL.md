# Yeelight BLE wire protocol

The protocol this component implements, as verified against
[hcoohb/hass-yeelightbt][spec] — the maintained Python implementation, and the
most complete reference available. Where it disagrees with the older
reverse-engineering notes in [Marcocanc/mi-lamp-re][re], the Python wins,
because it is the implementation known to work against current hardware. Those
disagreements are called out below.

[spec]: https://github.com/hcoohb/hass-yeelightbt
[re]: https://github.com/Marcocanc/mi-lamp-re

## GATT layout

| Role | UUID |
|---|---|
| Service | `8E2F0CBD-1A66-4B53-ACE6-B494E25F87BD` |
| Control (write) | `AA7D3F34-2D4F-41E0-807F-52FBF8CF7443` |
| Notify | `8F65073D-9F57-4AAA-AFEA-397D19D5BBEB` |

The Python reference addresses characteristics by UUID and never names a
service; the service UUID comes from the RE notes alone. ESPHome resolves
characteristics through their service, so this component needs it. If
resolution fails on your lamp, this UUID is the first suspect — the component
logs `Yeelight service/characteristics not found` and gives up cleanly.

## Frame format

Every request and response is exactly 18 bytes, zero padded.

```
[0]      0x43   magic / STX
[1]      command or response byte
[2..17]  payload, zero filled
```

## Commands

| Name | Byte | Payload |
|---|---|---|
| `CMD_PAIR` | `0x67` | `0x02` |
| `CMD_POWER` | `0x40` | `0x01` on / `0x02` off |
| `CMD_RGB` | `0x41` | R, G, B, `0x01`, brightness (1–100) |
| `CMD_BRIGHTNESS` | `0x42` | brightness (1–100) |
| `CMD_TEMP` | `0x43` | kelvin uint16 big-endian, brightness |
| `CMD_GETSTATE` | `0x44` | `0x02` |
| `CMD_GETNAME` | `0x52` | — |
| `CMD_GETVER` | `0x5C` | — |
| `CMD_GETSERIAL` | `0x5E` | — |

Brightness is **0–100, not 0–255**, and 0 is not a valid value — the component
scales and clamps to a minimum of 1. Colour temperature is 1700–6500 K.

`CMD_RGB` carries brightness in its own payload, so an RGB change is a single
write; no follow-up `CMD_BRIGHTNESS` is needed.

> **Sources disagree:** the RE notes describe `CMD_PAIR` as carrying a 16-byte
> client UUID, and give the 4th byte of `CMD_RGB` as `0x00` rather than `0x01`.
> This component follows the Python on both counts. If pairing or colour never
> works on your lamp, the `yeelight_bt.send_raw` action lets you try the
> RE-notes variants without changing any code.

## Responses

| Name | Byte | Meaning |
|---|---|---|
| `RES_GETSTATE` | `0x45` | state, see below |
| `RES_PAIR` | `0x63` | `[2]` = pairing code, see below |
| `RES_GETNAME` | `0x53` | ASCII name |
| `RES_GETVER` | `0x5D` | firmware version, field layout unresolved |
| `RES_GETSERIAL` | `0x5F` | serial, field layout unresolved |

### `RES_GETSTATE` — Bedside

| Byte | Field |
|---|---|
| `[2]` | power (`0x01` on) |
| `[3]` | mode: `0x01` colour, `0x02` white, `0x03` flow |
| `[4]` | red |
| `[5]` | green |
| `[6]` | blue |
| `[7]` | unknown |
| `[8]` | brightness, 0–100 |
| `[9..10]` | colour temperature, uint16 big-endian |

Note RGB occupies `[4][5][6]`. The RE notes place it at `[5][6][7]`, which
reads `(green, blue, unknown)` as the colour. The Python's
`struct.unpack(">xxBBBBBBBhx6x", ...)` and its field assignment settle it.

Byte `[7]` has no known purpose. The component logs it as `byte7=0x..` on every
state notification, so it can be identified if anyone works it out.

### `RES_GETSTATE` — Candela

| Byte | Field |
|---|---|
| `[2]` | power |
| `[3]` | brightness |
| `[4]` | mode |

Untested — no Candela hardware was available.

### `RES_PAIR` codes

| Code | Meaning | Component behaviour |
|---|---|---|
| `0x01` | pairing requested; lamp pulses, user must press its button | `PAIRING` |
| `0x02` | paired | `PAIRED` |
| `0x03` | unpaired | `UNPAIRED` |
| `0x04` | already paired | `PAIRED` |
| `0x06` | error | error; on Candela, treated as `PAIRING_UNSUPPORTED` |
| `0x07` | error | error; on Candela, treated as `PAIRING_UNSUPPORTED` |

`0x04` is why the component can send `CMD_PAIR` on every connect: an
already-paired lamp answers immediately, with no button press. The pairing
payload carries no client identity, so there is nothing for the ESP to persist —
the pairing lives in the lamp.

On a Bedside lamp, `0x06`/`0x07` are treated as a hard error; a factory reset is
usually needed. The RE notes suggest `0x07` means "pairing not supported" on
Candela, which is why that model treats it as `PAIRING_UNSUPPORTED` and proceeds
without pairing.

## Implementation notes

**Write pacing.** The lamp's GATT stack drops or corrupts commands sent back to
back. Writes go through a queue drained every `write_interval` (80 ms default),
coalescing queued frames that share a command byte, so a dragged brightness
slider collapses to one write.

**Echo suppression.** The lamp reports state right after a write. Incoming state
is ignored for 500 ms after any write of the component's own, and state pushed
into the frontend is marked so it is not written straight back to the lamp.

**Notifications before commands.** No command is sent until
`ESP_GATTC_REG_FOR_NOTIFY_EVT` arrives. Writing before the subscription
completes is a silent failure. ESP-IDF writes the CCCD as part of
`esp_ble_gattc_register_for_notify()`, so this component does not write the
descriptor by hand — matching the in-tree `anova` and `am43` components.

## Open questions

Answers welcome, ideally with a log.

1. **Byte `[7]` of `RES_GETSTATE`.** Purpose unknown. Logged on every state
   notification.
2. **`CMD_TEMP` while in colour mode.** The Python sends it with no preceding
   mode change, and this component does the same, on the assumption that the
   lamp switches mode implicitly.
3. **Does the lamp push unsolicited state?** If it reports state after its own
   touch control is used without being asked, `poll_interval` could default to
   `0s` and save meaningful BLE traffic. Set `poll_interval: 0s` and watch for
   `RES_GETSTATE` you did not request.
4. **`RES_GETVER` / `RES_GETSERIAL` field layout.** The Python unpacks the
   version as `">xxBHHHH6x"` and casts the resulting tuple to `str`, which looks
   like a bug there, so the real layout is not established. This component logs
   the raw payload instead of inventing a format.
