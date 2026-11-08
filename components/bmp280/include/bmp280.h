#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "sensor.hpp"

class Bmp280Reading {
public:
  Bmp280Reading() = default;

  double temperature_celsius() const { return temperature_celsius_; }
  double temperature_fahrenheit() const {
    return temperature_celsius_ * 9.0 / 5.0 + 32.0;
  }
  double pressure_pascals() const { return pressure_pascals_; }
  double pressure_hectopascals() const { return pressure_pascals_ / 100.0; }

  // Pressure altitude depends on the local sea-level reference pressure.
  double altitude_meters(double sea_level_pressure_pa) const;

private:
  friend class Bmp280;

  double temperature_celsius_ = 0;
  double pressure_pascals_ = 0;
};

class Bmp280 : public sensors::Sensor<Bmp280Reading> {
public:
  static constexpr uint8_t ADDRESS_SDO_LOW = 0x76;
  static constexpr uint8_t ADDRESS_SDO_HIGH = 0x77;

  explicit Bmp280(i2c_master_bus_handle_t bus,
                  uint8_t address = ADDRESS_SDO_LOW)
      : bus_(bus), address_(address) {}
  ~Bmp280() override;

protected:
  esp_err_t do_init() override;
  esp_err_t acquire_latest(Bmp280Reading &out, bool &available) override;
  bool is_same_reading(const Bmp280Reading &lhs,
                       const Bmp280Reading &rhs) const override;

private:
  esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length);
  esp_err_t write_register(uint8_t reg, uint8_t value);
  esp_err_t read_calibration();

  i2c_master_bus_handle_t bus_ = nullptr;
  i2c_master_dev_handle_t device_ = nullptr;
  uint8_t address_ = ADDRESS_SDO_LOW;

  uint16_t dig_t1_ = 0;
  int16_t dig_t2_ = 0;
  int16_t dig_t3_ = 0;
  uint16_t dig_p1_ = 0;
  int16_t dig_p2_ = 0;
  int16_t dig_p3_ = 0;
  int16_t dig_p4_ = 0;
  int16_t dig_p5_ = 0;
  int16_t dig_p6_ = 0;
  int16_t dig_p7_ = 0;
  int16_t dig_p8_ = 0;
  int16_t dig_p9_ = 0;
};
