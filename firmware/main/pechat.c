/*
 * Цикл печати. Раз в 4 с GET <server>/api/pechat/esp/vzyat:
 *   204 — заданий нет; 200 — тело = PWG raster, X-Zadanie-Id, Content-Length.
 * Тело читаем кусками по 16 КБ прямо в буфер USB-передачи и сразу отдаём в
 * принтер — весь файл в памяти не держим. Потом POST esp/gotovo {id, ok, oshibka}.
 *
 * Пульс: каждый запрос с токеном обновляет poslednij на сервере, так что опрос
 * и есть пульс. Без принтера задания не берём (иначе сожжём попытки), а раз в
 * 60 с шлём лёгкий GET esp/pulse — сервер видит, что плата жива.
 *
 * Отчёт «готово» важнее всего: если он потерялся, через 10 минут сервер вернёт
 * задание в очередь и путёвка напечатается дважды. Поэтому неотправленный отчёт
 * повторяем, пока не уйдёт, и новых заданий до этого не берём.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "obshchee.h"

static const char *TAG = "pechat";

#define OPROS_MS        4000
#define PULS_MS         60000
#define IPP_MS          60000     /* как часто спрашивать у принтера тонер/бумагу */
#define USB_TIMEOUT_MS  45000     /* принтер греется/занят — ждём, но не вечно (меньше сторожа 60 с) */
#define BEZ_SERVERA_US  (20LL * 60 * 1000000)   /* 20 мин без ответа сервера при живом Wi-Fi — перезагрузка */

typedef struct {
    long id;
} otvet_t;

static int64_t s_ne_ranshe = 0;     /* раньше этого времени следующее задание не брать (пауза legacy) */
/* 1.8: прогрев после (пере)подключения принтера или сорванного задания */
#define PRIGREV_S     20
#define PRIGREV_MAX_S 300
static bool s_prigret = false;       /* принтер уже сказал «готов» после подключения */
static int64_t s_podkl_videl = -1;   /* usb_pr_podkl_us(), на который уже среагировали */
static int64_t s_prigrev_s = 0;      /* от этого момента считаем прогрев */
static int s_sryv_podryad = 0;       /* сколько заданий подряд принтер сорвал по IPP */
#define IPP_MERTV_S   60             /* IPP не отвечает столько после срыва/подключения — печатаем legacy */
static bool s_ipp_mertv = false;     /* IPP завис: печатаем старым каналом, пока не оживёт */
static long s_dolg_id = 0;          /* неотправленный отчёт */
static bool s_dolg_ok = false;
static char s_dolg_osh[200];

static esp_err_t sobytie_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data &&
        !strcasecmp(e->header_key, "X-Zadanie-Id")) {
        ((otvet_t *)e->user_data)->id = strtol(e->header_value, NULL, 10);
    }
    return ESP_OK;
}

static void url(char *dst, size_t len, const char *put)
{
    snprintf(dst, len, "%s/api/pechat/esp/%s", g_nastr.server, put);
}

/* Состояние для сервера: что видит плата (лог на сервере пишется при изменении). */
static void zagolovok_platy(esp_http_client_handle_t c)
{
    char h[420];
    snprintf(h, sizeof(h), "fw=%s; usb=%s%s%s", FW_VERSIYA, usb_pr_opisanie(),
             ipp_kratko()[0] ? "; " : "", ipp_kratko());
    for (char *p = h; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e) *p = '?';
    esp_http_client_set_header(c, "X-Plata", h);
}

static esp_http_client_handle_t klient(const char *u, otvet_t *o, esp_http_client_method_t m)
{
    esp_http_client_config_t cfg = {
        .url = u,
        .method = m,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = sobytie_http,
        .user_data = o,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "x-pechat-token", g_nastr.token);
        zagolovok_platy(c);
    }
    return c;
}

/* JSON-строка без кавычек/слэшей/управляющих — текст ошибки наш, ASCII+UTF-8 */
static void bez_kavychek(char *s)
{
    for (; *s; s++) if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = ' ';
}

static bool otchet(long id, bool ok, const char *osh)
{
    char u[200], telo[320], o2[220];
    url(u, sizeof(u), "gotovo");
    strlcpy(o2, osh ? osh : "", sizeof(o2));
    bez_kavychek(o2);
    if (ok) snprintf(telo, sizeof(telo), "{\"id\":%ld,\"ok\":true}", id);
    else snprintf(telo, sizeof(telo), "{\"id\":%ld,\"ok\":false,\"oshibka\":\"%s\"}", id, o2);
    esp_http_client_handle_t c = klient(u, NULL, HTTP_METHOD_POST);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, telo, strlen(telo));
    esp_err_t e = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (e == ESP_OK && (st == 200 || st == 404)) {   /* 404: задание уже не наше — повторять бессмысленно */
        ESP_LOGI(TAG, "%s отчёт по заданию %ld отправлен (%d)", ok ? "✅" : "❌", id, st);
        return true;
    }
    ESP_LOGW(TAG, "⚠️ отчёт по заданию %ld не ушёл: %s, HTTP %d", id, esp_err_to_name(e), st);
    return false;
}

static void otchitatsya(long id, bool ok, const char *osh)
{
    if (otchet(id, ok, osh)) return;
    s_dolg_id = id;
    s_dolg_ok = ok;
    strlcpy(s_dolg_osh, osh ? osh : "", sizeof(s_dolg_osh));
}

static bool puls(void)
{
    char u[200];
    url(u, sizeof(u), "pulse");
    esp_http_client_handle_t c = klient(u, NULL, HTTP_METHOD_GET);
    if (!c) return false;
    esp_err_t e = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (e == ESP_OK && st == 200) return true;
    if (e == ESP_OK && st == 403) posl_oshibka_postavit("сервер не принял токен (403) — проверьте токен");
    else posl_oshibka_postavit("пульс: %s, HTTP %d", esp_err_to_name(e), st);
    return false;
}

/* Один опрос. Возвращает: 1 — была работа, 0 — пусто, -1 — ошибка связи/сервера. */
static int opros(void)
{
    char u[200];
    url(u, sizeof(u), "vzyat");
    otvet_t o = {0};
    esp_http_client_handle_t c = klient(u, &o, HTTP_METHOD_GET);
    if (!c) return -1;
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        posl_oshibka_postavit("сервер недоступен: %s", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return -1;
    }
    int64_t dlina = esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    if (st == 204) { esp_http_client_cleanup(c); return 0; }
    if (st != 200) {
        if (st == 403) posl_oshibka_postavit("сервер не принял токен (403) — проверьте токен");
        else posl_oshibka_postavit("сервер ответил HTTP %d", st);
        esp_http_client_cleanup(c);
        return -1;
    }
    if (o.id <= 0 || dlina <= 0) {
        posl_oshibka_postavit("ответ без X-Zadanie-Id/Content-Length (id %ld, длина %lld)", o.id, dlina);
        esp_http_client_cleanup(c);
        return -1;
    }

    ESP_LOGI(TAG, "🖨 задание %ld: %lld байт PWG", o.id, dlina);
    sost_postavit(SOST_PECHAT);
    int64_t t0 = esp_timer_get_time();
    int64_t vsego = 0;
    char osh[200] = "";
    bool usb_slomalsya = false;
    uint8_t *buf = usb_pr_bufer();

    /* IPP Print-Job: у задания явный конец и подтверждение «напечатано» —
     * пачка путёвок не слипается. Не вышло открыть канал — сырым потоком, как раньше. */
    /* IPP молчит или сорвал 2 задания подряд (document-format-error) — старым каналом */
    bool ipp_mozhno = g_ipp.pechat_ipp && !s_ipp_mertv && s_sryv_podryad < 2;
    bool cherez_ipp = ipp_mozhno && usb_pr_kanal_ipp(true);
    if (g_ipp.pechat_ipp && !ipp_mozhno)
        ESP_LOGW(TAG, "⚠️ IPP %s — печатаю старым каналом (legacy)", s_ipp_mertv ? "завис" : "сорвал 2 задания подряд");
    else if (g_ipp.pechat_ipp && !cherez_ipp)
        ESP_LOGW(TAG, "⚠️ IPP недоступен (%s) — печатаю сырым потоком", usb_pr_ipp_osh());
    if (cherez_ipp) {
        size_t h = ipp_zagolovok_pechati(buf, USB_PR_KUSOK, (size_t)dlina);
        if (!h) snprintf(osh, sizeof(osh), "IPP: не собрался заголовок задания");
        else if (usb_pr_zapisat(h, USB_TIMEOUT_MS, osh, sizeof(osh)) != 0) usb_slomalsya = true;
    }
    while (vsego < dlina && !osh[0]) {
        esp_task_wdt_reset();
        /* набираем полный кусок: USB-передачи крупными порциями быстрее */
        size_t nado = (dlina - vsego) < USB_PR_KUSOK ? (size_t)(dlina - vsego) : USB_PR_KUSOK;
        size_t est = 0;
        while (est < nado) {
            int n = esp_http_client_read(c, (char *)buf + est, nado - est);
            if (n <= 0) {
                snprintf(osh, sizeof(osh), "сеть: обрыв на %lld из %lld байт (%d)", vsego + est, dlina, n);
                break;
            }
            est += n;
            esp_task_wdt_reset();   /* каждое чтение до 30 с: сторож 60 с не должен сработать на медленной сети */
        }
        if (osh[0]) break;
        if (cherez_ipp && vsego + (int64_t)est == dlina) usb_pr_zlp_dalee();   /* конец тела HTTP */
        if (usb_pr_zapisat(est, USB_TIMEOUT_MS, osh, sizeof(osh)) != 0) { usb_slomalsya = true; break; }
        vsego += est;
    }
    esp_http_client_cleanup(c);

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (!osh[0])
        ESP_LOGI(TAG, "✅ задание %ld ушло в принтер%s: %lld байт за %lld мс (%lld КБ/с)", o.id,
                 cherez_ipp ? " по IPP" : "", vsego, ms, ms ? vsego * 1000 / 1024 / ms : 0);
    /* по IPP «напечатано» — только когда принтер сам это скажет */
    if (cherez_ipp && !osh[0]) ipp_zavershit_pechat(osh, sizeof(osh));
    if (cherez_ipp) usb_pr_kanal_ipp(false);

    if (cherez_ipp) {
        if (osh[0]) {
            s_sryv_podryad++;
            /* следующее задание — только когда принтер снова скажет «готов» */
            s_prigret = false;
            s_prigrev_s = esp_timer_get_time();
        } else s_sryv_podryad = 0;
    } else if (osh[0] && usb_slomalsya && strlen(osh) + 110 < sizeof(osh)) {
        /* и старый канал не берёт данные — принтер завис целиком, само не пройдёт */
        strlcat(osh, " — перезапустите принтер (выкл/вкл), потом RST на плате", sizeof(osh));
    }

    if (!osh[0]) {
        posl_oshibka_postavit("%s", "");
        otchitatsya(o.id, true, NULL);
        /* сырой поток без конца задания: принтер узнаёт конец по паузе — даём её */
        if (!cherez_ipp && g_ipp.pauza_s > 0) s_ne_ranshe = esp_timer_get_time() + (int64_t)g_ipp.pauza_s * 1000000;
    } else {
        posl_oshibka_postavit("задание %ld: %s", o.id, osh);
        if (cherez_ipp) usb_pr_ipp_zanovo();   /* оборванный HTTP: заново SET_INTERFACE сбросит канал */
        /* полстраницы растра в принтере: без сброса он ждёт продолжения */
        else if (vsego > 0 || usb_slomalsya) usb_pr_sbros();
        otchitatsya(o.id, false, osh);
    }
    return 1;
}

static void zadacha_pechat(void *arg)
{
    esp_task_wdt_add(NULL);
    int64_t posl_puls = 0, posl_svyaz = esp_timer_get_time(), posl_ipp = 0;
    bool bylo_ipp = false;   /* принтер был готов на прошлом круге: сброс при отключении */
    for (;;) {
        esp_task_wdt_reset();
        int64_t t = esp_timer_get_time();

        if (!set_est() || !g_nastr.token[0]) {
            posl_svyaz = t;   /* без Wi-Fi счётчик «сервер молчит» не тикает */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (t - posl_svyaz > BEZ_SERVERA_US) {
            ESP_LOGE(TAG, "❌ сервер молчит 20 минут при живом Wi-Fi — перезагружаюсь");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        if (s_dolg_id) {
            if (otchet(s_dolg_id, s_dolg_ok, s_dolg_osh)) { s_dolg_id = 0; posl_svyaz = t; }
            else { sost_postavit(SOST_OSHIBKA); vTaskDelay(pdMS_TO_TICKS(10000)); }
            continue;
        }

        if (!usb_pr_gotov()) {
            if (bylo_ipp) { ipp_obnovit(); bylo_ipp = false; posl_ipp = 0; }   /* очистит строки */
            s_prigret = false;   /* подключится снова — сначала прогрев */
            s_ipp_mertv = false;
            s_sryv_podryad = 0;
            sost_postavit(SOST_NET_PRINTERA);
            if (t - posl_puls > (int64_t)PULS_MS * 1000 || posl_puls == 0) {
                posl_puls = t;
                if (puls()) posl_svyaz = t;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* состояние принтера — до опроса, чтобы X-Plata ушёл уже свежим */
        if (posl_ipp == 0 || t - posl_ipp > (int64_t)IPP_MS * 1000 || ipp_nado()) {
            posl_ipp = t;
            bylo_ipp = true;
            ipp_obnovit();
            esp_task_wdt_reset();
            if (s_ipp_mertv && ipp_sost_printera() != 0) {
                s_ipp_mertv = false;
                s_sryv_podryad = 0;
                ESP_LOGI(TAG, "✅ IPP снова отвечает — печатаю по IPP");
            }
        }

        if (esp_timer_get_time() < s_ne_ranshe) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        /* 1.8: принтер только что (пере)подключился или сорвал задание — не шлём сразу.
         * HP после переподключения ещё секунд 20–60 приходит в себя; задание, посланное в это
         * время, он принимает еле-еле и бракует (document-format-error), а потом виснет до
         * выключения. Ждём не меньше PRIGREV_S и printer-state=idle, но не дольше PRIGREV_MAX_S. */
        if (g_ipp.pechat_ipp && !s_prigret) {
            int64_t podkl = usb_pr_podkl_us();
            if (podkl != s_podkl_videl) { s_podkl_videl = podkl; s_prigrev_s = podkl; }
            int64_t proshlo = (esp_timer_get_time() - s_prigrev_s) / 1000000;
            bool idle = ipp_sost_printera() == 3;
            if (proshlo >= PRIGREV_S && idle) {
                s_prigret = true;
                ESP_LOGI(TAG, "✅ принтер готов через %lld с — беру задания", proshlo);
            } else if (proshlo >= IPP_MERTV_S && ipp_sost_printera() == 0) {
                /* 15.11: HP повесил IPP (не отвечает вовсе), а legacy-канал при этом печатает
                 * нормально — не ждём выключения принтера, печатаем по-старому */
                s_prigret = true;
                s_ipp_mertv = true;
                ESP_LOGW(TAG, "⚠️ IPP молчит %lld с — печатаю старым каналом (legacy), пока IPP не оживёт", proshlo);
            } else if (proshlo >= PRIGREV_MAX_S) {
                s_prigret = true;
                ESP_LOGW(TAG, "⚠️ принтер %lld с не говорит «готов» — всё равно пробую печатать", proshlo);
            } else {
                sost_postavit(SOST_PRIGREV);
                if (t - posl_ipp > 10000000LL) { posl_ipp = t; bylo_ipp = true; ipp_obnovit(); esp_task_wdt_reset(); }
                if (t - posl_puls > (int64_t)PULS_MS * 1000 || posl_puls == 0) {
                    posl_puls = t;
                    if (puls()) posl_svyaz = t;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        int r = opros();
        if (r == 1) posl_ipp = 0;   /* после задания — спросить сразу: могла кончиться бумага */
        if (r >= 0) posl_svyaz = esp_timer_get_time();
        if (r == 0) { sost_postavit(SOST_GOTOV); posl_oshibka_postavit("%s", ""); }
        else if (r < 0) sost_postavit(SOST_OSHIBKA);
        else sost_postavit(posl_oshibka()[0] ? SOST_OSHIBKA : SOST_GOTOV);
        if (r != 1) vTaskDelay(pdMS_TO_TICKS(OPROS_MS));   /* после задания — сразу следующее */
    }
}

void pechat_start(void)
{
    xTaskCreatePinnedToCore(zadacha_pechat, "pechat", 10240, NULL, 5, NULL, 1);
}
