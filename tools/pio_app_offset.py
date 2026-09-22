"""
PlatformIO post-build script: flash the application at the offset the partition
table actually specifies.

With the two-OTA-slot table this project uses, the app lives at ota_0
(0x20000), not at the 0x10000 a single-app table uses. The platform gets this
wrong on the upload path and passes 0x10000, which now falls inside the otadata
partition (0xf000..0x11000), so esptool refuses the entire write:

    Detected overlap at address: 0x10000 for file: firmware.bin

For the record, the faulty code is in `builder/frameworks/espidf.py` of
platform-espressif32: it aligns its running offset for each partition but never
adds the partition's size, so any table with blank offset columns - which is
every stock ESP-IDF table - comes out wrong.

Replacing ESP32_APP_OFFSET from here is NOT enough on its own: the upload action
does not pick the new value up. So this also publishes the offset as
S2_APP_OFFSET, and `upload_command` in platformio.ini uses that. Both variables
are set: the second is what actually flashes, the first keeps anything else that
reads it (debug configurations) consistent.

The offset is read out of the partition CSV rather than hardcoded, so it stays
correct if the table is ever changed. Remove this, and the upload_command line,
once the platform computes the offset correctly.
"""

import os

Import("env")  # noqa: F821


def _parse_size(value):
    """Accept the CSV's 0x..., 1700K, 1M and plain decimal size forms."""
    value = str(value).strip()
    if not value:
        return 0
    if value.startswith("0x"):
        return int(value, 16)
    if value[-1].upper() in ("K", "M"):
        return int(value[:-1]) * (1024 if value[-1].upper() == "K" else 1024 * 1024)
    return int(value) if value.isdigit() else 0


def partition_table_offset():
    """
    Where the partition table itself lives, from the generated sdkconfig. The
    first partition starts in the 4 KB immediately after it.
    """
    sdkconfig = os.path.join(
        env.subst("$PROJECT_DIR"), "sdkconfig." + env.subst("$PIOENV")  # noqa: F821
    )
    if os.path.isfile(sdkconfig):
        with open(sdkconfig) as fp:
            for line in fp:
                if line.startswith("CONFIG_PARTITION_TABLE_OFFSET="):
                    return _parse_size(line.split("=", 1)[1])
    return 0x8000


def app_offset_from_table(csv_path):
    """
    Offset of the partition the bootloader will run: `factory` if the table has
    one, otherwise `ota_0`. Mirrors gen_esp32part.py's rules - partitions begin
    after the partition table, app partitions align to 64 KB, everything else
    to 4 KB, and a blank offset column means "straight after the previous one".
    """
    next_offset = partition_table_offset() + 0x1000
    with open(csv_path) as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tokens = [t.strip() for t in line.split(",")]
            if len(tokens) < 5:
                continue
            ptype, subtype = tokens[1], tokens[2]
            align = 0x10000 if ptype in ("app", "0") else 0x1000
            if tokens[3]:
                offset = _parse_size(tokens[3])
            else:
                offset = (next_offset + align - 1) & ~(align - 1)
            if subtype in ("factory", "ota_0"):
                return offset
            next_offset = offset + _parse_size(tokens[4])
    return None


csv_path = env.subst("$PARTITIONS_TABLE_CSV")  # noqa: F821
if csv_path and os.path.isfile(csv_path):
    offset = app_offset_from_table(csv_path)
    if offset is not None:
        # ESP32_APP_OFFSET for consistency, S2_APP_OFFSET because that is the
        # one upload_command reads and therefore the one that flashes.
        env.Replace(ESP32_APP_OFFSET=hex(offset))  # noqa: F821
        env.Replace(S2_APP_OFFSET=hex(offset))  # noqa: F821
        print("app offset from %s: %s" % (os.path.basename(csv_path), hex(offset)))
