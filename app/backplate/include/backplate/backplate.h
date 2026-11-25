#ifndef BACKPLATE_BACKPLATE_H
#define BACKPLATE_BACKPLATE_H

#include <stddef.h>
#include <stdint.h>

/* The ecobee3 backplate: a KL14 microcontroller on a UART that owns the HVAC relays and the
 * temperature, humidity, occupancy and proximity sensors. ThermoDeck never drives the
 * relays. It only sends "all relays off" at startup and then stays silent, so the
 * backplate's own comms watchdog keeps them off. BACKPLATE.md documents the protocol. */

#define BACKPLATE_DEV "/dev/ttySP1"

/* Message types (host -> backplate) used here. */
#define BP_TX_GROUPED_OUTPUT 0x10 /* payload: u16 relay bitmask, big-endian */

/* Opens and configures the UART (19200 8N1 raw). Returns the fd, or -1. */
int backplate_open(const char *dev);

/* CRC-16/X-25 over type + payload, as the backplate checks it. */
uint16_t backplate_crc(const uint8_t *data, size_t len);

/* Builds a frame (F2 86 len type payload crc_hi crc_lo) into out. Returns its length, or
 * -1 if it doesn't fit. */
int backplate_frame(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t outlen);

/* Opens dev (NULL = BACKPLATE_DEV) and commands every relay off. 0 if sent. */
int backplate_relays_off(const char *dev);

/* Commands every relay off, then starts a thread that only reads the sensor messages
 * (it never writes to the backplate again). */
void backplate_start(const char *dev);

/* now_ms() of the last time something came near the screen (proximity sensor), or 0 if
 * nothing has yet. Safe to call from any thread. */
uint64_t backplate_near_ms(void);

#endif
