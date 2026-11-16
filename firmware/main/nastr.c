/* Настройки платы в NVS: Wi-Fi, адрес сервера, токен. */
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "obshchee.h"

static const char *TAG = "nastr";
#define NS "pechat"
nastr_t g_nastr;

static void prochitat(nvs_handle_t h, const char *k, char *dst, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, k, dst, &l) != ESP_OK) dst[0] = 0;
}

esp_err_t nastr_zagruzit(void)
{
    memset(&g_nastr, 0, sizeof(g_nastr));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        prochitat(h, "ssid", g_nastr.ssid, sizeof(g_nastr.ssid));
        prochitat(h, "parol", g_nastr.parol, sizeof(g_nastr.parol));
        prochitat(h, "server", g_nastr.server, sizeof(g_nastr.server));
        prochitat(h, "token", g_nastr.token, sizeof(g_nastr.token));
        nvs_close(h);
    }
    if (!g_nastr.server[0]) strlcpy(g_nastr.server, SERVER_PO_UMOLCH, sizeof(g_nastr.server));
    ESP_LOGI(TAG, "✅ сеть «%s», сервер %s, токен %s", g_nastr.ssid[0] ? g_nastr.ssid : "-",
             g_nastr.server, g_nastr.token[0] ? "есть" : "НЕТ");
    return ESP_OK;
}

esp_err_t nastr_sohranit(const nastr_t *n)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "ssid", n->ssid);
    nvs_set_str(h, "parol", n->parol);
    nvs_set_str(h, "server", n->server);
    nvs_set_str(h, "token", n->token);
    e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "✅ настройки сохранены (сеть «%s»)", n->ssid);
    return e;
}

esp_err_t nastr_steret(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_all(h);
    e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "⚠️ настройки стёрты");
    return e;
}

bool nastr_polnye(void) { return g_nastr.ssid[0] && g_nastr.token[0] && g_nastr.server[0]; }
