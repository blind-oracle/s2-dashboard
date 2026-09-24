# s2-dashboard

CAN statistics logger for the **LiveWire S2 Del Mar** electric motorcycle, running on a
**Waveshare ESP32-S3-Zero** with an **MCP2551** CAN transceiver. It taps the secondary
CAN bus at the Data Link Connector (DLC), decodes every frame described by the
community [livewire-s2-can-db](https://github.com/inklit/livewire-s2-can-db) database
and logs the result over the board's USB serial port. This is the data-gathering
stage of a future dashboard.

It also drives an **ST7789V2 240x280 IPS display** as a rider-facing gauge: power
out and regen, torque, pack voltage, energy used this key cycle and battery state
of health. Screens are cycled from one of the motorcycle's own handlebar
buttons, read off the bus, so no extra switch has to be wired.

The same data goes out over **Bluetooth LE**, so a phone can read it without a
laptop: a Nordic UART Service that published Android apps display as text with no
development, and a documented custom GATT service for a purpose-built client.

Firmware updates are **over Wi-Fi**: hold the screen button and the dashboard
serves an upload page from its own access point, so the board never has to come
out from behind the bodywork.

Firmware: ESP-IDF 5.5 (C), built with PlatformIO. The default configuration is a
**passive tap** (TWAI listen-only mode, never acknowledges or transmits). Active
UDS polling of the diagnostic modules through the BCM gateway is implemented but
off by default.

## Hardware

### Pins (ESP32-S3-Zero)

| Function | GPIO | Notes |
|---|---|---|
| CAN TX (to MCP2551 TXD, pin 1) | **GPIO4** | left header, no strapping/on-board function |
| CAN RX (from MCP2551 RXD, pin 4) | **GPIO5** | **through a resistor divider**, see below |
| Display SCLK | **GPIO12** | SPI2 (FSPI) native pin, fast IO_MUX path |
| Display MOSI / DIN | **GPIO11** | SPI2 native pin |
| Display CS | **GPIO10** | SPI2 native pin |
| Display DC / RS | **GPIO9** | |
| Display RST | **GPIO8** | set to -1 if tied to the board reset |
| Display BL | **GPIO7** | PWM dimmed; -1 if hardwired on |
| Screen button | **GPIO6** | optional, bench only; the handlebar button is the default |
| Status LED | GPIO21 | on-board WS2812, already wired |
| Console | USB-C | native USB Serial/JTAG, no UART bridge on this board |

Every pin is configurable in `menuconfig` (S2 Dashboard menu). The six display
signals are a contiguous run on the right-hand header (GP12 down to GP7), which
suits a ribbon; note the header has only one GND and one 3V3 pad, both at the top
of the *left* row, so power needs its own leads.

Avoid for any of these: GPIO0 (BOOT), 3/45/46 (strapping), 19/20 (USB),
21 (on-board LED), 26-37 (flash/PSRAM), 43/44 (UART0), and 22-25 (do not exist).

### MCP2551 on a 3.3 V MCU

The MCP2551 is a 5 V transceiver. Its TXD input accepts 3.3 V logic directly
(V_IH min 2.0 V), but its RXD output swings to 5 V and **must not** be connected to
the ESP32-S3 directly (3.6 V absolute maximum).

```
DLC pin 4 (R/Y, +12 V acc) --[1 A fuse]--> 5 V buck IN+   OUT+ 5 V --+--> ESP32-S3-Zero "5V" pin
DLC pin 3 (BK/GN, GND)     ------------->  buck IN-       OUT- GND --+--> ESP32 GND, MCP2551 VSS (pin 2)
                                                                     +--> MCP2551 VDD (pin 3) + 100 nF
DLC pin 2 (W/R,  CAN-H)  --twisted--> MCP2551 CANH (pin 7)
DLC pin 5 (W/BK, CAN-L)  --twisted--> MCP2551 CANL (pin 6)      NO 120 ohm terminator on the tap!

ESP32 GPIO4 (TX) ----------------------> MCP2551 TXD (pin 1)
MCP2551 RXD (pin 4) --[2.2 k]--+-------> ESP32 GPIO5 (RX)      5 V -> 3.0 V
                               +--[3.3 k]--> GND
MCP2551 RS (pin 8) --> GND   (high-speed mode; never leave floating)
MCP2551 VREF (pin 5) --> n/c
```

Notes:

- The DLC is a stub off an already terminated bus. If your MCP2551 breakout has a
  120 Ω resistor (often a jumper), remove it.
- The ESP32-S3-Zero "5V" pin is the raw USB VBUS net with no diode. Power the board
  from **either** USB **or** the buck converter, not both, unless you add a Schottky
  diode from the buck to the 5V pin.
- The MCP2551 needs 4.5-5.5 V: feed it from the 5 V rail, not from 3V3.
- One common ground: DLC pin 3, buck, ESP32 GND, MCP2551 VSS and the divider.
- DLC pinout (JST MWT 6-way, from the database header): pin 2 = CAN-H (W/R),
  pin 5 = CAN-L (W/BK), pin 3 = ground (BK/GN), pin 4 = accessory 12 V (R/Y).
  Bus: 500 kbit/s classical CAN, 11-bit identifiers.

A 3.3 V-IO transceiver (MCP2562 with VIO=3V3, TJA1051T/3, SN65HVD230) removes the
need for the divider.

### ST7789 display

A 240x280 IPS module with an ST7789V2 controller (the common 1.69" one), on SPI2:

```
Module        ESP32-S3-Zero
VCC   ------> 3V3
GND   ------> GND
DIN   ------> GPIO11   (MOSI)
CLK   ------> GPIO12   (SCLK)
CS    ------> GPIO10
DC    ------> GPIO9
RST   ------> GPIO8
BL    ------> GPIO7    (PWM dimmed; tie to 3V3 and set the option to -1 instead)

Button: GPIO6 ------ switch ------ GND     (OPTIONAL - only if you select a GPIO
                                           switch instead of a handlebar button.
                                           Internal pull-up, no external parts
                                           needed, though 10 k + 100 nF is wise
                                           in a vehicle)
```

The module is 3.3 V logic and 3.3 V supply, so unlike the CAN transceiver it needs
no level shifting. Keep the SPI leads short; drop `S2_DISPLAY_SPI_HZ` to 20 MHz if
the image tears.

**Why not TFT_eSPI.** TFT_eSPI is an Arduino library and this is a plain ESP-IDF
project, so using it would mean pulling the whole Arduino core in as an ESP-IDF
component. ESP-IDF ships an ST7789 panel driver in-tree, so `src/display/panel.c`
uses that directly over DMA with no extra dependency. The V2 variant needs no
vendor register additions: the module maker's own ESP-IDF example drives this
glass with the stock driver and nothing else.

**The row offset matters.** The ST7789 has 240x320 of frame memory and this panel
shows rows 20 to 299 of it, so the firmware sets a gap of 0, 20. Those 280 rows
sit in the *middle* of the memory, which is why the offset stays 20 when the
image is turned upside down, unlike the end-justified 240x240 panels where it
moves between 80 and 0.

**If the image looks wrong**, one option fixes each symptom:

| Symptom | Option |
|---|---|
| Photographic negative | `S2_DISPLAY_INVERT_COLOR` (default y) |
| Red and blue swapped | `S2_DISPLAY_BGR` (default y) |
| Upside down | `S2_DISPLAY_ROTATE_180` |
| Shifted by a constant number of pixels, or a band wrapped from the far edge | `S2_DISPLAY_X_GAP` / `_Y_GAP` (0 and 20 here) |
| Mirrored left-right or top-bottom | `S2_DISPLAY_MIRROR_X` / `_MIRROR_Y` (some modules ship mirrored; rotation alone cannot fix that) |
| Torn, shifted or noisy pixels | lower `S2_DISPLAY_SPI_HZ` |
| Nothing at all | check DC and RST, then CS; the backlight stays dark until the first frame is drawn |

The module's backlight is active high and its logic is 3.3 V, matching the board.
The datasheet write cycle allows up to 62.5 MHz; the vendor's own examples use
40 MHz, which is the default here.

## Building and flashing

```
pio run                 # build
pio run -t upload       # flash over USB-C (hold BOOT while plugging in if no port shows up)
                        # only needed once; after that hold the screen button to update over Wi-Fi
pio device monitor      # USB CDC console; baud rate is irrelevant
pio run -t menuconfig   # change pins / logging / UDS options ("S2 Dashboard" menu)
pio test -e native      # host-side unit tests of the decoding core
```

The console is on the USB Serial/JTAG controller. Output produced while no host is
attached is dropped after 50 ms, so open the monitor first if you want the boot
banner. `platformio.ini` keeps DTR/RTS low so opening the monitor does not reset the
board.

### Toolchain notes

- The board definition and ESP-IDF 5.5 come from the **pioarduino** fork of
  platform-espressif32; `platformio.ini` pins the exact release URL so builds are
  reproducible.
- PlatformIO Core 6.2.0 and pioarduino (all releases up to 55.03.311, July 2026) disagree about the SCons
  version and the platform deletes Core's SCons package during configuration
  (pioarduino issue #529). `tools/pio_scons_guard.py`, wired in as a `pre:` extra
  script, reinstalls it before the build needs it. Remove it once pioarduino ships a
  fix. Alternatives: PlatformIO Core 6.1.19, or pioarduino's own core.
- `sdkconfig.defaults` is the source of truth for the SDK configuration. The
  generated `sdkconfig.waveshare_esp32_s3_zero` is git-ignored; delete it to apply
  changed defaults (defaults never override an existing generated file).
- After adding or removing a `.c` file, `touch src/CMakeLists.txt` so PlatformIO
  re-runs CMake (source lists are snapshots).

## Configuration (menuconfig, "S2 Dashboard")

| Option | Default | Meaning |
|---|---|---|
| `S2_CAN_TX_GPIO` / `S2_CAN_RX_GPIO` | 4 / 5 | transceiver pins |
| `S2_CAN_BITRATE` | 500000 | bus bitrate |
| `S2_CAN_LISTEN_ONLY` | y | passive tap; must be off for UDS |
| `S2_LOG_RAW_FRAMES` | n | candump-style line for every frame |
| `S2_LOG_SIGNAL_CHANGES` | y | log decoded signals when they change |
| `S2_LOG_CHANGE_MIN_INTERVAL_MS` | 250 | per-signal rate limit for change logs |
| `S2_LOG_UNKNOWN_IDS` | y | log frames whose id is not in the database |
| `S2_SUMMARY_PERIOD_MS` | 2000 | vehicle summary block period (0 = off) |
| `S2_FRAME_TABLE_PERIOD_S` | 30 | per-id statistics table period (0 = off) |
| `S2_E2E_SKIP_BAD_CRC` | y | do not decode protected frames failing the CRC |
| `S2_CAN_RX_QUEUE_LEN` / `S2_CAN_TX_QUEUE_DEPTH` | 256 / 8 | frame queues |
| `S2_LOG_QUEUE_LEN` | 96 | log line ring buffer (x160 bytes) |
| `S2_UDS_ENABLE` | n | poll diagnostic DIDs (transmits on the bus) |
| `S2_UDS_REQUEST_TIMEOUT_MS` / `S2_UDS_INTER_REQUEST_GAP_MS` / `S2_UDS_ROUND_PERIOD_MS` | 500 / 50 / 5000 | poller timing |
| `S2_UDS_POLL_TCU` | n | include the telematics module (GPS position) |
| `S2_UDS_LOG_RAW` | y | hex-dump UDS responses |
| `S2_STATUS_LED_ENABLE` / `_GPIO` | y / 21 | WS2812 status LED |
| `S2_DISPLAY_ENABLE` | y | drive the ST7789 panel |
| `S2_DISPLAY_SPI_HOST` | 2 | SPI2 (FSPI); host 3 has no fast pins on the S3 |
| `S2_DISPLAY_SCLK_GPIO` / `_MOSI_GPIO` / `_CS_GPIO` / `_DC_GPIO` / `_RST_GPIO` / `_BL_GPIO` | 12 / 11 / 10 / 9 / 8 / 7 | panel pins; CS, RST and BL accept -1 |
| `S2_DISPLAY_SPI_HZ` | 40000000 | pixel clock, 62.5 MHz ceiling |
| `S2_DISPLAY_ROTATE_180` | n | turn the image upside down |
| `S2_DISPLAY_X_GAP` / `_Y_GAP` | 0 / 20 | offset into the 240x320 frame memory |
| `S2_DISPLAY_INVERT_COLOR` / `_BGR` | y / y | colour fixes, see the table above |
| `S2_DISPLAY_MIRROR_X` / `_MIRROR_Y` | n / n | extra mirroring on top of the rotation |
| `S2_DISPLAY_REFRESH_HZ` | 10 | full-frame redraw rate |
| `S2_DISPLAY_BAND_ROWS` | 40 | rows per SPI transfer |
| `S2_DISPLAY_SCREENS` | 6 | screens the button cycles through; six are drawn |
| What cycles the screens | handlebar button | or a GPIO switch, or nothing |
| Which handlebar button | info / scroll | horn, high beam, either brake, hazards, cruise arm |
| `S2_DISPLAY_BUTTON_GPIO` / `_ACTIVE_LOW` | 6 / y | only when the GPIO switch is selected |
| `S2_DISPLAY_BL_PWM` / `_BRIGHTNESS` | y / 90 | LEDC-dimmed backlight |
| `S2_DISPLAY_POWER_FULL_SCALE_KW` / `_REGEN_FULL_SCALE_KW` | 70 / 20 | gauge ends |
| `S2_TORQUE_COUNTS_PER_NM_X100` | 335 | torque scale, hundredths (3.35 counts/Nm) |
| Tyre pressure unit | PSI | or bar, or kPa as broadcast |
| `S2_ACCEL_COUNTS_PER_G` | 7760 | IMU scale, see the caveat above |
| `S2_DISPLAY_POWER_TAU_MS` | 200 | smoothing on the power figure; 0 disables it |
| `S2_BLE_ENABLE` | y | advertise the telemetry services over Bluetooth LE |
| `S2_BLE_DEVICE_NAME` | `S2-DASH` | name shown in the phone's scan list |
| `S2_BLE_NUS_ENABLE` | y | also expose the Nordic UART Service (plain text) |
| `S2_BLE_DASH_HZ` | 5 | dashboard notify rate, 1-20 Hz |
| `S2_BLE_SNAPSHOT_MS` | 1000 | full-snapshot period |
| `S2_BLE_TEXT_PERIOD_MS` | 1000 | text digest period on the UART service |
| `S2_OTA_ENABLE` | y | allow the firmware to update itself over Wi-Fi |
| `S2_OTA_AP_SSID` | `S2-DASH-OTA` | update-mode access point name |
| `S2_OTA_LONG_PRESS_MS` | 2000 | hold time that enters update mode |
| `S2_OTA_IDLE_TIMEOUT_S` | 300 | leave update mode after this long idle |
| `S2_OTA_SELF_TEST_S` | 30 | confirm a new image after this long |
| `S2_OTA_AP_TX_POWER_QDBM` | 52 | access point transmit power, quarter dBm, so 13 dBm |
| `S2_OTA_FRESH_RF_CAL` | y | recalibrate the radio on entry to update mode |

## What the display shows

Five screens, cycled by the bike's own info/scroll button. Below the rows, one
dot per screen marks where you are, and the new screen is noted in the serial
log.

**Screen 1, ride** is the one to leave it on while moving:

| Element | Source | Notes |
|---|---|---|
| Ring gauge + large number, kW | pack voltage x pack current (0x181) | positive = power out to the motor (amber, red above 45 kW), negative = regen (green, fills the other way from the white zero mark) |
| `nnn ~Nm` | 0x161 `torque_delivered` | the `~` marks it as an estimate, see the caveat below |
| `nnn.n Vdc` | 0x181 `pack_voltage` | 12-bit word, 1 V/count |
| `nn.nn kWh` | 0x186 `trip_energy_consumed_wh` | the bike's own trip meter: energy used since **key-on**, which it also zeroes at each key cycle and clamps at 0 while charging |
| `nnnn rpm` | 0x160 `motor_rpm` | |

**Screen 2, battery**: cell voltage min/avg/max (0x182), pack current (0x181,
charge-positive as the bike reports it, so a discharge reads negative), state of
charge (0x185) and state of health. State of health lives here rather than on
the ride screen because it is UDS-only and moves over months.

**Screen 3, thermal**: pack temperature min/avg/max across the three 0x183
sensors, then coolant and inverter from the 0x163 temperature mux, and ambient.

**Screen 4, chassis**: longitudinal acceleration as the extremes seen since boot
either side of the current value, in estimated g and in raw counts, then tyre
pressures in the unit chosen in menuconfig.

**Screen 5, telematics**: GPS latitude and longitude, cellular signal and the
serving network. Every value here is UDS-only, so the screen says so when UDS
polling is off, which is the default.

**Screen 6, peaks**: the largest values seen since the board last restarted.
Power at both ends, regenerating and driving; pack current at both ends,
charging and discharging; and the highest torque. Read the next section for why
these are not simply the largest numbers the ride screen showed you.

**Missing and stale data are visually distinct**, because a dashboard that
invents numbers is worse than one that admits it cannot read them:

- **Never seen** shows dashes in the dimmest grey (`---`, `----`, `---.-`,
  `--.--`) with the unit dimmed too, and the ring draws no fill at all. Dashes,
  never zeros: `0.0 kW` is a legitimate reading.
- **Gone stale** keeps the last value but greys it, the ring redraws dim at the
  last angle, and the header word changes from `POWER` to `STALE` - the only
  text on screen that changes wording. The window matches how often each frame
  actually arrives: 2 s for the fast ride values, 5 s for cells and
  temperatures, 10 s for the latched mux channels, 5 minutes for tyre pressures,
  and 15 minutes for the GPS fix.
- **State of health** reads `n/a` when UDS polling is off and `--` when it is on
  but the battery has not answered yet, so the two reasons are distinguishable.

### What is not on these screens, and why

Three things asked for turned out not to be available as asked:

- **Charger temperature is not broadcast.** The onboard charger reports a
  thermal value over UDS (OBC DID 0201) but the database records its scale and
  offset as still unknown, so any number shown would be invented. What the
  thermal screen does show instead is the real 0x163 mux: selector 1 is the
  motor/coolant loop and selector 0 is the inverter winding. Selectors 2 to 5
  are a constant limit table in every capture, so they are ignored.
- **Cell temperatures are not reported per cell.** The bus carries three pack
  sensors on 0x183, so the min/avg/max is across those three, not across cells.
  The pack's own min/mean/max does exist over UDS on RESS DID 0x020D if you ever
  want the real thing.
- **GPS is not on the broadcast bus at all.** Frame 0x394 looks like a position
  slot but is all-zero on every capture. The only source is the TCU over UDS
  (DID 0200, two big-endian float32), which needs `S2_UDS_ENABLE` **and**
  `S2_UDS_POLL_TCU`, and UDS transmits on the bike's bus. TCU polling is off by
  default because a position in a shared log is privacy-sensitive.

### Why the longitudinal acceleration is marked with a tilde

0x122 is an accelerometer: the vertical axis rests near -7760 counts, which is
1 g, and that is the scale `S2_ACCEL_COUNTS_PER_G` uses. But the database records
that the longitudinal channel did **not** correlate with measured vehicle
acceleration across 2469 paired samples, so even the axis assignment is
unproven. The raw count is shown next to the g figure for that reason. Treat the
number as an indication, not a measurement.

### Why a glanced power figure reads low, and where to find the real one

The ride screen's power number is honest but lossy, in two compounding ways.

**It is sampled far too slowly.** Vehicle state keeps only the latest value of
each signal, and the display reads it ten times a second. The battery frame
arrives much faster than that, so most voltage and current pairs are overwritten
before the screen ever looks, and the pair it does read is an arbitrary instant
rather than the peak of that window.

**It is smoothed.** A low-pass filter keeps the digits from churning, at a cost
that is easy to underestimate:

| Time into a burst | Fraction shown | Reads low by |
| --- | --- | --- |
| 0.1 s | 33% | 3.0x |
| 0.2 s | 56% | 1.8x |
| 0.5 s | 87% | 1.15x |
| 1.0 s | 98% | 1.02x |

Anything shorter than about 0.17 s is at least halved by the filter alone. On
top of that, power climbs through a burst as revs build, so the peak lands
exactly when the filter is still catching up. A one to three second pull,
glanced at once, can easily look half what it was. Regeneration events are
shorter still, which is why both directions look wrong.

**The peaks screen is not affected.** Those figures are captured one CAN frame
at a time inside the decoder, gated on the frame's end-to-end check and the same
plausibility window the gauge uses, so they are the real extremes. The ring also
carries a peak-hold marker showing the highest power of the last few seconds,
so a glance catches the peak even while the digits lag.

`S2_DISPLAY_POWER_TAU_MS` sets the smoothing, defaulting to 200 ms. Setting it
to 0 turns smoothing off entirely, at the cost of a number that jumps around.

**The arithmetic itself checks out.** Power is `-(pack volts x pack amps)`, and
the current decode matches the database's documented formula exactly, with its
zero point pinned by contactor-open frames reading exactly zero amps. Five
independent anchors each break by about a factor of two if the current scale
were doubled: the 63 kW rating against 57.6 kW measured at wide-open throttle,
a recorded 69 kW peak, the bike's own watt-hour meter agreeing to within 12%
over a ride, Level 1 charging matching wall power, and the measured 16 mm² HV
cable. The one genuinely open question is that the absolute scale rests on a
single wall-power match; the test that would settle it is a Level 2 charge,
which should read **+12 to +13 A** on the battery screen.

### Why torque is an estimate

The database gives torque in raw counts and brackets the scale at 3.0-3.7
counts/Nm, but that bracket was anchored on the peak torque *request*; the later
peak *delivered* figure implies about 4.4 counts/Nm. Real uncertainty is roughly
plus or minus 30-40%, so the firmware prints the Nm value with a leading `~` and
`S2_TORQUE_COUNTS_PER_NM_X100` is adjustable. Only a dyno or an OEM figure will
settle it.

### Why state of health needs UDS

Searching the whole database for a broadcast SoH signal comes up empty: the
battery management system is on the *primary* bus and the secondary bus carries
no health figure. The only source is UDS RESS DID 0x020E, which the database
itself labels "SoH-*like*" (it held 97% while the displayed charge ran from 100%
down to 22%, which proves it is not a charge reading but does not prove it is
health). Reading it **transmits on the motorcycle's bus**, so it appears only
with `S2_UDS_ENABLE` on. In the default passive build the field reads `SOH n/a`.

### Cost

The framebuffer is one 134,400-byte DMA-capable allocation (240 x 280 x 16 bpp).
It is taken before the CAN queues and task stacks so it gets the least
fragmented heap, and the boot banner prints the free and largest-block figures
next to it so the real margin is visible on your board. A full frame is 27 ms of
SPI DMA at 40 MHz, so the default 10 Hz refresh keeps the bus about 27% busy and
costs the CPU only the few milliseconds of rendering. The display task runs on
core 0 at a low priority; CAN decoding stays on core 1.

The panel is brought up before the CAN bus, so the screen shows its placeholder
dashes from the first moment rather than staying dark until traffic arrives.

## Cycling screens from the handlebar

The secondary bus carries the handlebar controls, so the screens can be paged
from the bars with no switch wired to the board. This is the default; a GPIO
switch is still selectable for bench work, where there is no bus to listen to.

**There are no media buttons to use.** The S2 has no infotainment, and the
database notes that it does not reuse the combustion-Harley CAN map, where
0x383 was the radio. There is nothing for track forward or back, or volume.
What does exist are these two-state controls:

| Option | Signal | Confidence | Worth knowing |
| --- | --- | --- | --- |
| **info / scroll** (default) | 0x354 D1 | STRONG | the button the rider already uses to page the instrument cluster, and it does nothing else |
| horn | 0x354 D2 | CONFIRMED | sounds the horn every time |
| high beam | 0x152 D3 | STRONG | a flash-to-pass tap cycles the screen too |
| front brake lever | 0x15A D6 | CONFIRMED | fires on every brake application |
| rear brake lever | 0x133 D4 | CONFIRMED | fires on every brake application |
| hazard switch | 0x15A D2 | CONFIRMED byte | whether it follows the switch or the blink is not pinned down; if it blinks, screens will advance repeatedly |
| cruise arm switch | 0x160 D6 bit 7 | CONFIRMED | latching, so one press to arm and one to disarm |

The info/scroll button is the default for the obvious reason: every other option
does something else to the motorcycle when you press it.

### How a press is detected

The render loop polls the selected signal on every pass, which is every 10 ms
plus however long a frame took, under the same short lock the rest of the display
uses. A press is a transition into the held state.

Two details stop it misfiring:

- **The detector arms on its first sample.** A control already held when the
  firmware starts, or when the frame reappears after the ignition cycles, does
  not count as a press.
- **It reads the change counter as well as the level.** `vs_signal_t::changes`
  counts every transition, so a press that began and ended between two polls is
  still counted rather than silently lost. The number of presses follows exactly
  from the change delta and the two endpoint levels.

One poll advances at most four screens. A real press yields one, and so does a
press that fell entirely between polls, so that bound only ever trims a signal
flapping faster than a human can press: the hazards option, if it turns out to
follow the blink.

What this cannot work around is the frame rate of 0x354 itself, which the
database does not record. If the left switchgear frame turns out to be slow
enough that a quick tap falls entirely between two frames, the decoder never sees
the press and nothing downstream can recover it. The periodic frame table in the
serial log prints the measured rate of every id, so that is the place to check.

Not every option is a momentary button. The hazard and cruise switches latch, so
they give one press when switched on and another when switched off. The brake
levers are momentary but fire whenever you brake.

## What the log looks like

Boot banner, then three kinds of lines (all prefixed with boot-relative seconds):

```
[    12.345] 0x160 SPEED_160: vehicle_speed=23.4 km/h motor_rpm=677 rpm
[    12.400] 0x133 IGNITION_REAR_BRAKE_133: ignition_d1=224(MOTOR ENABLED)
[    12.410] 0x163 mux sel0: inverter/winding-class temp=41 degC
[    12.500] UNKNOWN 0x180 first seen [8] 00 01 00 00 00 00 2A 5B
[    12.900] 0x161 MOTOR_POWER_161: undocumented bytes D5=00 (first)
```

- **Change lines**: one line per frame listing the signals whose value differs
  from what was last printed, with the physical unit or, for enumerations, the
  meaning from the database comments in parentheses. Each signal is printed at
  most once per `S2_LOG_CHANGE_MIN_INTERVAL_MS`; a change suppressed by that
  limit is printed as soon as the interval has elapsed, so final states are never
  lost. A trailing `?` marks a signal the database tags as tentative, suspect or
  retracted. Multiplexer selector/payload signals are not printed raw; the
  derived lines carry their meaning.
- **Derived lines**: multiplexed temperatures (0x163), the 0x164 energy registers,
  cruise-control state, HV contactor state, ABS init/ready, VIN, charge cable.
- **Undocumented bytes**: bytes of known frames that no signal covers, logged on
  change (rate limited) so new information is not lost. Constant heartbeat and
  reserved frames are checked against their documented payloads instead.
- **Unknown ids**: first sighting and payload changes of ids not in the database;
  `0x7D0-0x7FF` is labelled `DIAG` (UDS traffic).

Every 2 s a summary block prints bus statistics (frames/s, error state, CRC
failures, alive-counter gaps, queue/log drops) and the latest values grouped as
motion / drive / battery / energy / thermal / chassis / state / lamps / misc / IMU
/ derived. A `*` marks a value older than 5 s. Every 30 s a frame table lists each
id with its rate, count, E2E status and last payload, followed by unknown ids.

The status LED shows blue while idle, green blinking while frames flow, yellow
when CRC failures occur, red on error-passive / bus-off.

### Raw capture

Enable `S2_LOG_RAW_FRAMES` to get `(sec.usec) can0 160#0003E80BB8000000` lines
compatible with `can-utils` tooling (`log2asc`, SavvyCAN import after a trivial
reformat). At a live bus rate of several hundred frames per second this is
several hundred lines per second; the log writer drops lines rather than stalling
CAN reception, and counts the drops in the summary.

## What is decoded

All **60 broadcast messages / 113 signals** of the DBC are decoded generically from
`src/gen/s2_dbc_gen.c`, generated by `tools/dbc2c.py` from the vendored database in
`can-db/`. On top of that, `src/s2_overlay.c` carries the knowledge that lives only
in the database's comment blocks:

- **E2E protection**: the database lists 36 protected ids (6-bit alive counter in
  D7, CRC-8/SAE-J1850 in D8 over D1..D7); 32 of them have message definitions and
  are CRC-checked, the other four (0x180, 0x1C7, 0x1C9, 0x35A) are logged as
  unknown ids. Frames failing the CRC are counted and (by default) not decoded;
  counter gaps estimate lost frames (gap accounting restarts after a corrupted
  frame).
- **Enumerations**: ride modes, system state machine (OFF / IGN-ON / ENABLING /
  MOTOR ENABLED / CHARGING), brakes, lamps, drive-ready, TC intervention level,
  start-inhibit reason bits (kickstand, fork lock, throttle), kickstand, charge
  connector, cooling, slow mode, TPMS warning, ABS, telematics registration, ...
- **Multiplexers**: 0x163 (6-phase temperature mux: inverter-class and
  motor/coolant-class temperatures) and 0x164 (4-phase battery-energy register).
- **Derived**: VIN from 0x56D/56E/56F, cruise state from 0x160 D5/D6, HV contactor
  open detection from 0x181, ABS init/ready handshake from 0x3C8, SoC estimate
  from the 0x186 Wh meter, pack power from 0x181 V x I.
- **Sentinels**: 0xFF/0xFFFF "asleep" payloads, gyro 0xFFFE, BMS pre-init zeros.

Key signals and where they come from:

| Quantity | Frame | Note |
|---|---|---|
| vehicle speed, motor rpm (signed) | 0x160 | rpm sign is the only reverse indicator |
| wheel speeds front/rear | 0x326 | unsigned, rest value 2, no CRC |
| throttle, torque request/delivered | 0x161 | torque in counts (zero = 5000) |
| pack voltage / current / power | 0x181 | current charge-positive |
| SoC | 0x185 | nibble-shifted 12-bit percent |
| cell min/avg/max voltage | 0x182 | 96S pack |
| trip energy, remaining energy | 0x186 | Wh |
| pack temperatures, ambient, slow mode | 0x183 | two different scales in one frame |
| inverter / motor-coolant temperature | 0x163 mux | |
| odometer (m) | 0x562 | 0x3C6 is a lagging echo |
| ride mode, TC | 0x134 (+0x260 mirror) | |
| system / drive state | 0x133, 0x136, 0x162, 0x184 | |
| brakes, lamps, horn, hazards | 0x15A, 0x133, 0x324, 0x131, 0x152, 0x354 | |
| tyre pressures, aux 12 V | 0x33A, 0x334, 0x332 | |
| charge port / charger cooling | 0x1A7, 0x1A6 | |
| IMU accel / gyro (counts) | 0x122, 0x126 | ~7770 counts per g |
| wall clock | 0x3C4 | free-running, not civil time |

### Caveats inherited from the database

The database is a community reverse-engineering effort at v0.1. Signals are tagged
CONFIRMED / STRONG / TENTATIVE in its comments; this firmware mirrors the tags
(`s2_overlay_confidence`) and marks tentative values with `?` in the log. Some
decodes are explicitly retracted or suspect in the database and are logged raw
only (0x322 D3 "motion state", 0x33A D4/D5 "tyre temperatures", 0x3CC fault-latch
semantics). The DBC's SG_ lines and comments disagree in a few places (for
example the cooling-pump bit of 0x163: the SG_ says D3 bit 6, one comment says
bit 5); the firmware follows the SG_ lines, which the database's own validation
tooling uses.

## UDS diagnostics polling (optional)

With `S2_CAN_LISTEN_ONLY` off and `S2_UDS_ENABLE` on, a poller reads the DIDs of
`can-db/uds_catalog.json` (per-cell voltages and temperatures, pack current, motor
rpm, wheel speeds, brake pressure, charger state, ride-mode calibration, ...) with
service 0x22 through the BCM gateway, one request at a time (ISO-TP with flow
control, so multi-frame responses cannot interleave). Locked (0x27) and refuted
DIDs are skipped; the telematics module is off by default because it exposes the
GPS position. Decoded values follow the catalog formulas (`src/uds_decode.c`),
everything else is hex-dumped.

**This transmits on the motorcycle's CAN bus.** Enable it only after the passive
tap has been verified (clean frame table, no CRC failures) and preferably on a
stand. The requests use the same tester ids as the instrument cluster's own
diagnostics screen; the poller backs off for a couple of seconds whenever it sees
another tester's request, and negative responses received while another tester
was active are marked as uncertain in the log.

## Bluetooth LE

The firmware advertises as a BLE peripheral named `S2-DASH`, carrying two
services at once: the **Nordic UART Service** for apps that already exist, and a
**custom telemetry service** for an app written against the contract below.

The ESP32-S3 has no Bluetooth Classic radio, so there is no SPP option and the
bike cannot be paired as a serial port. BLE GATT is the only route, and the
stack is NimBLE in a peripheral-only role, pinned to core 0 so the CAN decoder
keeps core 1 to itself.

### Reading it with an app that already exists

No phone-side development is needed for a spot check:

| App | What to use | What you get |
| --- | --- | --- |
| nRF Connect for Mobile | generic GATT browser | every service, with each characteristic labelled by its `0x2901` descriptor; subscribe to any of them |
| nRF Toolbox | the UART module | the `name=value` text digest, and a text box to type commands |
| Serial Bluetooth Terminal | BLE mode, nRF/micro:bit profile or a custom UUID profile | the same text stream as a scrolling terminal |

The text side sends a short digest once a second rather than all 113 signals,
because a full dump every second would swamp a terminal. Type `all` for the
complete set on demand.

### Commands

Write ASCII to either the UART RX characteristic or the custom command
characteristic. Case does not matter and a trailing CR or LF is ignored.

| Command | Effect |
| --- | --- |
| `help`, `?` | list the commands |
| `info` | firmware version, database version, VIN, uptime |
| `all` | one-shot text dump of every signal that has been seen |
| `catalog` | one-shot binary catalog on the stream characteristic |
| `stream on` / `stream off` | start or stop the periodic text digest |
| `rate <1-20>` | dashboard notify rate, in Hz |

**Every command only changes what the device reports.** None can leave
listen-only mode, transmit a CAN frame or start a UDS request, and that boundary
is deliberate: anyone within radio range can connect.

Text replies (`help`, `info`, `all`) are notified on the UART service, so with
`S2_BLE_NUS_ENABLE` off a custom client reads the info characteristic and the
binary stream instead. `catalog` works either way.

### GATT contract

Nordic UART Service, the de-facto text pipe that the apps above look for:

| UUID | Properties | Purpose |
| --- | --- | --- |
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | service | |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | write, write-without-response | commands to the device |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | notify | `name=value` lines, newline separated |

Custom telemetry service, for a purpose-built client:

| UUID | Properties | Purpose |
| --- | --- | --- |
| `5332da00-1b2c-4f3e-8a9d-1e0f2a3b4c5d` | service | |
| `5332da01-...` | read, notify | ride dashboard, fixed 24-byte frame |
| `5332da02-...` | notify | telemetry stream, fragmented records |
| `5332da03-...` | write, write-without-response | commands, same grammar as above |
| `5332da04-...` | read | device info as text |

The service UUID is in the scan response rather than the advertisement, because
a 128-bit UUID and a name do not both fit in 31 bytes.

Rather than one characteristic per signal, values travel as records inside these
four. 113 characteristics would mean several hundred attributes to enumerate on
every connect and a separate subscription write per signal; a record carries the
same information and the record's index is the signal's index, which is stable
for a database version and described by the catalog.

**Every multi-byte field is little-endian and floats are IEEE-754 binary32.**

#### Dashboard frame, 24 bytes

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | protocol version (currently 1) |
| 1 | u8 | flags; bit 0 = UDS polling is enabled in this build |
| 2 | u16 | freshness, 2 bits per field, in the order power, torque, volts, energy, SoH |
| 4 | f32 | power out of the pack, kW (negative is regen) |
| 8 | f32 | torque delivered, Nm (an estimate, see below) |
| 12 | f32 | pack voltage, V |
| 16 | f32 | energy used this key-on cycle, kWh |
| 20 | f32 | state of health, % |

Each 2-bit freshness field is 0 for never received, 1 for live and 2 for stale.
A stale value is the last one known, not a current reading. These are the same
five values the screen shows, derived by the same rules, so the two cannot
disagree.

#### Stream fragments

A snapshot of all 113 signals exceeds any ATT MTU, so record messages are split
across notifications. Every fragment starts with the same 8-byte header:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | protocol version |
| 1 | u8 | message type: 1 = snapshot, 2 = catalog |
| 2 | u8 | fragment index, 0-based |
| 3 | u8 | fragment count |
| 4 | u16 | records in this fragment |
| 6 | u16 | sequence number, one per complete message |

Collect fragments 0 to count-1 that share a sequence number; discard the partial
message if the sequence number changes. At the default 247-byte MTU a snapshot is
four fragments and the catalog is 29.

A **snapshot record** is 8 bytes: `u16` signal index, `u16` age in deciseconds
(`0xFFFF` means never seen), `f32` physical value.

A **catalog record** is 56 bytes: `u16` signal index, `u16` CAN identifier,
`f32` minimum, `f32` maximum, 28 bytes of NUL-padded name, 16 bytes of
NUL-padded unit. Names and units longer than their field are truncated. Fetch it
once per connection and a client can label and range any signal without
compiling the database in.

### Cost

Enabling BLE takes the firmware from about 343 KB to about 754 KB, which is 72%
of the 1 MiB `factory` partition, and static RAM from about 29.6 KB to about
42.6 KB. With UDS polling also on it is 74% of the partition.

`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` is on: measured here it costs 110 KB of flash
and gives back 16.8 KB of internal RAM. That is the right way round, because the
134 KB framebuffer already claims most of the runtime pool, and because running
out of flash fails the build visibly whereas running out of heap fails at runtime
inside the BLE stack. If the app partition ever runs out,
`partitions_singleapp_large.csv` raises it to 1500 KB.

`CONFIG_TWAI_ISR_IN_IRAM` was turned off to make room. It was a latency
optimisation rather than a requirement; the driver's RX queue absorbs the
difference at 500 kbit/s.

## Updating the firmware

The dashboard can flash itself over Wi-Fi. Hold the screen button, it reboots
into update mode, brings up its own access point and serves an upload page.
Connect a phone, open the page in any browser, pick a `.bin` and that is it. No
app, no stored credentials, no internet.

### The one-time wired migration

**This only works from a firmware built with the two-slot partition table.** A
board flashed with the old single-slot table has nowhere to put a second image,
so it needs one last cable:

```
pio run -t erase        # optional but safest: the table itself is changing
pio run -t upload       # writes the bootloader, the new table and the app
```

The boot banner then reports `running ota_0 at 0x020000`. After that, updates
are wireless. The layout:

| Partition | Offset | Size | |
| --- | --- | --- | --- |
| nvs | `0x9000` | 24 KB | unmoved from the old table |
| otadata | `0xf000` | 8 KB | which slot to boot |
| phy_init | `0x11000` | 4 KB | |
| ota_0 | `0x20000` | 1700 KB | |
| ota_1 | `0x1d0000` | 1700 KB | |

540 KB of the 4 MB part is left spare. The image is about 1.19 MB with Wi-Fi
compiled in, which is 70% of a slot.

Two `platformio.ini` lines make this work, and both are needed. The table is
selected by `board_build.partitions`, **not** by the Kconfig partition option. The PlatformIO ESP-IDF builder reads the former and
ignores the latter, taking only `PARTITION_TABLE_OFFSET` from sdkconfig. Both are
set so menuconfig tells the truth, but only the `platformio.ini` line has any
effect.

The second line is `upload_command`, with `tools/pio_app_offset.py` behind it.
The platform computes the app's flash offset incorrectly on the upload path and
passes 0x10000, the single-app default. On a two-slot table 0x10000 is inside
`otadata`, so esptool refuses the whole write:

```
Detected overlap at address: 0x10000 for file: firmware.bin
```

The script reads the real offset out of the partition CSV and `upload_command`
passes it through. Both can go once the platform is fixed. If you ever see that
error again, check what offset the script prints during a build against `ota_0`
in the table.

### Doing an update

1. **Hold the screen button** for two seconds, parked. The dashboard refuses
   while the bike is rolling, and reboots into update mode otherwise.
2. The screen shows a network name, a **passphrase** and an address. The
   passphrase is generated fresh every time and never stored, so only somebody
   looking at the bike can connect.
3. Join that network, open the address, pick the `.bin` from
   `.pio/build/waveshare_esp32_s3_zero/firmware.bin`.
4. The screen tracks progress and the version transition. On success it reboots
   into the new firmware.

Hold the button again to cancel. Update mode also gives up on its own after five
minutes with nothing uploaded, so the access point is never left running.

### If the access point is not visible

The screen will tell you. It shows **RADIO NOT UP** in red instead of a
passphrase whenever the driver has not reported `WIFI_EVENT_AP_START`, because
`esp_wifi_start()` returning success does not mean the radio is beaconing. When
the access point is up, the channel and the actual transmit power appear under
the connection details.

Two things make this failure mode likely on a vehicle, and both have a knob:

- **Transmit power.** Wi-Fi at full power is the largest peak current anything
  on this board draws. The default is deliberately 13 dBm rather than the 20 dBm
  the PHY would otherwise use, set by `S2_OTA_AP_TX_POWER_QDBM` in quarter-dBm
  units. Lower it further if the access point is unreliable; a phone at the bike
  has link margin to spare. The driver quantises to
  {8, 20, 28, 34, 44, 52, 56, 60, 66, 72, 80}, so only those values do anything.
- **Stale radio calibration.** ESP-IDF caches an RF calibration blob in NVS and
  reuses it on every boot, checking only its format version, the chip's MAC and
  its length. A blob calibrated while the supply was noisy or sagging passes all
  of those, is never refreshed, and produces a radio that looks dead across
  reboots. `S2_OTA_FRESH_RF_CAL` erases it on entry to update mode so a full
  calibration runs, costing about 100 ms on a path taken rarely and on purpose.

`CONFIG_ESP_PHY_REDUCE_TX_POWER` is also enabled, so a boot following a brownout
drops transmit power to the lowest table entry on its own.

The serial log now reports the reset reason on every boot, including
`BROWNOUT - the supply collapsed`. That matters because the brownout detector
sits at its least sensitive setting of 2.44 V and the message it prints goes out
over a USB console the brownout itself takes down, so on the bike the event
would otherwise leave no trace.

If the screen says the radio is up but no phone sees the network, the log will
show whether any probe requests arrived. Probe requests prove the receive path
works and that a phone is scanning the channel; beacons going out but nothing
coming back points at the supply or the antenna rather than at the firmware.
Espressif's limit for supply ripple is **80 mV peak to peak**, which a 12 V buck
converter with long leads and no bulk capacitance at the board can exceed
easily. Measure the 5 V pad with a scope before changing anything else, add a
bulk capacitor across the 5 V and ground pads, and keep the switching converter
away from the antenna end of the board.

### Rollback

A newly flashed image runs on probation. If it stays up for 30 seconds it is
marked valid; if it panics or trips the watchdog first, the bootloader reverts to
the slot that was working. The serial log says `image confirmed` when it passes,
and the boot banner reports `image pending-verify` while it has not.

Update mode refuses to start while an image is still on probation, because
ESP-IDF will not begin a second update from an unconfirmed one.

### What it does and does not protect against

Images are checked for the `0xE9` app magic on the first byte and validated
against their checksum before the boot slot is switched, so a truncated upload or
the wrong file is rejected rather than booted. Images are **not signed**: someone
who can read the passphrase off the screen can flash the device. That is the
trade that was chosen, and the mitigations are the per-session passphrase, the
standstill check and rollback.

### Notes

- **Update mode needs the display**, since the button is the only way in and the
  screen is the only place the passphrase appears. A display-less build says so
  at boot.
- **BLE is never started in update mode.** The BLE controller executes from
  flash and erasing flash stalls the cache, so a controller left running during
  an update can starve. Coming up with it never started removes the problem and
  leaves the heap for the Wi-Fi stack. Rebooting into update mode, rather than
  tearing the radio down in place, is why no teardown code exists.
- **CAN keeps running**, so the handlebar button can cancel and the standstill
  check stays live. Its interrupt handler is also in flash, so some frames are
  lost during erases; that costs nothing while parked and cannot disturb a bus
  the firmware never acknowledges.
- Writes use `OTA_WITH_SEQUENTIAL_WRITES`, which erases a 4 KB sector at a time
  inside each write, rather than the one long partition-wide erase that
  `OTA_SIZE_UNKNOWN` would do. That keeps every cache-off window short.
- `CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` must stay
  enabled. Turning them off saves about 27 KB of IRAM but moves Wi-Fi code into
  flash, which is exactly the wrong trade for the one workload that erases flash
  while the radio is serving a connection.

## Project layout

```
platformio.ini           envs + board_build.partitions (the two-slot OTA table)
sdkconfig.defaults       SDK configuration (USB console, 4 MB flash, NimBLE, OTA, ...)
can-db/                  vendored database (DBC + UDS catalog, CC BY 4.0)
tools/dbc2c.py           DBC -> src/gen/s2_dbc_gen.[ch]
tools/uds2c.py           UDS catalog -> src/gen/s2_uds_gen.[ch]
tools/pio_scons_guard.py PlatformIO workaround (see Toolchain notes)
tools/check_button_mapping.sh  each handlebar-button Kconfig choice -> the right table entry
tools/pio_app_offset.py  flash the app at the offset the partition table specifies
include/s2_dbc.h         signal/message data model (shared with host tests)
src/main.c               tasks: decoder, summary, UDS poller, LED/housekeeping
src/can_bus.[ch]         TWAI node (esp_driver_twai), ISR -> queue, stats, bus-off recovery
src/decoder.[ch]         frame dispatch, E2E check, signal extraction, change log
src/dbc_decode.[ch]      generic Motorola/Intel bit extraction + scaling
src/e2e.[ch]             CRC-8/SAE-J1850 and alive-counter tracking
src/s2_overlay.[ch]      comment-derived knowledge: enumerations, muxes, derived states
src/vehicle_state.[ch]   latest values, per-message and unknown-id statistics
src/summary.[ch]         periodic summary and frame table
src/log_writer.[ch]      non-blocking console writer (ring buffer + task)
src/uds_client.[ch]      ISO-TP transport + UDS 0x22 poller
src/uds_decode.[ch]      per-DID interpretation
src/status_led.[ch]      WS2812 via RMT
src/vehicle_button.[ch]  handlebar-button table and press detection (pure)
src/ride_limits.h        plausibility/freshness rules and the per-frame extremes (pure)
src/ota.[ch]             update mode: access point, upload page, image write, rollback
src/ble_proto.[ch]       BLE wire formats and command grammar (pure, host-testable)
src/ble_telemetry.[ch]   NimBLE peripheral: GATT table, advertising, publisher task
src/display/panel.[ch]   SPI bus, framebuffer, banded DMA flush, backlight
src/display/gfx.[ch]     RGB565 renderer: shapes, ring gauge, font, 7-segment digits
src/display/screens.[ch] the screen layouts (pure, host-testable)
src/display/display.[ch] render task, data snapshot, screen button
tools/mkfont.py          5x7 font -> src/gen/s2_font5x7.[ch] (original artwork)
src/Kconfig.projbuild    menuconfig options
test/sdkconfig.h         host-test stub for the generated sdkconfig
test/test_decoder/       Unity tests (pio test -e native)
test/test_gfx/           renderer primitive tests
test/test_screens/       layout tests: frame bounds, ring clearance, placeholders
test/test_ble_proto/     wire-format, fragmentation and command-parser tests
test/test_vehicle_button/ button table and press-detection tests
test/test_ride_extremes/ peak tracking: both ends, gating, and what sampling misses
```

### Updating the database

Copy the new `.dbc` and `uds_catalog.json` into `can-db/`, run
`python3 tools/dbc2c.py` and `python3 tools/uds2c.py`, review the diff in
`src/gen/`, then re-read the comment blocks of changed frames and update
`src/s2_overlay.c` (the generators only consume BO_/SG_ lines and the E2E id lists
in the header; enumeration texts live in the overlay because the database keeps
them in comments, not VAL_ lines). `python3 tools/dbc2c.py --check` fails when the
generated files are stale, and the generator aborts on constructs the decoder does
not support (multiplexed signals, 29-bit ids).

## License

No licence has been chosen for the firmware yet (add a `LICENSE` file). The
vendored database in `can-db/` is CC BY 4.0 by its authors; keep the attribution
in `can-db/README.md` when redistributing.
