/*
 * USB Host: ищем у устройства интерфейс класса 0x07 (принтер) с протоколом
 * 0x02 (двунаправленный) или 0x01 (однонаправленный) — legacy, сырой PWG raster,
 * и интерфейсы с протоколом 0x04 — IPP-over-USB (HTTP/IPP). С 1.6 печатаем по IPP
 * (Print-Job: явный конец задания и «напечатано» от принтера), legacy — запасной путь.
 * У HP M28a IPP — alt 1 интерфейсов 1–3, пакеты по 512 при заявленных 64 (см. otkryt).
 *
 * Потоки: задача usb_lib крутит библиотеку, задача usb_klient обрабатывает
 * события и колбэки передач. Передачи отправляет задача печати и ждёт семафор.
 * Закрыть устройство можно только когда передачи нет — поэтому флаг v_rabote.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "obshchee.h"

static const char *TAG = "usb";

static usb_host_client_handle_t s_klient = NULL;
static SemaphoreHandle_t s_mux;          /* короткая защита полей ниже */
static SemaphoreHandle_t s_gotovo;       /* колбэк передачи → задача печати */
static usb_transfer_t *s_xfer = NULL;    /* bulk OUT, USB_PR_KUSOK байт */
static usb_transfer_t *s_ctrl = NULL;    /* управляющие запросы */

static uint8_t s_novyj_adr = 0;          /* пришёл NEW_DEV, ещё не открыт */
static usb_device_handle_t s_dev = NULL;
static bool s_intf_zanyat = false;
static uint8_t s_intf = 0, s_alt = 0, s_ep_out = 0, s_ep_in = 0;
static uint16_t s_mps = 64;
static volatile bool s_gotov = false;    /* интерфейс захвачен, можно печатать */
static volatile bool s_ushel = false;    /* DEV_GONE пришёл */
static volatile bool s_v_rabote = false; /* передача в полёте */
static char s_opis[200] = "нет устройства";

/* IPP-over-USB (протокол 0x04): только чтобы спрашивать у принтера тонер/бумагу.
 * Печать идёт по-прежнему через legacy-интерфейс выше. */
/* У HP M28a их три (интерфейсы 1–3, alt 1); ipp-usb держит их пулом.
 * Берём все и, если один не ответил, в следующий раз пробуем другой. */
#define IPP_MAX 3
typedef struct { uint8_t intf, alt, out, in; bool zanyat, alt_vybran; } ipp_if_t;
static ipp_if_t s_ipp[IPP_MAX];
static int s_n_ipp = 0, s_ipp_tek = 0;
static char s_ipp_osh[120] = "";         /* последняя ошибка обмена — для /ipp */
static bool s_zlp_dalee = false;         /* следующей передаче в usb_pr_zapisat — ZLP в конце */
static int s_mps_in = 64;                /* MPS каналов IPP IN (меняется /ipp?mps=…) */
static void ipp_pravit_mps(const usb_config_desc_t *cfg, const ipp_if_t *p, int mps);
static usb_transfer_t *s_ipp_tx = NULL;  /* bulk OUT: HTTP-запрос */
static usb_transfer_t *s_ipp_rx = NULL;  /* bulk IN: ответ кусками */
static SemaphoreHandle_t s_ipp_sem;
#define IPP_TX_MAX 1024
#define IPP_RX_KUSOK 512                 /* кратно MPS 64 */

static volatile int64_t s_podkl_us = 0;  /* когда принтер последний раз подключился (захвачен) */

bool usb_pr_gotov(void) { return s_gotov; }
int64_t usb_pr_podkl_us(void) { return s_podkl_us; }
uint8_t *usb_pr_bufer(void) { return s_xfer ? s_xfer->data_buffer : NULL; }
const char *usb_pr_opisanie(void) { return s_opis; }

static void kolbek(usb_transfer_t *t) { xSemaphoreGive((SemaphoreHandle_t)t->context); }

/* Управляющий запрос синхронно (из задачи usb_klient или печати — колбэк
 * вызывается в usb_klient, поэтому из usb_klient звать НЕЛЬЗЯ: зависнем). */
static SemaphoreHandle_t s_ctrl_sem;
static esp_err_t upravl(uint8_t bm, uint8_t req, uint16_t val, uint16_t idx, uint16_t dlina,
                        uint8_t *otvet, size_t *poluch)
{
    usb_setup_packet_t *sp = (usb_setup_packet_t *)s_ctrl->data_buffer;
    sp->bmRequestType = bm;
    sp->bRequest = req;
    sp->wValue = val;
    sp->wIndex = idx;
    sp->wLength = dlina;
    s_ctrl->num_bytes = sizeof(usb_setup_packet_t) + dlina;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = kolbek;
    s_ctrl->context = s_ctrl_sem;
    s_ctrl->timeout_ms = 2000;
    xSemaphoreTake(s_ctrl_sem, 0);
    esp_err_t e = usb_host_transfer_submit_control(s_klient, s_ctrl);
    if (e != ESP_OK) return e;
    if (xSemaphoreTake(s_ctrl_sem, pdMS_TO_TICKS(3000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return ESP_FAIL;
    size_t n = s_ctrl->actual_num_bytes > sizeof(usb_setup_packet_t) ? s_ctrl->actual_num_bytes - sizeof(usb_setup_packet_t) : 0;
    if (otvet && n) memcpy(otvet, s_ctrl->data_buffer + sizeof(usb_setup_packet_t), n);
    if (poluch) *poluch = n;
    return ESP_OK;
}

/* Разбор конфигурации: печатаем всё и выбираем интерфейс принтера. */
static bool vybrat_interfejs(const usb_config_desc_t *cfg)
{
    int luchshij_proto = 0;
    int off = 0;
    s_n_ipp = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
        const usb_intf_desc_t *in = (const usb_intf_desc_t *)d;
        ESP_LOGI(TAG, "  интерфейс %d alt %d: класс 0x%02x подкласс 0x%02x протокол 0x%02x, конечных точек %d",
                 in->bInterfaceNumber, in->bAlternateSetting, in->bInterfaceClass,
                 in->bInterfaceSubClass, in->bInterfaceProtocol, in->bNumEndpoints);
        uint8_t out = 0, inp = 0;
        uint16_t mps = 64;
        for (int i = 0; i < in->bNumEndpoints; i++) {
            int eoff = off;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(in, i, cfg->wTotalLength, &eoff);
            if (!ep) break;
            bool bulk = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK;
            ESP_LOGI(TAG, "    точка 0x%02x %s, тип %d, MPS %d", ep->bEndpointAddress,
                     (ep->bEndpointAddress & 0x80) ? "IN" : "OUT", ep->bmAttributes & 3, ep->wMaxPacketSize);
            if (!bulk) continue;
            if (ep->bEndpointAddress & 0x80) { if (!inp) inp = ep->bEndpointAddress; }
            else if (!out) { out = ep->bEndpointAddress; mps = ep->wMaxPacketSize; }
        }
        if (in->bInterfaceClass != 0x07 || in->bInterfaceSubClass != 0x01 || !out) continue;
        int proto = in->bInterfaceProtocol;
        if (proto == 0x04) {
            if (inp && s_n_ipp < IPP_MAX) {
                ipp_if_t *p = &s_ipp[s_n_ipp++];
                memset(p, 0, sizeof(*p));
                p->intf = in->bInterfaceNumber;
                p->alt = in->bAlternateSetting;
                p->out = out;
                p->in = inp;
                ESP_LOGI(TAG, "    IPP-over-USB: интерфейс %d alt %d — для состояния принтера", p->intf, p->alt);
            }
            continue;
        }
        if (proto != 0x01 && proto != 0x02) {
            ESP_LOGI(TAG, "    протокол 0x%02x — пропускаю", proto);
            continue;
        }
        /* 0x02 предпочтительнее 0x01; при равенстве — первый (обычно alt 0) */
        if (proto > luchshij_proto) {
            luchshij_proto = proto;
            s_intf = in->bInterfaceNumber;
            s_alt = in->bAlternateSetting;
            s_ep_out = out;
            s_ep_in = inp;
            s_mps = mps;
        }
    }
    if (luchshij_proto) {
        ESP_LOGI(TAG, "✅ выбран интерфейс %d alt %d, протокол 0x%02x, OUT 0x%02x, IN 0x%02x",
                 s_intf, s_alt, luchshij_proto, s_ep_out, s_ep_in);
        /* IPP как другой alt того же интерфейса одновременно с печатью не взять */
        int k = 0;
        for (int i = 0; i < s_n_ipp; i++) {
            if (s_ipp[i].intf == s_intf) { ESP_LOGW(TAG, "⚠️ IPP на интерфейсе печати %d — пропускаю", s_intf); continue; }
            s_ipp[k++] = s_ipp[i];
        }
        s_n_ipp = k;
        s_ipp_tek = 0;
        return true;
    }
    return false;
}

static void zakryt(void)
{
    s_gotov = false;
    if (s_dev) {
        for (int i = 0; i < s_n_ipp; i++) {
            ipp_if_t *p = &s_ipp[i];
            if (!p->zanyat) continue;
            usb_host_endpoint_halt(s_dev, p->out);
            usb_host_endpoint_flush(s_dev, p->out);
            usb_host_endpoint_halt(s_dev, p->in);
            usb_host_endpoint_flush(s_dev, p->in);
            usb_host_interface_release(s_klient, s_dev, p->intf);
            p->zanyat = false;
            p->alt_vybran = false;
        }
        if (s_intf_zanyat) {
            usb_host_endpoint_halt(s_dev, s_ep_out);
            usb_host_endpoint_flush(s_dev, s_ep_out);
            usb_host_interface_release(s_klient, s_dev, s_intf);
        }
        usb_host_device_close(s_klient, s_dev);
    }
    s_dev = NULL;
    s_intf_zanyat = false;
    s_n_ipp = 0;
    s_mps_in = 64;     /* новое устройство — новое описание, без наших правок */
    strlcpy(s_opis, "нет устройства", sizeof(s_opis));
}

/* Открыть устройство. Только разбор и claim — без управляющих запросов
 * (их колбэки идут через эту же задачу). ID принтера спросим из задачи печати. */
static bool s_nado_id = false;
static void otkryt(uint8_t adr)
{
    if (usb_host_device_open(s_klient, adr, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "❌ не открылось устройство %d", adr);
        s_dev = NULL;
        return;
    }
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cfg;
    usb_device_info_t info;
    usb_host_get_device_descriptor(s_dev, &dd);
    usb_host_device_info(s_dev, &info);
    ESP_LOGI(TAG, "📩 устройство %d: VID 0x%04x PID 0x%04x, класс 0x%02x, USB %x.%02x, скорость %s, MPS0 %d",
             adr, dd->idVendor, dd->idProduct, dd->bDeviceClass, dd->bcdUSB >> 8, dd->bcdUSB & 0xff,
             info.speed == USB_SPEED_FULL ? "full" : (info.speed == USB_SPEED_LOW ? "low" : "?"), dd->bMaxPacketSize0);
    usb_print_device_descriptor(dd);
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "❌ нет конфигурации");
        snprintf(s_opis, sizeof(s_opis), "%04x:%04x без конфигурации", dd->idVendor, dd->idProduct);
        return;
    }
    ESP_LOGI(TAG, "  конфигурация: %d байт, интерфейсов %d, питание %s", cfg->wTotalLength, cfg->bNumInterfaces,
             (cfg->bmAttributes & 0x40) ? "своё" : "от шины");
    usb_print_config_descriptor(cfg, NULL);
    if (!vybrat_interfejs(cfg)) {
        ESP_LOGE(TAG, "❌ у устройства нет интерфейса принтера 0x07/0x01|0x02");
        snprintf(s_opis, sizeof(s_opis), "%04x:%04x — не принтер (нет legacy-интерфейса)", dd->idVendor, dd->idProduct);
        return;
    }
    esp_err_t e = usb_host_interface_claim(s_klient, s_dev, s_intf, s_alt);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "❌ claim интерфейса %d: %s", s_intf, esp_err_to_name(e));
        snprintf(s_opis, sizeof(s_opis), "%04x:%04x — claim не удался", dd->idVendor, dd->idProduct);
        return;
    }
    s_intf_zanyat = true;
    s_xfer->device_handle = s_dev;
    s_xfer->bEndpointAddress = s_ep_out;
    snprintf(s_opis, sizeof(s_opis), "%04x:%04x if%d alt%d out%02x mps%d", dd->idVendor, dd->idProduct,
             s_intf, s_alt, s_ep_out, s_mps);

    /* IPP-интерфейс — необязательный: не вышло, печать всё равно работает */
    /* HP M28a шлёт на IPP пакеты по 512 при заявленных 64 (на full-speed это babble).
     * Сразу настраиваем каналы IN на больший пакет: честные 64-байтные пакеты другого
     * принтера при этом тоже принимаются — как короткие. Проверено 12.11.2026. */
    if (g_ipp.mps_ipp > 64) {
        for (int i = 0; i < s_n_ipp; i++) ipp_pravit_mps(cfg, &s_ipp[i], g_ipp.mps_ipp);
        s_mps_in = g_ipp.mps_ipp;
    }
    bool pervyj = true;
    for (int i = 0; i < s_n_ipp; i++) {
        ipp_if_t *p = &s_ipp[i];
        e = usb_host_interface_claim(s_klient, s_dev, p->intf, p->alt);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "⚠️ claim IPP-интерфейса %d alt %d: %s", p->intf, p->alt, esp_err_to_name(e));
            continue;
        }
        p->zanyat = true;
        size_t l = strlen(s_opis);
        snprintf(s_opis + l, sizeof(s_opis) - l, pervyj ? " ipp%d" : ",%d", p->intf);
        pervyj = false;
    }
    s_nado_id = true;
    s_podkl_us = esp_timer_get_time();
    s_gotov = true;
}

static void sobytie(const usb_host_client_event_msg_t *m, void *arg)
{
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_novyj_adr = m->new_dev.address;
    } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "⚠️ устройство отключено");
        s_ushel = true;
        s_gotov = false;
    }
}

static void zadacha_lib(void *arg)
{
    for (;;) {
        uint32_t f;
        usb_host_lib_handle_events(portMAX_DELAY, &f);
        if (f & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) usb_host_device_free_all();
    }
}

static void zadacha_klient(void *arg)
{
    for (;;) {
        usb_host_client_handle_events(s_klient, pdMS_TO_TICKS(200));
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_ushel && !s_v_rabote) {
            zakryt();
            s_ushel = false;
        }
        if (s_novyj_adr && !s_dev && !s_v_rabote) {
            uint8_t a = s_novyj_adr;
            s_novyj_adr = 0;
            otkryt(a);
        }
        xSemaphoreGive(s_mux);
    }
}

/* IEEE-1284 Device ID: модель и язык принтера. Лишь для журнала. */
static void sprosit_id(void)
{
    uint8_t b[260];
    size_t n = 0;
    /* GET_DEVICE_ID: class, IN, interface; wValue = индекс конфигурации (0),
     * wIndex = (интерфейс << 8) | alt */
    if (upravl(0xA1, 0x00, 0, (s_intf << 8) | s_alt, 255, b, &n) == ESP_OK && n > 2) {
        size_t dl = ((size_t)b[0] << 8) | b[1];
        if (dl > n) dl = n;
        char id[200];
        size_t k = 0;
        for (size_t i = 2; i < dl && k + 1 < sizeof(id); i++) id[k++] = (b[i] >= 0x20 && b[i] < 0x7f) ? b[i] : '.';
        id[k] = 0;
        ESP_LOGI(TAG, "📩 IEEE-1284 ID: %s", id);
        /* в описание (уходит на сервер в X-Plata) — только MDL */
        const char *mdl = strstr(id, "MDL:");
        if (mdl) {
            char m[60];
            size_t j = 0;
            for (mdl += 4; *mdl && *mdl != ';' && j + 1 < sizeof(m); mdl++) m[j++] = *mdl;
            m[j] = 0;
            size_t l = strlen(s_opis);
            snprintf(s_opis + l, sizeof(s_opis) - l, " %s", m);
        }
    } else {
        ESP_LOGW(TAG, "⚠️ GET_DEVICE_ID не ответил");
    }
    uint8_t st = 0;
    if (upravl(0xA1, 0x01, 0, s_intf, 1, &st, &n) == ESP_OK && n == 1) {
        ESP_LOGI(TAG, "📩 состояние порта 0x%02x: %s%s%s", st, (st & 0x20) ? "НЕТ БУМАГИ " : "",
                 (st & 0x10) ? "выбран " : "", (st & 0x08) ? "без ошибок" : "ОШИБКА");
    }
}

void usb_pr_sbros(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev || !s_intf_zanyat) { xSemaphoreGive(s_mux); return; }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);
    /* SOFT_RESET (класс принтера): bmRequestType 0x21, wIndex = интерфейс.
     * Linux usblp шлёт с recipient=other (0x23); пробуем оба. */
    if (upravl(0x21, 0x02, 0, s_intf, 0, NULL, NULL) != ESP_OK)
        upravl(0x23, 0x02, 0, s_intf, 0, NULL, NULL);
    s_v_rabote = false;
    ESP_LOGW(TAG, "⚠️ мягкий сброс принтера отправлен");
}

int usb_pr_zapisat(size_t n, uint32_t timeout_ms, char *err, size_t errlen)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev) {
        xSemaphoreGive(s_mux);
        snprintf(err, errlen, "USB: принтер не подключён");
        return -1;
    }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);

    if (s_nado_id) { s_nado_id = false; sprosit_id(); }

    const uint8_t ep = s_xfer->bEndpointAddress;   /* legacy OUT или OUT IPP (usb_pr_kanal_ipp) */
    s_xfer->num_bytes = n;
    s_xfer->callback = kolbek;
    s_xfer->context = s_gotovo;
    s_xfer->timeout_ms = timeout_ms;
    s_xfer->flags = s_zlp_dalee ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    s_zlp_dalee = false;
    xSemaphoreTake(s_gotovo, 0);
    int rez = 0;
    esp_err_t e = usb_host_transfer_submit(s_xfer);
    if (e != ESP_OK) {
        snprintf(err, errlen, "USB: передача не ушла (%s)", esp_err_to_name(e));
        rez = -1;
    } else if (xSemaphoreTake(s_gotovo, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        /* принтер не принимает данные: отменяем передачу и чистим точку */
        snprintf(err, errlen, "USB: принтер не принял данные за %u с", (unsigned)(timeout_ms / 1000));
        usb_host_endpoint_halt(s_dev, ep);
        usb_host_endpoint_flush(s_dev, ep);
        xSemaphoreTake(s_gotovo, pdMS_TO_TICKS(2000));  /* колбэк отменённой передачи */
        usb_host_endpoint_clear(s_dev, ep);
        rez = -1;
    } else if (s_xfer->status != USB_TRANSFER_STATUS_COMPLETED || s_xfer->actual_num_bytes != (int)n) {
        snprintf(err, errlen, "USB: передача статус %d, ушло %d из %u", s_xfer->status,
                 s_xfer->actual_num_bytes, (unsigned)n);
        if (s_xfer->status == USB_TRANSFER_STATUS_STALL) {
            /* STALL: снять halt на точке (CLEAR_FEATURE ENDPOINT_HALT) */
            upravl(0x02, 0x01, 0, ep, 0, NULL, NULL);
            usb_host_endpoint_halt(s_dev, ep);
            usb_host_endpoint_flush(s_dev, ep);
            usb_host_endpoint_clear(s_dev, ep);
        }
        rez = -1;
    }
    s_v_rabote = false;
    return rez;
}

/* Отменить зависшую передачу на точке IPP и вернуть точку в строй. */
static void ipp_sbros(uint8_t ep, bool v_polete)
{
    usb_host_endpoint_halt(s_dev, ep);
    usb_host_endpoint_flush(s_dev, ep);
    if (v_polete) xSemaphoreTake(s_ipp_sem, pdMS_TO_TICKS(2000));   /* колбэк отменённой */
    usb_host_endpoint_clear(s_dev, ep);
}

static const char *status_txt(usb_transfer_status_t s)
{
    switch (s) {
    case USB_TRANSFER_STATUS_COMPLETED: return "ok";
    case USB_TRANSFER_STATUS_ERROR: return "ERROR";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "TIMED_OUT";
    case USB_TRANSFER_STATUS_CANCELED: return "CANCELED";
    case USB_TRANSFER_STATUS_STALL: return "STALL";
    case USB_TRANSFER_STATUS_OVERFLOW: return "OVERFLOW";
    case USB_TRANSFER_STATUS_SKIPPED: return "SKIPPED";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "NO_DEVICE";
    default: return "?";
    }
}

/* Ручки отладки IPP: меняются со страницы /ipp, живут до перезагрузки. */
/* По умолчанию — то, что заработало с HP M28a 12.11.2026: IPP IN по 512, печать Print-Job. */
ipp_ruchki_t g_ipp = {.rx = 512, .sbros = 1, .setif = 1, .zlp = 1, .intf = -1,
                      .posle_out_ms = 0, .pechat_ipp = 1, .pauza_s = 20, .mps_out = 0, .mps_ipp = 512};
static char s_ipp_dump[300] = "";        /* что легло в буфер приёма при ошибке */
const char *usb_pr_ipp_dump(void) { return s_ipp_dump; }

#define METKA 0xA5   /* заполнитель буфера приёма: видно, писал ли туда DMA */

static void zapomnit_bufer(const usb_transfer_t *t)
{
    int n = t->num_bytes, chuzhih = 0, posl = -1;
    for (int i = 0; i < n; i++) if (t->data_buffer[i] != METKA) { chuzhih++; posl = i; }
    int w = snprintf(s_ipp_dump, sizeof(s_ipp_dump), "буфер %d байт: не-метка %d, последняя на %d; начало:", n, chuzhih, posl);
    for (int i = 0; i < 48 && i < n && w + 4 < (int)sizeof(s_ipp_dump); i++)
        w += snprintf(s_ipp_dump + w, sizeof(s_ipp_dump) - w, " %02x", t->data_buffer[i]);
}

/* 0 — ок; иначе текст причины в s_ipp_osh. STALL снимаем и на принтере. */
static bool zhdat_ipp(usb_transfer_t *t, uint32_t ms, const char *chto)
{
    t->callback = kolbek;
    t->context = s_ipp_sem;
    bool vhod = t->bEndpointAddress & 0x80;
    if (vhod) memset(t->data_buffer, METKA, t->num_bytes);
    xSemaphoreTake(s_ipp_sem, 0);
    esp_err_t e = usb_host_transfer_submit(t);
    if (e != ESP_OK) {
        snprintf(s_ipp_osh, sizeof(s_ipp_osh), "%s ep%02x: submit %s", chto, t->bEndpointAddress, esp_err_to_name(e));
        return false;
    }
    if (xSemaphoreTake(s_ipp_sem, pdMS_TO_TICKS(ms)) != pdTRUE) {
        snprintf(s_ipp_osh, sizeof(s_ipp_osh), "%s ep%02x: нет ответа за %u мс", chto, t->bEndpointAddress, (unsigned)ms);
        ipp_sbros(t->bEndpointAddress, true);
        return false;
    }
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        snprintf(s_ipp_osh, sizeof(s_ipp_osh), "%s ep%02x: статус %s, байт %d", chto, t->bEndpointAddress,
                 status_txt(t->status), t->actual_num_bytes);
        if (vhod) zapomnit_bufer(t);
        if (t->status == USB_TRANSFER_STATUS_STALL)
            upravl(0x02, 0x01, 0, t->bEndpointAddress, 0, NULL, NULL);   /* CLEAR_FEATURE ENDPOINT_HALT */
        ipp_sbros(t->bEndpointAddress, false);
        return false;
    }
    return true;
}

const char *usb_pr_ipp_osh(void) { return s_ipp_osh; }

/* Включить у принтера alt IPP (SET_INTERFACE). IPP у HP — alt 1 интерфейса со
 * служебным каналом HP (alt 0): claim лишь занимает точки. Шлём из задачи печати,
 * не из otkryt(): колбэк управляющего запроса идёт через usb_klient. */
static bool ipp_alt(ipp_if_t *p)
{
    if (p->alt_vybran) return true;
    if (g_ipp.setif) {
        esp_err_t e = upravl(0x01, 0x0B, p->alt, p->intf, 0, NULL, NULL);
        if (e != ESP_OK) {
            snprintf(s_ipp_osh, sizeof(s_ipp_osh), "SET_INTERFACE %d alt %d: %s", p->intf, p->alt, esp_err_to_name(e));
            return false;
        }
    }
    if (g_ipp.sbros) {   /* у принтера toggle-биты сброшены — сбрасываем и у себя */
        ipp_sbros(p->out, false);
        ipp_sbros(p->in, false);
    }
    p->alt_vybran = true;
    ESP_LOGI(TAG, "✅ IPP: интерфейс %d alt %d (setif=%d sbros=%d)", p->intf, p->alt, g_ipp.setif, g_ipp.sbros);
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

void usb_pr_ipp_zanovo(void)   /* /ipp?reset=1: в следующий раз снова SET_INTERFACE */
{
    for (int i = 0; i < s_n_ipp; i++) s_ipp[i].alt_vybran = false;
}

/* HP на IPP-интерфейсах шлёт пакеты длиннее, чем обещает описание (64 на full-speed) —
 * похоже, по 512, как на high-speed. Контроллер считает это babble (OVERFLOW).
 * Обход: отпустить IPP-интерфейсы, поправить wMaxPacketSize в описании конфигурации
 * (оно в RAM у драйвера) и занять их снова — канал настроится на больший пакет. */
/* размер пакета в описании точек IPP-интерфейса (IN; OUT — если g_ipp.mps_out) */
static void ipp_pravit_mps(const usb_config_desc_t *cfg, const ipp_if_t *p, int mps)
{
    int off = 0;
    const usb_intf_desc_t *in = usb_parse_interface_descriptor(cfg, p->intf, p->alt, &off);
    for (int k = 0; in && k < in->bNumEndpoints; k++) {
        int eoff = off;
        usb_ep_desc_t *ep = (usb_ep_desc_t *)usb_parse_endpoint_descriptor_by_index(in, k, cfg->wTotalLength, &eoff);
        if (!ep) break;
        if ((ep->bEndpointAddress & 0x80) || g_ipp.mps_out) ep->wMaxPacketSize = mps;
    }
}

static volatile int s_nado_mps = 0;
void usb_pr_ipp_mps(int mps) { s_nado_mps = mps; }
int usb_pr_ipp_mps_tek(void) { return s_mps_in; }
bool usb_pr_ipp_mps_nado(void) { return s_nado_mps != 0; }

void usb_pr_ipp_primenit_mps(void)
{
    int mps = s_nado_mps;
    s_nado_mps = 0;
    if (mps != 64 && mps != 128 && mps != 256 && mps != 512) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev) { xSemaphoreGive(s_mux); return; }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);

    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK) { s_v_rabote = false; return; }
    int zanyato = 0;
    for (int i = 0; i < s_n_ipp; i++) {
        ipp_if_t *p = &s_ipp[i];
        if (p->zanyat) {
            usb_host_endpoint_halt(s_dev, p->out); usb_host_endpoint_flush(s_dev, p->out);
            usb_host_endpoint_halt(s_dev, p->in);  usb_host_endpoint_flush(s_dev, p->in);
            usb_host_interface_release(s_klient, s_dev, p->intf);
            p->zanyat = false;
        }
        p->alt_vybran = false;
        ipp_pravit_mps(cfg, p, mps);
        esp_err_t e = usb_host_interface_claim(s_klient, s_dev, p->intf, p->alt);
        if (e == ESP_OK) { p->zanyat = true; zanyato++; }
        else ESP_LOGW(TAG, "⚠️ повторный claim IPP %d: %s", p->intf, esp_err_to_name(e));
    }
    s_mps_in = mps;
    snprintf(s_ipp_osh, sizeof(s_ipp_osh), "MPS IPP IN=%d%s, занято интерфейсов %d", mps,
             g_ipp.mps_out ? " (и OUT)" : "", zanyato);
    ESP_LOGI(TAG, "🔧 %s", s_ipp_osh);
    s_v_rabote = false;
}

static ipp_if_t *ipp_tek(void)
{
    if (g_ipp.intf >= 0) {
        for (int i = 0; i < s_n_ipp; i++) if (s_ipp[i].intf == g_ipp.intf && s_ipp[i].zanyat) return &s_ipp[i];
        snprintf(s_ipp_osh, sizeof(s_ipp_osh), "IPP-интерфейс %d не занят", g_ipp.intf);
        return NULL;
    }
    for (int k = 0; k < s_n_ipp; k++) {
        ipp_if_t *p = &s_ipp[(s_ipp_tek + k) % s_n_ipp];
        if (p->zanyat) return p;
    }
    strlcpy(s_ipp_osh, "нет IPP-интерфейса", sizeof(s_ipp_osh));
    return NULL;
}

static void ipp_tochki(ipp_if_t *p)
{
    s_ipp_tx->device_handle = s_dev;
    s_ipp_tx->bEndpointAddress = p->out;
    s_ipp_rx->device_handle = s_dev;
    s_ipp_rx->bEndpointAddress = p->in;
}

/* Читать HTTP-ответ с IPP IN, пока gotovo() не скажет «весь». */
static bool ipp_prinyat(uint8_t *otv, size_t max, size_t *poluch, bool (*gotovo)(const uint8_t *, size_t),
                        uint32_t vsego_ms)
{
    *poluch = 0;
    int rx = g_ipp.rx;
    if (rx < s_mps_in) rx = s_mps_in;              /* IN — кратно MPS канала */
    if (rx > IPP_RX_KUSOK) rx = IPP_RX_KUSOK;
    rx -= rx % s_mps_in;
    int64_t do_ = esp_timer_get_time() + (int64_t)vsego_ms * 1000;
    while (esp_timer_get_time() < do_) {
        s_ipp_rx->num_bytes = rx;
        s_ipp_rx->flags = 0;
        uint32_t ost = (uint32_t)((do_ - esp_timer_get_time()) / 1000);
        if (!zhdat_ipp(s_ipp_rx, ost < 100 ? 100 : ost, "ответ")) {
            size_t l = strlen(s_ipp_osh);
            snprintf(s_ipp_osh + l, sizeof(s_ipp_osh) - l, ", получено %u", (unsigned)*poluch);
            return false;
        }
        size_t k = s_ipp_rx->actual_num_bytes;
        if (k > max - *poluch) k = max - *poluch;
        memcpy(otv + *poluch, s_ipp_rx->data_buffer, k);
        *poluch += k;
        if (gotovo(otv, *poluch)) return true;
        if (*poluch >= max) { snprintf(s_ipp_osh, sizeof(s_ipp_osh), "ответ больше %u байт", (unsigned)max); return false; }
    }
    snprintf(s_ipp_osh, sizeof(s_ipp_osh), "ответ неполный за %u с, получено %u", (unsigned)(vsego_ms / 1000),
             (unsigned)*poluch);
    return false;
}

/* Один обмен «запрос — ответ» по одному IPP-интерфейсу. */
static bool ipp_obmen(ipp_if_t *p, const uint8_t *zapr, size_t n, uint8_t *otv, size_t max, size_t *poluch,
                      bool (*gotovo)(const uint8_t *, size_t))
{
    *poluch = 0;
    if (!ipp_alt(p)) return false;
    ipp_tochki(p);
    memcpy(s_ipp_tx->data_buffer, zapr, n);
    s_ipp_tx->num_bytes = n;
    s_ipp_tx->flags = g_ipp.zlp ? USB_TRANSFER_FLAG_ZERO_PACK : 0;   /* длина кратна 64 — принтер ждал бы конца */
    if (!zhdat_ipp(s_ipp_tx, 3000, "запрос")) return false;
    if (g_ipp.posle_out_ms > 0) vTaskDelay(pdMS_TO_TICKS(g_ipp.posle_out_ms));
    return ipp_prinyat(otv, max, poluch, gotovo, 6000);
}

bool usb_pr_ipp_zapros(const uint8_t *zapr, size_t n, uint8_t *otv, size_t max, size_t *poluch,
                       bool (*gotovo)(const uint8_t *, size_t))
{
    *poluch = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev || !s_n_ipp || n > IPP_TX_MAX) {
        xSemaphoreGive(s_mux);
        if (!s_n_ipp) strlcpy(s_ipp_osh, "нет IPP-интерфейса", sizeof(s_ipp_osh));
        return false;
    }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);

    /* текущий интерфейс; не ответил — следующий, по кругу (если интерфейс не задан ручкой) */
    bool ok = false;
    int obhod = g_ipp.intf >= 0 ? 1 : s_n_ipp;
    for (int popytka = 0; popytka < obhod && !ok; popytka++) {
        ipp_if_t *p = ipp_tek();
        if (!p) break;
        s_ipp_dump[0] = 0;
        ok = ipp_obmen(p, zapr, n, otv, max, poluch, gotovo);
        if (ok) s_ipp_osh[0] = 0;
        else {
            ESP_LOGW(TAG, "⚠️ IPP интерфейс %d: %s", p->intf, s_ipp_osh);
            if (g_ipp.intf < 0) s_ipp_tek = (int)((p - s_ipp) + 1) % s_n_ipp;
        }
    }
    s_v_rabote = false;
    return ok;
}

/* ---- печать через IPP: тот же s_xfer, но в OUT IPP-интерфейса ---- */
static ipp_if_t *s_kanal = NULL;

bool usb_pr_kanal_ipp(bool da)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!da || !s_gotov || !s_dev) {
        s_kanal = NULL;
        if (s_xfer) s_xfer->bEndpointAddress = s_ep_out;
        xSemaphoreGive(s_mux);
        return !da;
    }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);
    ipp_if_t *p = ipp_tek();
    bool ok = p && ipp_alt(p);
    if (ok) { s_kanal = p; s_xfer->bEndpointAddress = p->out; ipp_tochki(p); }
    s_v_rabote = false;
    return ok;
}

void usb_pr_zlp_dalee(void) { s_zlp_dalee = g_ipp.zlp; }

bool usb_pr_ipp_otvet(uint8_t *otv, size_t max, size_t *poluch, bool (*gotovo)(const uint8_t *, size_t),
                      uint32_t ms)
{
    *poluch = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev || !s_kanal) { xSemaphoreGive(s_mux); strlcpy(s_ipp_osh, "канал IPP не открыт", sizeof(s_ipp_osh)); return false; }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);
    ipp_tochki(s_kanal);
    bool ok = ipp_prinyat(otv, max, poluch, gotovo, ms);
    s_v_rabote = false;
    return ok;
}

/* Проверка: работает ли вообще приём с принтера — IN legacy-интерфейса (0x82).
 * Данных там обычно нет: «нет ответа» (NAK) = приём исправен; OVERFLOW = беда в приёме вообще. */
void usb_pr_test_leg(char *rez, size_t len)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev || !s_ep_in) { xSemaphoreGive(s_mux); snprintf(rez, len, "нет legacy IN"); return; }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);
    s_ipp_rx->device_handle = s_dev;
    s_ipp_rx->bEndpointAddress = s_ep_in;
    s_ipp_rx->num_bytes = 64;
    s_ipp_rx->flags = 0;
    if (zhdat_ipp(s_ipp_rx, 1500, "legacy IN")) snprintf(rez, len, "legacy IN 0x%02x: пришло %d байт", s_ep_in, s_ipp_rx->actual_num_bytes);
    else snprintf(rez, len, "%s", s_ipp_osh);
    s_v_rabote = false;
}

bool usb_pr_port(uint8_t *st)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_gotov || !s_dev || !s_intf_zanyat) { xSemaphoreGive(s_mux); return false; }
    s_v_rabote = true;
    xSemaphoreGive(s_mux);
    size_t n = 0;
    bool ok = upravl(0xA1, 0x01, 0, s_intf, 1, st, &n) == ESP_OK && n == 1;
    s_v_rabote = false;
    return ok;
}

void usb_pr_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_gotovo = xSemaphoreCreateBinary();
    s_ctrl_sem = xSemaphoreCreateBinary();
    s_ipp_sem = xSemaphoreCreateBinary();
    usb_host_config_t hc = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&hc));
    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = sobytie, .callback_arg = NULL},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cc, &s_klient));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(USB_PR_KUSOK, 0, &s_xfer));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 512, 0, &s_ctrl));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(IPP_TX_MAX, 0, &s_ipp_tx));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(IPP_RX_KUSOK, 0, &s_ipp_rx));
    xTaskCreatePinnedToCore(zadacha_lib, "usb_lib", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(zadacha_klient, "usb_klient", 5120, NULL, 9, NULL, 0);

    /* принтер мог быть воткнут до старта: подбираем уже перечисленные */
    vTaskDelay(pdMS_TO_TICKS(1500));
    uint8_t adr[4];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(adr), adr, &n) == ESP_OK && n > 0 && !s_novyj_adr) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (!s_dev) s_novyj_adr = adr[0];
        xSemaphoreGive(s_mux);
    }
    ESP_LOGI(TAG, "✅ USB Host запущен, жду принтер на порту USB (OTG)");
}
