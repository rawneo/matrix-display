# Matrix Display — ESP8266 port

The same clock / weather / animations firmware, rewritten in C++ for a
**NodeMCU ESP8266** so the display no longer needs a Raspberry Pi. One `main.cpp`
replaces the Flask app: web UI (with the live canvas replica), NTP clock with the
original luma `LCD_FONT` digits, Open-Meteo weather, DHT11 room temp, and all the
animations from the Pi version.

## Hardware

| Part | Notes |
|---|---|
| NodeMCU ESP8266 (V2/V3) | CH340 or CP2102, either works |
| MAX7219 8×8 LED matrix ×7 | cascaded, 56×8 pixels |
| DHT11 sensor | optional |
| 5 V 2 A supply | power banks may auto-shut off on the ESP's idle draw |

### Wiring

| Signal | NodeMCU pin | GPIO |
|---|---|---|
| MAX7219 DIN | D7 | 13 (HW MOSI) |
| MAX7219 CLK | D5 | 14 (HW SCLK) |
| MAX7219 CS  | D8 | 15 |
| DHT11 data  | D2 | 4 |
| DHT11 VCC   | 3V3 | |

Matrix VCC/GND go to the external 5 V supply, **common ground** with the NodeMCU.

Gotchas learned the hard way:

- **GPIO15 (D8) is a boot-strap pin.** With the matrix wired but unpowered it
  drags the strap and the board won't boot *or* flash over USB. Power the matrix
  first, disconnect D8 while USB-flashing — or just use OTA (below).
- If your "7-module panel" is really seven **individual** MAX7219 boards, every
  OUT→IN joint needs all five wires. `FC16_HW` renders correctly either way.
- If animations run mirrored, flip `FLIP_X` in `config.h`. Mounted upside down →
  `ROTATE_180`.

## Build & flash

Uses [PlatformIO](https://platformio.org/):

```bash
cd esp8266
cp include/config.h.example include/config.h   # fill in WiFi, location, TZ
pio run -t upload
```

`config.h` holds your WiFi credentials and is **gitignored — never commit it**.

First flash goes over USB: set `upload_protocol` / `upload_port` in
`platformio.ini` to your serial port (e.g. `/dev/cu.usbserial-10`). After that,
OTA works — the checked-in `platformio.ini` is already set to
`upload_protocol = espota` with the device's static IP, so `pio run -t upload`
updates it over WiFi. No USB, no boot-strap trouble.

## HTTP API

Same routes as the Pi version (`/startclock`, `/weather`, `/dht`, `/ledtext`,
`/stopledtext`, `/charset`, the animation routes, `GET /frame`, `GET /health`),
plus a few ESP-specific ones:

| Route | Action |
|---|---|
| `POST /reboot` | restart the board |
| `POST /testtime` (`t=88:88`) | freeze the clock digits for a pixel test; `/startclock` clears it |
| `GET /dhtraw` | raw DHT sensor status as JSON |

## Clock face

Modules 1–3 show `HH:MM` using the digit glyphs from luma.core's legacy
`LCD_FONT` — pixel-identical to the original Raspberry Pi build — with a small
blinking 1-column colon. Modules 4–7 scroll the date ("Thursday July 23") at
100 ms/column. Brightness drops to minimum from 21:00 to 06:00.
