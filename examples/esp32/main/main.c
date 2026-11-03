#include "hermes_lite.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "sdkconfig.h"
#include "json.h"

/* Hermes-Lite on the AI-Thinker ESP32-S (plain ESP32, 4MB flash).
 *
 * Serial commands:
 *   wifi <ssid> <password>   provision + connect (stored in NVS)
 *   skills                   list skill files on SPIFFS
 *   sessions                 list session history files
 *   session <name>           switch to another session (restarts the agent)
 *   reset                    wipe the CURRENT session and restart the agent
 *   free                     show heap usage
 *   anything else            goes to the agent */

#define SPIFFS_BASE "/spiffs"
#define SESSIONS_DIR SPIFFS_BASE "/sessions"
#define DEFAULT_SESSION "default"
#define SKILLS_DIR SPIFFS_BASE "/skills"

static char s_reply[4096];

/* ---------------- sessions ---------------- */

/* Sessions are per-name JSONL history files under /spiffs/sessions/. */

/* Resolved path must live in static storage: hl_config_t.history_path is a
 * borrowed string that must outlive the agent, and the REPL's `line` buffer
 * is reused every command (same pattern as s_key/s_url below). */
static char s_session_path[160];

static const char *session_path(const char *name)
{
    snprintf(s_session_path, sizeof s_session_path, "%s/%s.jsonl", SESSIONS_DIR, name);
    return s_session_path;
}

/* Session names must be plain single path components (no separators,
 * no traversal, not hidden). */
static int valid_session_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.')
        return 0;
    if (!strcmp(name, ".."))
        return 0;
    if (strchr(name, '/') || strchr(name, '\\'))
        return 0;
    return 1;
}

static int s_n_sessions;

static int sessions_file_cb(void *ud, const char *name)
{
    (void)ud;
    if (name[0] != '.') {
        printf("  %s\n", name);
        s_n_sessions++;
    }
    return 0;
}

/* ---------------- wifi ---------------- */

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
static int s_wifi_started;
static int s_disc_count; /* consecutive disconnects -> backoff */
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* exponential backoff: 1s, 2s, 4s ... capped at 30s */
static void schedule_reconnect(void)
{
    s_disc_count++;
    long delay_ms = 1000L << (s_disc_count > 5 ? 5 : s_disc_count);
    if (delay_ms > 30000L)
        delay_ms = 30000L;
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t targs = {
            .callback = reconnect_cb,
            .name = "wifi_reconnect",
        };
        esp_timer_create(&targs, &s_reconnect_timer);
    }
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    printf("[app] wifi down; reconnect in %lds\n", delay_ms / 1000);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        schedule_reconnect(); /* agent fails fast offline; we keep retrying */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_disc_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static int wifi_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_connect();
    } else {
        esp_wifi_start();
        s_wifi_started = 1;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? 0 : -1;
}

static int wifi_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("hl", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

static int wifi_load(char *ssid, size_t scap, char *pass, size_t pcap)
{
    nvs_handle_t h;
    if (nvs_open("hl", NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t l1 = scap, l2 = pcap;
    esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &l1);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &l2);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK) ? 0 : -1;
}

/* runtime-configurable LLM settings (NVS, namespace "hl") */
static int cfg_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("hl", NVS_READWRITE, &h) != ESP_OK)
        return -1;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
    return 0;
}

static int cfg_load_str(const char *key, char *buf, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open("hl", NVS_READONLY, &h) != ESP_OK)
        return -1;
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static void mask_secret(const char *s, char *out, size_t cap)
{
    if (strlen(s) <= 10)
        snprintf(out, cap, "***");
    else
        snprintf(out, cap, "%.10s***", s);
}

/* ---------------- tools ---------------- */

static jval_t *jnum(jval_t *v, const char *k)
{
    jval_t *x = hl_jobj_get(v, k);
    return (x && x->type == J_NUM) ? x : NULL;
}

static int tool_gpio_write(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *pin = jnum(v, "pin");
    jval_t *lvl = jnum(v, "level");
    if (!pin || !lvl) {
        snprintf(out, cap, "error: need pin (int) and level (0/1)");
        return 1;
    }
    int p = (int)pin->num;
    if (p < 0 || p > 39 || p == 6 || (p >= 11 && p <= 17)) {
        snprintf(out, cap, "error: pin %d not usable (flash/PSRAM pins)", p);
        return 1;
    }
    gpio_reset_pin(p);
    gpio_set_direction(p, GPIO_MODE_OUTPUT);
    gpio_set_level(p, lvl->num != 0);
    snprintf(out, cap, "ok: GPIO%d = %d", p, lvl->num != 0);
    return 0;
}

static int tool_gpio_read(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *pin = jnum(v, "pin");
    if (!pin) {
        snprintf(out, cap, "error: need pin (int)");
        return 1;
    }
    int p = (int)pin->num;
    if (p < 0 || p > 39 || p == 6 || (p >= 11 && p <= 17)) {
        snprintf(out, cap, "error: pin %d not usable", p);
        return 1;
    }
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << p,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&gc);
    snprintf(out, cap, "GPIO%d reads %d", p, gpio_get_level(p));
    return 0;
}

static int tool_adc_read(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    static int width_set;
    if (!width_set) {
        adc1_config_width(ADC_WIDTH_BIT_12);
        width_set = 1;
    }
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *ch = jnum(v, "channel");
    if (!ch || ch->num < 0 || ch->num > 7) {
        snprintf(out, cap, "error: need channel 0-7 (ADC1, GPIO32-39)");
        return 1;
    }
    adc1_channel_t chan = (adc1_channel_t)(ADC1_CHANNEL_0 + (int)ch->num);
    adc1_config_channel_atten(chan, ADC_ATTEN_DB_11);
    int raw = adc1_get_raw(chan);
    /* ~11dB atten: 150mV..2450mV range over 0..4095, rough linear estimate */
    int mv = raw * 2450 / 4095;
    snprintf(out, cap, "ADC1 ch%d raw=%d (~%d mV)", (int)ch->num, raw, mv);
    return 0;
}

static int tool_pwm_write(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *pin = jnum(v, "pin");
    jval_t *duty = jnum(v, "duty");
    if (!pin || !duty) {
        snprintf(out, cap, "error: need pin (int) and duty (0-1023)");
        return 1;
    }
    int p = (int)pin->num;
    if (p < 0 || p > 39 || p == 6 || (p >= 11 && p <= 17)) {
        snprintf(out, cap, "error: pin %d not usable (flash/PSRAM pins)", p);
        return 1;
    }
    int d = (int)duty->num;
    if (d < 0 || d > 1023) {
        snprintf(out, cap, "error: duty must be 0-1023");
        return 1;
    }
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {
        .gpio_num = p,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = d,
        .hpoint = 0,
    };
    ledc_channel_config(&cc);
    snprintf(out, cap, "ok: GPIO%d PWM duty %d/1023 (5 kHz, LEDC ch0)", p, d);
    return 0;
}

static int tool_i2c_scan(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    (void)args;
    /* lazy one-time bus init on the common devkit pins (SDA=21, SCL=22) */
    static int bus_ready;
    if (!bus_ready) {
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = 21,
            .scl_io_num = 22,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master.clk_speed = 100000,
        };
        if (i2c_param_config(I2C_NUM_0, &conf) != ESP_OK ||
            i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
            snprintf(out, cap, "error: i2c init failed (SDA=21, SCL=22)");
            return 1;
        }
        bus_ready = 1;
    }
    int n_found = 0, off = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        /* 1-byte probe: an ACK of the address means a device responded */
        const uint8_t dummy = 0x00;
        esp_err_t r = i2c_master_write_to_device(I2C_NUM_0, addr, &dummy, 1,
                                                 pdMS_TO_TICKS(100));
        if (r == ESP_OK) {
            if (n_found == 0)
                off += snprintf(out + off, cap - off,
                                "found device(s) on SDA=21/SCL=22: ");
            else
                off += snprintf(out + off, cap - off, ", ");
            off += snprintf(out + off, cap - off, "0x%02X", addr);
            n_found++;
        }
    }
    if (n_found == 0)
        snprintf(out, cap, "no I2C devices found (SDA=21, SCL=22)");
    return 0;
}

/* file_read/file_write paths must stay inside SPIFFS */
static int spiffs_path_ok(const char *path, char *out, size_t cap)
{
    if (strncmp(path, SPIFFS_BASE "/", strlen(SPIFFS_BASE "/")) != 0) {
        snprintf(out, cap, "error: path must be under /spiffs/");
        return 0;
    }
    for (const char *p = path; (p = strstr(p, "..")) != NULL; p += 2) {
        if ((p == path || p[-1] == '/') && (p[2] == '/' || p[2] == 0)) {
            snprintf(out, cap, "error: path must not contain '..'");
            return 0;
        }
    }
    return 1;
}

#define FILE_TOOL_CAP 2048
/* large scratch lives in static storage, not on the task stack */
static char s_file_buf[FILE_TOOL_CAP + 1]; /* file_read contents */
static char s_file_arena[4096];            /* file_write JSON parse arena */

static int tool_file_read(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    char arena[512];
    jval_t *v = hl_json_parse(arena, sizeof arena, args && *args ? args : "{}", NULL);
    jval_t *path = hl_jobj_get(v, "path");
    if (!path || path->type != J_STR || !path->str[0]) {
        snprintf(out, cap, "error: need path (string)");
        return 1;
    }
    if (!spiffs_path_ok(path->str, out, cap))
        return 1;
    size_t len = 0;
    if (hl_platform_esp32()->fs_read(path->str, s_file_buf, FILE_TOOL_CAP, &len) != 0) {
        snprintf(out, cap, "error: cannot read %s", path->str);
        return 1;
    }
    s_file_buf[len] = 0;
    if (len == 0)
        snprintf(out, cap, "%s: (empty)", path->str);
    else
        snprintf(out, cap, "%s", s_file_buf);
    return 0;
}

static int tool_file_write(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    jval_t *v = hl_json_parse(s_file_arena, sizeof s_file_arena,
                              args && *args ? args : "{}", NULL);
    jval_t *path = v ? hl_jobj_get(v, "path") : NULL;
    jval_t *content = v ? hl_jobj_get(v, "content") : NULL;
    if (!path || path->type != J_STR || !path->str[0] ||
        !content || content->type != J_STR) {
        snprintf(out, cap, "error: need path (string) and content (string, max 2048)");
        return 1;
    }
    if (!spiffs_path_ok(path->str, out, cap))
        return 1;
    size_t clen = strlen(content->str);
    if (clen > FILE_TOOL_CAP) {
        snprintf(out, cap, "error: content too large (max 2048 bytes)");
        return 1;
    }
    if (hl_platform_esp32()->fs_write(path->str, content->str, clen) != 0) {
        snprintf(out, cap, "error: cannot write %s", path->str);
        return 1;
    }
    snprintf(out, cap, "ok: wrote %u bytes to %s", (unsigned)clen, path->str);
    return 0;
}

static int tool_sys_info(void *ud, const char *args, char *out, size_t cap)
{
    (void)ud;
    (void)args;
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    long up = (long)(esp_timer_get_time() / 1000000LL);
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        rssi = ap.rssi;
    snprintf(out, cap,
             "free_heap=%lu min_free=%lu uptime=%lds idf=%s wifi_rssi=%d",
             (unsigned long)free_heap, (unsigned long)min_heap, up,
             esp_get_idf_version(), rssi);
    return 0;
}

/* ---------------- app ---------------- */

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t r = esp_vfs_spiffs_register(&conf);
    printf("[app] spiffs: %s\n", r == ESP_OK ? "mounted" : esp_err_to_name(r));
}

static void on_fragment(void *ud, const char *frag)
{
    (void)ud;
    fputs(frag, stdout);
    fflush(stdout);
}

/* OTA skill sync: GET <base>/index.json -> {"skills":[{"name","file"}]}
 * then GET <base>/<file> for each entry, saved as <skills_dir>/<name>.md */
static void do_sync(const hl_platform_t *plat, const char *base, const char *skills_dir)
{
    if (!plat->http_get) {
        printf("[app] sync: http_get not supported by this platform\n");
        return;
    }
    static char idx_buf[4096];
    static char url[512];
    size_t blen = strlen(base);
    snprintf(url, sizeof url, "%s%sindex.json", base, blen && base[blen - 1] == '/' ? "" : "/");
    size_t len = 0;
    int st = plat->http_get(url, "", idx_buf, sizeof idx_buf - 1, &len);
    if (st != 200) {
        printf("[app] sync: index fetch failed (http %d)\n", st);
        return;
    }
    idx_buf[len] = 0;

    char arena[2048];
    jval_t *root = hl_json_parse(arena, sizeof arena, idx_buf, NULL);
    jval_t *skills = root ? hl_jobj_get(root, "skills") : NULL;
    if (!skills || skills->type != J_ARR) {
        printf("[app] sync: bad index.json\n");
        return;
    }
    int n = hl_jarr_len(skills), ok = 0;
    for (int i = 0; i < n; i++) {
        jval_t *e = hl_jarr_get(skills, i);
        jval_t *nm = hl_jobj_get(e, "name");
        jval_t *fl = hl_jobj_get(e, "file");
        if (!nm || !fl || nm->type != J_STR || fl->type != J_STR ||
            !nm->str[0] || strchr(nm->str, '/') || nm->str[0] == '.')
            continue;
        snprintf(url, sizeof url, "%s%s%s", base,
                 blen && base[blen - 1] == '/' ? "" : "/", fl->str);
        len = 0;
        st = plat->http_get(url, "", idx_buf, sizeof idx_buf - 1, &len);
        if (st != 200 || len == 0) {
            printf("[app] sync: %s failed (http %d)\n", nm->str, st);
            continue;
        }
        idx_buf[len] = 0;
        char path[300];
        snprintf(path, sizeof path, "%s/%s.md", skills_dir, nm->str);
        if (plat->fs_write(path, idx_buf, len) == 0) {
            printf("[app] synced skill '%s' (%zu bytes)\n", nm->str, len);
            ok++;
        }
    }
    printf("[app] sync done: %d/%d skills\n", ok, n);
}

static int skills_file_cb(void *ud, const char *name)
{
    if (name[0] != '.')
        printf("  %s\n", name);
    return 0;
}

void app_main(void)
{
    printf("\n=== Hermes-Lite ESP32 (AI-Thinker ESP32-S) ===\n");

    esp_err_t nvs_r = nvs_flash_init();
    if (nvs_r == ESP_ERR_NVS_NO_FREE_PAGES || nvs_r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    mount_spiffs();

    s_wifi_events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wicfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);

    char ssid[65] = "", pass[65] = "";
    if (wifi_load(ssid, sizeof ssid, pass, sizeof pass) == 0) {
        printf("[app] connecting to '%s'...\n", ssid);
        if (wifi_connect(ssid, pass, 15000) == 0)
            printf("[app] wifi connected\n");
        else
            printf("[app] wifi connect failed; use 'wifi <ssid> <pass>'\n");
    } else {
        printf("[app] no wifi credentials; send 'wifi <ssid> <pass>' over serial\n");
    }

    hl_config_t cfg = {
        .api_url = CONFIG_HL_API_URL,
        .api_key = CONFIG_HL_API_KEY,
        .model = CONFIG_HL_MODEL,
        .system_prompt =
            "You are Hermes Lite running on an ESP32 microcontroller. "
            "Answer briefly in the user's language. Use tools for hardware state.",
        .skills_dir = SKILLS_DIR,
        /* sessions/<name>.jsonl; the pointer is borrowed by the agent and
         * must stay valid for the agent's lifetime (static s_session_path) */
        .history_path = session_path(DEFAULT_SESSION),
        .max_ctx_chars = 8000,
        .buf_req = 24576,
        .buf_resp = 24576,
        .buf_arena = 24576,
        .buf_hist = 8192,
        .buf_skill = 8192,
    };

    /* runtime overrides (serial-provisioned, persisted in NVS) */
    {
        static char ovr[192];
        if (cfg_load_str("apikey", ovr, sizeof ovr) == 0 && ovr[0])
            cfg.api_key = ovr;
        static char ovr_url[160];
        if (cfg_load_str("apiurl", ovr_url, sizeof ovr_url) == 0 && ovr_url[0])
            cfg.api_url = ovr_url;
        static char ovr_model[64];
        if (cfg_load_str("model", ovr_model, sizeof ovr_model) == 0 && ovr_model[0])
            cfg.model = ovr_model;
    }

    hl_tool_t tools[] = {
        { .name = "gpio_write",
          .description = "Set an output GPIO level. Args: pin (0-39, not 6/11-17), level (0/1).",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},"
              "\"level\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"level\"]}",
          .fn = tool_gpio_write },
        { .name = "gpio_read",
          .description = "Read a GPIO as input (pull-up enabled). Args: pin.",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"}},"
              "\"required\":[\"pin\"]}",
          .fn = tool_gpio_read },
        { .name = "adc_read",
          .description = "Read ADC1 channel 0-7 (GPIO32-39), 12-bit with 11dB atten.",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"channel\":{\"type\":\"integer\"}},"
              "\"required\":[\"channel\"]}",
          .fn = tool_adc_read },
        { .name = "sys_info",
          .description = "Device status: free heap, minimum heap, uptime, IDF version, wifi RSSI.",
          .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
          .fn = tool_sys_info },
        { .name = "pwm_write",
          .description = "Write a PWM duty cycle to a pin via LEDC timer0/channel0 "
                         "(5 kHz, 10-bit). Args: pin (0-39, not 6/11-17), duty (0-1023).",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},"
              "\"duty\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"duty\"]}",
          .fn = tool_pwm_write },
        { .name = "i2c_scan",
          .description = "Scan the I2C bus (SDA=21, SCL=22) and list found device addresses.",
          .parameters_json = "{\"type\":\"object\",\"properties\":{}}",
          .fn = tool_i2c_scan },
        { .name = "file_read",
          .description = "Read a text file from SPIFFS (max 2 KB). Args: path (under /spiffs/).",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
              "\"required\":[\"path\"]}",
          .fn = tool_file_read },
        { .name = "file_write",
          .description = "Write text to a file on SPIFFS (max 2 KB, overwrites). "
                         "Args: path (under /spiffs/), content.",
          .parameters_json =
              "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
              "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
          .fn = tool_file_write },
    };

    hl_agent_t *agent = hl_agent_new(&cfg, hl_platform_esp32(), tools, 8);
    if (!agent) {
        printf("[app] agent init failed (heap too low?) free=%lu\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }

    printf("commands: 'wifi <ssid> <pass>' | 'key <api-key>' | 'url <endpoint>' | "
           "'model <name>' | 'skills' | 'sessions' | 'session <name>' | "
           "'reset' | 'free'\n");
    char line[512];
    for (;;) {
        printf("\nhermes> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;

        if (!strncmp(line, "wifi ", 5)) {
            char *sp = strchr(line + 5, ' ');
            if (!sp) {
                printf("usage: wifi <ssid> <pass>\n");
                continue;
            }
            *sp = 0;
            const char *ssid_in = line + 5, *pass_in = sp + 1;
            wifi_save(ssid_in, pass_in);
            printf("[app] connecting to '%s'...\n", ssid_in);
            printf("[app] %s\n",
                   wifi_connect(ssid_in, pass_in, 15000) == 0 ? "wifi connected"
                                                              : "connect failed");
        } else if (!strncmp(line, "key ", 4)) {
            static char s_key[512];
            const char *key = line + 4;
            while (*key == ' ')
                key++;
            cfg_save_str("apikey", key);
            snprintf(s_key, sizeof s_key, "%s", key);
            static char masked[32];
            mask_secret(key, masked, sizeof masked);
            cfg.api_key = s_key;
            printf("[app] api key set (%s); 'reset' to apply\n", masked);
        } else if (!strncmp(line, "url ", 4)) {
            static char s_url[512];
            const char *u = line + 4;
            while (*u == ' ')
                u++;
            cfg_save_str("apiurl", u);
            snprintf(s_url, sizeof s_url, "%s", u);
            cfg.api_url = s_url;
            printf("[app] endpoint set: %s; 'reset' to apply\n", u);
        } else if (!strncmp(line, "model ", 6)) {
            static char s_model[512];
            const char *m = line + 6;
            while (*m == ' ')
                m++;
            cfg_save_str("model", m);
            snprintf(s_model, sizeof s_model, "%s", m);
            cfg.model = s_model;
            printf("[app] model set: %s; 'reset' to apply\n", m);
        } else if (!strncmp(line, "sync", 4) &&
                   (line[4] == ' ' || line[4] == '\0')) {
            static char s_sync_url[512];
            if (line[4] == ' ') {
                const char *u = line + 5;
                while (*u == ' ')
                    u++;
                if (*u) {
                    cfg_save_str("syncurl", u);
                    snprintf(s_sync_url, sizeof s_sync_url, "%s", u);
                }
            } else if (cfg_load_str("syncurl", s_sync_url, sizeof s_sync_url) != 0 ||
                       !s_sync_url[0]) {
                printf("[app] no sync url saved; use 'sync <base-url>'\n");
                continue;
            }
            do_sync(hl_platform_esp32(), s_sync_url, SKILLS_DIR);
        } else if (!strcmp(line, "skills")) {
            printf("skills in " SKILLS_DIR ":\n");
            hl_platform_esp32()->fs_list(SKILLS_DIR, skills_file_cb, NULL);
        } else if (!strcmp(line, "sessions")) {
            printf("sessions in " SESSIONS_DIR ":\n");
            s_n_sessions = 0;
            hl_platform_esp32()->fs_list(SESSIONS_DIR, sessions_file_cb, NULL);
            if (!s_n_sessions)
                printf("  (none yet)\n");
        } else if (!strcmp(line, "session")) {
            printf("usage: session <name>\n");
        } else if (!strncmp(line, "session ", 8)) {
            const char *name = line + 8;
            while (*name == ' ')
                name++;
            if (!valid_session_name(name)) {
                printf("usage: session <name> (plain name, no '/', '..' or leading '.')\n");
            } else {
                hl_agent_free(agent);
                /* s_session_path (static) keeps the borrowed string alive */
                cfg.history_path = session_path(name);
                agent = hl_agent_new(&cfg, hl_platform_esp32(), tools, 8);
                printf("[app] session '%s' active (%s)\n", name, s_session_path);
            }
        } else if (!strcmp(line, "reset")) {
            /* wipe the CURRENT session's history file, then rebuild */
            remove(cfg.history_path);
            hl_agent_free(agent);
            agent = hl_agent_new(&cfg, hl_platform_esp32(), tools, 8);
            printf("[app] agent restarted, history wiped (%s)\n", s_session_path);
        } else if (!strcmp(line, "free")) {
            printf("free_heap=%lu min_free=%lu\n",
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                   (unsigned long)esp_get_minimum_free_heap_size());
        } else {
            long t0 = (long)hl_platform_esp32()->time_ms();
            int r = hl_agent_chat_stream(agent, line, on_fragment, NULL, s_reply,
                                         sizeof s_reply);
            long dt = (long)hl_platform_esp32()->time_ms() - t0;
            if (r == 0)
                printf("\n(%ld ms)\n", dt);
            else
                printf("\nerror: %s\n", s_reply[0] ? s_reply : "see log above");
        }
    }
}
