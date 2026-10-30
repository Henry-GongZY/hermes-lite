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
#include "sdkconfig.h"
#include "json.h"

/* Hermes-Lite on the AI-Thinker ESP32-S (plain ESP32, 4MB flash).
 *
 * Serial commands:
 *   wifi <ssid> <password>   provision + connect (stored in NVS)
 *   skills                   list skill files on SPIFFS
 *   reset                    wipe conversation history and restart the agent
 *   free                     show heap usage
 *   anything else            goes to the agent */

#define SPIFFS_BASE "/spiffs"
#define HISTORY_PATH SPIFFS_BASE "/history.jsonl"
#define SKILLS_DIR SPIFFS_BASE "/skills"

static char s_reply[4096];

/* ---------------- wifi ---------------- */

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
static int s_wifi_started;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        esp_wifi_connect(); /* keep retrying; agent will fail fast offline */
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
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
        .history_path = HISTORY_PATH,
        .max_ctx_chars = 8000,
        .buf_req = 24576,
        .buf_resp = 24576,
        .buf_arena = 24576,
        .buf_hist = 8192,
        .buf_skill = 8192,
    };

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
    };

    hl_agent_t *agent = hl_agent_new(&cfg, hl_platform_esp32(), tools, 4);
    if (!agent) {
        printf("[app] agent init failed (heap too low?) free=%lu\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }

    printf("commands: 'wifi <ssid> <pass>' | 'skills' | 'reset' | 'free'\n");
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
        } else if (!strcmp(line, "skills")) {
            printf("skills in " SKILLS_DIR ":\n");
            hl_platform_esp32()->fs_list(SKILLS_DIR, skills_file_cb, NULL);
        } else if (!strcmp(line, "reset")) {
            remove(HISTORY_PATH);
            hl_agent_free(agent);
            agent = hl_agent_new(&cfg, hl_platform_esp32(), tools, 4);
            printf("[app] agent restarted, history wiped\n");
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
