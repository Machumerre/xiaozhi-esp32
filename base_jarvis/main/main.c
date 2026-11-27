/*
 * Base con orugas para Jarvis (cabeza SparkBot) — ESP32-C3 SuperMini.
 *
 * La cabeza manda órdenes de texto por el conector magnético (UART 115200, sin fin de línea):
 *   "x<giro> y<avance>"  joystick (-1..1). Ej.: "x0.0 y1.0" adelante, "x-1.0 y0.0" gira a la izquierda
 *   "d1"                 baile
 *   "w<n>"               modo de luces (2..8)
 * Como no sabemos qué pin del conector es el TX de la cabeza, se escucha en los dos (GPIO0 y GPIO1).
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "base";

// ------------------------------------------------------------------ pines
#define PIN_CABEZA_A 0      // a un pin de datos del conector magnético
#define PIN_CABEZA_B 1      // al otro pin de datos
#define PIN_IZQ_1 3         // DRV8833 AIN1
#define PIN_IZQ_2 4         // DRV8833 AIN2
#define PIN_DER_1 5         // DRV8833 BIN1
#define PIN_DER_2 6         // DRV8833 BIN2
#define PIN_LEDS 7          // tira WS2812 (barra de luz)
#define NUM_LEDS 6

// ------------------------------------------------------------------ ajustes
#define INVERTIR_IZQ 0      // pon 1 si la oruga izquierda va al revés (o da la vuelta a sus cables)
#define INVERTIR_DER 1      // los motores van en espejo, por eso uno empieza invertido
#define MARCHA_MS 800       // cuánto dura un "avanza" / "retrocede"
#define GIRO_MS 380         // cuánto dura un "gira"
#define PWM_MIN 0.38f       // por debajo de esto un N20 no arranca
#define RAMPA 0.06f         // cambio de velocidad por tick de 10 ms (arranques suaves)
#define BRILLO 70           // brillo máximo de los LED (0..255)

// ------------------------------------------------------------------ estado
static led_strip_handle_t tira;
static volatile float obj_izq, obj_der;      // velocidades objetivo (-1..1)
static float vel_izq, vel_der;               // velocidades actuales (con rampa)
static volatile int64_t fin_movimiento;      // us; 0 = sin límite
static volatile int modo_luz = 2;
static volatile bool bailando;

// ------------------------------------------------------------------ motores
static const int pines_motor[4] = {PIN_IZQ_1, PIN_IZQ_2, PIN_DER_1, PIN_DER_2};

static void motores_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t c = {
            .gpio_num = pines_motor[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
}

static void motor(int canal, float v, bool invertir) {
    if (invertir) v = -v;
    float a = fabsf(v);
    uint32_t duty = a < 0.02f ? 0 : (uint32_t)((PWM_MIN + (1.0f - PWM_MIN) * fminf(a, 1.0f)) * 1023);
    uint32_t d1 = v > 0 ? duty : 0, d2 = v < 0 ? duty : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, canal, d1);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, canal);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, canal + 1, d2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, canal + 1);
}

static float acercar(float v, float objetivo) {
    if (v < objetivo) return fminf(v + RAMPA, objetivo);
    return fmaxf(v - RAMPA, objetivo);
}

static float limitar(float v) { return fmaxf(-1.0f, fminf(1.0f, v)); }

static void mover(float giro, float avance, int ms) {
    obj_izq = limitar(avance + giro);
    obj_der = limitar(avance - giro);
    fin_movimiento = ms > 0 ? esp_timer_get_time() + (int64_t)ms * 1000 : 0;
}

static void parar(void) { mover(0, 0, 0); }

// ------------------------------------------------------------------ baile
typedef struct { float giro, avance; int ms; } paso_t;

static void tarea_baile(void *arg) {
    static const paso_t pasos[] = {
        {-1, 0, 180}, {1, 0, 180}, {-1, 0, 180}, {1, 0, 180},
        {0, 0.8f, 250}, {0, -0.8f, 250},
        {1, 0, 900},
        {0, 0, 150}, {-0.6f, 0.6f, 300}, {0.6f, 0.6f, 300}, {0, -0.7f, 400},
        {-1, 0, 900}, {0, 0, 0},
    };
    int modo_previo = modo_luz;
    bailando = true;
    modo_luz = 5;
    for (size_t i = 0; i < sizeof(pasos) / sizeof(pasos[0]) && bailando; i++) {
        mover(pasos[i].giro, pasos[i].avance, 0);
        vTaskDelay(pdMS_TO_TICKS(pasos[i].ms > 0 ? pasos[i].ms : 10));
    }
    parar();
    modo_luz = modo_previo;
    bailando = false;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------------ órdenes
static void ejecutar(const char *s) {
    ESP_LOGI(TAG, "orden: %s", s);
    const char *px = strchr(s, 'x'), *py = strchr(s, 'y');
    if (px || py) {
        float giro = px ? strtof(px + 1, NULL) : 0, avance = py ? strtof(py + 1, NULL) : 0;
        bailando = false;
        if (fabsf(giro) < 0.01f && fabsf(avance) < 0.01f) parar();
        else mover(giro, avance, fabsf(avance) > 0.01f ? MARCHA_MS : GIRO_MS);
        return;
    }
    const char *pd = strchr(s, 'd');
    if (pd && !bailando) {
        xTaskCreate(tarea_baile, "baile", 3072, NULL, 5, NULL);
        return;
    }
    const char *pw = strchr(s, 'w');
    if (pw) {
        int m = atoi(pw + 1);
        if (m >= 2 && m <= 8) modo_luz = m;
    }
}

typedef struct { uart_port_t puerto; int pin; } enlace_t;

static void tarea_enlace(void *arg) {
    const enlace_t *e = arg;
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(e->puerto, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(e->puerto, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(e->puerto, UART_PIN_NO_CHANGE, e->pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(e->pin);

    char linea[64];
    int n = 0;
    uint8_t b[32];
    while (true) {
        // La cabeza no manda fin de línea: una orden termina cuando hay 20 ms de silencio
        int r = uart_read_bytes(e->puerto, b, sizeof(b), pdMS_TO_TICKS(20));
        for (int i = 0; i < r; i++) {
            char c = (char)b[i];
            if (c == '\n' || c == '\r' || c == 0) continue;
            if (c < 32 || c > 126) { n = 0; continue; }   // basura (pin equivocado o ruido)
            if (n < (int)sizeof(linea) - 1) linea[n++] = c;
        }
        if (r == 0 && n > 0) {
            linea[n] = 0;
            ejecutar(linea);
            n = 0;
        }
    }
}

// ------------------------------------------------------------------ luces
static void pixel(int i, float r, float g, float b) {
    led_strip_set_pixel(tira, i, (uint32_t)(fminf(r, 1) * BRILLO), (uint32_t)(fminf(g, 1) * BRILLO),
                        (uint32_t)(fminf(b, 1) * BRILLO));
}

static void arcoiris(float h, float *r, float *g, float *b) {
    h = fmodf(h, 1.0f) * 6;
    float x = 1 - fabsf(fmodf(h, 2) - 1);
    int s = (int)h;
    float c[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    *r = c[s][0], *g = c[s][1], *b = c[s][2];
}

static void luces(float t) {
    const float cr = 0.37f, cg = 0.89f, cb = 1.0f;   // cian Jarvis
    bool girando = fabsf(vel_izq - vel_der) > 0.5f;
    bool avanzando = !girando && (fabsf(vel_izq) > 0.05f || fabsf(vel_der) > 0.05f);
    for (int i = 0; i < NUM_LEDS; i++) {
        float p = (float)i / (NUM_LEDS - 1), r = 0, g = 0, b = 0;
        if (girando && !bailando) {
            // intermitente del lado hacia el que gira (ámbar)
            bool izq = vel_der > vel_izq;
            bool lado = izq ? p < 0.5f : p > 0.5f;
            float on = fmodf(t, 0.4f) < 0.2f ? 1 : 0.05f;
            if (lado) r = on, g = on * 0.45f;
            else r = cr * 0.15f, g = cg * 0.15f, b = cb * 0.15f;
        } else if (avanzando && !bailando) {
            // flujo de luz hacia fuera desde el centro
            float d = fabsf(p - 0.5f) * 2, o = 0.5f + 0.5f * sinf(t * 18 - d * 6 * (vel_izq > 0 ? 1 : -1));
            r = cr * o, g = cg * o, b = cb * o;
        } else switch (modo_luz) {
            case 2: { float o = 0.18f + 0.5f * (0.5f + 0.5f * sinf(t * 1.6f)); r = cr * o, g = cg * o, b = cb * o; break; }
            case 3: r = cr, g = cg, b = cb; break;
            case 4: { float c = 0.5f + 0.5f * sinf(t * 3.2f), o = fmaxf(0, 1 - fabsf(p - c) * 3.5f);
                      r = cr * o, g = cg * o, b = cb * o; break; }
            case 5: arcoiris(t * 0.4f + p * 0.6f, &r, &g, &b); break;
            case 6: r = g = b = 1; break;
            case 7: { bool f = fmodf(t, 0.5f) < 0.25f; if ((p < 0.5f) == f) r = 1; else b = 1; break; }
            default: break;   // 8 = apagado
        }
        pixel(i, r, g, b);
    }
    led_strip_refresh(tira);
}

static void luces_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LEDS,
        .max_leds = NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &tira));
    led_strip_clear(tira);
}

// ------------------------------------------------------------------ arranque
void app_main(void) {
    motores_init();
    luces_init();

    // animación de encendido: la barra se llena de cian
    for (int k = 0; k <= NUM_LEDS; k++) {
        for (int i = 0; i < NUM_LEDS; i++) pixel(i, i < k ? 0.37f : 0, i < k ? 0.89f : 0, i < k ? 1 : 0);
        led_strip_refresh(tira);
        vTaskDelay(pdMS_TO_TICKS(70));
    }

    static const enlace_t enlaces[2] = {{UART_NUM_0, PIN_CABEZA_A}, {UART_NUM_1, PIN_CABEZA_B}};
    xTaskCreate(tarea_enlace, "enlace_a", 3072, (void *)&enlaces[0], 6, NULL);
    xTaskCreate(tarea_enlace, "enlace_b", 3072, (void *)&enlaces[1], 6, NULL);
    ESP_LOGI(TAG, "Base Jarvis lista");

    int tick = 0;
    while (true) {
        if (fin_movimiento && esp_timer_get_time() > fin_movimiento) parar();
        vel_izq = acercar(vel_izq, obj_izq);
        vel_der = acercar(vel_der, obj_der);
        motor(0, vel_izq, INVERTIR_IZQ);
        motor(2, vel_der, INVERTIR_DER);
        if (++tick % 3 == 0) luces(esp_timer_get_time() / 1e6f);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
