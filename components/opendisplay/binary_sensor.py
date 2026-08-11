"""Busy binary sensor.

Defaults to false at boot and publishes on every busy transition, including the
physical refresh -- busy is NOT cleared at end-of-transfer.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import CONF_OPENDISPLAY_ID, OPENDISPLAY_CLIENT_SCHEMA

DEPENDENCIES = ["opendisplay"]

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(OPENDISPLAY_CLIENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENDISPLAY_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.set_busy_binary_sensor(var))
