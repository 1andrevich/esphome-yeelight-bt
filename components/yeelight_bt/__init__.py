"""Yeelight Bluetooth-only lamp support for ESPHome.

Controls Yeelight BLE lamps (Bedside Lamp "XMCTD_*", Candela "yeelight_ms*")
directly from an ESP32 over BLE GATT, exposing them as native `light` entities.

The component is only usable through its `light` platform; see light.py.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import ble_client, light

CODEOWNERS = ["@radle"]
DEPENDENCIES = ["ble_client", "esp32"]

yeelight_bt_ns = cg.esphome_ns.namespace("yeelight_bt")

YeelightBTLight = yeelight_bt_ns.class_(
    "YeelightBTLight",
    light.LightOutput,
    cg.Component,
    ble_client.BLEClientNode,
)

Model = yeelight_bt_ns.enum("Model", is_class=True)

MODELS = {
    "bedside": Model.BEDSIDE,
    "candela": Model.CANDELA,
}

# Actions and triggers
PairAction = yeelight_bt_ns.class_("PairAction", automation.Action)
RequestStateAction = yeelight_bt_ns.class_("RequestStateAction", automation.Action)
SendRawAction = yeelight_bt_ns.class_("SendRawAction", automation.Action)
PairedTrigger = yeelight_bt_ns.class_("PairedTrigger", automation.Trigger.template())

CONF_POLL_INTERVAL = "poll_interval"
CONF_WRITE_INTERVAL = "write_interval"
CONF_ON_PAIRED = "on_paired"
