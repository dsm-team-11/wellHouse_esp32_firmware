#include "evt_queue.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "evt_queue";
static const char *NS  = "evtq";

static nvs_handle_t     s_nvs;
static SemaphoreHandle_t s_lock;
static size_t           s_cap;
static uint32_t         s_head;   /* index of oldest entry */
static uint32_t         s_count;  /* number of stored entries */

static void slot_key(uint32_t idx, char *out, size_t out_len)
{
    snprintf(out, out_len, "e%03u", (unsigned)(idx % s_cap));
}

static void persist_meta(void)
{
    nvs_set_u32(s_nvs, "head", s_head);
    nvs_set_u32(s_nvs, "count", s_count);
    nvs_commit(s_nvs);
}

int evt_queue_init(size_t capacity)
{
    s_cap = capacity;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_open(NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (nvs_get_u32(s_nvs, "head", &s_head) != ESP_OK)  s_head = 0;
    if (nvs_get_u32(s_nvs, "count", &s_count) != ESP_OK) s_count = 0;
    if (s_count > s_cap) {
        s_count = s_cap;
    }

    ESP_LOGI(TAG, "queue ready (cap=%u, pending=%u)", (unsigned)s_cap, (unsigned)s_count);
    return ESP_OK;
}

size_t evt_queue_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

int evt_queue_push(const char *json)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Full: drop the oldest to make room. */
    if (s_count == s_cap) {
        s_head = (s_head + 1) % s_cap;
        s_count--;
        ESP_LOGW(TAG, "queue full, dropped oldest event");
    }

    uint32_t tail = (s_head + s_count) % s_cap;
    char key[8];
    slot_key(tail, key, sizeof(key));

    esp_err_t err = nvs_set_str(s_nvs, key, json);
    if (err == ESP_OK) {
        s_count++;
        persist_meta();
    } else {
        ESP_LOGE(TAG, "store failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_lock);
    return err;
}

int evt_queue_peek_oldest(char *buf, size_t buf_len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    char key[8];
    slot_key(s_head, key, sizeof(key));
    size_t len = buf_len;
    esp_err_t err = nvs_get_str(s_nvs, key, buf, &len);

    xSemaphoreGive(s_lock);
    return err;
}

int evt_queue_pop_oldest(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    char key[8];
    slot_key(s_head, key, sizeof(key));
    nvs_erase_key(s_nvs, key);
    s_head = (s_head + 1) % s_cap;
    s_count--;
    persist_meta();

    xSemaphoreGive(s_lock);
    return ESP_OK;
}
