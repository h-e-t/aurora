# SD card component

The component mounts a FAT-formatted SD card over SPI and provides synchronous
file reads and writes. The board diagram maps the SD reader to these ESP32 pins:

| SD signal | ESP32 GPIO |
| --- | ---: |
| CS | 2 |
| MOSI | 23 |
| MISO | 19 |
| CLK | 18 |

The default SPI clock is 10 MHz to start conservatively. The SD card must be
powered at the voltage required by its reader board, and its ground must be
shared with the ESP32. `init()` never formats a card if mounting fails.

## Session and file behavior

Each successful card mount creates a previously unused `/sdcard/run_NNNNNN/`
directory. It uses `mkdir`, so a restart after a partial run advances to a new
directory instead of truncating old data. A caller can provide a safe session
identifier, such as a compact GPS UTC timestamp (`20261107T173000Z`); a numeric
suffix is added if that name already exists. With no time source, numbering is
used.

`start_file("flight.csv", header)` creates a file exclusively in the current
session, writes the header, and remembers both for reconnects. It cannot
overwrite an existing file. `write_line()` is convenient for CSV or
line logs, while `write()` and `write_bytes()` accept caller-serialized text or
binary data. `read_file()` reads a bounded amount from a path relative to
`/sdcard`, so older sessions can also be inspected after mounting.

If the card is absent during startup, `init()` still succeeds in offline mode.
The writer retries on later writes. If removal is detected by a failed card
operation, it closes that run; after reinsertion, the next successful retry
creates a new numbered folder and replays the header. There is no card-detect
wire in the current pinout, so removal is detected only when an operation fails.
While offline, records are buffered in an 8 KiB RAM queue and replayed into the
new session after reconnect. If that queue fills, the write returns
`ESP_ERR_NO_MEM`; callers should treat it as a nonfatal storage outage and
report the lost record.

Writes are serialized with a mutex. The default `flush_each_write` setting calls
`fsync()` after each write to reduce the amount of buffered data lost during a
reset. SD writes are synchronous and can take tens of milliseconds; keep them
on the sensor/logging core. A high-rate IMU logger should later send records to
a bounded queue serviced by a dedicated storage task.

The current application writes `flight.csv` with this header:

```text
uptime_us,source,valid_mask,latitude_deg,longitude_deg,altitude_m,speed_mps,course_deg,satellites,hdop,temperature_c,pressure_pa
```

`uptime_us` is ESP-IDF monotonic time since boot. `valid_mask` is hexadecimal:
GPS bits 0–5 indicate position, altitude, speed, course, satellite count, and
HDOP; BMP280 bits 8–9 indicate temperature and pressure. A clear bit means the
corresponding numeric placeholder should be ignored. GPS UTC can later be used
as the session identifier and can also be added as a separate CSV column.
