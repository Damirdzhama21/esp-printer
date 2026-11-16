/*
 * Плата ESP32-S3 у принтера: Wi-Fi → USB мост: печать из бота на обычный USB-принтер.
 * Общие объявления модулей.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define FW_VERSIYA        "esp-printer 1.9"
#define AP_SSID           "ESP-Printer"
#define AP_PAROL          "printer2026"
#define SERVER_PO_UMOLCH  ""

/* ---- настройки в NVS (nastr.c) ---- */
typedef struct {
    char ssid[33];
    char parol[65];
    char server[129];
    char token[96];
} nastr_t;

extern nastr_t g_nastr;
esp_err_t nastr_zagruzit(void);
esp_err_t nastr_sohranit(const nastr_t *n);
esp_err_t nastr_steret(void);
bool nastr_polnye(void);   /* есть сеть, токен и адрес сервера */

/* ---- состояние платы, по нему горит светодиод ---- */
typedef enum {
    SOST_START = 0,
    SOST_NET_WIFI,      /* синий мигает: нет Wi-Fi / открыт портал */
    SOST_NET_PRINTERA,  /* красный горит: принтер не найден */
    SOST_GOTOV,         /* зелёный горит: всё на связи */
    SOST_PECHAT,        /* белый горит: идёт печать */
    SOST_OSHIBKA,       /* красный мигает: сервер/токен/ошибка печати */
    SOST_SBROS,         /* частое мигание всеми: стираю настройки */
    SOST_PRIGREV,       /* жёлтый мигает: принтер только что подключился, жду «готов» */
} sost_t;

void sost_postavit(sost_t s);
sost_t sost_tekushchee(void);
const char *sost_tekst(void);
void posl_oshibka_postavit(const char *fmt, ...);
const char *posl_oshibka(void);

/* ---- Wi-Fi и портал (set.c, portal.c) ---- */
void set_start(void);
bool set_est(void);          /* STA получила IP */
bool set_portal_otkryt(void);
int  set_rssi(void);
const char *set_ip(void);
void portal_start_http(void);   /* httpd: портал на AP и страница состояния в локалке */
void portal_dns_start(void);
void portal_dns_stop(void);
/* последний результат сканирования сетей в JSON-массив (для страницы) */
size_t set_skan_json(char *buf, size_t len, bool novyj);

/* ---- USB принтер (usb_pr.c) ---- */
void usb_pr_start(void);
bool usb_pr_gotov(void);
int64_t usb_pr_podkl_us(void);  /* esp_timer, когда принтер последний раз подключился */
int ipp_sost_printera(void);    /* printer-state из последнего ipp_obnovit: 3 idle, 0 — не ответил */
/* отправить n байт из буфера передачи; 0 = ок, иначе текст ошибки в err */
int  usb_pr_zapisat(size_t n, uint32_t timeout_ms, char *err, size_t errlen);
uint8_t *usb_pr_bufer(void);    /* буфер передачи (DMA), USB_PR_KUSOK байт */
#define USB_PR_KUSOK (16 * 1024)
void usb_pr_sbros(void);        /* мягкий сброс принтера после оборванного задания */
/* IPP-over-USB: отправить HTTP-запрос и читать ответ, пока gotovo() не скажет «весь» */
bool usb_pr_ipp_zapros(const uint8_t *zapr, size_t n, uint8_t *otv, size_t max, size_t *poluch,
                       bool (*gotovo)(const uint8_t *, size_t));
const char *usb_pr_ipp_osh(void);   /* последняя ошибка обмена по IPP (пусто — ок) */
const char *usb_pr_ipp_dump(void);  /* что легло в буфер приёма при последней ошибке */
void usb_pr_ipp_zanovo(void);       /* снова слать SET_INTERFACE при следующем обмене */
void usb_pr_test_leg(char *rez, size_t len);  /* проверка приёма на legacy IN */
/* печать через IPP: переключить s_xfer на OUT IPP (true) или обратно на legacy (false) */
bool usb_pr_kanal_ipp(bool da);
void usb_pr_zlp_dalee(void);        /* следующей usb_pr_zapisat — ZLP в конце (последний кусок) */
bool usb_pr_ipp_otvet(uint8_t *otv, size_t max, size_t *poluch, bool (*gotovo)(const uint8_t *, size_t),
                      uint32_t ms);

/* ручки отладки IPP: меняются со страницы /ipp?ключ=значение, до перезагрузки */
typedef struct {
    int rx;            /* размер одного приёма с IN, кратно 64 (64..512) */
    int sbros;         /* сбрасывать точки у себя после SET_INTERFACE */
    int setif;         /* слать SET_INTERFACE на alt IPP */
    int zlp;           /* ZLP после запроса/последнего куска */
    int intf;          /* -1 — перебирать; иначе номер IPP-интерфейса */
    int posle_out_ms;  /* пауза между запросом и чтением ответа */
    int pechat_ipp;    /* 1 — печатать заданием IPP Print-Job, 0 — сырым потоком в legacy */
    int pauza_s;       /* legacy: пауза перед следующим заданием, чтобы принтер закрыл прошлое */
    int mps_out;       /* при mps=…: править размер пакета и у OUT, не только у IN */
    int mps_ipp;       /* размер пакета IPP IN при подключении принтера (HP: 512; 64 — как в описании) */
} ipp_ruchki_t;
/* /ipp?mps=512: перезанять IPP-интерфейсы с другим wMaxPacketSize (обход babble у HP) */
void usb_pr_ipp_mps(int mps);
bool usb_pr_ipp_mps_nado(void);
void usb_pr_ipp_primenit_mps(void);   /* из задачи печати */
int usb_pr_ipp_mps_tek(void);
extern ipp_ruchki_t g_ipp;
bool usb_pr_port(uint8_t *st);  /* GET_PORT_STATUS: 0x20 нет бумаги, 0x10 выбран, 0x08 без ошибок */

/* ---- состояние принтера по IPP (ipp.c) ---- */
void ipp_obnovit(void);         /* спросить принтер; звать из задачи печати между заданиями */
const char *ipp_kratko(void);   /* ASCII для X-Plata: toner=63; prn=idle; prichiny=... */
const char *ipp_tekst(void);    /* по-русски для страницы платы */
const char *ipp_otladka(void);  /* для /ipp: ошибка и начало сырого ответа */
void ipp_zaprosit(void);        /* попросить задачу печати спросить принтер сейчас */
void ipp_zaprosit_test(void);   /* ...и вместо опроса проверить приём на legacy IN */
/* печать через IPP: заголовок Print-Job (HTTP+IPP) перед PWG; после — ответ и ожидание конца */
size_t ipp_zagolovok_pechati(uint8_t *b, size_t max, size_t dlina_doc);
bool ipp_zavershit_pechat(char *osh, size_t len);
/* проход /ipp_raw: IPP-запрос (без HTTP) с ноутбука в принтер; ответ — тело IPP или NULL */
const uint8_t *ipp_prohod(const uint8_t *zapr, size_t n, long *dl, uint32_t ms);
bool ipp_nado(void);            /* просили? (и сбросить просьбу) */
const char *usb_pr_opisanie(void); /* VID:PID и IEEE-1284 ID для X-Plata */

/* ---- цикл печати (pechat.c) ---- */
void pechat_start(void);

/* ---- журнал в памяти для страницы /log (zhurnal.c) ---- */
void zhurnal_start(void);
size_t zhurnal_kopiya(char *buf, size_t len);
