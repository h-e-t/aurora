#pragma once

#include <optional>

#include "esp_err.h"

namespace sensors {

/**
 * Common nonblocking interface for sensors that produce typed readings.
 *
 * Each instance tracks the last reading it returned. A later read returns the
 * newest available reading only when is_same_reading() says it has changed.
 * This per-instance cursor is intended for one consumer.
 */
template <typename Reading> class Sensor {
public:
  virtual ~Sensor() = default;

  esp_err_t init() {
    const esp_err_t result = do_init();
    initialized_ = result == ESP_OK;
    if (initialized_) {
      last_returned_.reset();
    }
    return result;
  }

  /**
   * Check for the newest valid reading without waiting for one.
   *
   * On ESP_OK, updated is true and out is assigned only when the available
   * reading differs from the last reading returned by this instance. If no
   * valid reading is available yet, or it has not changed, updated is false
   * and out is left untouched.
   */
  esp_err_t read_latest(Reading &out, bool &updated) {
    updated = false;
    if (!initialized_) {
      return ESP_ERR_INVALID_STATE;
    }

    Reading latest{};
    bool available = false;
    const esp_err_t result = acquire_latest(latest, available);
    if (result != ESP_OK || !available) {
      return result;
    }

    if (last_returned_ && is_same_reading(*last_returned_, latest)) {
      return ESP_OK;
    }

    out = latest;
    last_returned_ = latest;
    updated = true;
    return ESP_OK;
  }

protected:
  /**
   * Fetch or refresh the newest valid reading without blocking.
   * Set available=false when the device has not produced a valid reading yet.
   */
  virtual esp_err_t acquire_latest(Reading &out, bool &available) = 0;

  /** Compare readings using fields and tolerances meaningful to this sensor. */
  virtual bool is_same_reading(const Reading &lhs,
                               const Reading &rhs) const = 0;

  /** Device-specific initialization implementation. */
  virtual esp_err_t do_init() = 0;

private:
  bool initialized_ = false;
  std::optional<Reading> last_returned_;
};

} // namespace sensors
