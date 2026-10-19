#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/roms"

static const char *TAG = "rom";

static const char *const partitions[] = { "rom_arthur120", "rom_riscos200", "rom_riscos311" };
static const uint32_t sizes[] = { 512 * 1024, 512 * 1024, 2 * 1024 * 1024 };

static char s_name[64];

static bool is_os(arc_os_t os, const uint8_t *buf)
{
    arc_os_t found;
    return arc_rom_identify(buf, sizes[os], &found, s_name, sizeof(s_name)) != NULL && found == os;
}

static bool from_partition(arc_os_t os, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           partitions[os]);
    if (part == NULL || part->size < sizes[os]) {
        return false;
    }
    /* Through a mapping, not esp_partition_read(): that turns the caches
     * off while it reads, which stalls the LCD's refill (picture artefacts)
     */
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, sizes[os], ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, sizes[os]);
    esp_partition_munmap(handle);
    return is_os(os, buf);
}

/* Any file of the size that holds the OS (Arthur's images often have a
 * byte too many: those too)
 */
static bool from_sd(arc_os_t os, uint8_t *buf)
{
    DIR *dir = opendir(ROM_FOLDER);
    if (dir == NULL) {
        return false;
    }
    bool ok = false;
    struct dirent *e;
    while (!ok && (e = readdir(dir)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, e->d_name);
        if (e->d_name[0] == '.' || stat(path, &st) != 0 || st.st_size < sizes[os] ||
            st.st_size > sizes[os] + 16) {
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            continue;
        }
        ok = fread(buf, 1, sizes[os], f) == sizes[os] && is_os(os, buf);
        fclose(f);
        if (ok) {
            ESP_LOGI(TAG, "ROM from %s", path);
        }
    }
    closedir(dir);
    return ok;
}

bool rom_store_load(arc_os_t os, uint8_t *buf, uint32_t *size, const char **name)
{
    if ((unsigned)os > ARC_OS_RISCOS3) {
        return false;
    }
    if (!from_partition(os, buf) && !from_sd(os, buf)) {
        return false;
    }
    *size = sizes[os];
    *name = s_name;
    return true;
}
