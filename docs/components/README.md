# Flight-computer component documentation

Adding this folder does not affect the firmware build: ESP-IDF CMake builds the source lists declared by the project and component `CMakeLists.txt` files, and does not compile Markdown or PDF files.

## Hardware assumptions

- The firmware is currently configured for an ESP32-S2 (`idf.py set-target esp32s2`).
- The GPS is a u-blox NEO-6M module. The receiver data sheet covers the u-blox module; a breakout board may have additional power, antenna, and pinout details.
- The IMU is an InvenSense/TDK MPU-6050.
- The pressure sensor is a Bosch BMP280.
- The SD card reader board has not been identified by manufacturer or model. The ESP32-S2 has no SDMMC host controller, so the card must be accessed over SPI. The generic ESP-IDF storage and SPI documentation is linked below; board-specific voltage and pin information needs the reader board's model.

## Download the vendor PDFs

Run this from the repository root when network access is available:

```bash
bash docs/components/download-datasheets.sh
```

The PDFs will be saved under `docs/components/datasheets/`.

| Component | Document | Official source |
| --- | --- | --- |
| u-blox NEO-6M | NEO-6 data sheet (GPS.G6-HW-09005) | [PDF](https://content.u-blox.com/sites/default/files/products/documents/NEO-6_DataSheet_%28GPS.G6-HW-09005%29.pdf) |
| u-blox NEO-6M | u-blox 6 receiver description and protocol specification (GPS.G6-SW-10018) | [PDF](https://content.u-blox.com/sites/default/files/products/documents/u-blox6_ReceiverDescrProtSpec_%28GPS.G6-SW-10018%29_Public.pdf) |
| MPU-6050 | MPU-6000/MPU-6050 product specification | [PDF](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet.pdf) |
| MPU-6050 | Register map and descriptions | [PDF](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf) |
| BMP280 | BMP280 data sheet (BST-BMP280-DS001) | [PDF](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf) |

## ESP32-S2 SD card and storage docs

- [ESP-IDF v6.0 SD/SDIO/MMC driver API](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/api-reference/storage/sdmmc.html)
- [ESP-IDF SD SPI host driver API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/sdspi_host.html)
- [ESP-IDF FAT filesystem API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/storage/fatfs.html)
- [ESP32-S2 SD-card interface note](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s2/api-reference/storage/sdmmc.html) — documents that the S2 uses SPI for SD cards rather than an SDMMC host.
