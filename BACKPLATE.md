# The ecobee3 backplate

The ecobee3 ("Athena") has a second processor behind the screen: a **Kinetis KL14**
(Cortex-M0+) on the backplate. It owns the HVAC relays and the temperature, humidity,
occupancy and proximity sensors, plus the 915 MHz radio for room sensors. The i.MX28 running
ThermoDeck talks to it over one UART.

This was worked out from the stock software (`libcore-4.8.7.so`: `AthenaSerialInterface`,
`DevMgrRootDeviceTstat` in `dmAthenaDevice.cpp`, and the backplate firmware
`/idt/backplate-0.0.212.hex`), then checked against traffic captured on the device.

**ThermoDeck does not use the relays.** At startup it sends "all relays off" once
(`app/backplate/backplate.c`) and then sends nothing more. The relay commands below are
documented for reference only. ThermoDeck does read the port: `app/backplate/sensors.c` uses
the proximity readings for the screen timeout (see Proximity below).

## Safety: why the relays stay off

Two things in the backplate firmware keep the HVAC off while ThermoDeck runs:

1. **Comms watchdog.** If the host sends nothing for about 3 s, the KL14 switches every relay
   off. It does this again every time the silence lasts that long, and when the host speaks
   again it prints `Hello, Comms Back <n>` (debug message `0x46`).
2. **No configuration, no relays.** The KL14 refuses relay commands until it has received a
   relay configuration (`0x1B`) after the startup handshake. ThermoDeck never sends one, so
   the KL14 answers any relay command with error `02/0002` or `02/0003` ("relays will not be
   activated").

ThermoDeck's startup "all off" covers the case where ThermoDeck replaces a running stock UI
that had the heat or cooling on: a configured KL14 accepts the command at once, without
waiting for the watchdog.

## Link

| | |
|---|---|
| Port | `/dev/ttySP1` (i.MX28 AUART1) |
| Settings | 19200 baud, 8N1, raw, no flow control (115200 only on newer models) |
| Direction | The KL14 sends sensor data on its own, even when no host is running |

## Frames

```
F2 86 <n> <type> <payload: n-3 bytes> <crc_hi> <crc_lo>        n = payload + 3, frame <= 128 bytes
```

- **CRC-16/X-25** (polynomial 0x1021 reflected, init 0xFFFF, final xor 0xFFFF) over the type
  byte and the payload, sent big-endian. `backplate_crc()` implements it.
- Multi-byte values are big-endian.
- **ACKs.** A host message whose type has bit 7 set (`type | 0x80`) asks for an ACK. The KL14
  replies `0x50 [type]`. The stock software resends every 750 ms, up to 5 times. The type is
  included in the CRC with that bit set.

Example, the all-off command ThermoDeck sends: `F2 86 05 10 00 00 43 59`.

## Messages from the backplate

| Type | Name | Payload | Rate |
|---|---|---|---|
| `0x40` | Relay status | u16 relays on, u16 wire detected per terminal, u16 (unknown), u8 fault bits | reply to `0x10` / `0x11` |
| `0x41` | Main sensors | u16 temperature raw, u16 humidity raw, 3 × u16 supply voltage / current | ~4 s |
| `0x42` | Proximity | u8 detected, u16 raw reading, u16 threshold | 1 s |
| `0x43` | Occupancy | 10 × u16 motion samples | 1 s |
| `0x44` | RTC | backplate clock | on request |
| `0x45` | Version | firmware version | on request |
| `0x46` | Debug | text | as needed |
| `0x47` | Terminal voltages | 12 × u16 (one per relay terminal, G has two, then three supply values) | ~6 s |
| `0x48` | Hardware params | 3 bytes: radio address, power / PEK / RC / RH detection bits | on request |
| `0x49`–`0x4C`, `0x4E` | Room sensors | pairing and measurements over the 915 MHz radio | as needed |
| `0x4D` | Error | u8 code, u16 parameter (see below) | as needed, up to a few per second |
| `0x4F` | Overcurrent | relay overcurrent report | as needed |
| `0x50` | ACK | u8: the acknowledged type | as needed |
| `0x51` | Relay currents | per-relay current | as needed |

Temperature and humidity look like Sensirion SHT2x raw values. For example, 30380 is about
34.6 °C before ecobee's compensation for the heat from the electronics. Humidity 17302 is
about 27 %RH.

## Messages to the backplate

| Type | Name | Payload |
|---|---|---|
| `0x10` | Set grouped output | u16 relay bitmask (`00 00` = all off) |
| `0x11` | Set output | u8 relay index (terminal ID − 1), u8 on |
| `0x12` | Get version | none |
| `0x13` / `0x14` | Set / get RTC | time / none |
| `0x15`, `0x17`–`0x1A`, `0x21` | Room sensors | pairing and parameters |
| `0x16` | Get hardware params | none |
| `0x1B` | Relay configuration | u8 PEK mode (0 on ecobee3), u8 `RC<<1 \| RH<<2`, u16 mask of wired relays, u8 occupancy enabled |
| `0x1C` | Get error code | none |
| `0x1D` | Force radio frequency | |
| `0x1E` | Get health data | none |
| `0x1F` | Set chip heater | |
| `0x20` | Raw radio transmit | |
| `0x22` | Reset temperature sensor | none |
| `0x23` | Manufacturing relay override | |

## Relays (reference only: not used by ThermoDeck)

Terminal IDs on the ecobee3. A relay's bit in `0x10` / `0x40` is `ID − 1`, and its index in
`0x11` is the same.

| Terminal | Y1 | Y2 | G | O/B | W1 | W2 | ACC+ | ACC− | RH | RC | C |
|---|---|---|---|---|---|---|---|---|---|---|---|
| ID | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |

How the stock software drives them (`DevMgrRootDeviceTstat::Process`):

1. Send Get Error (`0x1C`), Get Version (`0x12`) and Get Hardware Params (`0x16`), repeating
   each every 3 s until it's answered.
2. Send the relay configuration (`0x1B`) and wait for its ACK. Only then are relay commands
   allowed.
3. Switch a relay with `0x11 [ID−1, on]` and wait up to 2 s for a `0x40` status confirming
   it.
4. While running, poll the RTC every 5 s and errors every 60 s. If nothing arrives from the
   KL14 for 75 s, send `0x10 00 00` and reset everything.

The kernel's `/dev/gpio` ioctl 400 only hands relay control over at boot
(`{2, on}` to command, `{3}` to read the state). It isn't the command path.

## Errors (`0x4D`)

The full list is in `DevMgrRootDeviceTstat::reportKL14Error`. The ones seen on this unit:

| Code / param | Meaning |
|---|---|
| `0B 0001` | Wiring: could not determine RC/RH status |
| `0B 0002` | Wiring: terminal voltages too low |
| `02 0002` | Relay command rejected: PEK state invalid, relays will not be activated |
| `02 0003` | Relay command rejected: no configuration received yet |
| `09 xxxx` | KL14 got a bad frame (1 header, 2 length, 3 CRC) |
| `0A xxxx` | KL14 lost communication with the host |

## Occupancy: a PIR sensor, not mmWave

The occupancy sensor behaves like a **passive infrared (PIR)** detector:

- **What arrives.** Ten samples per second, sent in batches of ten (`0x43`). Each sample is a
  small motion magnitude, 0–15 in an empty room. The stock software takes the largest sample
  in each second and counts the ones over a threshold (default 11, adjusted over time). That
  is how a pyroelectric sensor read through the KL14's ADC behaves: a warm body moving across
  its field makes a swing, and a still person makes none.
- **No radar on this model.** libcore does contain mmWave radar code (`ER_Radar*`, "radar
  stream not supported on this device") for newer ecobee models. Nothing in the ecobee3's
  backplate protocol or firmware carries radar data.

What that means in practice: it sees **motion, not presence**. Someone sitting still stops
registering after a few seconds. It needs line of sight, and its range is room-sized.

The **proximity** sensor (`0x42`) is a separate short-range active infrared reflectance
sensor. Its error messages ("couldn't set the hardware key") match Silicon Labs' Si114x
family. It reports a raw reflectance value and a threshold, and the stock UI uses it to wake
the screen when a hand approaches. `detected` is 1 while the raw value is above the
threshold.

**How ThermoDeck uses proximity (screen timeout).** On this unit the backplate's `detected`
flag stays at 1 even with nothing in front: the raw reading rests around 18000, steady within
±15, above a threshold of about 10800. Both values differ between boots. So ThermoDeck makes
its own decision. It counts "someone near" when the raw value rises more than `prox_delta`
(default 400) above a slowly averaged resting level, or when the flag changes from 0 to 1.
If the reading stays raised for 60 s, that becomes the new resting level, so something left
in front of the screen doesn't keep it awake. With no touch and nobody near for
`[display] dim_after` seconds (default 30), the backlight drops to `dim_brightness` (default
10%). A touch or someone coming near restores it, and the touch that wakes the screen doesn't
press anything. Set `[backplate] debug = 1` to log every reading while tuning `prox_delta`.

These are inferences from the firmware and the data, not from opening the case. To check:
walk past the thermostat and the `0x43` samples jump well above 15, then fall back when you
stand still.

## Ambient light

The ecobee3 has no ambient light sensor. The protocol has no light message, the only device
on `/dev/i2c-2` is the Apple HomeKit authentication chip, and the light-sensor code in
libcore belongs to other ecobee products.

## Looking at the traffic

On the device, with nothing else using the port:

```sh
stty -F /dev/ttySP1 19200 raw -echo -ixon -crtscts cs8
cat /dev/ttySP1 > /tmp/bp.bin & sleep 10; kill $!
hexdump -C /tmp/bp.bin | head
```

Each frame starts with `F2 86`. Don't leave anything writing to the port.
