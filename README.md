# Color Clock

A clock for the Elecrow CrowPanel 7" ESP32-S3 boards (800×480) whose background
color is the time: red = hour, green = minute, blue = second, each scaled to
0–255. The hex code, time, optional date and RGB values are drawn on top, in
whichever gray reads best against the current color.

## Supported boards

| Board | Flash | Build define |
|---|---|---|
| CrowPanel 7.0" HMI, SKU DIS08070H (TN), HW V2.x / V3.0 | 4 MB | `BOARD_CROWPANEL_7` (default) |
| CrowPanel Advance 7.0" (IPS), HW V1.3 / V1.4 / V1.5 | 16 MB | `BOARD_CROWPANEL_ADVANCE_7` |

Advance V1.0 / V1.2 use different backlight and touch-reset hardware and aren't
supported. If you're not sure which board you have,
`esptool --port /dev/ttyUSB0 flash_id` reports the flash size.

What each board gets:

- **Both:** brightness control, dim/off schedules, on-screen settings, web UI.
- **Advance only:** a battery-backed RTC (BM8563, CR1220 cell), so the clock
  keeps time without WiFi, and a buzzer for the hourly chime.

## Building

You need [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the
`esp32:esp32` core 3.x (tested with 3.3.8) and two libraries:

```sh
arduino-cli core install esp32:esp32
arduino-cli lib install LovyanGFX WiFiManager
```

Build as the generic **ESP32S3 Dev Module** with octal PSRAM:

```sh
# Basic CrowPanel 7.0 (DIS08070H)
FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=4M,PartitionScheme=huge_app,CDCOnBoot=default
arduino-cli compile --fqbn $FQBN --build-path build-basic .
arduino-cli upload  --fqbn $FQBN --build-path build-basic -p /dev/ttyUSB0 .

# CrowPanel Advance 7.0
FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default
arduino-cli compile --fqbn $FQBN --build-path build-adv \
  --build-property compiler.cpp.extra_flags=-DBOARD_CROWPANEL_ADVANCE_7 .
arduino-cli upload  --fqbn $FQBN --build-path build-adv -p /dev/ttyUSB0 .
```

A separate build path per board stops a cached build for one board being
uploaded to the other.

Don't use the core's built-in `elecrow_crowpanel_7` board definition (as of
3.3.8): it builds for quad PSRAM, so the board panics at boot, and it sends
serial output to USB CDC instead of the CH340 port.

Serial log: `arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200`. The
monitor doesn't reset the board, so press RESET to see the boot messages.

## First-time setup

1. On first boot the clock opens a WiFi hotspot, **`ColorClock-Setup`**
   (password `sopretty`). Join it and pick your network in the page that opens.
2. Once connected, open **http://colorclock.local**, or the IP address shown in
   the serial log, to set the time zone and everything else.

To forget the saved WiFi network, touch and hold the screen while the boot
splash is showing.

## Settings

Web UI (`http://colorclock.local`):

- NTP server and time zone (presets or a custom POSIX TZ string)
- 12/24-hour format, seconds, date
- Color gamma (the basic board's TN panel shows mid-tones pale; default 1.8 there, 1.0 on the Advance)
- Brightness
- Dim schedule and off schedule (start/end times; dim level)
- How long a tap keeps the screen bright
- Hourly chime (Advance only)

On screen: tap the **top-left and then the top-right corner** (either order)
within 5 seconds. Tabs cover clock format and brightness, the dim and off
schedules, and the chime. Changes apply immediately. Tap **Done** to save; the
screen also saves and closes on its own after 60 seconds without a touch.

While the screen is dimmed or off, a tap brightens it one step (off → dim →
full) for the wake time.

## Behavior without WiFi

If WiFi drops, the clock keeps running, shows a small "No WiFi" tag and
reconnects in the background. On the Advance the RTC also lets it boot and
show the right time with no WiFi at all. The basic board has no RTC, so after a
power cut it needs WiFi to get the time.

## Files

- `colorclock.ino`: the app (settings, web UI, WiFi, schedules, rendering)
- `crowpanel7.h/.cpp`: display, touch, backlight, RTC and buzzer driver for both boards
- `fonts.h`: **generated** anti-aliased fonts. Don't edit it; rerun
  `python3 tools/make_fonts.py` (needs Pillow with FreeType and DejaVu Sans Bold).
- `smooth_font.h`, `app_types.h`: small helper headers
