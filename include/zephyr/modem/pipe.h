/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <zephyr/kernel.h>

#ifndef ZEPHYR_MODEM_PIPE_
#define ZEPHYR_MODEM_PIPE_

#ifdef __cplusplus
extern "C" {
#endif

struct modem_pipe;

typedef int (*modem_pipe_api_open)(void *data);

typedef int (*modem_pipe_api_transmit)(void *data, const uint8_t *buf, size_t size);

typedef int (*modem_pipe_api_receive)(void *data, uint8_t *buf, size_t size);

typedef int (*modem_pipe_api_close)(void *data);

struct modem_pipe_api {
	modem_pipe_api_open open;
	modem_pipe_api_transmit transmit;
	modem_pipe_api_receive receive;
	modem_pipe_api_close close;
};

enum modem_pipe_state {
	MODEM_PIPE_STATE_CLOSED = 0,
	MODEM_PIPE_STATE_OPEN,
};

enum modem_pipe_event {
	MODEM_PIPE_EVENT_OPENED = 0,
	MODEM_PIPE_EVENT_RECEIVE_READY,
	MODEM_PIPE_EVENT_CLOSED,
};

typedef void (*modem_pipe_api_callback)(struct modem_pipe *pipe, enum modem_pipe_event event,
					void *user_data);

struct modem_pipe {
	void *data;
	struct modem_pipe_api *api;
	modem_pipe_api_callback callback;
	void *user_data;
	enum modem_pipe_state state;
	struct k_mutex lock;
	struct k_condvar condvar;
};

/**
 * @brief Initialize a modem pipe
 *
 * @param pipe Pipe to initialize
 */
void modem_pipe_init(struct modem_pipe *pipe, void *data, struct modem_pipe_api *api);

/**
 * @brief Open pipe
 */
int modem_pipe_open(struct modem_pipe *pipe);

/**
 * @brief Open pipe
 */
int modem_pipe_open_async(struct modem_pipe *pipe);

/**
 * @brief Set callback
 */
void modem_pipe_attach(struct modem_pipe *pipe, modem_pipe_api_callback callback, void *user_data);

/**
 * @brief Transmit data through pipe
 *
 * @param pipe Pipe to transmit through
 * @param buf Destination for reveived data
 * @param size Capacity of destination for recevied data
 *
 * @return Number of bytes placed in pipe
 */
int modem_pipe_transmit(struct modem_pipe *pipe, const uint8_t *buf, size_t size);

/**
 * @brief Reveive data through pipe
 *
 * @param pipe Pipe to receive from
 * @param buf Destination for reveived data
 * @param size Capacity of destination for recevied data
 *
 * @return Number of bytes received from pipe if any
 * @return -EPERM if pipe is closed
 * @return -errno code on error
 */
int modem_pipe_receive(struct modem_pipe *pipe, uint8_t *buf, size_t size);

/**
 * @brief Set callback
 */
void modem_pipe_release(struct modem_pipe *pipe);

/**
 * @brief Close pipe
 */
int modem_pipe_close(struct modem_pipe *pipe);

/**
 * @brief Release pipe
 */
int modem_pipe_close_async(struct modem_pipe *pipe);

void modem_pipe_notify_opened(struct modem_pipe *pipe);

void modem_pipe_notify_closed(struct modem_pipe *pipe);

/**
 * @brief Notify of event
 *
 * @param pipe Pipe to receive from
 * @param buf Destination for reveived data
 * @param size Capacity of destination for recevied data
 *
 * @return Number of bytes received from pipe
 *
 * @warning Internal
 */
void modem_pipe_notify_receive_ready(struct modem_pipe *pipe);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_MODEM_PIPE_ */
