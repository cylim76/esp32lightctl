#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * A mains-powered coordinator cannot force a third-party sleepy end device to
 * poll at a particular instant.  Spread a bounded set of idempotent ON/OFF
 * commands across a long enough window to meet several parent poll cycles.
 * Attempt zero is the immediate transmission.
 */
#define ZIGBEE_CONTROL_RETRY_ATTEMPTS 5U

uint32_t zigbee_control_retry_offset_ms(uint8_t attempt);
bool zigbee_control_retry_has_next(uint8_t attempt);

