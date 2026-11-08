#include "bmp280.h"

#include <cmath>
#include <limits>

namespace {
constexpr uint8_t kRegisterCalibration = 0x88;
constexpr uint8_t kRegisterId = 0xD0;
constexpr uint8_t kRegisterPressureData = 0xF7;
constexpr uint8_t kRegisterConfig = 0xF5;
constexpr uint8_t kRegisterControl = 0xF4;
constexpr uint8_t kBmp280Id = 0x58;
constexpr int kI2cTimeoutMs = 100;

uint16_t read_u16_le(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         (static_cast<uint16_t>(bytes[1]) << 8);
}

int16_t read_i16_le(const uint8_t *bytes) {
  return static_cast<int16_t>(read_u16_le(bytes));
}
} // namespace

double Bmp280Reading::altitude_meters(double sea_level_pressure_pa) const {
  if (pressure_pascals_ <= 0 || sea_level_pressure_pa <= 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return 44330.0 * (1.0 - std::pow(pressure_pascals_ / sea_level_pressure_pa,
                                   0.190294957));
}

Bmp280::~Bmp280() {
  if (device_ != nullptr) {
    i2c_master_bus_rm_device(device_);
  }
}

esp_err_t Bmp280::do_init() {
  if (bus_ == nullptr ||
      (address_ != ADDRESS_SDO_LOW && address_ != ADDRESS_SDO_HIGH)) {
    return ESP_ERR_INVALID_ARG;
  }

  i2c_device_config_t device_config{};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = address_;
  device_config.scl_speed_hz = 100000;
  esp_err_t result = i2c_master_bus_add_device(bus_, &device_config, &device_);
  if (result != ESP_OK) {
    return result;
  }

  uint8_t chip_id = 0;
  result = read_registers(kRegisterId, &chip_id, sizeof(chip_id));
  if (result != ESP_OK) {
    return result;
  }
  if (chip_id != kBmp280Id) {
    return ESP_ERR_NOT_FOUND;
  }

  result = read_calibration();
  if (result != ESP_OK) {
    return result;
  }
  if (dig_p1_ == 0) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Standby 500 ms, IIR filter x16; temperature x2, pressure x16, normal mode.
  result = write_register(kRegisterConfig, 0x90);
  if (result != ESP_OK) {
    return result;
  }
  return write_register(kRegisterControl, 0x57);
}

esp_err_t Bmp280::read_registers(uint8_t reg, uint8_t *data, size_t length) {
  if (device_ == nullptr || data == nullptr || length == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_master_transmit_receive(device_, &reg, sizeof(reg), data, length,
                                     kI2cTimeoutMs);
}

esp_err_t Bmp280::write_register(uint8_t reg, uint8_t value) {
  if (device_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(device_, data, sizeof(data), kI2cTimeoutMs);
}

esp_err_t Bmp280::read_calibration() {
  uint8_t calibration[24]{};
  const esp_err_t result =
      read_registers(kRegisterCalibration, calibration, sizeof(calibration));
  if (result != ESP_OK) {
    return result;
  }

  dig_t1_ = read_u16_le(&calibration[0]);
  dig_t2_ = read_i16_le(&calibration[2]);
  dig_t3_ = read_i16_le(&calibration[4]);
  dig_p1_ = read_u16_le(&calibration[6]);
  dig_p2_ = read_i16_le(&calibration[8]);
  dig_p3_ = read_i16_le(&calibration[10]);
  dig_p4_ = read_i16_le(&calibration[12]);
  dig_p5_ = read_i16_le(&calibration[14]);
  dig_p6_ = read_i16_le(&calibration[16]);
  dig_p7_ = read_i16_le(&calibration[18]);
  dig_p8_ = read_i16_le(&calibration[20]);
  dig_p9_ = read_i16_le(&calibration[22]);
  return ESP_OK;
}

esp_err_t Bmp280::acquire_latest(Bmp280Reading &out, bool &available) {
  available = false;
  uint8_t data[6]{};
  const esp_err_t result =
      read_registers(kRegisterPressureData, data, sizeof(data));
  if (result != ESP_OK) {
    return result;
  }

  const int32_t adc_pressure = (static_cast<int32_t>(data[0]) << 12) |
                               (static_cast<int32_t>(data[1]) << 4) |
                               (static_cast<int32_t>(data[2]) >> 4);
  const int32_t adc_temperature = (static_cast<int32_t>(data[3]) << 12) |
                                  (static_cast<int32_t>(data[4]) << 4) |
                                  (static_cast<int32_t>(data[5]) >> 4);
  if (adc_pressure == 0x80000 || adc_temperature == 0x80000) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  const int64_t temperature_delta =
      (static_cast<int64_t>(adc_temperature) >> 3) -
      (static_cast<int64_t>(dig_t1_) << 1);
  const int64_t temperature_var1 = (temperature_delta * dig_t2_) >> 11;
  const int64_t temperature_delta_2 =
      (static_cast<int64_t>(adc_temperature) >> 4) - dig_t1_;
  const int64_t temperature_var2 =
      (((temperature_delta_2 * temperature_delta_2) >> 12) * dig_t3_) >> 14;
  const int32_t t_fine =
      static_cast<int32_t>(temperature_var1 + temperature_var2);
  const double temperature_celsius = (t_fine * 5.0 + 128.0) / 25600.0;

  int64_t pressure_var1 = static_cast<int64_t>(t_fine) - 128000;
  int64_t pressure_var2 = pressure_var1 * pressure_var1 * dig_p6_;
  pressure_var2 += (pressure_var1 * dig_p5_) << 17;
  pressure_var2 += static_cast<int64_t>(dig_p4_) << 35;
  pressure_var1 = ((pressure_var1 * pressure_var1 * dig_p3_) >> 8) +
                  ((pressure_var1 * dig_p2_) << 12);
  pressure_var1 =
      (((static_cast<int64_t>(1) << 47) + pressure_var1) * dig_p1_) >> 33;
  if (pressure_var1 == 0) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  int64_t pressure = 1048576 - adc_pressure;
  pressure = (((pressure << 31) - pressure_var2) * 3125) / pressure_var1;
  pressure_var1 =
      (static_cast<int64_t>(dig_p9_) * (pressure >> 13) * (pressure >> 13)) >>
      25;
  pressure_var2 = (static_cast<int64_t>(dig_p8_) * pressure) >> 19;
  pressure = ((pressure + pressure_var1 + pressure_var2) >> 8) +
             (static_cast<int64_t>(dig_p7_) << 4);

  out.temperature_celsius_ = temperature_celsius;
  out.pressure_pascals_ = static_cast<double>(pressure) / 256.0;
  available = true;
  return ESP_OK;
}

bool Bmp280::is_same_reading(const Bmp280Reading &lhs,
                             const Bmp280Reading &rhs) const {
  return std::fabs(lhs.temperature_celsius_ - rhs.temperature_celsius_) <
             0.02 &&
         std::fabs(lhs.pressure_pascals_ - rhs.pressure_pascals_) < 0.5;
}
