#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

#define NAMESPACE   "arc"

static const char *TAG = "settings";

settings_t settings;

void settings_load(void)
{
    settings.model = ARC_MODEL_A440;
    settings.os = ARC_OS_RISCOS3;
    settings.ram_kb = 4096;
    settings.menu_middle = 0;

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 <= ARC_MODEL_A3020) {
        settings.model = u8;
    }
    if (nvs_get_u8(h, "os", &u8) == ESP_OK && u8 <= ARC_OS_RISCOS3) {
        settings.os = u8;
    }
    if (nvs_get_u16(h, "ram_kb", &u16) == ESP_OK &&
        (u16 == 512 || u16 == 1024 || u16 == 2048 || u16 == 4096)) {
        settings.ram_kb = u16;
    }
    if (nvs_get_u8(h, "menu", &u8) == ESP_OK && u8 <= 1) {
        settings.menu_middle = u8;
    }
    nvs_close(h);
    if (settings.model == ARC_MODEL_A3020) {
        settings.os = ARC_OS_RISCOS3;   /* the A3020 needs RISC OS 3 and 2 MB */
        if (settings.ram_kb < 2048) {
            settings.ram_kb = 2048;
        }
    }
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(h, "model", settings.model);
    nvs_set_u8(h, "os", settings.os);
    nvs_set_u16(h, "ram_kb", settings.ram_kb);
    nvs_set_u8(h, "menu", settings.menu_middle);
    nvs_commit(h);
    nvs_close(h);
}

/* cmos0-2 the A440's OSes, cmos5 the A3020's RISC OS 3 */
static void cmos_key(arc_model_t model, arc_os_t os, char *key, size_t len)
{
    snprintf(key, len, "cmos%d", (int)os + (model == ARC_MODEL_A3020 ? 3 : 0));
}

bool settings_load_cmos(arc_model_t model, arc_os_t os, uint8_t *cmos)
{
    char key[8];
    cmos_key(model, os, key, sizeof(key));
    nvs_handle_t h;
    size_t size = ARC_CMOS_SIZE;
    bool ok = false;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, key, cmos, &size) == ESP_OK && size == ARC_CMOS_SIZE;
        nvs_close(h);
    }
    return ok;
}

void settings_save_cmos(arc_model_t model, arc_os_t os, const uint8_t *cmos)
{
    char key[8];
    cmos_key(model, os, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, cmos, ARC_CMOS_SIZE);
        nvs_commit(h);
        nvs_close(h);
    }
}
