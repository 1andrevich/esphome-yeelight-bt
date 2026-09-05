# esphome-yeelight-bt

ESPHome external component for **Yeelight Bluetooth-only lamps** based on https://github.com/hcoohb/hass-yeelightbt.
 Talks to the lamp directly from an ESP32 over BLE GATT and exposes it as a native ESPHome
`light` entity — full RGB, colour temperature and brightness.

No cloud, Yeelight app or Home Assistant required. Works with HA as ESPHome entity,
and equally well over the plain ESPHome native API.

Compatible boards:
All ESP32 with BLE and Wi-Fi:
ESP32, ESP32-C3, ESP32-S3, ESP32-C6, ESP32-C5 etc.

## Status

Verified on a Yeelight Bedside Lamp Gen1 with a Seeed XIAO ESP32-C6:

- pairing, including surviving a reboot without re-pairing
- on/off, brightness, RGB colour, colour temperature
- state readback from the lamp (verified for on/off)

Not verified: Candela hardware (code paths exist, shipped untested), and state
readback for colour and brightness specifically.

| | Tested with |
|---|---|
| ESPHome | `2026.8.1` |
| ESP-IDF | `5.5.5` |
| Board | Seeed XIAO ESP32-C6 |

The `esp32_ble_client` API changes between ESPHome releases; older versions may
not compile.

## Supported lamps

| Model | Advertised name | Capabilities |
|---|---|---|
| Bedside Lamp | `XMCTD_*` | RGB, colour temperature (1700–6500 K), brightness |
| Candela | `yeelight_ms*` | Brightness only — **untested, no hardware available** |

Out of scope: Wi-Fi Yeelight lamps (they use the LAN protocol and are already
supported by ESPHome/HA), and more than one lamp per `ble_client`.

## Quick start

**1. Find your lamp's MAC.** Flash a config with just `esp32_ble_tracker:` and a
logger and read it from the scan, or use nRF Connect on a phone. Close the
Yeelight app completely first — including from the background — or the lamp will
not advertise.

**2. Add the component:**

```yaml
external_components:
  - source: github://1andrevich/esphome-yeelight-bt@main
    components: [yeelight_bt]

ble_client:
  - id: lamp_ble
    mac_address: "F8:24:41:XX:XX:XX"

light:
  - platform: yeelight_bt
    id: bedside
    output_id: bedside_lamp
    name: "Bedside Lamp"
    ble_client_id: lamp_ble
```

See [`example.yaml`](example.yaml) for a complete, working configuration.

**3. Pair.** Add a button and press it once:

```yaml
button:
  - platform: template
    name: "Pair Lamp"
    on_press:
      - yeelight_bt.pair: { id: bedside_lamp }
```

The log will say one of:

- `Already paired` — done, nothing to do.
- `Lamp is pulsing and waiting for you to PRESS ITS BUTTON` — press the lamp's
  own button, then look for `Paired`.
- A pairing error — the lamp may need a factory reset before it will pair again.

Pairing is stored **in the lamp**, not on the ESP, so nothing is written to
flash and re-pairing is not needed after a reboot.

## Configuration

| Option | Default | Meaning |
|---|---|---|
| `ble_client_id` | **required** | The `ble_client` holding this lamp's MAC. |
| `id` | generated | The light entity (a `LightState`). |
| `output_id` | generated | The component itself. **The `yeelight_bt.*` actions take this id, not `id`.** |
| `model` | `bedside` | `bedside` or `candela`. Sets the supported colour modes. Not auto-detected. |
| `poll_interval` | `30s` | How often to ask the lamp for its state. `0s` disables polling. |
| `write_interval` | `80ms` | Minimum gap between BLE writes. |
| `on_paired` | — | Automation fired when the lamp reports it is paired. |

Plus all the standard ESPHome light options (`name`, `icon`, `restore_mode`,
effects, and so on).

### Actions

```yaml
- yeelight_bt.pair: { id: bedside_lamp }
- yeelight_bt.request_state: { id: bedside_lamp }
- yeelight_bt.send_raw: { id: bedside_lamp, data: [0x43, 0x40, 0x01] }
```

`send_raw` writes an arbitrary frame, zero-padded to 18 bytes. It is an escape
hatch for protocol experiments — see [PROTOCOL.md](PROTOCOL.md).

## Hardware notes

**One connection only.** The lamp accepts exactly one BLE connection. If the
phone app holds it, the ESP cannot connect. Everyone hits this once; the
component logs a message naming this cause after three failed attempts.

**ESP32-C6 shares one radio** between Wi-Fi and BLE. Two settings matter:

```yaml
wifi:
  power_save_mode: none # otherwise BLE and the fallback AP both suffer
esp32_ble_tracker:
  software_coexistence: true # the default when wifi is configured
```

Power save is worth turning off if you see unstable BLE, or a fallback AP that
phones struggle to join.

**Antenna.** The XIAO C6's ceramic antenna is weak for BLE. Aim for better than
about −75 dBm at the install distance.

**`bluetooth_proxy` coexistence.** Both can run on one node, but BLE RAM
pressure is real — avoid stacking voice or audio components alongside.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Connection failed` repeatedly | Yeelight app (or another ESP) holds the lamp's single BLE connection. |
| `Yeelight service/characteristics not found` | Not a BLE Yeelight, or a firmware revision using different UUIDs. |
| Commands logged as dropped | Not paired yet. Run `yeelight_bt.pair` and press the lamp's button. |
| Pairing never leaves `PAIRING` | Press the lamp's physical button while it pulses. |
| Pairing error `0x06`/`0x07` | Factory reset the lamp, then pair again. |
| Colours or brightness lag behind | Raise `write_interval`; the lamp's GATT stack drops back-to-back writes. |

Set `logger: level: VERBOSE` to see every frame on the wire.

## Repository layout

```
components/yeelight_bt/   the component
example.yaml              complete working configuration
tests/                    compile-only configs used by CI
tools/test_client.py      aioesphomeapi client
PROTOCOL.md               the wire protocol, as verified
```

## Credits

Protocol derived from [hcoohb/hass-yeelightbt][spec] (the maintained Python
implementation, used as the reference) and [Marcocanc/mi-lamp-re][re] (original
reverse-engineering notes), with [rytilahti/python-yeelightbt][orig] as the
ancestor of both.

[spec]: https://github.com/hcoohb/hass-yeelightbt
[re]: https://github.com/Marcocanc/mi-lamp-re
[orig]: https://github.com/rytilahti/python-yeelightbt

## License

MIT — see [LICENSE](LICENSE).
