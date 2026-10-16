#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "cpc"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = CPC_MODEL_6128;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 < CPC_MODEL_COUNT) {
        settings.model = u8;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_commit(h);
    nvs_close(h);
}

uint16_t settings_ram_kb(void)
{
    return settings.model == CPC_MODEL_6128 || settings.model == CPC_MODEL_6128PLUS ? 128 : 64;
}
