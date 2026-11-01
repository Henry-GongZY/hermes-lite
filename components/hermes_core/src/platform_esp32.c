#include "hermes_lite.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"

/* ESP-IDF backend: esp_http_client (esp-tls with the built-in certificate
 * bundle) + VFS filesystem (SPIFFS is mounted at /spiffs by the app). */

typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
} rx_t;

static esp_err_t evh(esp_http_client_event_t *evt)
{
    rx_t *rx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rx && evt->data_len > 0) {
        size_t n = (size_t)evt->data_len;
        size_t room = rx->cap > rx->len ? rx->cap - rx->len : 0;
        if (n > room) {
            n = room;
            rx->overflow = 1;
        }
        memcpy(rx->buf + rx->len, evt->data, n);
        rx->len += n;
    }
    return ESP_OK;
}

static int http_post(const char *url, const char *auth_bearer,
                     const char *body, size_t body_len,
                     char *resp, size_t resp_cap, size_t *resp_len)
{
    rx_t rx = { resp, resp_cap, 0, 0 };
    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = evh,
        .user_data = &rx,
        .timeout_ms = 90000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
#if CONFIG_HL_TLS_INSECURE
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h)
        return -1;

    static char auth[192];
    snprintf(auth, sizeof auth, "Bearer %s", auth_bearer);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_post_field(h, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    if (err != ESP_OK) {
        printf("[hl:E] http transport: %s\n", esp_err_to_name(err));
        return -1;
    }
    if (rx.overflow)
        return -2;
    *resp_len = rx.len;
    return status;
}

/* ---- SSE streaming (line-buffered dispatch off esp_http_client events) ---- */

#define STREAM_LINE_CAP 8192

typedef struct {
    char line[STREAM_LINE_CAP];
    size_t len;
    int status;
    int aborted;
    int overflow;
    hl_line_fn on_line;
    void *ud;
    char *errbuf;
    size_t errcap, errlen;
} esctx_t;

static void sctx_dispatch(esctx_t *s)
{
    s->line[s->len] = 0;
    if (s->len > 0 && s->line[s->len - 1] == '\r')
        s->line[s->len - 1] = 0;
    s->len = 0;
    if (strncmp(s->line, "data:", 5) != 0)
        return;
    const char *payload = s->line + 5;
    if (*payload == ' ')
        payload++;
    if (!*payload)
        return;
    if (s->on_line(s->ud, payload))
        s->aborted = 1;
}

static void sctx_feed(esctx_t *s, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            sctx_dispatch(s);
            continue;
        }
        if (s->len + 1 >= STREAM_LINE_CAP) {
            s->overflow = 1;
            return;
        }
        s->line[s->len++] = ch;
    }
}

static esp_err_t stream_evh(esp_http_client_event_t *evt)
{
    esctx_t *s = evt->user_data;
    if (!s)
        return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        /* status line is always parsed before the first body byte */
        if (s->status == 0)
            s->status = esp_http_client_get_status_code(evt->client);
        if (s->status != 200) {
            size_t room = s->errcap > s->errlen ? s->errcap - s->errlen : 0;
            size_t n = (size_t)evt->data_len > room ? room : (size_t)evt->data_len;
            memcpy(s->errbuf + s->errlen, evt->data, n);
            s->errlen += n;
        } else {
            sctx_feed(s, evt->data, evt->data_len);
        }
    }
    return ESP_OK;
}

static int http_post_stream(const char *url, const char *auth_bearer,
                            const char *body, size_t body_len,
                            hl_line_fn on_line, void *ud,
                            char *errbuf, size_t errcap)
{
    esctx_t s = { 0 };
    s.on_line = on_line;
    s.ud = ud;
    s.errbuf = errbuf;
    s.errcap = errcap;

    esp_http_client_config_t hc = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = stream_evh,
        .user_data = &s,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
#if CONFIG_HL_TLS_INSECURE
        .crt_bundle_attach = NULL,
        .skip_cert_common_name_check = true,
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h)
        return -1;

    static char auth[192];
    snprintf(auth, sizeof auth, "Bearer %s", auth_bearer);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Accept", "text/event-stream");
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_post_field(h, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(h);
    int status = s.status ? s.status : esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    if (s.overflow)
        return -2;
    if (s.aborted)
        return -3;
    if (err != ESP_OK && status == 0) {
        printf("[hl:E] http stream transport: %s\n", esp_err_to_name(err));
        return -1;
    }
    return status;
}

static int fs_read(const char *path, char *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static int fs_write(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static int fs_append(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "ab");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

typedef struct {
    int (*cb)(void *, const char *);
    void *ud;
} list_t;

static int fs_list(const char *dir, int (*cb)(void *, const char *), void *ud)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)))
        if (cb(ud, e->d_name))
            break;
    closedir(d);
    return 0;
}

static void delay_ms(unsigned long ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static unsigned long time_ms(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000LL);
}

static void log_fn(int level, const char *msg)
{
    printf("[hl:%c] %s\n", level == HL_LOG_ERROR ? 'E' : level == HL_LOG_DEBUG ? 'D' : 'I',
           msg);
}

static const hl_platform_t g_platform = {
    .http_post = http_post,
    .http_post_stream = http_post_stream,
    .fs_read = fs_read,
    .fs_write = fs_write,
    .fs_append = fs_append,
    .fs_list = fs_list,
    .time_ms = time_ms,
    .delay_ms = delay_ms,
    .log = log_fn,
};

const hl_platform_t *hl_platform_esp32(void)
{
    return &g_platform;
}
