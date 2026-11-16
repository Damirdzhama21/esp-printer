/*
 * Журнал в памяти: последние ~6 КБ лога. Его видно на странице http://<ip>/log —
 * у заказчика нет UART-консоли, а разбираться с USB-дескрипторами принтера надо.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "obshchee.h"

#define RAZMER 6144
static char buf[RAZMER];
static size_t poz = 0;
static bool polnyj = false;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t prezhnij = NULL;

static int moj_vprintf(const char *fmt, va_list ap)
{
    char s[256];
    va_list kop;
    va_copy(kop, ap);
    int n = vsnprintf(s, sizeof(s), fmt, kop);
    va_end(kop);
    if (n > 0) {
        if (n > (int)sizeof(s) - 1) n = sizeof(s) - 1;
        portENTER_CRITICAL(&mux);
        for (int i = 0; i < n; i++) {
            buf[poz++] = s[i];
            if (poz >= RAZMER) { poz = 0; polnyj = true; }
        }
        portEXIT_CRITICAL(&mux);
    }
    return prezhnij ? prezhnij(fmt, ap) : vprintf(fmt, ap);
}

void zhurnal_start(void)
{
    prezhnij = esp_log_set_vprintf(moj_vprintf);
}

size_t zhurnal_kopiya(char *dst, size_t len)
{
    if (!len) return 0;
    size_t n = 0;
    portENTER_CRITICAL(&mux);
    if (polnyj) {
        for (size_t i = poz; i < RAZMER && n < len - 1; i++) dst[n++] = buf[i];
    }
    for (size_t i = 0; i < poz && n < len - 1; i++) dst[n++] = buf[i];
    portEXIT_CRITICAL(&mux);
    dst[n] = 0;
    return n;
}
