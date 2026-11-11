#pragma once
#include <stddef.h>
enum uart_port_t { UART_NUM_0 };
constexpr int ESP_OK = 0;
int uart_get_buffered_data_len(uart_port_t, size_t*);
