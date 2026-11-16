/*
 * IPP-over-USB у HP M28a: состояние принтера (тонер, готов/занят, причины) и
 * печать заданием Print-Job — как у ноутбука через ipp-usb, с явным концом
 * задания и подтверждением. Сырой поток в legacy остаётся запасным путём.
 *
 * Запрос — HTTP POST /ipp/print с телом IPP. Ответ: HTTP с Content-Length или
 * chunked, внутри IPP. Всё USB делает задача печати (между заданиями), страница
 * /ipp только просит и показывает итог.
 */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "obshchee.h"

static const char *TAG = "ipp";

#define OTV_MAX 8192

static char s_kratko[220] = "";   /* для X-Plata, ASCII */
static char s_tekst[300] = "";    /* для страницы, UTF-8 */
static char s_otladka[1200] = "ещё не спрашивали";
static char s_formaty[200] = "";  /* document-format-supported */
static volatile bool s_nado = false, s_test_leg = false;

const char *ipp_kratko(void) { return s_kratko; }
const char *ipp_tekst(void) { return s_tekst; }
const char *ipp_otladka(void) { return s_otladka; }
void ipp_zaprosit(void) { s_nado = true; }
void ipp_zaprosit_test(void) { s_test_leg = true; s_nado = true; }
bool ipp_nado(void) { bool n = s_nado; s_nado = false; return n; }

/* ---------- сборка IPP ---------- */

static size_t atr(uint8_t *p, uint8_t teg, const char *imya, const void *zn, uint16_t dl)
{
    size_t n = strlen(imya), i = 0;
    p[i++] = teg;
    p[i++] = n >> 8; p[i++] = n & 0xff;
    memcpy(p + i, imya, n); i += n;
    p[i++] = dl >> 8; p[i++] = dl & 0xff;
    memcpy(p + i, zn, dl); i += dl;
    return i;
}
#define ATR_S(p, teg, imya, s) atr((p), (teg), (imya), (s), strlen(s))

static size_t nachalo(uint8_t *ipp, uint16_t op, uint32_t id)
{
    size_t i = 0;
    ipp[i++] = 0x02; ipp[i++] = 0x00;                   /* IPP 2.0 */
    ipp[i++] = op >> 8; ipp[i++] = op & 0xff;
    ipp[i++] = id >> 24; ipp[i++] = id >> 16; ipp[i++] = id >> 8; ipp[i++] = id;
    ipp[i++] = 0x01;                                    /* operation-attributes */
    i += ATR_S(ipp + i, 0x47, "attributes-charset", "utf-8");
    i += ATR_S(ipp + i, 0x48, "attributes-natural-language", "en");
    i += ATR_S(ipp + i, 0x45, "printer-uri", "ipp://localhost/ipp/print");
    return i;
}

static size_t http(uint8_t *b, size_t max, size_t dlina_tela)
{
    int h = snprintf((char *)b, max,
                     "POST /ipp/print HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/ipp\r\n"
                     "Content-Length: %u\r\n\r\n", (unsigned)dlina_tela);
    return h > 0 && (size_t)h < max ? (size_t)h : 0;
}

static size_t zapros_sostoyaniya(uint8_t *b, size_t max)
{
    static const char *nuzhno[] = {"printer-state", "printer-state-reasons", "marker-levels",
                                   "marker-types", "document-format-supported"};
    uint8_t ipp[400];
    size_t i = nachalo(ipp, 0x000B, 1);                 /* Get-Printer-Attributes */
    for (size_t k = 0; k < sizeof(nuzhno) / sizeof(nuzhno[0]); k++)
        i += ATR_S(ipp + i, 0x44, k ? "" : "requested-attributes", nuzhno[k]);
    ipp[i++] = 0x03;
    size_t h = http(b, max, i);
    if (!h || h + i > max) return 0;
    memcpy(b + h, ipp, i);
    return h + i;
}

/* Get-Job-Attributes (0x0009) или Cancel-Job (0x0008) по номеру задания. */
static size_t zapros_raboty(uint8_t *b, size_t max, uint16_t op, int job_id)
{
    uint8_t ipp[400];
    size_t i = nachalo(ipp, op, 3);
    uint8_t id[4] = {(uint8_t)(job_id >> 24), (uint8_t)(job_id >> 16), (uint8_t)(job_id >> 8), (uint8_t)job_id};
    i += atr(ipp + i, 0x21, "job-id", id, 4);
    i += ATR_S(ipp + i, 0x42, "requesting-user-name", "esp-pechat");
    if (op == 0x0009) {
        i += ATR_S(ipp + i, 0x44, "requested-attributes", "job-state");
        i += ATR_S(ipp + i, 0x44, "", "job-state-reasons");
    }
    ipp[i++] = 0x03;
    size_t h = http(b, max, i);
    if (!h || h + i > max) return 0;
    memcpy(b + h, ipp, i);
    return h + i;
}

/* Заголовок Print-Job: HTTP + IPP, дальше сразу пойдёт PWG длиной dlina_doc. */
size_t ipp_zagolovok_pechati(uint8_t *b, size_t max, size_t dlina_doc)
{
    uint8_t ipp[400];
    size_t i = nachalo(ipp, 0x0002, 2);                 /* Print-Job */
    i += ATR_S(ipp + i, 0x42, "requesting-user-name", "esp-pechat");
    i += ATR_S(ipp + i, 0x42, "job-name", "putevka");
    i += ATR_S(ipp + i, 0x49, "document-format", "image/pwg-raster");
    ipp[i++] = 0x03;
    size_t h = http(b, max, i + dlina_doc);
    if (!h || h + i > max) return 0;
    memcpy(b + h, ipp, i);
    return h + i;
}

/* ---------- HTTP-ответ ---------- */

static const uint8_t *najti(const uint8_t *b, size_t n, const char *s)
{
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(b + i, s, k)) return b + i;
    return NULL;
}

static const char *zagolovok(const char *h, size_t n, const char *imya)
{
    size_t k = strlen(imya);
    for (size_t i = 0; i + k < n; i++) {
        if ((i == 0 || h[i - 1] == '\n') && !strncasecmp(h + i, imya, k) && h[i + k] == ':') {
            const char *z = h + i + k + 1;
            while (*z == ' ' || *z == '\t') z++;
            return z;
        }
    }
    return NULL;
}

typedef struct { size_t telo; size_t dlina; bool chunked; bool zagolovki; } http_t;
static http_t s_http;

static bool razobrat_zagolovki(const uint8_t *b, size_t n, http_t *h)
{
    const uint8_t *k = najti(b, n, "\r\n\r\n");
    if (!k) return false;
    size_t hl = k - b;
    h->telo = hl + 4;
    h->chunked = false;
    h->dlina = (size_t)-1;
    const char *z = zagolovok((const char *)b, hl, "Content-Length");
    if (z) h->dlina = strtoul(z, NULL, 10);
    z = zagolovok((const char *)b, hl, "Transfer-Encoding");
    if (z && !strncasecmp(z, "chunked", 7)) h->chunked = true;
    h->zagolovki = true;
    return true;
}

static long sobrat_chunked(uint8_t *b, size_t n, size_t telo)
{
    size_t r = telo, w = telo;
    for (;;) {
        const uint8_t *k = najti(b + r, n - r, "\r\n");
        if (!k) return -1;
        size_t razm = strtoul((const char *)b + r, NULL, 16);
        r = (k - b) + 2;
        if (razm == 0) return (long)(w - telo);
        if (r + razm + 2 > n) return -1;
        memmove(b + w, b + r, razm);
        w += razm;
        r += razm + 2;
    }
}

static bool otvet_ves(const uint8_t *b, size_t n)
{
    if (!s_http.zagolovki && !razobrat_zagolovki(b, n, &s_http)) return false;
    if (s_http.chunked) return najti(b + s_http.telo, n - s_http.telo, "0\r\n\r\n") != NULL;
    if (s_http.dlina != (size_t)-1) return n >= s_http.telo + s_http.dlina;
    return false;
}

/* тело IPP из полного HTTP-ответа; -1 — не 200 или не собралось */
static long telo_ipp(uint8_t *b, size_t pol)
{
    if (strncmp((char *)b, "HTTP/1.", 7) || strncmp((char *)b + 9, "200", 3)) return -1;
    if (s_http.chunked) return sobrat_chunked(b, pol, s_http.telo);
    return (long)(pol - s_http.telo < s_http.dlina ? pol - s_http.telo : s_http.dlina);
}

/* ---------- IPP-ответ ---------- */

typedef struct {
    int status;               /* IPP status-code */
    int sost;                 /* printer-state: 3 idle, 4 processing, 5 stopped */
    char prichiny[8][48];
    int n_prichin;
    int urovni[4];
    int n_urovnej;
    int job_id;               /* из ответа Print-Job / Get-Job-Attributes */
    int job_sost;             /* job-state: 3 pending, 4 held, 5 processing, 6 stopped, 7 canceled, 8 aborted, 9 completed */
    char job_prichina[64];    /* первая job-state-reasons кроме none */
} ipp_t;

static bool razobrat_ipp(const uint8_t *b, size_t n, ipp_t *o, bool formaty)
{
    memset(o, 0, sizeof(*o));
    if (n < 9) return false;
    o->status = (b[2] << 8) | b[3];
    size_t i = 8;
    char imya[48] = "";
    if (formaty) s_formaty[0] = 0;
    while (i < n) {
        uint8_t teg = b[i++];
        if (teg == 0x03) break;
        if (teg < 0x10) continue;
        if (i + 2 > n) return false;
        uint16_t ni = (b[i] << 8) | b[i + 1]; i += 2;
        if (i + ni + 2 > n) return false;
        if (ni) {
            size_t k = ni < sizeof(imya) - 1 ? ni : sizeof(imya) - 1;
            memcpy(imya, b + i, k); imya[k] = 0;
        }
        i += ni;
        uint16_t nz = (b[i] << 8) | b[i + 1]; i += 2;
        if (i + nz > n) return false;
        const uint8_t *z = b + i;
        i += nz;
        uint32_t chislo = nz == 4 ? ((uint32_t)z[0] << 24) | ((uint32_t)z[1] << 16) | ((uint32_t)z[2] << 8) | z[3] : 0;

        if (!strcmp(imya, "printer-state") && teg == 0x23 && nz == 4) {
            o->sost = (int)chislo;
        } else if (!strcmp(imya, "job-id") && teg == 0x21 && nz == 4) {
            o->job_id = (int)chislo;
        } else if (!strcmp(imya, "job-state") && teg == 0x23 && nz == 4) {
            o->job_sost = (int)chislo;
        } else if (!strcmp(imya, "job-state-reasons") && !o->job_prichina[0] && !(nz == 4 && !memcmp(z, "none", 4))) {
            size_t k = nz < sizeof(o->job_prichina) - 1 ? nz : sizeof(o->job_prichina) - 1;
            memcpy(o->job_prichina, z, k); o->job_prichina[k] = 0;
        } else if (!strcmp(imya, "marker-levels") && teg == 0x21 && nz == 4 && o->n_urovnej < 4) {
            o->urovni[o->n_urovnej++] = (int32_t)chislo;
        } else if (!strcmp(imya, "printer-state-reasons") && o->n_prichin < 8) {
            size_t k = nz < 47 ? nz : 47;
            memcpy(o->prichiny[o->n_prichin], z, k); o->prichiny[o->n_prichin][k] = 0;
            o->n_prichin++;
        } else if (formaty && !strcmp(imya, "document-format-supported")) {
            size_t l = strlen(s_formaty);
            if (l + nz + 2 < sizeof(s_formaty)) {
                if (l) s_formaty[l++] = ' ';
                memcpy(s_formaty + l, z, nz); s_formaty[l + nz] = 0;
            }
        }
    }
    return true;
}

/* начало ответа для /ipp: печатное как есть, прочее — \xNN */
static void otladka(const char *zagolovok_, const uint8_t *b, size_t n)
{
    size_t w = snprintf(s_otladka, sizeof(s_otladka),
                        "%s\nошибка USB/IPP: %s\nбуфер приёма: %s\nручки: rx=%d sbros=%d setif=%d zlp=%d intf=%d "
                        "posle_out_ms=%d pechat_ipp=%d pauza_s=%d mps=%d mps_out=%d\nформаты принтера: %s\n"
                        "получено байт: %u\n---\n",
                        zagolovok_, usb_pr_ipp_osh()[0] ? usb_pr_ipp_osh() : "нет",
                        usb_pr_ipp_dump()[0] ? usb_pr_ipp_dump() : "-", g_ipp.rx, g_ipp.sbros, g_ipp.setif, g_ipp.zlp,
                        g_ipp.intf, g_ipp.posle_out_ms, g_ipp.pechat_ipp, g_ipp.pauza_s, usb_pr_ipp_mps_tek(),
                        g_ipp.mps_out,
                        s_formaty[0] ? s_formaty : "?", (unsigned)n);
    if (w >= sizeof(s_otladka)) w = sizeof(s_otladka) - 1;
    for (size_t i = 0; i < n && w + 6 < sizeof(s_otladka); i++) {
        uint8_t c = b[i];
        if (c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7f)) s_otladka[w++] = c;
        else w += snprintf(s_otladka + w, sizeof(s_otladka) - w, "\\x%02x", c);
    }
    s_otladka[w < sizeof(s_otladka) ? w : sizeof(s_otladka) - 1] = 0;
}

/* Отправить готовый запрос (b, n байт; b — буфер OTV_MAX) и разобрать ответ.
 * Тело IPP ответа остаётся в b + *telo длиной *dl (для прохода /ipp_raw). */
static bool obmen(uint8_t *b, size_t n, ipp_t *o, bool formaty, const char *chto, size_t *telo, long *dl)
{
    size_t pol = 0;
    memset(&s_http, 0, sizeof(s_http));
    bool est = false;
    if (telo) *telo = 0;
    if (dl) *dl = -1;
    bool otvet = n && usb_pr_ipp_zapros(b, n, b, OTV_MAX, &pol, otvet_ves);
    char z[64];
    if (otvet) {
        uint8_t kopiya[160];
        size_t kn = pol < sizeof(kopiya) ? pol : sizeof(kopiya);
        memcpy(kopiya, b, kn);   /* для отладки — до сборки chunked на месте */
        long d = telo_ipp(b, pol);
        est = d > 0 && razobrat_ipp(b + s_http.telo, d, o, formaty);
        if (telo) *telo = s_http.telo;
        if (dl) *dl = d;
        snprintf(z, sizeof(z), "%s: %s", chto, est ? "ок" : "ответ не разобран");
        otladka(z, kopiya, kn);
    } else {
        snprintf(z, sizeof(z), "%s: нет ответа", chto);
        otladka(z, b, pol);
    }
    return est;
}

/* спросить состояние принтера; true — ответ разобран и успешен */
static bool sprosit(ipp_t *o, bool formaty)
{
    uint8_t *b = malloc(OTV_MAX);
    if (!b) return false;
    bool est = obmen(b, zapros_sostoyaniya(b, OTV_MAX), o, formaty, "Get-Printer-Attributes", NULL, NULL)
               && o->status <= 0xff;
    free(b);
    return est;
}

/* состояние задания (op 0x0009) или отмена (0x0008) */
static bool rabota(uint16_t op, int job_id, ipp_t *o)
{
    uint8_t *b = malloc(OTV_MAX);
    if (!b) return false;
    bool est = obmen(b, zapros_raboty(b, OTV_MAX, op, job_id), o, false,
                     op == 0x0008 ? "Cancel-Job" : "Get-Job-Attributes", NULL, NULL) && o->status <= 0xff;
    free(b);
    return est;
}

/* ---------- проход /ipp_raw: любой IPP-запрос с ноутбука в принтер ---------- */
#define PROHOD_MAX 2048
static uint8_t s_prohod_zapros[PROHOD_MAX];
static size_t s_prohod_n = 0;
static uint8_t *s_prohod_otvet = NULL;    /* malloc(OTV_MAX), отдаёт страница */
static long s_prohod_dl = -1;
static volatile bool s_prohod_nado = false;
static SemaphoreHandle_t s_prohod_gotovo = NULL;

/* из httpd: положить запрос, дождаться задачи печати; вернуть тело ответа (не освобождать) */
const uint8_t *ipp_prohod(const uint8_t *zapr, size_t n, long *dl, uint32_t ms)
{
    if (!s_prohod_gotovo) s_prohod_gotovo = xSemaphoreCreateBinary();
    if (n > PROHOD_MAX || s_prohod_nado) { *dl = -1; return NULL; }
    memcpy(s_prohod_zapros, zapr, n);
    s_prohod_n = n;
    xSemaphoreTake(s_prohod_gotovo, 0);
    s_prohod_nado = true;
    s_nado = true;   /* разбудить: задача печати спрашивает принтер между заданиями */
    if (xSemaphoreTake(s_prohod_gotovo, pdMS_TO_TICKS(ms)) != pdTRUE) { *dl = -1; return NULL; }
    *dl = s_prohod_dl;
    return s_prohod_dl > 0 ? s_prohod_otvet : NULL;
}

/* из задачи печати */
static void prohod_vypolnit(void)
{
    if (!s_prohod_nado) return;
    if (!s_prohod_otvet) s_prohod_otvet = malloc(OTV_MAX);
    s_prohod_dl = -1;
    if (s_prohod_otvet) {
        uint8_t *b = malloc(OTV_MAX);
        if (b) {
            size_t h = http(b, OTV_MAX, s_prohod_n);
            if (h && h + s_prohod_n <= OTV_MAX) {
                memcpy(b + h, s_prohod_zapros, s_prohod_n);
                ipp_t o = {0};
                size_t telo = 0;
                long dl = -1;
                obmen(b, h + s_prohod_n, &o, false, "проход /ipp_raw", &telo, &dl);
                if (dl > 0 && (size_t)dl <= OTV_MAX) { memcpy(s_prohod_otvet, b + telo, dl); s_prohod_dl = dl; }
            }
            free(b);
        }
    }
    s_prohod_nado = false;
    if (s_prohod_gotovo) xSemaphoreGive(s_prohod_gotovo);
}

static const char *job_sost_txt(int s)
{
    switch (s) {
    case 3: return "ждёт";
    case 4: return "задержано";
    case 5: return "печатается";
    case 6: return "остановлено";
    case 7: return "отменено";
    case 8: return "прервано";
    case 9: return "напечатано";
    default: return "?";
    }
}

/* ---------- состояние для сервера и страницы ---------- */

static void osnova(const char *p, char *dst, size_t len)
{
    strlcpy(dst, p, len);
    static const char *suf[] = {"-error", "-warning", "-report"};
    for (int i = 0; i < 3; i++) {
        size_t a = strlen(dst), b = strlen(suf[i]);
        if (a > b && !strcmp(dst + a - b, suf[i])) { dst[a - b] = 0; break; }
    }
}

static const char *po_russki(const char *o)
{
    static const struct { const char *k, *t; } sl[] = {
        {"media-empty", "нет бумаги"}, {"media-needed", "нет бумаги"}, {"media-low", "мало бумаги"},
        {"media-jam", "замятие бумаги"}, {"toner-low", "мало тонера"}, {"toner-empty", "тонер закончился"},
        {"marker-supply-low", "мало тонера"}, {"marker-supply-empty", "тонер закончился"},
        {"door-open", "открыта крышка"}, {"cover-open", "открыта крышка"},
        {"input-tray-missing", "нет лотка"}, {"output-area-full", "выходной лоток полон"},
        {"offline", "принтер офлайн"}, {"paused", "на паузе"}, {"shutdown", "выключается"},
    };
    for (size_t i = 0; i < sizeof(sl) / sizeof(sl[0]); i++) if (!strcmp(o, sl[i].k)) return sl[i].t;
    return NULL;
}

static void dobavit(char *s, size_t len, const char *fmt, const char *zn)
{
    size_t l = strlen(s);
    if (l < len) snprintf(s + l, len - l, fmt, zn);
}

static int s_prn_sost = 0;   /* printer-state из последнего опроса: 3 idle, 4 busy, 5 stop, 0 — не ответил */
int ipp_sost_printera(void) { return s_prn_sost; }

void ipp_obnovit(void)
{
    if (!usb_pr_gotov()) { s_kratko[0] = 0; s_tekst[0] = 0; s_prn_sost = 0; return; }
    if (usb_pr_ipp_mps_nado()) usb_pr_ipp_primenit_mps();
    prohod_vypolnit();   /* запрос с /ipp_raw, если есть */

    char kr[220] = "", tx[300] = "";
    if (s_test_leg) {
        s_test_leg = false;
        char t[140];
        usb_pr_test_leg(t, sizeof(t));
        ESP_LOGI(TAG, "🔧 проверка приёма: %s", t);
        snprintf(s_otladka, sizeof(s_otladka), "проверка приёма legacy IN: %s\n(«нет ответа» = приём исправен, "
                 "данных просто нет)\nбуфер: %s", t, usb_pr_ipp_dump()[0] ? usb_pr_ipp_dump() : "-");
        return;
    }

    uint8_t port = 0;
    if (usb_pr_port(&port)) {
        snprintf(kr, sizeof(kr), "port=%02x", port);
        if (port & 0x20) dobavit(tx, sizeof(tx), "%s", "нет бумаги (по USB-порту); ");
        if (!(port & 0x08)) dobavit(tx, sizeof(tx), "%s", "ошибка принтера (по USB-порту); ");
    }

    ipp_t o = {0};
    bool est = sprosit(&o, s_formaty[0] == 0);
    s_prn_sost = est ? o.sost : 0;
    if (!est) {
        char osh_ascii[60] = "";
        size_t j = 0;
        for (const char *s = usb_pr_ipp_osh(); *s && j + 1 < sizeof(osh_ascii); s++)
            if (isalnum((unsigned char)*s) || *s == '_') osh_ascii[j++] = *s;
            else if (j && osh_ascii[j - 1] != '-' && (*s == ' ' || *s == ':' || *s == ',')) osh_ascii[j++] = '-';
        osh_ascii[j] = 0;
        dobavit(kr, sizeof(kr), "%sipp=net", kr[0] ? "; " : "");
        if (osh_ascii[0]) dobavit(kr, sizeof(kr), "(%s)", osh_ascii);
    } else {
        char ur[40] = "";
        for (int i = 0; i < o.n_urovnej; i++) {
            char z[12];
            int v = o.urovni[i];
            if (v >= 0) snprintf(z, sizeof(z), "%d", v);
            else strlcpy(z, v == -3 ? "est" : "?", sizeof(z));
            dobavit(ur, sizeof(ur), i ? "/%s" : "%s", z);
        }
        const char *s = o.sost == 3 ? "idle" : o.sost == 4 ? "busy" : o.sost == 5 ? "stop" : "?";
        dobavit(kr, sizeof(kr), "%stoner=", kr[0] ? "; " : "");
        dobavit(kr, sizeof(kr), "%s", ur[0] ? ur : "?");
        dobavit(kr, sizeof(kr), "; prn=%s", s);
        if (g_ipp.pechat_ipp) dobavit(kr, sizeof(kr), "%s", "; pechat=ipp");

        if (o.n_urovnej) {
            char t[60];
            int v = o.urovni[0];
            if (v >= 0) snprintf(t, sizeof(t), "тонер %d%%; ", v);
            else strlcpy(t, v == -3 ? "тонер есть; " : "тонер: неизвестно; ", sizeof(t));
            dobavit(tx, sizeof(tx), "%s", t);
        }
        if (o.sost == 5) dobavit(tx, sizeof(tx), "%s", "принтер остановлен; ");
        bool pervaya = true;
        for (int i = 0; i < o.n_prichin; i++) {
            char os[48];
            osnova(o.prichiny[i], os, sizeof(os));
            if (!strcmp(os, "none") || strstr(o.prichiny[i], "-report")) continue;
            dobavit(kr, sizeof(kr), pervaya ? "; prichiny=%s" : ",%s", os);
            pervaya = false;
            const char *ru = po_russki(os);
            dobavit(tx, sizeof(tx), "%s; ", ru ? ru : os);
        }
    }
    size_t l = strlen(tx);
    if (l >= 2 && !strcmp(tx + l - 2, "; ")) tx[l - 2] = 0;
    if (!tx[0] && est) strlcpy(tx, "всё в порядке", sizeof(tx));

    if (strcmp(kr, s_kratko)) ESP_LOGI(TAG, "📩 состояние принтера: %s (%s)", tx[0] ? tx : "-", kr);
    strlcpy(s_kratko, kr, sizeof(s_kratko));
    strlcpy(s_tekst, tx, sizeof(s_tekst));
}

/* После того как весь PWG ушёл в OUT IPP: прочитать ответ Print-Job и дождаться,
 * пока принтер напечатает (printer-state снова idle). Канал IPP открыт вызывающим. */
bool ipp_zavershit_pechat(char *osh, size_t len)
{
    uint8_t *b = malloc(OTV_MAX);
    if (!b) { snprintf(osh, len, "IPP: нет памяти"); return false; }
    memset(&s_http, 0, sizeof(s_http));
    size_t pol = 0;
    bool ok = usb_pr_ipp_otvet(b, OTV_MAX, &pol, otvet_ves, 30000);
    if (!ok) {
        snprintf(osh, len, "IPP Print-Job: нет ответа (%s)", usb_pr_ipp_osh());
        otladka("Print-Job: нет ответа", b, pol);
        free(b);
        return false;
    }
    uint8_t kopiya[160];
    size_t kn = pol < sizeof(kopiya) ? pol : sizeof(kopiya);
    memcpy(kopiya, b, kn);
    long dl = telo_ipp(b, pol);
    ipp_t o = {0};
    bool est = dl > 0 && razobrat_ipp(b + s_http.telo, dl, &o, false);
    otladka(est ? "Print-Job: ответ получен" : "Print-Job: ответ не разобран", kopiya, kn);
    free(b);
    if (!est) { snprintf(osh, len, "IPP Print-Job: ответ не разобран"); return false; }
    if (o.status > 0xff) { snprintf(osh, len, "IPP Print-Job: принтер отказал, статус 0x%04x", o.status); return false; }
    int job = o.job_id;
    ESP_LOGI(TAG, "✅ Print-Job принят (статус 0x%04x), задание принтера %d: %s%s%s", o.status, job,
             job_sost_txt(o.job_sost), o.job_prichina[0] ? ", " : "", o.job_prichina);
    usb_pr_kanal_ipp(false);

    /* 13.11.2026: HP принимал задание, говорил «свободен» и печатал его позже — вместе
     * с повторами. Поэтому «напечатано» — только когда ЭТО задание стало completed. */
    if (job > 0) {
        int64_t do_ = esp_timer_get_time() + 180LL * 1000000;
        int posl = o.job_sost;
        char prich[64];
        strlcpy(prich, o.job_prichina, sizeof(prich));
        vTaskDelay(pdMS_TO_TICKS(1000));
        while (esp_timer_get_time() < do_) {
            esp_task_wdt_reset();
            ipp_t j = {0};
            if (rabota(0x0009, job, &j)) {
                if (j.job_sost != posl || strcmp(j.job_prichina, prich)) {
                    ESP_LOGI(TAG, "📩 задание принтера %d: %s%s%s", job, job_sost_txt(j.job_sost),
                             j.job_prichina[0] ? ", " : "", j.job_prichina);
                    posl = j.job_sost;
                    strlcpy(prich, j.job_prichina, sizeof(prich));
                }
                if (j.job_sost == 9) { ESP_LOGI(TAG, "✅ задание принтера %d напечатано", job); return true; }
                if (j.job_sost == 7 || j.job_sost == 8) {
                    snprintf(osh, len, "принтер %s задание %d%s%s", j.job_sost == 7 ? "отменил" : "прервал", job,
                             j.job_prichina[0] ? ": " : "", j.job_prichina);
                    return false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        /* зависло: отменить на принтере, иначе вылезет потом дубликатом после повтора */
        ipp_t c = {0};
        bool otm = rabota(0x0008, job, &c);
        snprintf(osh, len, "задание %d не напечаталось за 3 мин (%s%s%s)%s", job, job_sost_txt(posl),
                 prich[0] ? ", " : "", prich, otm ? " — отменено на принтере" : " — отменить не удалось");
        ESP_LOGW(TAG, "⚠️ %s", osh);
        return false;
    }

    /* номер задания не пришёл — по-старому: ждём, пока принтер освободится; до 3 минут */
    ESP_LOGW(TAG, "⚠️ в ответе Print-Job нет job-id — жду, пока принтер освободится");
    int64_t do_ = esp_timer_get_time() + 180LL * 1000000;
    vTaskDelay(pdMS_TO_TICKS(2000));
    while (esp_timer_get_time() < do_) {
        esp_task_wdt_reset();
        ipp_t s = {0};
        if (sprosit(&s, false)) {
            if (s.sost == 3) { ESP_LOGI(TAG, "✅ принтер снова свободен — задание напечатано"); return true; }
            if (s.sost == 5) {
                char p[60] = "";
                for (int i = 0; i < s.n_prichin && !p[0]; i++)
                    if (strcmp(s.prichiny[i], "none")) osnova(s.prichiny[i], p, sizeof(p));
                const char *ru = po_russki(p);
                snprintf(osh, len, "принтер остановился: %s", ru ? ru : (p[0] ? p : "?"));
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    snprintf(osh, len, "IPP: принтер не освободился за 3 минуты");
    return false;
}
