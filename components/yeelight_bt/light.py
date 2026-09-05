"""Config schema and codegen for the `yeelight_bt` light platform."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import ble_client, light
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_ID,
    CONF_MODEL,
    CONF_OUTPUT_ID,
    CONF_TRIGGER_ID,
)

from . import (
    CONF_ON_PAIRED,
    CONF_POLL_INTERVAL,
    CONF_WRITE_INTERVAL,
    MODELS,
    PairAction,
    PairedTrigger,
    RequestStateAction,
    SendRawAction,
    YeelightBTLight,
)

DEPENDENCIES = ["ble_client", "esp32"]

# The Bedside Lamp is the primary target and supports the full colour set, so
# the RGB schema is used for both models. Capability is gated at runtime in
# get_traits(); a Candela therefore advertises brightness only, even though the
# schema would accept RGB effects.
CONFIG_SCHEMA = (
    light.RGB_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(YeelightBTLight),
            cv.Optional(CONF_MODEL, default="bedside"): cv.enum(MODELS, lower=True),
            # 0 disables polling entirely. Poll only if the lamp turns out not
            # to push unsolicited state after its own touch control is used.
            cv.Optional(
                CONF_POLL_INTERVAL, default="30s"
            ): cv.positive_time_period_milliseconds,
            # Drain interval for the outbound queue. The lamp's GATT stack
            # drops or corrupts commands sent back to back.
            cv.Optional(
                CONF_WRITE_INTERVAL, default="80ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_PAIRED): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PairedTrigger),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_write_interval(config[CONF_WRITE_INTERVAL]))

    for conf in config.get(CONF_ON_PAIRED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


YEELIGHT_BT_ACTION_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(YeelightBTLight),
    }
)


@automation.register_action(
    "yeelight_bt.pair", PairAction, YEELIGHT_BT_ACTION_SCHEMA, synchronous=True
)
async def yeelight_bt_pair_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "yeelight_bt.request_state", RequestStateAction, YEELIGHT_BT_ACTION_SCHEMA, synchronous=True
)
async def yeelight_bt_request_state_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "yeelight_bt.send_raw",
    SendRawAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(YeelightBTLight),
            # Frames shorter than 18 bytes are zero padded on the C++ side.
            cv.Required(CONF_DATA): cv.All(
                cv.ensure_list(cv.hex_uint8_t), cv.Length(min=2, max=18)
            ),
        }
    ),
    # play() enqueues and returns; play_next_() is never deferred.
    synchronous=True,
)
async def yeelight_bt_send_raw_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_data(config[CONF_DATA]))
    return var
