"""Activity text sensor.

Publishes one of: idle, receiving, validating, writing, refreshing, error.
Values are produced by ``state_to_string()`` in transfer_engine.cpp -- keep the
two in step.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_OPENDISPLAY_ID, OPENDISPLAY_CLIENT_SCHEMA

DEPENDENCIES = ["opendisplay"]

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(OPENDISPLAY_CLIENT_SCHEMA)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_OPENDISPLAY_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(parent.set_activity_text_sensor(var))
