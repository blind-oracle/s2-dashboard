# s2-dashboard

CAN statistics logger for the **LiveWire S2 Del Mar** electric motorcycle, running on a
**Waveshare ESP32-S3-Zero** with an **MCP2551** CAN transceiver. It taps the secondary
CAN bus at the Data Link Connector (DLC), decodes every frame described by the
community [livewire-s2-can-db](https://github.com/inklit/livewire-s2-can-db) database
and logs the result over the board's USB serial port. This is the data-gathering
stage of a future dashboard.

It also drives an **ST7789V2 240x280 IPS display** as a rider-facing gauge: power
out and regen, torque, pack voltage, energy used this key cycle and battery state
of health, with a button to cycle between screens.

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
| Screen button | **GPIO6** | to GND, internal pull-up |
| Status LED | GPIO21 | on-board WS2812, already wired |
| Console | USB-C | native USB Serial/JTAG, no UART bridge on this board |

Every pin is configurable in `menuconfig` (S2 Dashboard menu). The six display
signals are a contiguous run on the right-hand header (GP12 down to GP7), which
suits a ribbon; note the header has only one GND and one 3V3 pad, both at the top
of the *left* row, so power and the button return need their own leads.

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

Button: GPIO6 ------ switch ------ GND     (internal pull-up; no external parts needed,
                                            though 10 k + 100 nF is wise in a vehicle)
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
| `S2_DISPLAY_SCREENS` | 4 | screens the button cycles through |
| `S2_DISPLAY_BUTTON_GPIO` / `_ACTIVE_LOW` | 6 / y | screen button; -1 disables |
| `S2_DISPLAY_BL_PWM` / `_BRIGHTNESS` | y / 90 | LEDC-dimmed backlight |
| `S2_DISPLAY_POWER_FULL_SCALE_KW` / `_REGEN_FULL_SCALE_KW` | 70 / 20 | gauge ends |
| `S2_TORQUE_COUNTS_PER_NM_X100` | 335 | torque scale, hundredths (3.35 counts/Nm) |

## What the display shows

**Screen 1 (ride screen)** shows all five quantities at once:

| Element | Source | Notes |
|---|---|---|
| Ring gauge + large number, kW | pack voltage x pack current (0x181) | positive = power out to the motor (amber, red above 45 kW), negative = regen (green, fills the other way from the white zero mark) |
| `nnn ~Nm` | 0x161 `torque_delivered` | the `~` marks it as an estimate, see the caveat below |
| `nnn.n Vdc` | 0x181 `pack_voltage` | 12-bit word, 1 V/count |
| `nn.nn kWh` | 0x186 `trip_energy_consumed_wh` | the bike's own trip meter: energy used since **key-on**, which it also zeroes at each key cycle and clamps at 0 while charging |
| `SOH nn%` | UDS RESS DID 0x020E | UDS-only, see the caveat below |

Below the rows, one dot per screen marks where you are. **Screens 2 to 4 are
deliberately empty** placeholders (`SCREEN 2 / (empty)`) for future content.

**The button** on GPIO6 cycles screens on release, and the new screen is noted in
the serial log.

**Missing and stale data are visually distinct**, because a dashboard that
invents numbers is worse than one that admits it cannot read them:

- **Never seen** shows dashes in the dimmest grey (`---`, `----`, `---.-`,
  `--.--`) with the unit dimmed too, and the ring draws no fill at all. Dashes,
  never zeros: `0.0 kW` is a legitimate reading.
- **Gone stale** (older than 2 s, or 5 s for the slower energy frame) keeps the last
  value but greys it, the ring redraws dim at the last angle, and the header word
  changes from `POWER` to `STALE` - the only text on screen that changes wording.
- **State of health** reads `SOH n/a` when UDS polling is off (the normal
  passive-tap build) and `SOH --%` when it is on but the battery has not answered
  yet, so the two reasons are distinguishable.

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

## Project layout

```
platformio.ini           envs: waveshare_esp32_s3_zero (firmware), native (host tests)
sdkconfig.defaults       SDK configuration (USB console, 4 MB flash, 1 kHz tick, ...)
can-db/                  vendored database (DBC + UDS catalog, CC BY 4.0)
tools/dbc2c.py           DBC -> src/gen/s2_dbc_gen.[ch]
tools/uds2c.py           UDS catalog -> src/gen/s2_uds_gen.[ch]
tools/pio_scons_guard.py PlatformIO workaround (see Toolchain notes)
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
