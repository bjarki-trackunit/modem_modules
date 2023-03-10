/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>

#include <zephyr/modem/modem_pipe.h>

#ifndef ZEPHYR_MODEM_MODEM_BACKEND_UART
#define ZEPHYR_MODEM_MODEM_BACKEND_UART

struct modem_backend_uart;

struct modem_backend_uart_work {
	struct k_work work;
	struct modem_backend_uart *backend;
};

struct modem_backend_uart {
	const struct device *uart;
	struct ring_buf receive_rdb[2];
	uint8_t receive_rdb_used;
	struct ring_buf transmit_rb;
	atomic_t transmit_buf_len;
	uint32_t transmit_buf_put_limit;
	struct modem_pipe pipe;
	struct modem_backend_uart_work receive_ready_work;
};

struct modem_backend_uart_config {
	const struct device *uart;
	uint8_t *receive_buf;
	uint32_t receive_buf_size;
	uint8_t *transmit_buf;
	uint32_t transmit_buf_size;
};

struct modem_pipe *modem_backend_uart_init(struct modem_backend_uart *backend,
					   const struct modem_backend_uart_config *config);

#endif /* ZEPHYR_MODEM_MODEM_BACKEND_UART */
