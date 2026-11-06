idf_component_register(SRCS "main.c" "settings.c" "rom_store.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_hw_support esp_timer emu archimedes arm2 esp_partition nvs_flash esp_psram)
