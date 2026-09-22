#!/usr/bin/env python3
"""Host regression checks for the production LED manager and UART TX path.

Run from WSL: python3 tests/led_activity_host_test.py
Uses mocked GPIO, clock and scheduler; does not validate the physical board.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

MOCK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int gpio_num_t;
typedef int portMUX_TYPE;
typedef int uart_port_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_NUM_NC -1
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static int64_t mock_now;
static int levels[49];
static unsigned gpio_writes;
static int64_t esp_timer_get_time(void) { return mock_now; }
static int gpio_set_level(int pin, int level) {
    assert(pin >= 0 && pin < 49);
    levels[pin] = level;
    ++gpio_writes;
    return 0;
}
static int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
static void vTaskDelay(int delay) { (void)delay; }
static int xTaskCreate(void (*fn)(void*), const char *name, int stack, void *arg, int priority, void *out) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out;
    return pdPASS;
}
'''

TEST = r'''
#include "sx_led_manager.c"

/* Compile the production sendDataToUart function with a controllable driver. */
#define TX_WAIT_PRIMARY_MS 1000
#define TX_WAIT_RETRY_MS 500
static int write_result, write_calls, wait_result;
static void update_tx_mirror(const uint8_t *data, size_t len) { (void)data; (void)len; }
static int uart_write_bytes(int uart, const char *data, size_t len) {
    (void)uart; (void)data; (void)len; ++write_calls; return write_result;
}
static int uart_wait_tx_done(int uart, int timeout) {
    (void)uart; (void)timeout; assert(levels[LED_COM1] == 1); return wait_result;
}
static void uart_tx_led_on(int uart) { (void)uart; sx_led_manager_serial_begin(LED_COM1); }
static void uart_tx_led_off(int uart) { (void)uart; sx_led_manager_serial_end(LED_COM1); }
static void send_uart_to_websocket_from_port(const uint8_t *data, size_t len, bool tx, int uart) {
    (void)data; (void)len; (void)tx; (void)uart;
    /* Slow logging must not keep an activity LED on. */
    mock_now += 200000;
    serial_led_update();
    assert(levels[LED_COM1] == 0);
}
UART_TX_FUNCTION

static void advance(int64_t us) { mock_now += us; serial_led_update(); }

int main(void) {
    memset(levels, -1, sizeof(levels));
    assert(sx_led_manager_init() == ESP_OK);
    const int pins[] = {LED_COM1, LED_COM2, LED_232};
    for (unsigned i = 0; i < 3; ++i) assert(levels[pins[i]] == 0);
    puts("PASS idle lamps are off at boot (active-high)");

    /* Each interface must light alone and retain a short RX pulse. */
    for (unsigned i = 0; i < 3; ++i) {
        sx_led_manager_serial_activity(pins[i]);
        for (unsigned j = 0; j < 3; ++j) assert(levels[pins[j]] == (i == j));
        advance(99999);
        assert(levels[pins[i]] == 1);
        advance(1);
        assert(levels[pins[i]] == 0);
    }
    puts("PASS three independent RX indicators and 100 ms expiry");

    sx_led_manager_serial_activity(LED_COM1);
    advance(80000);
    sx_led_manager_serial_activity(LED_COM1);
    advance(20000);
    assert(levels[LED_COM1] == 1);
    advance(80000);
    assert(levels[LED_COM1] == 0);
    puts("PASS new data extends visibility past the previous deadline");

    /* Two overlapping transfers share one lamp, while RX refreshes it. */
    sx_led_manager_serial_begin(LED_COM1);
    sx_led_manager_serial_begin(LED_COM1);
    sx_led_manager_serial_end(LED_COM1);
    advance(500000);
    assert(levels[LED_COM1] == 1);
    sx_led_manager_serial_end(LED_COM1);
    advance(50000);
    sx_led_manager_serial_activity(LED_COM1);
    advance(50000);
    assert(levels[LED_COM1] == 1);
    advance(50000);
    assert(levels[LED_COM1] == 0);
    puts("PASS overlapping TX/RX cannot prematurely extinguish a shared lamp");

    /* RS422: COM1 TX and COM2 RX are separate physical indicators. */
    sx_led_manager_serial_begin(LED_COM1);
    sx_led_manager_serial_activity(LED_COM2);
    advance(100000);
    assert(levels[LED_COM1] == 1 && levels[LED_COM2] == 0 && levels[LED_232] == 0);
    sx_led_manager_serial_clear(LED_COM1);
    assert(levels[LED_COM1] == 0);
    sx_led_manager_serial_end(LED_COM1);
    advance(200000);
    assert(levels[LED_COM1] == 0);
    puts("PASS RS422 direction independence and stop cleanup");

    unsigned before = gpio_writes;
    sx_led_manager_serial_activity(GPIO_NUM_NC);
    sx_led_manager_serial_begin(LED_LAN);
    sx_led_manager_serial_end(LED_LAN);
    sx_led_manager_serial_clear(GPIO_NUM_NC);
    assert(gpio_writes == before);
    puts("PASS unavailable ports cannot affect another LED");

    for (unsigned t = 0; t < 8; ++t) {
        assert(state_level(SX_LED_NETWORK_OFF, t) == 0);
        assert(state_level(SX_LED_NETWORK_ONLINE, t) == 1);
        assert(state_level(SX_LED_NETWORK_STARTING, t) == (t % 8 < 4));
        assert(state_level(SX_LED_NETWORK_PROVIDER, t) == (t % 8 < 4));
        assert(state_level(SX_LED_NETWORK_ONLINE_AND_PROVIDER, t) ==
               (t % 8 == 0 || t % 8 == 2));
        assert(state_level(SX_LED_NETWORK_ERROR, t) == (t % 4 == 0));
    }
    puts("PASS network polarity preserves existing status patterns");

    set_all_leds_level(1);
    const int all_pins[] = {LED_SYS, LED_LAN, LED_COM1, LED_WIFI, LED_COM2, LED_4G, LED_232};
    for (unsigned i = 0; i < sizeof(all_pins) / sizeof(all_pins[0]); ++i)
        assert(levels[all_pins[i]] == 1);
    set_all_leds_level(0);
    for (unsigned i = 0; i < sizeof(all_pins) / sizeof(all_pins[0]); ++i)
        assert(levels[all_pins[i]] == 0);
    sx_led_manager_indicate_restart();
    assert(s_action_active && s_action_blinks_requested == 1);
    sx_led_manager_indicate_factory_reset();
    assert(s_action_active && s_action_blinks_requested == 3);
    s_action_active = false;
    s_action_blinks_requested = 0;
    puts("PASS restart and factory reset drive all seven LEDs together");

    char data[] = "test";
    for (int failure = -1; failure <= 0; ++failure) {
        before = gpio_writes;
        write_result = failure;
        assert(sendDataToUart(data, 4, 0) == failure);
        assert(gpio_writes == before);
    }
    int calls = write_calls;
    assert(sendDataToUart(NULL, 4, 0) == 0);
    assert(sendDataToUart(data, 0, 0) == 0);
    assert(write_calls == calls);
    puts("PASS failed and empty TX never light the lamp");

    write_result = 4;
    for (int timeout = 0; timeout <= 1; ++timeout) {
        wait_result = timeout;
        assert(sendDataToUart(data, 4, 0) == 4);
        assert(levels[LED_COM1] == 0);
    }
    puts("PASS successful TX lights through drain and expires independently of logging/timeouts");
    return 0;
}
'''

def main():
    source = (ROOT / 'main/async_uart.c').read_text()
    start = source.index('int sendDataToUart(')
    end = source.index('\nvoid tx_tasks_to_channel(', start)
    with tempfile.TemporaryDirectory(prefix='sp603-led-test-') as temp:
        out = Path(temp)
        gpio_numbers = '\n'.join(f'#define GPIO_NUM_{n} {n}' for n in range(49))
        (out / 'mock.h').write_text(MOCK + '\n' + gpio_numbers)
        for header in ['driver/gpio.h', 'esp_err.h', 'esp_log.h', 'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h']:
            path = out / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "mock.h"\n')
        (out / 'test.c').write_text(TEST.replace('UART_TX_FUNCTION', source[start:end]))
        exe = out / 'test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(out), '-I', str(ROOT / 'main/include'),
                        '-I', str(ROOT / 'main'), str(out / 'test.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()
