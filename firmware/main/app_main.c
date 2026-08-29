#include "sdkconfig.h"

#include "esp_log.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "AirDAP firmware requires an ESP32-S3 target"
#endif

static const char *TAG = "airdap";

void app_main(void)
{
    ESP_LOGI(TAG, "AirDAP firmware started");
}
