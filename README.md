# Aurora

## Migration plan

Do not rewrite the entire application in one pass. Use this order:

1. Install and pin an ESP-IDF version.
2. Create an empty native ESP-IDF project.
3. Make `app_main()` build, flash, and print a log message.
4. Add GPIO and LED control.
5. Add I2C and validate the MPU6050 independently.
6. Add SPI and validate the BMP280 independently.
7. Add LEDC and validate each servo with the motor/fin mechanism disconnected.
8. Add UART and validate GPS parsing.
9. Port the navigation math without hardware dependencies.
10. Add sensor-to-navigation queues.
11. Add the flight-control task.
12. Add actuator arming and safety interlocks.
13. Add telemetry and fault handling.
14. Only then test the complete system.

## Native Ubuntu build

This project is built for the ESP32-S2 with ESP-IDF v6.0.2. 

For a new Ubuntu machine, install ESP-IDF using [Espressif's Linux setup instructions](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s2/get-started/linux-macos-start-project.html). The ESP-IDF checkout used on this machine is `../esp/esp-idf-v6.0.2`, installed with the `esp32s2` target.

From the Aurora repository root, activate that installation and build:

```bash
source ../esp/esp-idf-v6.0.2/export.sh
idf.py set-target esp32s2  # first build only, or when changing targets
idf.py build
```

The firmware image is written to `build/mekf_firmware.bin`. To flash a connected board, run `idf.py flash`; to open its serial monitor, run `idf.py monitor`.

## Hardware documentation

See [docs/components/README.md](docs/components/README.md) for official component references and a script to download the vendor PDFs.

## Format C/C++ files on commit

The repository hook formats staged C/C++ files with `.clang-format` while preserving unstaged edits. Enable it once in this clone:

```bash
git config --local core.hooksPath .githooks
```

The hook requires Python 3 and `clang-format` on `PATH`.
