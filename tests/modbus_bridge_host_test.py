#!/usr/bin/env python3
"""Host regression checks for the production Modbus TCP/RTU converters."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

TEST = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MODBUS_TCP_MAX_ADU 260
#define MODBUS_RTU_MAX_ADU 256

PRODUCTION_HELPERS

int main(void)
{
    const uint8_t tcp_request[] = {
        0x12, 0x34, 0x00, 0x00, 0x00, 0x06,
        0x11, 0x03, 0x00, 0x6b, 0x00, 0x03,
    };
    const uint8_t expected_rtu_request[] = {
        0x11, 0x03, 0x00, 0x6b, 0x00, 0x03, 0x76, 0x87,
    };
    uint8_t rtu[MODBUS_RTU_MAX_ADU] = {0};
    size_t rtu_len = 0;
    uint16_t transaction_id = 0;
    uint8_t unit_id = 0;
    uint8_t function_code = 0;
    assert(modbus_tcp_adu_to_rtu(tcp_request, sizeof(tcp_request),
                                 rtu, sizeof(rtu), &rtu_len,
                                 &transaction_id, &unit_id, &function_code));
    assert(transaction_id == 0x1234 && unit_id == 0x11 && function_code == 0x03);
    assert(rtu_len == sizeof(expected_rtu_request));
    assert(memcmp(rtu, expected_rtu_request, rtu_len) == 0);
    puts("PASS Modbus TCP request converts to the known RTU CRC vector");

    const uint8_t rtu_response[] = {
        0x11, 0x03, 0x06, 0xae, 0x41, 0x56, 0x52, 0x43, 0x40, 0x49, 0xad,
    };
    const uint8_t expected_tcp_response[] = {
        0x12, 0x34, 0x00, 0x00, 0x00, 0x09,
        0x11, 0x03, 0x06, 0xae, 0x41, 0x56, 0x52, 0x43, 0x40,
    };
    uint8_t tcp[MODBUS_TCP_MAX_ADU] = {0};
    size_t tcp_len = 0;
    assert(modbus_rtu_adu_to_tcp(rtu_response, sizeof(rtu_response),
                                 transaction_id, unit_id, function_code,
                                 tcp, sizeof(tcp), &tcp_len));
    assert(tcp_len == sizeof(expected_tcp_response));
    assert(memcmp(tcp, expected_tcp_response, tcp_len) == 0);
    puts("PASS RTU response restores transaction ID and MBAP length");

    uint8_t exception[] = {0x11, 0x83, 0x02, 0x00, 0x00};
    uint16_t exception_crc = modbus_crc16(exception, 3);
    exception[3] = (uint8_t)exception_crc;
    exception[4] = (uint8_t)(exception_crc >> 8U);
    assert(modbus_rtu_adu_to_tcp(exception, sizeof(exception),
                                 transaction_id, unit_id, function_code,
                                 tcp, sizeof(tcp), &tcp_len));
    assert(tcp_len == 9 && tcp[7] == 0x83 && tcp[8] == 0x02);
    puts("PASS Modbus exception response is accepted");

    uint8_t invalid_request[sizeof(tcp_request)];
    memcpy(invalid_request, tcp_request, sizeof(invalid_request));
    invalid_request[3] = 1;
    assert(!modbus_tcp_adu_to_rtu(invalid_request, sizeof(invalid_request),
                                  rtu, sizeof(rtu), &rtu_len,
                                  &transaction_id, &unit_id, &function_code));
    memcpy(invalid_request, tcp_request, sizeof(invalid_request));
    invalid_request[5] = 5;
    assert(!modbus_tcp_adu_to_rtu(invalid_request, sizeof(invalid_request),
                                  rtu, sizeof(rtu), &rtu_len,
                                  &transaction_id, &unit_id, &function_code));
    puts("PASS invalid protocol ID and MBAP length are rejected");

    uint8_t invalid_response[sizeof(rtu_response)];
    memcpy(invalid_response, rtu_response, sizeof(invalid_response));
    invalid_response[sizeof(invalid_response) - 1] ^= 1;
    assert(!modbus_rtu_adu_to_tcp(invalid_response, sizeof(invalid_response),
                                  transaction_id, unit_id, function_code,
                                  tcp, sizeof(tcp), &tcp_len));
    memcpy(invalid_response, rtu_response, sizeof(invalid_response));
    invalid_response[0] = 0x12;
    assert(!modbus_rtu_adu_to_tcp(invalid_response, sizeof(invalid_response),
                                  transaction_id, unit_id, function_code,
                                  tcp, sizeof(tcp), &tcp_len));
    memcpy(invalid_response, rtu_response, sizeof(invalid_response));
    invalid_response[1] = 0x04;
    assert(!modbus_rtu_adu_to_tcp(invalid_response, sizeof(invalid_response),
                                  transaction_id, unit_id, function_code,
                                  tcp, sizeof(tcp), &tcp_len));
    puts("PASS CRC, unit ID and function mismatches are rejected");

    assert(!modbus_rtu_adu_to_tcp(rtu_response, sizeof(rtu_response),
                                  transaction_id, unit_id, function_code,
                                  tcp, 8, &tcp_len));
    uint8_t oversized_rtu[MODBUS_RTU_MAX_ADU + 1] = {0};
    assert(!modbus_rtu_adu_to_tcp(oversized_rtu, sizeof(oversized_rtu),
                                  transaction_id, unit_id, function_code,
                                  tcp, sizeof(tcp), &tcp_len));
    puts("PASS output capacity and RTU maximum length are enforced");
    return 0;
}
'''


def main():
    source = (ROOT / 'main/sx_serial_server.c').read_text()
    start = source.index('static uint16_t modbus_crc16(')
    end = source.index('\nstatic bool config_changed(', start)
    helpers = source[start:end]
    with tempfile.TemporaryDirectory(prefix='sp603-modbus-test-') as temp:
        test_source = Path(temp) / 'test.c'
        test_source.write_text(TEST.replace('PRODUCTION_HELPERS', helpers))
        executable = Path(temp) / 'test'
        subprocess.run([
            'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            str(test_source), '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
