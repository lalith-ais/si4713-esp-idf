# si4713-esp-idf

A pure C driver for the Silicon Labs **Si4713** FM transmitter, written for
**ESP-IDF** (v5.x `i2c_master` driver). Ported from the
[Adafruit_Si4713](https://github.com/adafruit/Adafruit-Si4713-Library)
Arduino C++ library, and tested on real hardware (ESP32, transmitting to a
Yamaha FM receiver).

## Why

The existing Si4713 libraries are all Arduino/C++. This is a drop-in
ESP-IDF component: no Arduino shim, no C++, no hidden global state — just
a struct you own and a set of functions that take a pointer to it.

## Features

- Tune, set TX power, read tune status / ASQ / noise level
- RDS: PS (station name) and RadioText
- GPIO control pins (GP1/GP2)
- Every call returns `esp_err_t` — nothing silently fails
- Bounded timeouts on all polling loops (CTS wait, tune/measure completion)
  instead of spinning forever if the chip is missing or wedged
- No global/static state, so more than one instance is safe

## Hardware notes

- **Run the Si4713 breakout at 3V3, not 5V.** If you power it at 5V, SDA/SCL
  get pulled up to 5V and you'll have a bad time on a 3V3 ESP32 GPIO.
- **Wire the reset pin.** It's not optional in practice — without a real
  reset pulse the chip can come up in an inconsistent state. Pass the GPIO
  number in `si4713_config_t.rst_pin`; if you've tied RST high in hardware
  instead, pass `GPIO_NUM_NC` and skip the reset step.
- Needs 3 wires total beyond power: SDA, SCL, RST.

## The frequency units gotcha

`si4713_tune_fm()` and `si4713_read_tune_measure()` take frequency in
**10 kHz steps** (i.e. MHz × 100), matching the raw `TX_TUNE_FREQ` argument
in the datasheet — **not** kHz, despite what the parameter name in the
original Adafruit driver suggested. So:

```c
si4713_tune_fm(&radio, 8850);   // 88.50 MHz
si4713_tune_fm(&radio, 10230);  // 102.30 MHz
```

## RDS station name is fixed at 8 characters

This is an RDS/RBDS standard limit, not a chip limitation — the Program
Service (PS) field is always exactly 8 characters, transmitted as four
2-character groups. There's no way around it at the protocol level.

Stations with longer names (e.g. BBC World Service) get around this with
"scrolling PS" — rapidly cycling through overlapping 8-character windows
("BBC Worl" → "BC World" → "C World " → ...) so that receivers which
refresh their display on every PS update show it as scrolling text. This is
off-spec but widely tolerated; a receiver that doesn't refresh that fast
just shows a static (if slightly jumbled) fragment instead.

For anything longer than 8 characters, prefer RadioText instead:

```c
si4713_set_rds_station(&radio, "MYRADIO");        // PS, 8 chars, static
si4713_set_rds_buffer(&radio, "Now playing: ...");// RT, up to 64 chars
```

If you want scrolling PS behaviour, call `si4713_set_rds_station()`
repeatedly on a timer with a sliding window over your longer string —
the driver doesn't do this for you automatically, since not all receivers
handle it gracefully.

## Usage

Drop this repo (or just `si4713.c` / `si4713.h` / `CMakeLists.txt`) into
`components/si4713/` in your ESP-IDF project.

```c
#include "si4713.h"
#include "driver/i2c_master.h"

i2c_master_bus_config_t bus_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = I2C_SDA_GPIO,
    .scl_io_num = I2C_SCL_GPIO,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t bus_handle;
i2c_new_master_bus(&bus_cfg, &bus_handle);

si4713_config_t cfg = {
    .i2c_addr = SI4713_ADDR1,
    .scl_speed_hz = 100000,
    .rst_pin = SI4713_RST_GPIO,
};

static si4713_dev_t radio;
si4713_init(bus_handle, &cfg, &radio);
si4713_probe(&radio);                       // reset + power up + rev check

si4713_tune_fm(&radio, 8850);               // 88.50 MHz
si4713_set_tx_power(&radio, 115, 0);

si4713_begin_rds(&radio, 0xADAF);
si4713_set_rds_station(&radio, "MYRADIO");
si4713_set_rds_buffer(&radio, "Hello from ESP-IDF");
```


## API overview

| Function | Purpose |
|---|---|
| `si4713_init()` | attach device to an existing I2C bus |
| `si4713_reset()` | pulse the reset pin |
| `si4713_probe()` | reset + power up + verify part number |
| `si4713_power_up()` / `si4713_power_down()` | chip power state |
| `si4713_get_rev()` | read part number / firmware info |
| `si4713_tune_fm()` | tune to a frequency (10 kHz units) |
| `si4713_set_tx_power()` | set output power / antenna cap |
| `si4713_get_status()` | read interrupt status bits |
| `si4713_read_tune_status()` | read back tuned freq, dBµV, antenna cap, noise |
| `si4713_read_tune_measure()` | measure noise at a given frequency |
| `si4713_read_asq()` | read audio signal quality metrics |
| `si4713_set_property()` | set any `SI4713_PROP_*` chip property |
| `si4713_begin_rds()` | configure default RDS properties |
| `si4713_set_rds_station()` | set PS (station name, 8 chars) |
| `si4713_set_rds_buffer()` | set RadioText (up to 64 chars) |
| `si4713_set_gpio_ctrl()` / `si4713_set_gpio()` | GP1/GP2 direction / level |

## Thread safety

Not thread-safe by design. If more than one task needs to touch a given
`si4713_dev_t`, serialise access yourself (e.g. own it from a single task
or actor and message-pass into it).

## Credit / license

Ported from [Adafruit_Si4713](https://github.com/adafruit/Adafruit-Si4713-Library)
by Limor Fried/Ladyada (Adafruit Industries), BSD licensed. This port
carries the same BSD license — see `LICENSE`.
