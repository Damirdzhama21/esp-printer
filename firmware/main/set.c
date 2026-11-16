/*
 * Wi-Fi. Если сеть сохранена — подключаемся как клиент. Если сети нет, токена нет
 * или подключиться не вышло за 45 с — поднимаем точку ESP-Printer (APSTA) с
 * портальной страничкой, продолжая изредка пробовать старую сеть: так смена
 * пароля на роутере не превращает плату в кирпич до приезда человека.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "obshchee.h"

static const char *TAG = "set";

static volatile bool s_est_ip = false;
static volatile bool s_portal = false;
static char s_ip[16] = "";
static int64_t s_poteryana_us = 0;    /* когда последний раз потеряли/не нашли сеть */
static SemaphoreHandle_t s_skan_mux;
static char s_skan[1536] = "[]";

bool set_est(void) { return s_est_ip; }
bool set_portal_otkryt(void) { return s_portal; }
const char *set_ip(void) { return s_ip; }

int set_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_est_ip && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

static void json_str(char *dst, size_t len, const char *s)
{
    size_t n = 0;
    for (; *s && n + 2 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c >= 0x20) dst[n++] = c;
    }
    dst[n] = 0;
}

/* Сканирование. В APSTA оно на пару секунд уводит точку с канала, это терпимо. */
size_t set_skan_json(char *buf, size_t len, bool novyj)
{
    xSemaphoreTake(s_skan_mux, portMAX_DELAY);
    if (novyj) {
        wifi_scan_config_t sc = {.show_hidden = false};
        if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
            uint16_t n = 20;
            wifi_ap_record_t *zap = calloc(n, sizeof(*zap));
            if (zap && esp_wifi_scan_get_ap_records(&n, zap) == ESP_OK) {
                size_t p = 0;
                p += snprintf(s_skan + p, sizeof(s_skan) - p, "[");
                for (int i = 0; i < n && p + 90 < sizeof(s_skan); i++) {
                    char e[70];
                    json_str(e, sizeof(e), (const char *)zap[i].ssid);
                    if (!e[0]) continue;
                    p += snprintf(s_skan + p, sizeof(s_skan) - p, "%s{\"s\":\"%s\",\"r\":%d,\"z\":%d}",
                                  p > 1 ? "," : "", e, zap[i].rssi, zap[i].authmode != WIFI_AUTH_OPEN);
                }
                snprintf(s_skan + p, sizeof(s_skan) - p, "]");
            }
            free(zap);
        } else {
            ESP_LOGW(TAG, "⚠️ сканирование не удалось (Wi-Fi занят подключением)");
        }
    }
    size_t n = strlcpy(buf, s_skan, len);
    xSemaphoreGive(s_skan_mux);
    return n;
}

static void portal_otkryt(void)
{
    if (s_portal) return;
    ESP_LOGW(TAG, "⚠️ открываю точку «%s» для настройки", AP_SSID);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    s_portal = true;
    portal_dns_start();
}

static void portal_zakryt(void)
{
    if (!s_portal) return;
    ESP_LOGI(TAG, "✅ сеть есть — закрываю точку настройки");
    portal_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal = false;
}

static void sobytiya(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_nastr.ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (s_est_ip) {
            ESP_LOGW(TAG, "⚠️ Wi-Fi пропал (причина %d)", d->reason);
            s_poteryana_us = esp_timer_get_time();
        }
        s_est_ip = false;
        s_ip[0] = 0;
        /* переподключение делает задача set_storozh: здесь не долбим без паузы */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "📩 к точке ESP-Printer подключился телефон/ноутбук");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "✅ Wi-Fi «%s», адрес %s", g_nastr.ssid, s_ip);
        s_est_ip = true;
    }
}

/* Раз в секунду: решаем, нужен ли портал, и когда снова пробовать сеть. */
static void set_storozh(void *arg)
{
    int64_t posl_popytka = 0;
    for (;;) {
        int64_t t = esp_timer_get_time();
        bool nuzhen_portal = !nastr_polnye() ||
                             (!s_est_ip && t - s_poteryana_us > 45LL * 1000000);
        if (nuzhen_portal) portal_otkryt();
        else if (s_est_ip && s_portal) portal_zakryt();

        if (!s_est_ip) {
            if (sost_tekushchee() != SOST_SBROS) sost_postavit(SOST_NET_WIFI);
            /* без портала пробуем каждые 10 с, с порталом — раз в минуту, чтобы
             * сканирование каналов не рвало телефону связь с точкой */
            int64_t pauza = (s_portal ? 60LL : 10LL) * 1000000;
            if (g_nastr.ssid[0] && t - posl_popytka > pauza) {
                posl_popytka = t;
                esp_wifi_disconnect();
                esp_wifi_connect();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void set_start(void)
{
    s_skan_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta, "esp-printer");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* храним сами, в nastr */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, sobytiya, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sobytiya, NULL);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    strlcpy((char *)ap.ap.password, AP_PAROL, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 3;
    ap.ap.channel = 6;

    wifi_config_t st = {0};
    strlcpy((char *)st.sta.ssid, g_nastr.ssid, sizeof(st.sta.ssid));
    strlcpy((char *)st.sta.password, g_nastr.parol, sizeof(st.sta.password));
    st.sta.threshold.authmode = g_nastr.parol[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    st.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    st.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* AP-конфиг задаём в режиме APSTA, потом решаем, оставить ли точку */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &st));
    esp_wifi_set_ps(WIFI_PS_NONE);   /* без энергосбережения: меньше задержек на потоке */
    s_portal = true;                 /* сейчас APSTA; сторож закроет, если не нужен */
    if (nastr_polnye()) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_portal = false;
    } else {
        portal_dns_start();
    }
    s_poteryana_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());

    portal_start_http();
    if (s_portal) {
        char tmp[8];
        set_skan_json(tmp, sizeof(tmp), true);   /* сразу список сетей для страницы */
    }
    xTaskCreate(set_storozh, "set_storozh", 3584, NULL, 5, NULL);
}
