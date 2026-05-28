#include "eva_cdc.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"

extern void chip_usb_set_persist_flags(uint32_t flags);
#define USBDC_PERSIST_ENA (1U << 31)
#define EVA_CDC_MAX_HANDLERS 32
#define EVA_CDC_BOOTLOADER_EDGE_US 250000ULL

static const char *TAG = "eva_cdc";
static eva_cdc_t *s_active_cdc;

struct eva_cdc_s {
    QueueHandle_t rx_queue;
    eva_cdc_line_fn line_fn;
    void *line_user;
    struct {
        const char *cmd;
        eva_cdc_handler_fn fn;
        void *user;
    } handlers[EVA_CDC_MAX_HANDLERS];
    size_t handler_count;
    uint64_t line_opened_at_us;
    bool rx_since_open;
    volatile bool prev_rts_state;
    volatile bool usb_console_host_open;
};

static char *trim_args(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

void eva_cdc_send(eva_cdc_t *self, const char *line)
{
    (void)self;
    if (!line) return;
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, strlen(line));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
}

void eva_cdc_sendf(eva_cdc_t *self, const char *fmt, ...)
{
    if (!fmt) return;

    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    eva_cdc_send(self, buf);
}

void eva_cdc_send_binary(eva_cdc_t *self, const uint8_t *data, size_t size)
{
    (void)self;
    if (!data || size == 0) return;

    const size_t chunk = 1024;
    size_t off = 0;
    while (off < size) {
        size_t n = (size - off > chunk) ? chunk : (size - off);
        size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + off, n);
        if (written == 0) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
            continue;
        }
        off += written;
        if ((off & 0x1fff) == 0) {
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
        }
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(200));
}

static void cdc_command_task(void *arg)
{
    eva_cdc_t *self = (eva_cdc_t *)arg;
    char line[192];
    size_t len = 0;

    while (true) {
        uint8_t ch;
        if (!self || xQueueReceive(self->rx_queue, &ch, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = '\0';
                if (self->line_fn) {
                    self->line_fn(self->line_user, line);
                }
                len = 0;
            }
            continue;
        }
        if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        } else {
            len = 0;
            eva_cdc_send(self, "ERR command too long\r\n");
        }
    }
}

bool eva_cdc_register(eva_cdc_t *self, const char *cmd, eva_cdc_handler_fn fn, void *user)
{
    if (!self || !cmd || !cmd[0] || !fn) return false;
    if (self->handler_count >= EVA_CDC_MAX_HANDLERS) {
        ESP_LOGW(TAG, "handler table full for %s", cmd);
        return false;
    }

    self->handlers[self->handler_count].cmd = cmd;
    self->handlers[self->handler_count].fn = fn;
    self->handlers[self->handler_count].user = user;
    self->handler_count++;
    return true;
}

bool eva_cdc_dispatch(eva_cdc_t *self, char *line)
{
    if (!self || !line) return false;

    char *cmd = trim_args(line);
    if (!cmd[0]) return true;

    for (size_t i = 0; i < self->handler_count; ++i) {
        const char *registered = self->handlers[i].cmd;
        size_t n = strlen(registered);
        if (strncmp(cmd, registered, n) != 0) {
            continue;
        }
        if (cmd[n] != '\0' && cmd[n] != ' ' && cmd[n] != '\t') {
            continue;
        }

        char *args = trim_args(cmd + n);
        self->handlers[i].fn(self->handlers[i].user, args);
        return true;
    }
    return false;
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    eva_cdc_t *self = s_active_cdc;
    if (!self || !self->rx_queue) return;

    uint8_t buf[64];
    size_t rx_size = 0;
    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_size) != ESP_OK) {
        return;
    }
    if (rx_size > 0) {
        self->rx_since_open = true;
    }
    for (size_t i = 0; i < rx_size; ++i) {
        (void)xQueueSend(self->rx_queue, &buf[i], 0);
    }
}

static void cdc_line_state_changed(int itf, cdcacm_event_t *event)
{
    (void)itf;
    eva_cdc_t *self = s_active_cdc;
    if (!self || !event) return;

    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    bool was_open = self->usb_console_host_open;
    bool is_open = dtr && rts;
    uint64_t now_us = (uint64_t)esp_timer_get_time();

    /* Host is considered "open" only when BOTH DTR and RTS are asserted —
     * matches the legacy main.c behaviour and avoids false-positive
     * activations from spurious DTR-only USB enumerations. */
    self->usb_console_host_open = is_open;
    if (is_open && !was_open) {
        self->line_opened_at_us = now_us;
        self->rx_since_open = false;
    }

    /* Reboot only for a deliberate bootloader gesture: DTR+RTS asserted,
     * no command bytes received, then RTS drops quickly while DTR stays high.
     * pyserial may close by dropping RTS after a command such as screenshot;
     * rx_since_open keeps that ordinary command session from looking like a
     * bootloader request. */
    bool rts_falling_with_dtr = self->prev_rts_state && !rts && dtr;
    bool recent_open = self->line_opened_at_us > 0 &&
        (now_us - self->line_opened_at_us) <= EVA_CDC_BOOTLOADER_EDGE_US;
    if (rts_falling_with_dtr && !self->rx_since_open && recent_open) {
        ESP_LOGW(TAG, "USB RTS falling edge: rebooting to keep CDC alive");
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, 0x4);
        chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
        esp_restart();
    } else if (rts_falling_with_dtr) {
        ESP_LOGD(TAG, "ignored RTS falling edge dtr=%d rx=%d recent=%d",
                 dtr, self->rx_since_open, recent_open);
    }
    self->prev_rts_state = rts;
}

eva_cdc_t *eva_cdc_create(eva_cdc_line_fn line_fn, void *user)
{
    eva_cdc_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    self->line_fn = line_fn;
    self->line_user = user;
    self->rx_queue = xQueueCreate(512, sizeof(uint8_t));
    if (self->rx_queue) {
        xTaskCreate(cdc_command_task, "cdc_cmd", 8192, self, 5, NULL);
    }
    s_active_cdc = self;

    tinyusb_config_t tusb_cfg = { 0 };
    if (tinyusb_driver_install(&tusb_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB driver install failed");
        return self;
    }

    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_changed,
        .callback_line_coding_changed = NULL,
    };
    if (tusb_cdc_acm_init(&acm_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "TinyUSB CDC ACM init failed");
        return self;
    }

    esp_tusb_init_console(TINYUSB_CDC_ACM_0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    (void)esp_log_set_vprintf((vprintf_like_t)vprintf);
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "USB debug console ready");
    return self;
}

void eva_cdc_destroy(eva_cdc_t *self)
{
    if (!self) return;
    if (s_active_cdc == self) {
        s_active_cdc = NULL;
    }
    if (self->rx_queue) {
        vQueueDelete(self->rx_queue);
    }
    free(self);
}

bool eva_cdc_host_open(const eva_cdc_t *self)
{
    return self ? self->usb_console_host_open : false;
}
