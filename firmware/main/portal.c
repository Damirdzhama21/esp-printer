/*
 * Веб-страница платы. На точке ESP-Printer — это captive-портал настройки (DNS
 * отвечает на любое имя адресом 192.168.4.1, неизвестные адреса уводятся на
 * главную, поэтому телефон сам показывает страницу). В локальной сети та же
 * страница по http://<ip платы>/ показывает состояние и журнал /log.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "obshchee.h"

static const char *TAG = "portal";

/* ---------------- DNS: на любой вопрос — 192.168.4.1 ---------------- */
static volatile bool s_dns_rabotaet = false;
static TaskHandle_t s_dns_zadacha = NULL;

static void zadacha_dns(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "❌ DNS не поднялся");
        if (s >= 0) close(s);
        s_dns_zadacha = NULL;
        vTaskDelete(NULL);
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t b[512];
    while (s_dns_rabotaet) {
        struct sockaddr_in ot;
        socklen_t ol = sizeof(ot);
        int n = recvfrom(s, b, sizeof(b) - 16, 0, (struct sockaddr *)&ot, &ol);
        if (n < 12) continue;
        if (b[2] & 0x80) continue;                         /* это ответ, не вопрос */
        if (((b[4] << 8) | b[5]) != 1) continue;           /* один вопрос */
        /* конец имени вопроса */
        int p = 12;
        while (p < n && b[p]) p += b[p] + 1;
        p += 5;                                            /* 0 + QTYPE + QCLASS */
        if (p > n) continue;
        uint16_t qtype = (b[p - 4] << 8) | b[p - 3];
        b[2] = 0x84 | (b[2] & 0x01);                       /* ответ, авторитетный, RD как был */
        b[3] = 0x00;
        b[6] = 0; b[7] = (qtype == 1) ? 1 : 0;             /* ответ только на A */
        b[8] = b[9] = b[10] = b[11] = 0;
        n = p;
        if (qtype == 1) {
            const uint8_t otv[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(b + n, otv, sizeof(otv));
            n += sizeof(otv);
        }
        sendto(s, b, n, 0, (struct sockaddr *)&ot, ol);
    }
    close(s);
    s_dns_zadacha = NULL;
    vTaskDelete(NULL);
}

void portal_dns_start(void)
{
    if (s_dns_rabotaet) return;
    s_dns_rabotaet = true;
    xTaskCreate(zadacha_dns, "dns", 3072, NULL, 4, &s_dns_zadacha);
}

void portal_dns_stop(void) { s_dns_rabotaet = false; }

/* ---------------- HTTP ---------------- */

static void html_esc(httpd_req_t *r, const char *s)
{
    char b[8];
    for (; *s; s++) {
        const char *z = NULL;
        switch (*s) {
        case '<': z = "&lt;"; break;
        case '>': z = "&gt;"; break;
        case '&': z = "&amp;"; break;
        case '"': z = "&quot;"; break;
        }
        if (z) httpd_resp_sendstr_chunk(r, z);
        else { b[0] = *s; b[1] = 0; httpd_resp_sendstr_chunk(r, b); }
    }
}

static const char *STIL =
    "<!doctype html><html lang=ru><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP-Printer</title><style>"
    "body{font-family:sans-serif;max-width:480px;margin:16px auto;padding:0 12px;color:#222}"
    "h1{font-size:20px}label{display:block;margin:12px 0 4px;font-weight:bold}"
    "input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}"
    "button{margin-top:16px;width:100%;padding:12px;font-size:17px;background:#1565c0;color:#fff;border:0;border-radius:6px}"
    ".s{background:#f3f3f3;padding:10px;border-radius:6px;font-size:14px}.m{color:#666;font-size:13px}"
    "</style></head><body>";

static esp_err_t glavnaya(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, STIL);
    httpd_resp_sendstr_chunk(r, "<h1>Wi-Fi принтер: плата</h1><div class=s>");
    char t[320];
    snprintf(t, sizeof(t), "Состояние: <b>%s</b><br>Wi-Fi: ", sost_tekst());
    httpd_resp_sendstr_chunk(r, t);
    if (set_est()) {
        snprintf(t, sizeof(t), "«%.32s», адрес %s, сигнал %d дБм", g_nastr.ssid, set_ip(), set_rssi());
        html_esc(r, t);
        httpd_resp_sendstr_chunk(r, "<br>");
    } else httpd_resp_sendstr_chunk(r, "нет<br>");
    httpd_resp_sendstr_chunk(r, "Принтер: ");
    html_esc(r, usb_pr_opisanie());
    if (ipp_tekst()[0]) { httpd_resp_sendstr_chunk(r, "<br>Тонер и бумага: <b>"); html_esc(r, ipp_tekst()); httpd_resp_sendstr_chunk(r, "</b>"); }
    if (posl_oshibka()[0]) { httpd_resp_sendstr_chunk(r, "<br>Последняя ошибка: "); html_esc(r, posl_oshibka()); }
    httpd_resp_sendstr_chunk(r, "<br><span class=m>" FW_VERSIYA " · <a href=/log>журнал</a> · <a href=/ipp>опрос принтера</a></span></div>");

    httpd_resp_sendstr_chunk(r,
        "<form method=post action=/save>"
        "<label>Сеть Wi-Fi</label><select id=sel onchange=\"document.getElementById('ssid').value=this.value\">"
        "<option value=''>— выбрать из найденных —</option></select>"
        "<input id=ssid name=ssid maxlength=32 placeholder='имя сети' value=\"");
    html_esc(r, g_nastr.ssid);
    httpd_resp_sendstr_chunk(r,
        "\" required><a href=# onclick='skan(1);return false' class=m>обновить список</a>"
        "<label>Пароль Wi-Fi</label><input name=parol type=password maxlength=64 placeholder='");
    httpd_resp_sendstr_chunk(r, g_nastr.parol[0] ? "оставить прежний" : "пароль сети");
    httpd_resp_sendstr_chunk(r, "'><label>Токен принтера</label><input name=token maxlength=95 autocomplete=off placeholder='");
    httpd_resp_sendstr_chunk(r, g_nastr.token[0] ? "задан, оставить прежний" : "выдаёт администратор");
    httpd_resp_sendstr_chunk(r, "'><label>Адрес сервера</label><input name=server maxlength=128 placeholder='https://ваш-сервер' required value=\"");
    html_esc(r, g_nastr.server);
    httpd_resp_sendstr_chunk(r,
        "\"><button>Сохранить и перезагрузить</button></form>"
        "<script>function skan(n){fetch('/scan'+(n?'?new=1':'')).then(r=>r.json()).then(a=>{"
        "var s=document.getElementById('sel');s.length=1;a.sort((x,y)=>y.r-x.r).forEach(w=>{"
        "var o=document.createElement('option');o.value=w.s;o.text=w.s+' ('+w.r+' дБм'+(w.z?'':', открытая')+')';s.add(o)})})}"
        "skan(0)</script></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

static esp_err_t skan(httpd_req_t *r)
{
    char q[16] = "";
    bool novyj = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && strstr(q, "new=1");
    char *b = malloc(1600);
    if (!b) return httpd_resp_send_500(r);
    set_skan_json(b, 1600, novyj);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, b);
    free(b);
    return ESP_OK;
}

/* application/x-www-form-urlencoded: достать поле и раскодировать %XX и + */
static void pole(const char *telo, const char *imya, char *dst, size_t len)
{
    dst[0] = 0;
    size_t il = strlen(imya);
    const char *p = telo;
    while (p && *p) {
        if (!strncmp(p, imya, il) && p[il] == '=') {
            p += il + 1;
            size_t n = 0;
            while (*p && *p != '&' && n + 1 < len) {
                if (*p == '+') { dst[n++] = ' '; p++; }
                else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                    char h[3] = {p[1], p[2], 0};
                    dst[n++] = (char)strtol(h, NULL, 16);
                    p += 3;
                } else dst[n++] = *p++;
            }
            dst[n] = 0;
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void obrezat(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static void perezagruzka(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t sohranit(httpd_req_t *r)
{
    if (r->content_len > 1500) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "слишком длинно");
    char *b = calloc(1, r->content_len + 1);
    if (!b) return httpd_resp_send_500(r);
    int poluch = 0;
    while (poluch < (int)r->content_len) {
        int n = httpd_req_recv(r, b + poluch, r->content_len - poluch);
        if (n <= 0) { free(b); return httpd_resp_send_500(r); }
        poluch += n;
    }
    nastr_t n = g_nastr;
    char vr[130];
    pole(b, "ssid", vr, sizeof(n.ssid)); obrezat(vr);
    if (vr[0]) strlcpy(n.ssid, vr, sizeof(n.ssid));
    pole(b, "parol", vr, sizeof(n.parol));
    if (vr[0]) strlcpy(n.parol, vr, sizeof(n.parol));
    else if (strcmp(n.ssid, g_nastr.ssid)) n.parol[0] = 0;   /* другая сеть без пароля = открытая */
    pole(b, "token", vr, sizeof(n.token)); obrezat(vr);
    if (vr[0]) strlcpy(n.token, vr, sizeof(n.token));
    pole(b, "server", vr, sizeof(n.server)); obrezat(vr);
    if (vr[0]) {
        size_t l = strlen(vr);
        while (l && vr[l - 1] == '/') vr[--l] = 0;
        if (!strncmp(vr, "https://", 8) || !strncmp(vr, "http://", 7)) strlcpy(n.server, vr, sizeof(n.server));
    }
    free(b);
    nastr_sohranit(&n);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(r, STIL);
    httpd_resp_sendstr_chunk(r, "<h1>Сохранено</h1><p>Плата перезагружается и подключается к сети «");
    html_esc(r, n.ssid);
    httpd_resp_sendstr_chunk(r, "».</p><p>Если всё верно, огонёк станет <b>зелёным</b> (или красным, если не воткнут принтер). "
                                "Если снова замигает <b>синим</b> — сеть или пароль не подошли, подключитесь к ESP-Printer ещё раз.</p></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
    xTaskCreate(perezagruzka, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t zhurnal(httpd_req_t *r)
{
    char *b = malloc(6200);
    if (!b) return httpd_resp_send_500(r);
    zhurnal_kopiya(b, 6200);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, b);
    free(b);
    return ESP_OK;
}

/* Отладка IPP: попросить плату спросить принтер сейчас и показать прошлый ответ.
 * Сам USB отсюда не трогаем — им владеет задача печати; она спросит за ~4 с. */
static void ruchka(const char *q, const char *k, int *dst, int min, int max)
{
    char v[12];
    if (httpd_query_key_value(q, k, v, sizeof(v)) == ESP_OK) {
        int x = atoi(v);
        if (x < min) x = min;
        if (x > max) x = max;
        *dst = x;
    }
}

static esp_err_t ipp_str(httpd_req_t *r)
{
    /* /ipp?rx=64&sbros=0&setif=1&zlp=1&intf=2&posle_out_ms=100&pechat_ipp=1&pauza_s=20&reset=1&test=leg */
    char q[200] = "";
    bool test = false;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        ruchka(q, "rx", &g_ipp.rx, 64, 512);
        ruchka(q, "sbros", &g_ipp.sbros, 0, 1);
        ruchka(q, "setif", &g_ipp.setif, 0, 1);
        ruchka(q, "zlp", &g_ipp.zlp, 0, 1);
        ruchka(q, "intf", &g_ipp.intf, -1, 15);
        ruchka(q, "posle_out_ms", &g_ipp.posle_out_ms, 0, 3000);
        ruchka(q, "pechat_ipp", &g_ipp.pechat_ipp, 0, 1);
        ruchka(q, "pauza_s", &g_ipp.pauza_s, 0, 120);
        ruchka(q, "mps_out", &g_ipp.mps_out, 0, 1);
        int mps = 0;
        ruchka(q, "mps", &mps, 0, 512);
        if (mps) usb_pr_ipp_mps(mps);
        char v[8];
        if (httpd_query_key_value(q, "reset", v, sizeof(v)) == ESP_OK) usb_pr_ipp_zanovo();
        if (httpd_query_key_value(q, "test", v, sizeof(v)) == ESP_OK && !strcmp(v, "leg")) test = true;
    }
    if (test) ipp_zaprosit_test(); else ipp_zaprosit();
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(r, "Запрос к принтеру поставлен — обновите страницу через 5–10 секунд.\n\n");
    httpd_resp_sendstr_chunk(r, "Для сервера: ");
    httpd_resp_sendstr_chunk(r, ipp_kratko()[0] ? ipp_kratko() : "-");
    httpd_resp_sendstr_chunk(r, "\nНа странице: ");
    httpd_resp_sendstr_chunk(r, ipp_tekst()[0] ? ipp_tekst() : "-");
    httpd_resp_sendstr_chunk(r, "\nПринтер: ");
    httpd_resp_sendstr_chunk(r, usb_pr_opisanie());
    httpd_resp_sendstr_chunk(r, "\n\nПоследний обмен по IPP:\n");
    httpd_resp_sendstr_chunk(r, ipp_otladka());
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

/* POST /ipp_raw: тело — IPP-запрос (application/ipp, без HTTP-обёртки). Плата передаёт его
 * принтеру по IPP-over-USB и возвращает тело ответа. Для отладки с ноутбука: Get-Jobs,
 * Get-Job-Attributes, любые атрибуты — без перепрошивки. */
static esp_err_t ipp_raw(httpd_req_t *r)
{
    if (r->content_len == 0 || r->content_len > 2048) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "1..2048 bytes");
    uint8_t *z = malloc(r->content_len);
    if (!z) return httpd_resp_send_500(r);
    size_t est = 0;
    while (est < r->content_len) {
        int n = httpd_req_recv(r, (char *)z + est, r->content_len - est);
        if (n <= 0) { free(z); return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv"); }
        est += n;
    }
    long dl = -1;
    const uint8_t *otv = ipp_prohod(z, est, &dl, 25000);
    free(z);
    if (!otv || dl <= 0) {
        httpd_resp_set_status(r, "504 Gateway Timeout");
        return httpd_resp_sendstr(r, "printer did not answer (see /ipp)");
    }
    httpd_resp_set_type(r, "application/ipp");
    return httpd_resp_send(r, (const char *)otv, dl);
}

/* всё неизвестное (проверки captive-портала Android/iOS/Windows) — на главную */
static esp_err_t na_glavnuyu(httpd_req_t *r, httpd_err_code_t e)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_send(r, "", 0);
    return ESP_OK;
}

void portal_start_http(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;
    c.max_open_sockets = 5;
    c.lru_purge_enable = true;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &c) != ESP_OK) { ESP_LOGE(TAG, "❌ веб-страница не поднялась"); return; }
    const httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = glavnaya},
        {.uri = "/scan", .method = HTTP_GET, .handler = skan},
        {.uri = "/save", .method = HTTP_POST, .handler = sohranit},
        {.uri = "/log", .method = HTTP_GET, .handler = zhurnal},
        {.uri = "/ipp", .method = HTTP_GET, .handler = ipp_str},
        {.uri = "/ipp_raw", .method = HTTP_POST, .handler = ipp_raw},
    };
    for (int i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(h, &u[i]);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, na_glavnuyu);
    ESP_LOGI(TAG, "✅ веб-страница платы поднята (порт 80)");
}
