/*
 * esp-pechat: облачный принт-сервер на ESP32-S3 для HP LaserJet M28a.
 *
 * Плата сама стучится на сервер печати (как агент ноутбука), забирает задание
 * уже готовым PWG raster и перекладывает байты в USB-принтер. Драйвера на плате
 * нет и не нужно: растр готовит сервер (ghostscript).
 *
 * Светодиод (WS2812 на GPIO48):
 *   синий мигает   — нет Wi-Fi, открыта точка ESP-Printer для настройки
 *   красный горит  — принтер не найден по USB
 *   зелёный горит  — всё готово, жду задания
 *   белый горит    — печатаю
 *   красный мигает — ошибка сервера/токена/печати (подробности на http://<ip>/)
 *   жёлтый мигает  — принтер только что подключился, жду от него «готов» (до 5 мин)
 * Кнопка BOOT: держать 5 секунд (после включения) — стереть настройки.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "obshchee.h"

static const char *TAG = "main";

#define LED_GPIO    48
#define KNOPKA_GPIO 0
#define YARKOST     24   /* из 255: на столе ярче не надо */

static volatile sost_t g_sost = SOST_START;
static char g_oshibka[160] = "";
static led_strip_handle_t g_led = NULL;

void sost_postavit(sost_t s)
{
    if (g_sost != s) {
        static const char *imena[] = {"старт", "нет Wi-Fi", "нет принтера", "готов", "печать", "ошибка", "сброс", "прогрев принтера"};
        ESP_LOGI(TAG, "состояние: %s", imena[s]);
    }
    g_sost = s;
}
sost_t sost_tekushchee(void) { return g_sost; }
const char *sost_tekst(void)
{
    switch (g_sost) {
    case SOST_NET_WIFI: return "нет Wi-Fi (открыта точка ESP-Printer)";
    case SOST_NET_PRINTERA: return "принтер не найден по USB";
    case SOST_GOTOV: return "готов, жду задания";
    case SOST_PECHAT: return "печатаю";
    case SOST_OSHIBKA: return "ошибка";
    case SOST_SBROS: return "стираю настройки";
    case SOST_PRIGREV: return "принтер только что подключился — жду, пока скажет «готов»";
    default: return "запуск";
    }
}

void posl_oshibka_postavit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_oshibka, sizeof(g_oshibka), fmt, ap);
    va_end(ap);
    if (g_oshibka[0]) ESP_LOGE(TAG, "❌ %s", g_oshibka);
}
const char *posl_oshibka(void) { return g_oshibka; }

static void led(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_led) return;
    if (r | g | b) {
        led_strip_set_pixel(g_led, 0, r, g, b);
        led_strip_refresh(g_led);
    } else {
        led_strip_clear(g_led);
    }
}

static void zadacha_led(void *arg)
{
    const uint8_t Y = YARKOST;
    for (int t = 0;; t++) {
        bool vkl = (t % 10) < 5;              /* мигание 1 Гц при шаге 100 мс */
        switch (g_sost) {
        case SOST_NET_WIFI:     vkl ? led(0, 0, Y) : led(0, 0, 0); break;
        case SOST_NET_PRINTERA: led(Y, 0, 0); break;
        case SOST_GOTOV:        led(0, Y, 0); break;
        case SOST_PECHAT:       led(Y, Y, Y); break;
        case SOST_OSHIBKA:      vkl ? led(Y, 0, 0) : led(0, 0, 0); break;
        case SOST_SBROS:        (t & 1) ? led(Y, Y, Y) : led(0, 0, 0); break;
        case SOST_PRIGREV:      vkl ? led(Y, Y / 2, 0) : led(0, 0, 0); break;
        default:                led(Y / 2, Y / 2, 0); break;  /* жёлтый: запуск */
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void led_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &g_led) != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ светодиод не завёлся, работаю без него");
        g_led = NULL;
    }
    led(0, 0, 0);
}

/* Кнопка BOOT (GPIO0). При включении её зажимать нельзя — плата уйдёт в режим
 * прошивки. Поэтому сброс так: включить, дождаться огонька, держать 5 секунд. */
static void zadacha_knopka(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << KNOPKA_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int derzhim = 0;
    for (;;) {
        if (gpio_get_level(KNOPKA_GPIO) == 0) {
            if (++derzhim == 50) {    /* 50 × 100 мс */
                ESP_LOGW(TAG, "⚠️ BOOT 5 с — стираю настройки и перезагружаюсь");
                sost_postavit(SOST_SBROS);
                nastr_steret();
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
        } else {
            derzhim = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    zhurnal_start();
    /* «Certificate validated» на каждый запрос к серверу вытесняет из журнала всё полезное */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    ESP_LOGI(TAG, "✅ %s, причина перезагрузки %d, свободно %u байт",
             FW_VERSIYA, (int)esp_reset_reason(), (unsigned)esp_get_free_heap_size());

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    led_init();
    xTaskCreate(zadacha_led, "led", 2560, NULL, 3, NULL);
    xTaskCreate(zadacha_knopka, "knopka", 3072, NULL, 4, NULL);

    nastr_zagruzit();
    usb_pr_start();       /* USB Host раньше всего: принтер может быть уже воткнут */
    set_start();          /* Wi-Fi + портал/страница состояния */
    pechat_start();       /* цикл опроса сервера */
}
