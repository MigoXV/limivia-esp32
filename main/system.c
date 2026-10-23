#include "services.h"
#include "peripherals.h"
#include "driver/gpio.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

esp_err_t v3_nvs_error;
atomic_bool v3_inhibit_requested;
atomic_bool v3_inhibit_value, v3_runtime_ready, v3_boot_healthy;
atomic_bool v3_inhibit_busy;
atomic_int v3_inhibit_error;
static esp_err_t fs_error;
static atomic_bool inhibited;
static RTC_DATA_ATTR volatile uint32_t rtc_inhibit;
void v3_inhibit_latch(bool value) { rtc_inhibit = value ? 0x4c565354u : 0; }
static int checkpoint_error;
bool v3_autostart_inhibited(void) { return atomic_load(&inhibited); }
static int checkpoint_save(const cJSON *p) {
    if (!v3_storage_ready())
        return ESP_ERR_INVALID_STATE;
    char *text = cJSON_PrintUnformatted(p);
    if (!text)
        return ESP_ERR_NO_MEM;
    size_t n = strlen(text);
    if (n > 2048) {
        cJSON_free(text);
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen("/data/.sleep.tmp", "wb");
    if (!f) {
        cJSON_free(text);
        return ESP_FAIL;
    }
    bool ok = fwrite(text, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    cJSON_free(text);
    if (fclose(f))
        ok = false;
    if (!ok) {
        unlink("/data/.sleep.tmp");
        return ESP_FAIL;
    }
    return rename("/data/.sleep.tmp", "/data/.sleep") ? ESP_FAIL : ESP_OK;
}
cJSON *v3_checkpoint_take(void) {
    if (!v3_storage_ready())
        return NULL;
    FILE *f = fopen("/data/.sleep", "rb");
    if (!f)
        return NULL;
    char bytes[2049];
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    bool ok = !ferror(f) && n < sizeof(bytes);
    fclose(f);
    cJSON *p = NULL;
    uint32_t wake = esp_sleep_get_wakeup_causes();
    if (ok && esp_reset_reason() == ESP_RST_DEEPSLEEP &&
        (wake & ((1u << ESP_SLEEP_WAKEUP_TIMER) | (1u << ESP_SLEEP_WAKEUP_GPIO))))
        p = v3_parse(bytes, n);
    if (!ok || (esp_reset_reason() == ESP_RST_DEEPSLEEP && !p))
        checkpoint_error = ESP_ERR_INVALID_ARG;
    if (unlink("/data/.sleep")) {
        checkpoint_error = ESP_FAIL;
        cJSON_Delete(p);
        p = NULL;
    }
    return p;
}
static int64_t reset_at;
static struct {
    FILE *file;
    size_t expected, used;
    uint32_t id;
    char name[49], sha[65];
    bool secret;
    psa_hash_operation_t hash;
} upload;
static struct {
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    size_t expected, used;
    uint32_t id;
    char sha[65], profile[8];
    psa_hash_operation_t hash;
} ota;
static void hash_hex(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}
static bool sha_valid(const char *sha) {
    if (strlen(sha) != 64)
        return false;
    for (int i = 0; i < 64; ++i)
        if (!strchr("0123456789abcdef", sha[i]))
            return false;
    return true;
}
bool v3_storage_name(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 48 || s[0] == '.')
        return false;
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-' && s[i] != '.')
            return false;
    return true;
}
bool v3_storage_ready(void) { return fs_error == ESP_OK; }
static void path_for(char out[64], const char *name) { snprintf(out, 64, "/data/%s", name); }
int v3_storage_read_bytes(const char *name, bool secret, char **out, size_t *length) {
    if (!v3_storage_ready() || !v3_storage_name(name))
        return ESP_ERR_INVALID_STATE;
    char path[64];
    snprintf(path, sizeof(path), secret ? "/data/.secret-%s" : "/data/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size < 0 || size > 16384) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    bool ok = fread(data, 1, size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(data);
        return ESP_FAIL;
    }
    data[size] = 0;
    *out = data;
    *length = size;
    return ESP_OK;
}
int v3_storage_save(const char *name, const char *data, size_t n) {
    if (!v3_storage_ready() || !v3_storage_name(name))
        return ESP_ERR_INVALID_STATE;
    FILE *f = fopen("/data/.atomic", "wb");
    if (!f)
        return ESP_FAIL;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f))
        ok = false;
    if (!ok) {
        unlink("/data/.atomic");
        return ESP_FAIL;
    }
    char path[64];
    path_for(path, name);
    if (rename("/data/.atomic", path)) {
        unlink("/data/.atomic");
        return ESP_FAIL;
    }
    return ESP_OK;
}
int v3_storage_load(const char *name, cJSON **out) {
    if (!v3_storage_ready() || !v3_storage_name(name))
        return ESP_ERR_INVALID_STATE;
    char path[64];
    path_for(path, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return ESP_FAIL;
    }
    long n = ftell(f);
    if (n <= 0 || n > 16384) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    char *text = malloc((size_t)n + 1);
    if (!text) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    bool ok = fread(text, 1, n, f) == (size_t)n;
    fclose(f);
    text[n] = 0;
    *out = ok ? v3_parse(text, n) : NULL;
    free(text);
    return *out ? ESP_OK : ESP_ERR_INVALID_ARG;
}
static int inhibit_store(bool value) {
    if (v3_nvs_error)
        return v3_nvs_error;
    nvs_handle_t n;
    int err = nvs_open("limivia3", NVS_READWRITE, &n);
    if (err)
        return err;
    err = nvs_set_u8(n, "flow_inhibit", value);
    if (!err)
        err = nvs_commit(n);
    nvs_close(n);
    if (!err)
        inhibited = value;
    return err;
}
void v3_services_init(void) {
    v3_guard_init();
    esp_vfs_littlefs_conf_t fs = {.base_path = "/data",
                                  .partition_label = "littlefs",
                                  .format_if_mount_failed = false,
                                  .dont_mount = false};
    fs_error = esp_vfs_littlefs_register(&fs);
    if (!v3_nvs_error) {
        nvs_handle_t n;
        if (nvs_open("limivia3", NVS_READONLY, &n) == ESP_OK) {
            uint8_t value = 0;
            nvs_get_u8(n, "flow_inhibit", &value);
            inhibited = value;
            nvs_close(n);
        }
    }
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && rtc_inhibit == 0x4c565354u) {
        atomic_store(&inhibited, true);
        atomic_store(&v3_inhibit_value, true);
        atomic_store(&v3_inhibit_requested, true);
    }
    // USB recovery remains available on data initialization errors. Never erase NVS/FS here.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (v3_nvs_error)
            esp_ota_mark_app_invalid_rollback_and_reboot();
        // Final confirmation is deferred until the runtime/transport starts successfully.
    }
}
cJSON *v3_system_status(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(p, "minimum_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(p, "largest_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(p, "reset_reason", esp_reset_reason());
    cJSON_AddNumberToObject(p, "nvs_error", v3_nvs_error);
    cJSON_AddNumberToObject(p, "filesystem_error", fs_error);
    cJSON_AddBoolToObject(p, "persisted_autostart_inhibited", inhibited);
    cJSON_AddNumberToObject(p, "checkpoint_error", checkpoint_error);
    cJSON_AddBoolToObject(p, "inhibit_pending", atomic_load(&v3_inhibit_requested));
    cJSON_AddNumberToObject(p, "inhibit_error", atomic_load(&v3_inhibit_error));
    return p;
}
static int storage(const char *op, const cJSON *p, cJSON **out) {
    if (!strcmp(op, "storage.status")) {
        *out = cJSON_CreateObject();
        size_t total = 0, used = 0;
        int err = esp_littlefs_info("littlefs", &total, &used);
        cJSON_AddNumberToObject(*out, "mount_error", fs_error);
        cJSON_AddNumberToObject(*out, "total", total);
        cJSON_AddNumberToObject(*out, "used", used);
        return err;
    }
    if (!strcmp(op, "storage.format")) {
        if (strcmp(v3_str(p, "confirm", ""), "erase_littlefs") || upload.file)
            return ESP_ERR_INVALID_ARG;
        if (!fs_error)
            esp_vfs_littlefs_unregister("littlefs");
        int err = esp_littlefs_format("littlefs");
        if (err) {
            fs_error = err;
            return err;
        }
        esp_vfs_littlefs_conf_t fs = {
            .base_path = "/data", .partition_label = "littlefs", .format_if_mount_failed = false};
        fs_error = esp_vfs_littlefs_register(&fs);
        return fs_error;
    }
    if (fs_error)
        return fs_error;
    if (!strcmp(op, "storage.record") || !strcmp(op, "storage.append")) {
        const char *name = v3_str(p, "name", "");
        const cJSON *value = v3_get(p, "value");
        if (!v3_storage_name(name) || !value)
            return ESP_ERR_INVALID_ARG;
        char *text = cJSON_PrintUnformatted(value);
        if (!text)
            return ESP_ERR_NO_MEM;
        size_t n = strlen(text);
        if (n > 8192) {
            cJSON_free(text);
            return ESP_ERR_INVALID_SIZE;
        }
        if (!strcmp(op, "storage.record")) {
            int err = v3_storage_save(name, text, n);
            cJSON_free(text);
            return err;
        }
        if (!v3_integer(v3_get(p, "limit_bytes"), 1, 262144)) {
            cJSON_free(text);
            return ESP_ERR_INVALID_ARG;
        }
        char path[64];
        path_for(path, name);
        struct stat st;
        size_t old_size = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
        if (old_size + n + 1 > (size_t)v3_int(p, "limit_bytes", 0)) {
            cJSON_free(text);
            return ESP_ERR_INVALID_SIZE;
        }
        FILE *old = fopen(path, "rb"), *temp = fopen("/data/.record", "wb");
        if (!temp) {
            if (old)
                fclose(old);
            cJSON_free(text);
            return ESP_FAIL;
        }
        bool ok = true;
        uint8_t chunk[512];
        size_t copied = 0;
        while (old) {
            size_t count = fread(chunk, 1, sizeof(chunk), old);
            if (!count) {
                if (ferror(old))
                    ok = false;
                break;
            }
            if (fwrite(chunk, 1, count, temp) != count) {
                ok = false;
                break;
            }
            copied += count;
        }
        if (old)
            fclose(old);
        if (copied != old_size)
            ok = false;
        if (ok)
            ok = fwrite(text, 1, n, temp) == n && fputc('\n', temp) != EOF && fflush(temp) == 0 &&
                 fsync(fileno(temp)) == 0;
        cJSON_free(text);
        if (fclose(temp))
            ok = false;
        if (!ok) {
            unlink("/data/.record");
            return ESP_FAIL;
        }
        if (rename("/data/.record", path))
            return ESP_FAIL;
        *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(*out, "size", old_size + n + 1);
        return ESP_OK;
    }
    if (!strcmp(op, "storage.list")) {
        DIR *dir = opendir("/data");
        if (!dir)
            return ESP_FAIL;
        *out = cJSON_CreateObject();
        cJSON *a = cJSON_AddArrayToObject(*out, "files");
        int page = v3_int(p, "page", 0), index = 0, count = 0;
        if (page < 0 || page > 1024) {
            closedir(dir);
            return ESP_ERR_INVALID_ARG;
        }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.')
                continue;
            if (index++ < page * 16)
                continue;
            if (count++ >= 16)
                break;
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", entry->d_name);
            char path[280];
            snprintf(path, sizeof(path), "/data/%s", entry->d_name);
            struct stat st;
            if (!stat(path, &st))
                cJSON_AddNumberToObject(f, "size", st.st_size);
            v3_push(a, f);
        }
        closedir(dir);
        cJSON_AddBoolToObject(*out, "more", count > 16);
        return ESP_OK;
    }
    if (!strcmp(op, "storage.begin")) {
        const char *name = v3_str(p, "name", ""), *sha = v3_str(p, "sha256", "");
        if (upload.file)
            return ESP_ERR_INVALID_STATE;
        if (!v3_storage_name(name) || !sha_valid(sha) || !v3_integer(v3_get(p, "size"), 1, 262144))
            return ESP_ERR_INVALID_ARG;
        upload.file = fopen("/data/.upload", "wb");
        if (!upload.file)
            return ESP_FAIL;
        strcpy(upload.name, name);
        upload.secret = v3_bool(p, "secret", false);
        strcpy(upload.sha, sha);
        upload.expected = v3_int(p, "size", 0);
        upload.used = 0;
        upload.id = (uint32_t)esp_timer_get_time() & 0x7fffffff;
        if (!upload.id)
            upload.id = 1;
        upload.hash = psa_hash_operation_init();
        if (psa_hash_setup(&upload.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            fclose(upload.file);
            upload.file = NULL;
            return ESP_FAIL;
        }
        *out = cJSON_CreateObject();
        v3_add_u32(*out, "upload_id", upload.id);
        return ESP_OK;
    }
    if (!strcmp(op, "storage.chunk") || !strcmp(op, "storage.commit") ||
        !strcmp(op, "storage.abort")) {
        if (!upload.file || upload.id != (uint32_t)v3_int(p, "upload_id", 0))
            return ESP_ERR_INVALID_STATE;
        if (!strcmp(op, "storage.abort")) {
            fclose(upload.file);
            upload.file = NULL;
            psa_hash_abort(&upload.hash);
            unlink("/data/.upload");
            return ESP_OK;
        }
        if (!strcmp(op, "storage.chunk")) {
            if (!v3_integer(v3_get(p, "offset"), upload.used, upload.used))
                return ESP_ERR_INVALID_ARG;
            uint8_t bytes[768];
            size_t n;
            if (v3_payload(p, bytes, sizeof(bytes), &n) || n > upload.expected - upload.used)
                return ESP_ERR_INVALID_ARG;
            if (fwrite(bytes, 1, n, upload.file) != n)
                return ESP_FAIL;
            if (psa_hash_update(&upload.hash, bytes, n) != PSA_SUCCESS)
                return ESP_FAIL;
            upload.used += n;
            *out = cJSON_CreateObject();
            cJSON_AddNumberToObject(*out, "offset", upload.used);
            return ESP_OK;
        }
        if (upload.used != upload.expected)
            return ESP_ERR_INVALID_SIZE;
        uint8_t hash[32];
        char sha[65];
        size_t hash_size = 0;
        if (psa_hash_finish(&upload.hash, hash, sizeof(hash), &hash_size) != PSA_SUCCESS ||
            hash_size != 32)
            return ESP_FAIL;
        hash_hex(hash, sha);
        bool ok =
            !strcmp(sha, upload.sha) && fflush(upload.file) == 0 && fsync(fileno(upload.file)) == 0;
        if (fclose(upload.file))
            ok = false;
        upload.file = NULL;
        psa_hash_abort(&upload.hash);
        if (!ok) {
            unlink("/data/.upload");
            return ESP_ERR_INVALID_CRC;
        }
        char path[64];
        snprintf(path, sizeof(path), upload.secret ? "/data/.secret-%s" : "/data/%s", upload.name);
        return rename("/data/.upload", path) ? ESP_FAIL : ESP_OK;
    }
    const char *name = v3_str(p, "name", "");
    if (!v3_storage_name(name))
        return ESP_ERR_INVALID_ARG;
    char path[64];
    snprintf(path, sizeof(path),
             !strcmp(op, "storage.delete") && v3_bool(p, "secret", false) ? "/data/.secret-%s"
                                                                          : "/data/%s",
             name);
    if (!strcmp(op, "storage.delete"))
        return unlink(path) ? ESP_ERR_NOT_FOUND : ESP_OK;
    if (!strcmp(op, "storage.read")) {
        if (!v3_integer(v3_get(p, "offset"), 0, 262144) ||
            !v3_integer(v3_get(p, "length"), 0, V3_BUFFER_MAX))
            return ESP_ERR_INVALID_ARG;
        FILE *f = fopen(path, "rb");
        if (!f)
            return ESP_ERR_NOT_FOUND;
        int offset = v3_int(p, "offset", 0);
        if (fseek(f, offset, SEEK_SET)) {
            fclose(f);
            return ESP_FAIL;
        }
        uint8_t *bytes = malloc(V3_BUFFER_MAX);
        if (!bytes) {
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        size_t n = fread(bytes, 1, v3_int(p, "length", 0), f);
        bool bad = ferror(f);
        fclose(f);
        int err = bad ? ESP_FAIL : v3_binary_result(bytes, n, out);
        free(bytes);
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
static int ota_work(const char *op, const cJSON *p, cJSON **out) {
    if (!strcmp(op, "ota.status")) {
        *out = cJSON_CreateObject();
        const esp_partition_t *running = esp_ota_get_running_partition();
        cJSON_AddStringToObject(*out, "running", running ? running->label : "");
        cJSON_AddBoolToObject(*out, "uploading", ota.handle != 0);
        cJSON_AddNumberToObject(*out, "offset", ota.used);
        cJSON *slots = cJSON_AddArrayToObject(*out, "slots");
        for (int i = 0; i < 2; ++i) {
            const esp_partition_t *slot = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0 + i, NULL);
            if (!slot)
                continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", slot->label);
            cJSON_AddNumberToObject(item, "offset", slot->address);
            cJSON_AddNumberToObject(item, "size", slot->size);
            esp_ota_img_states_t state;
            if (esp_ota_get_state_partition(slot, &state) == ESP_OK)
                cJSON_AddNumberToObject(item, "state", state);
            v3_push(slots, item);
        }
        return ESP_OK;
    }
    if (!strcmp(op, "ota.begin")) {
        const char *profile = v3_str(p, "profile", ""), *sha = v3_str(p, "sha256", "");
        if (ota.handle)
            return ESP_ERR_INVALID_STATE;
        if (strcmp(v3_str(p, "board", ""), "luatos-esp32c3-usb") || v3_int(p, "protocol", 0) != 3 ||
            (strcmp(profile, "bus") && strcmp(profile, "signal") && strcmp(profile, "iot")) ||
            !sha_valid(sha) || !v3_integer(v3_get(p, "size"), sizeof(esp_image_header_t), 0x1b0000))
            return ESP_ERR_INVALID_ARG;
        ota.partition = esp_ota_get_next_update_partition(NULL);
        if (!ota.partition)
            return ESP_ERR_NOT_FOUND;
        ota.expected = v3_int(p, "size", 0);
        ota.used = 0;
        strcpy(ota.sha, sha);
        strcpy(ota.profile, profile);
        int err = esp_ota_begin(ota.partition, ota.expected, &ota.handle);
        if (err) {
            ota.handle = 0;
            return err;
        }
        ota.hash = psa_hash_operation_init();
        if (psa_hash_setup(&ota.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            esp_ota_abort(ota.handle);
            ota.handle = 0;
            return ESP_FAIL;
        }
        ota.id = ((uint32_t)esp_timer_get_time() & 0x7fffffff) | 1;
        *out = cJSON_CreateObject();
        v3_add_u32(*out, "upload_id", ota.id);
        return ESP_OK;
    }
    if (!ota.handle || ota.id != (uint32_t)v3_int(p, "upload_id", 0))
        return ESP_ERR_INVALID_STATE;
    if (!strcmp(op, "ota.abort")) {
        int err = esp_ota_abort(ota.handle);
        ota.handle = 0;
        psa_hash_abort(&ota.hash);
        return err;
    }
    if (!strcmp(op, "ota.chunk")) {
        if (!v3_integer(v3_get(p, "offset"), ota.used, ota.used))
            return ESP_ERR_INVALID_ARG;
        uint8_t bytes[768];
        size_t n;
        if (v3_payload(p, bytes, sizeof(bytes), &n) || !n || n > ota.expected - ota.used)
            return ESP_ERR_INVALID_ARG;
        if (!ota.used) {
            if (n < sizeof(esp_image_header_t))
                return ESP_ERR_INVALID_SIZE;
            const esp_image_header_t *header = (const void *)bytes;
            if (header->magic != ESP_IMAGE_HEADER_MAGIC || header->chip_id != ESP_CHIP_ID_ESP32C3)
                return ESP_ERR_INVALID_ARG;
        }
        int err = esp_ota_write(ota.handle, bytes, n);
        if (err)
            return err;
        if (psa_hash_update(&ota.hash, bytes, n) != PSA_SUCCESS)
            return ESP_FAIL;
        ota.used += n;
        *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(*out, "offset", ota.used);
        return ESP_OK;
    }
    if (!strcmp(op, "ota.commit")) {
        if (ota.used != ota.expected)
            return ESP_ERR_INVALID_SIZE;
        uint8_t hash[32];
        char sha[65];
        size_t hash_size = 0;
        if (psa_hash_finish(&ota.hash, hash, sizeof(hash), &hash_size) != PSA_SUCCESS ||
            hash_size != 32)
            return ESP_FAIL;
        psa_hash_abort(&ota.hash);
        hash_hex(hash, sha);
        if (strcmp(sha, ota.sha)) {
            esp_ota_abort(ota.handle);
            ota.handle = 0;
            return ESP_ERR_INVALID_CRC;
        }
        int err = esp_ota_end(ota.handle);
        ota.handle = 0;
        if (err)
            return err;
        esp_app_desc_t desc;
        if ((err = esp_ota_get_partition_description(ota.partition, &desc)))
            return err;
        // Profile/board identity is embedded in the app project name by the build.
        char expected[32];
        snprintf(expected, sizeof(expected), "limivia-%s-v3rpc", ota.profile);
        if (strcmp(desc.project_name, expected) || strncmp(desc.version, "0.3.", 4))
            return ESP_ERR_INVALID_ARG;
        if ((err = esp_ota_set_boot_partition(ota.partition)))
            return err;
        if (v3_bool(p, "reboot", false))
            reset_at = esp_timer_get_time() / 1000 + 1000;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
bool v3_flash_operation(const char *op, const cJSON *p) {
    return (!strncmp(op, "storage.", 8) && strcmp(op, "storage.status") &&
            strcmp(op, "storage.list") && strcmp(op, "storage.read")) ||
           (!strncmp(op, "ota.", 4) && strcmp(op, "ota.status")) ||
           (!strcmp(op, "wifi.configure") && v3_bool(p, "persist", false)) ||
           (!strcmp(op, "mqtt.configure") && v3_bool(p, "persist", false)) ||
           !strcmp(op, "power.sleep") || !strcmp(op, "system.nvs_format");
}
static int system_work(const v3_work *w, cJSON **out, bool *external) {
    (void)external;
    const char *op = w->action;
    const cJSON *p = w->params;
    if (!strcmp(op, "flow.load")) {
        const char *name = v3_str(p, "name", "");
        cJSON *definition = NULL;
        int err = v3_storage_load(name, &definition);
        if (err)
            return err;
        *out = cJSON_CreateObject();
        cJSON_AddStringToObject(*out, "name", name);
        v3_put(*out, "definition", definition);
        return ESP_OK;
    }
    if (!strncmp(op, "storage.", 8))
        return storage(op, p, out);
    if (!strncmp(op, "ota.", 4))
        return ota_work(op, p, out);
    if (!strcmp(op, "system.info")) {
        *out = v3_system_status();
        return ESP_OK;
    }
    if (!strcmp(op, "system.time")) {
        *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(*out, "monotonic_us", (double)esp_timer_get_time());
        return ESP_OK;
    }
    if (!strcmp(op, "system.nvs_format")) {
        if (strcmp(v3_str(p, "confirm", ""), "erase_nvs"))
            return ESP_ERR_INVALID_ARG;
        nvs_flash_deinit();
        int err = nvs_flash_erase();
        if (err)
            return err;
        v3_nvs_error = nvs_flash_init();
        return v3_nvs_error;
    }
    if (!strcmp(op, "system.restart")) {
        reset_at = esp_timer_get_time() / 1000 + 1000;
        return ESP_OK;
    }
    if (!strncmp(op, "wifi.", 5) || !strncmp(op, "mqtt.", 5) || !strncmp(op, "espnow.", 7))
        return v3_network_work(w, out, external);
    if (!strcmp(op, "power.configure")) {
        if (v3_get(p, "modem_sleep")) {
            int err = v3_network_modem_sleep(v3_str(p, "modem_sleep", ""));
            if (err)
                return err;
        }
        esp_pm_config_t pm = {.max_freq_mhz = 160,
                              .min_freq_mhz = 40,
                              .light_sleep_enable = v3_bool(p, "auto_light_sleep", false)};
        return esp_pm_configure(&pm);
    }
    if (!strcmp(op, "power.sleep")) {
        const char *mode = v3_str(p, "mode", "");
        if (strcmp(mode, "light") && strcmp(mode, "deep"))
            return ESP_ERR_INVALID_ARG;
        if (!v3_integer(v3_get(p, "duration_ms"), 1, 86400000))
            return ESP_ERR_INVALID_ARG;
        int pin = v3_int(p, "wake_pin", -1);
        if (v3_get(p, "wake_pin") &&
            (!v3_integer(v3_get(p, "wake_pin"), 0, 21) || !v3_pin_allowed(pin) ||
             (!strcmp(mode, "deep") && pin > 5) || !v3_integer(v3_get(p, "wake_level"), 0, 1)))
            return ESP_ERR_INVALID_ARG;
        if (v3_cancelled(w))
            return ESP_ERR_INVALID_STATE;
        // Allow the accepted-job reply to leave before USB/radio clocks disappear.
        // A congested transport may still lose that reply; hosts must never retry sleep writes.
        if (!v3_get(p, "checkpoint"))
            for (int i = 0; i < 20; ++i) {
                vTaskDelay(pdMS_TO_TICKS(5));
                if (v3_cancelled(w))
                    return ESP_ERR_INVALID_STATE;
            }
        if (v3_get(p, "checkpoint")) {
            if (atomic_load(&v3_inhibit_value))
                return ESP_ERR_INVALID_STATE;
            int err = inhibit_store(false);
            if (err)
                return err;
            if ((err = checkpoint_save(v3_get(p, "checkpoint"))))
                return err;
        } else if (v3_storage_ready())
            unlink("/data/.sleep");
        if (v3_cancelled(w)) {
            if (v3_storage_ready())
                unlink("/data/.sleep");
            return ESP_ERR_INVALID_STATE;
        }
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        int err = esp_sleep_enable_timer_wakeup((uint64_t)v3_int(p, "duration_ms", 0) * 1000);
        if (err)
            return err;
        if (v3_get(p, "wake_pin")) {
            if (!strcmp(mode, "deep")) {
                err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
                    1ULL << pin, v3_int(p, "wake_level", 0) ? ESP_GPIO_WAKEUP_GPIO_HIGH
                                                            : ESP_GPIO_WAKEUP_GPIO_LOW);
                if (err)
                    return err;
            } else {
                err = gpio_wakeup_enable(pin, v3_int(p, "wake_level", 0) ? GPIO_INTR_HIGH_LEVEL
                                                                         : GPIO_INTR_LOW_LEVEL);
                if (err)
                    return err;
                if ((err = esp_sleep_enable_gpio_wakeup()))
                    return err;
            }
        }
        if (!strcmp(mode, "deep")) {
            if (v3_cancelled(w)) {
                if (v3_storage_ready())
                    unlink("/data/.sleep");
                return ESP_ERR_INVALID_STATE;
            }
            esp_deep_sleep_start();
            return ESP_FAIL;
        }
        int64_t before = esp_timer_get_time();
        err = esp_light_sleep_start();
        if (!err) {
            *out = cJSON_CreateObject();
            cJSON_AddNumberToObject(*out, "elapsed_us", (double)(esp_timer_get_time() - before));
            v3_add_u32(*out, "wakeup_causes", esp_sleep_get_wakeup_causes());
        }
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
int v3_system_work(const v3_work *w, cJSON **out, bool *external) {
    bool flash = v3_flash_operation(w->action, w->params);
    if (flash && !v3_flash_begin())
        return ESP_ERR_INVALID_STATE;
    int err = system_work(w, out, external);
    if (flash)
        v3_flash_end();
    return err;
}
void v3_services_poll(void) {
    if (atomic_load(&v3_runtime_ready) && !atomic_load(&v3_boot_healthy) && v3_flash_begin()) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        int err = ESP_OK;
        if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY)
            err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK)
            atomic_store(&v3_boot_healthy, true);
        v3_flash_end();
    }
    if (atomic_load(&v3_inhibit_requested) && v3_flash_begin()) {
        atomic_store(&v3_inhibit_busy, true);
        if (atomic_exchange(&v3_inhibit_requested, false))
            atomic_store(&v3_inhibit_error, inhibit_store(atomic_load(&v3_inhibit_value)));
        atomic_store(&v3_inhibit_busy, false);
        v3_flash_end();
    }
    if (reset_at && esp_timer_get_time() / 1000 >= reset_at)
        esp_restart();
    v3_network_poll();
}
