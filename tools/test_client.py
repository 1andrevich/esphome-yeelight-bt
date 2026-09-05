#!/usr/bin/env python3
"""Minimal aioesphomeapi client for driving the yeelight_bt light entity.

There is no Home Assistant in this setup, so this is the reference consumer of
the device's native API.

    pip install aioesphomeapi

    python tools/test_client.py --host yeelight-bedside.local --key <API_KEY> --list
    python tools/test_client.py --host ... --key ... --on --brightness 60
    python tools/test_client.py --host ... --key ... --rgb 255,80,0 --brightness 100
    python tools/test_client.py --host ... --key ... --kelvin 2700 --brightness 40
    python tools/test_client.py --host ... --key ... --off
    python tools/test_client.py --host ... --key ... --watch

--key is the base64 `api.encryption.key` from your secrets file.
"""

from __future__ import annotations

import argparse
import asyncio
import sys

try:
    from aioesphomeapi import APIClient
except ImportError:  # pragma: no cover
    sys.exit("aioesphomeapi is not installed. Run: pip install aioesphomeapi")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="device hostname or IP")
    parser.add_argument("--port", type=int, default=6053)
    parser.add_argument("--key", required=True, help="api.encryption.key (base64)")
    parser.add_argument("--password", default="", help="legacy API password, normally empty")
    parser.add_argument("--entity", default=None, help="object_id of the light; defaults to the first light found")

    parser.add_argument("--list", action="store_true", help="list entities and services, then exit")
    parser.add_argument("--watch", action="store_true", help="subscribe to state changes and print them")

    group = parser.add_mutually_exclusive_group()
    group.add_argument("--on", action="store_true")
    group.add_argument("--off", action="store_true")

    parser.add_argument("--brightness", type=float, default=None, help="0-100")
    parser.add_argument("--rgb", default=None, help="R,G,B each 0-255")
    parser.add_argument("--kelvin", type=int, default=None, help="1700-6500")
    parser.add_argument("--transition", type=float, default=None, help="transition length in seconds")
    return parser.parse_args()


async def resolve_light(client: APIClient, wanted: str | None):
    entities, services = await client.list_entities_services()

    lights = [e for e in entities if type(e).__name__ == "LightInfo"]
    if not lights:
        raise SystemExit("No light entities found on this device.")

    if wanted is None:
        return lights[0], entities, services

    for light in lights:
        if light.object_id == wanted or light.name == wanted:
            return light, entities, services

    available = ", ".join(light.object_id for light in lights)
    raise SystemExit(f"Light {wanted!r} not found. Available: {available}")


def describe(entities, services) -> None:
    print("Entities:")
    for entity in entities:
        kind = type(entity).__name__.replace("Info", "")
        print(f"  [{kind}] object_id={entity.object_id!r} name={entity.name!r} key={entity.key}")
        modes = getattr(entity, "supported_color_modes", None)
        if modes:
            print(f"      supported_color_modes={modes}")
        if getattr(entity, "min_mireds", None):
            print(f"      mireds={entity.min_mireds}..{entity.max_mireds}")
    print("Services:")
    for service in services:
        print(f"  {service.name}")
    if not services:
        print("  (none)")


async def main() -> None:
    args = parse_args()

    client = APIClient(
        address=args.host,
        port=args.port,
        password=args.password,
        noise_psk=args.key or None,
    )

    await client.connect(login=True)
    try:
        light, entities, services = await resolve_light(client, args.entity)

        if args.list:
            describe(entities, services)
            return

        print(f"Using light: object_id={light.object_id!r} key={light.key}")

        # Build the light_command kwargs from whatever the user asked for.
        command: dict = {"key": light.key}

        if args.off:
            command["state"] = False
        elif args.on or args.brightness is not None or args.rgb or args.kelvin:
            command["state"] = True

        if args.brightness is not None:
            command["brightness"] = max(0.0, min(1.0, args.brightness / 100.0))

        if args.rgb:
            try:
                red, green, blue = (int(part) for part in args.rgb.split(","))
            except ValueError:
                raise SystemExit("--rgb wants three comma-separated numbers, e.g. 255,80,0")
            command["rgb"] = (red / 255.0, green / 255.0, blue / 255.0)

        if args.kelvin:
            if not 1700 <= args.kelvin <= 6500:
                raise SystemExit("--kelvin must be between 1700 and 6500")
            # The API speaks mireds.
            command["color_temperature"] = 1_000_000 / args.kelvin

        if args.transition is not None:
            command["transition_length"] = args.transition

        if len(command) > 1:
            print(f"Sending: {command}")
            client.light_command(**command)
            # Give the queued BLE writes a moment to drain before disconnecting.
            await asyncio.sleep(1.5)

        if args.watch:
            print("Watching for state changes. Touch the lamp's own control to test readback. Ctrl-C to stop.")

            def on_state(state) -> None:
                if getattr(state, "key", None) != light.key:
                    return
                bits = [f"state={getattr(state, 'state', None)}"]
                for attr in ("brightness", "color_mode", "red", "green", "blue", "color_temperature"):
                    value = getattr(state, attr, None)
                    if value is not None:
                        bits.append(f"{attr}={value}")
                print("  " + " ".join(bits))

            client.subscribe_states(on_state)
            while True:
                await asyncio.sleep(3600)
    finally:
        await client.disconnect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
