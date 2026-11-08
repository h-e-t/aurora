#include "neo6m.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

int hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

bool parse_number(const char *text, double &result) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || !std::isfinite(value)) {
    return false;
  }
  result = value;
  return true;
}

bool parse_unsigned(const char *text, unsigned long &result) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  result = value;
  return true;
}

bool parse_coordinate(const char *text, char hemisphere, bool latitude,
                      double &degrees) {
  double nmea_value = 0;
  if (!parse_number(text, nmea_value) || nmea_value < 0) {
    return false;
  }

  const double whole_degrees = std::floor(nmea_value / 100.0);
  const double minutes = nmea_value - whole_degrees * 100.0;
  const double limit = latitude ? 90.0 : 180.0;
  if (minutes >= 60.0 || whole_degrees > limit ||
      (whole_degrees == limit && minutes != 0.0)) {
    return false;
  }

  if ((latitude && hemisphere != 'N' && hemisphere != 'S') ||
      (!latitude && hemisphere != 'E' && hemisphere != 'W')) {
    return false;
  }

  degrees = whole_degrees + minutes / 60.0;
  if (hemisphere == 'S' || hemisphere == 'W') {
    degrees = -degrees;
  }
  return true;
}

bool close_enough(double lhs, double rhs, double tolerance) {
  return std::fabs(lhs - rhs) <= tolerance;
}

} // namespace

esp_err_t Neo6M::do_init() {
  uart_config_t uart_config{};
  uart_config.baud_rate = 9600;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;

  esp_err_t result = uart_param_config(UART_PORT, &uart_config);
  if (result != ESP_OK) {
    return result;
  }

  result = uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE);
  if (result != ESP_OK) {
    return result;
  }

  result = uart_driver_install(UART_PORT, GPS_BUFFER_SIZE, 0, 0, nullptr, 0);
  if (result != ESP_OK) {
    return result;
  }

  latest_fix_ = GpsFix{};
  have_fix_ = false;
  sentence_length_ = 0;
  discarding_overlong_sentence_ = false;
  return ESP_OK;
}

esp_err_t Neo6M::read_sentence(char *out, size_t capacity, TickType_t timeout) {
  if (out == nullptr || capacity < 2) {
    return ESP_ERR_INVALID_ARG;
  }

  while (true) {
    uint8_t byte = 0;
    if (uart_read_bytes(UART_PORT, &byte, 1, timeout) <= 0) {
      return ESP_ERR_TIMEOUT;
    }

    if (byte == '\n') {
      if (discarding_overlong_sentence_) {
        discarding_overlong_sentence_ = false;
        sentence_length_ = 0;
        return ESP_ERR_INVALID_SIZE;
      }
      if (sentence_length_ == 0) {
        continue;
      }
      if (sentence_length_ >= capacity) {
        sentence_length_ = 0;
        return ESP_ERR_INVALID_SIZE;
      }

      for (size_t i = 0; i < sentence_length_; ++i) {
        out[i] = sentence_buffer_[i];
      }
      out[sentence_length_] = '\0';
      sentence_length_ = 0;
      return ESP_OK;
    }

    if (byte == '\r' || discarding_overlong_sentence_) {
      continue;
    }
    if (sentence_length_ >= sizeof(sentence_buffer_) - 1) {
      sentence_length_ = 0;
      discarding_overlong_sentence_ = true;
      continue;
    }
    sentence_buffer_[sentence_length_++] = static_cast<char>(byte);
  }
}

esp_err_t Neo6M::acquire_latest(GpsFix &out, bool &available) {
  while (true) {
    char sentence[GPS_NMEA_LINE_MAX];
    const esp_err_t result = read_sentence(sentence, sizeof(sentence), 0);
    if (result == ESP_ERR_TIMEOUT) {
      break;
    }
    if (result == ESP_ERR_INVALID_SIZE) {
      continue;
    }
    if (result != ESP_OK) {
      return result;
    }
    parse_sentence(sentence);
  }

  available = have_fix_;
  if (available) {
    out = latest_fix_;
  }
  return ESP_OK;
}

bool Neo6M::parse_sentence(char *sentence) {
  if (sentence == nullptr || sentence[0] != '$') {
    return false;
  }

  char *checksum_start = std::strchr(sentence, '*');
  if (checksum_start == nullptr || checksum_start[1] == '\0' ||
      checksum_start[2] == '\0') {
    return false;
  }
  const int high = hex_digit(checksum_start[1]);
  const int low = hex_digit(checksum_start[2]);
  if (high < 0 || low < 0) {
    return false;
  }

  uint8_t checksum = 0;
  for (char *cursor = sentence + 1; cursor < checksum_start; ++cursor) {
    checksum ^= static_cast<uint8_t>(*cursor);
  }
  if (checksum != static_cast<uint8_t>((high << 4) | low)) {
    return false;
  }

  *checksum_start = '\0';
  char *fields[20]{};
  size_t field_count = 1;
  fields[0] = sentence;
  for (char *cursor = sentence; *cursor != '\0'; ++cursor) {
    if (*cursor == ',') {
      *cursor = '\0';
      if (field_count == sizeof(fields) / sizeof(fields[0])) {
        return false;
      }
      fields[field_count++] = cursor + 1;
    }
  }

  if (std::strlen(fields[0]) < 6) {
    return false;
  }
  const char *sentence_type = fields[0] + 3;
  bool parsed = false;
  if (std::strncmp(sentence_type, "RMC", 3) == 0) {
    parsed = parse_rmc(fields, field_count);
  } else if (std::strncmp(sentence_type, "GGA", 3) == 0) {
    parsed = parse_gga(fields, field_count);
  }
  if (parsed) {
    have_fix_ = true;
  }
  return parsed;
}

bool Neo6M::parse_rmc(char **fields, size_t count) {
  if (count < 10 || fields[2][0] == '\0') {
    return false;
  }
  if (fields[2][0] == 'V') {
    latest_fix_.valid_ = false;
    latest_fix_.has_position_ = false;
    latest_fix_.has_speed_ = false;
    latest_fix_.has_course_ = false;
    return true;
  }
  if (fields[2][0] != 'A') {
    return false;
  }

  double latitude = 0;
  double longitude = 0;
  if (!parse_coordinate(fields[3], fields[4][0], true, latitude) ||
      !parse_coordinate(fields[5], fields[6][0], false, longitude)) {
    return false;
  }

  latest_fix_.valid_ = true;
  latest_fix_.has_position_ = true;
  latest_fix_.latitude_degrees_ = latitude;
  latest_fix_.longitude_degrees_ = longitude;

  double value = 0;
  latest_fix_.has_speed_ = parse_number(fields[7], value) && value >= 0;
  if (latest_fix_.has_speed_) {
    latest_fix_.speed_knots_ = value;
  }
  latest_fix_.has_course_ =
      parse_number(fields[8], value) && value >= 0 && value <= 360;
  if (latest_fix_.has_course_) {
    latest_fix_.course_degrees_ = value;
  }
  return true;
}

bool Neo6M::parse_gga(char **fields, size_t count) {
  if (count < 10) {
    return false;
  }
  unsigned long quality = 0;
  if (!parse_unsigned(fields[6], quality)) {
    return false;
  }

  double latitude = 0;
  double longitude = 0;
  if (quality > 0 &&
      (!parse_coordinate(fields[2], fields[3][0], true, latitude) ||
       !parse_coordinate(fields[4], fields[5][0], false, longitude))) {
    return false;
  }

  latest_fix_.valid_ = quality > 0;
  if (quality == 0) {
    latest_fix_.has_position_ = false;
    latest_fix_.has_altitude_ = false;
  } else {
    latest_fix_.has_position_ = true;
    latest_fix_.latitude_degrees_ = latitude;
    latest_fix_.longitude_degrees_ = longitude;

    double altitude = 0;
    latest_fix_.has_altitude_ = parse_number(fields[9], altitude) &&
                                count > 10 && std::strcmp(fields[10], "M") == 0;
    if (latest_fix_.has_altitude_) {
      latest_fix_.altitude_meters_ = altitude;
    }
  }

  unsigned long satellites = 0;
  latest_fix_.has_satellite_count_ =
      parse_unsigned(fields[7], satellites) && satellites <= UINT8_MAX;
  if (latest_fix_.has_satellite_count_) {
    latest_fix_.satellite_count_ = static_cast<uint8_t>(satellites);
  }

  double hdop = 0;
  latest_fix_.has_hdop_ = parse_number(fields[8], hdop) && hdop >= 0;
  if (latest_fix_.has_hdop_) {
    latest_fix_.hdop_ = hdop;
  }
  return true;
}

bool Neo6M::is_same_reading(const GpsFix &lhs, const GpsFix &rhs) const {
  if (lhs.valid_ != rhs.valid_ || lhs.has_position_ != rhs.has_position_ ||
      lhs.has_altitude_ != rhs.has_altitude_ ||
      lhs.has_speed_ != rhs.has_speed_ || lhs.has_course_ != rhs.has_course_ ||
      lhs.has_satellite_count_ != rhs.has_satellite_count_ ||
      lhs.has_hdop_ != rhs.has_hdop_) {
    return false;
  }

  if (lhs.has_position_ &&
      (!close_enough(lhs.latitude_degrees_, rhs.latitude_degrees_, 1e-7) ||
       !close_enough(lhs.longitude_degrees_, rhs.longitude_degrees_, 1e-7))) {
    return false;
  }
  if (lhs.has_altitude_ &&
      !close_enough(lhs.altitude_meters_, rhs.altitude_meters_, 0.05)) {
    return false;
  }
  if (lhs.has_speed_ &&
      !close_enough(lhs.speed_knots_, rhs.speed_knots_, 0.01)) {
    return false;
  }
  if (lhs.has_course_) {
    double difference =
        std::fmod(std::fabs(lhs.course_degrees_ - rhs.course_degrees_), 360.0);
    difference = difference > 180.0 ? 360.0 - difference : difference;
    if (difference > 0.1) {
      return false;
    }
  }
  if (lhs.has_satellite_count_ &&
      lhs.satellite_count_ != rhs.satellite_count_) {
    return false;
  }
  return !lhs.has_hdop_ || close_enough(lhs.hdop_, rhs.hdop_, 0.01);
}
