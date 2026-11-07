#include "esp_log.h"

namespace {
constexpr char kTag[] = "mekf_firmware";
}

extern "C" void app_main() {
  ESP_LOGI(kTag, "Native ESP-IDF MEKF firmware started");
}
