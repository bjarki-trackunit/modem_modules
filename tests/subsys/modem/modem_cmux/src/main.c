/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*************************************************************************************************/
/*                                        Dependencies                                           */
/*************************************************************************************************/
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/modem/modem_cmux.h>
#include <modem_pipe_mock.h>

/*************************************************************************************************/
/*                                         Definitions                                           */
/*************************************************************************************************/
#define EVENT_CMUX_CONNECTED            BIT(0)
#define EVENT_CMUX_DLCI1_OPEN           BIT(1)
#define EVENT_CMUX_DLCI2_OPEN           BIT(2)
#define EVENT_CMUX_DLCI1_CLOSED         BIT(3)
#define EVENT_CMUX_DLCI2_CLOSED         BIT(4)
#define EVENT_CMUX_DISCONNECTED         BIT(5)

/*************************************************************************************************/
/*                                          Instances                                            */
/*************************************************************************************************/
static struct modem_cmux cmux;
static uint8_t cmux_receive_buf[128];
static struct modem_cmux_dlci dlcis[2];
static struct k_event cmux_event;

static struct modem_pipe_mock bus_mock;
static uint8_t bus_mock_rx_buf[128];
static uint8_t bus_mock_tx_buf[128];
static struct modem_pipe bus_pipe;

static uint8_t dlci1_receive_buf[128];
static struct modem_pipe dlci1_pipe;

static uint8_t dlci2_receive_buf[128];
static struct modem_pipe dlci2_pipe;

static uint8_t buffer1[4096];
static uint8_t buffer2[4096];

/*************************************************************************************************/
/*                                         CMUX frames                                           */
/*************************************************************************************************/
static uint8_t cmux_frame_control_open_ack[] = {
	0xF9, 0x03, 0x73, 0x01, 0xD7, 0xF9
};

static uint8_t cmux_frame_dlci1_open_ack[] = {
	0xF9, 0x07, 0x73, 0x01, 0x15, 0xF9
};

static uint8_t cmux_frame_dlci2_open_ack[] = {
	0xF9, 0x0B, 0x73, 0x01, 0x92, 0xF9
};

/*************************************************************************************************/
/*                                     DLCI2 AT CMUX frames                                      */
/*************************************************************************************************/
static uint8_t cmux_frame_dlci2_at_cgdcont[] = {
	0xF9, 0x0B, 0xEF, 0x43, 0x41, 0x54, 0x2B, 0x43, 0x47, 0x44, 0x43, 0x4F, 0x4E, 0x54, 0x3D,
	0x31, 0x2C, 0x22, 0x49, 0x50, 0x22, 0x2C, 0x22, 0x74, 0x72, 0x61, 0x63, 0x6B, 0x75, 0x6E,
	0x69, 0x74, 0x2E, 0x6D, 0x32, 0x6D, 0x22, 0x23, 0xF9
};

static uint8_t cmux_frame_data_dlci2_at_cgdcont[] = {
	0x41, 0x54, 0x2B, 0x43, 0x47, 0x44, 0x43, 0x4F, 0x4E, 0x54, 0x3D, 0x31, 0x2C, 0x22, 0x49,
	0x50, 0x22, 0x2C, 0x22, 0x74, 0x72, 0x61, 0x63, 0x6B, 0x75, 0x6E, 0x69, 0x74, 0x2E, 0x6D,
	0x32, 0x6D, 0x22
};

static uint8_t cmux_frame_dlci2_at_newline[] = {
	0xF9, 0x0B, 0xEF, 0x05, 0x0D, 0x0A, 0xB7, 0xF9
};

static uint8_t cmux_frame_data_dlci2_at_newline[] = {
	0x0D, 0x0A
};

/*************************************************************************************************/
/*                                    DLCI1 AT CMUX frames                                       */
/*************************************************************************************************/
static uint8_t cmux_frame_dlci1_at_at[] = {
	0xF9, 0x07, 0xEF, 0x05, 0x41, 0x54, 0x30, 0xF9
};

static uint8_t cmux_frame_data_dlci1_at_at[] = {
	0x41, 0x54
};

static uint8_t cmux_frame_dlci1_at_newline[] = {
	0xF9, 0x07, 0xEF, 0x05, 0x0D, 0x0A, 0x30, 0xF9
};

static uint8_t cmux_frame_data_dlci1_at_newline[] = {
	0x0D, 0x0A
};

/*************************************************************************************************/
/*                                DLCI1 AT CMUX Desync frames                                    */
/*************************************************************************************************/
static uint8_t cmux_frame_dlci1_at_at_desync[] = {
	0x41, 0x54, 0x30, 0xF9
};

static uint8_t cmux_frame_resync[] = {
	0xF9, 0xF9, 0xF9
};

/*************************************************************************************************/
/*                                   DLCI2 PPP CMUX frames                                       */
/*************************************************************************************************/
static uint8_t cmux_frame_dlci2_ppp_52[] = {
	0xF9, 0x09, 0xEF, 0x69, 0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x21, 0x7D, 0x20, 0x7D,
	0x20, 0x7D, 0x38, 0x7D, 0x22, 0x7D, 0x26, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x20,
	0x7D, 0x23, 0x7D, 0x24, 0xC0, 0x23, 0x7D, 0x25, 0x7D, 0x26, 0x53, 0x96, 0x7D, 0x38, 0xAA,
	0x7D, 0x27, 0x7D, 0x22, 0x7D, 0x28, 0x7D, 0x22, 0xD5, 0xA8, 0x7E, 0x97, 0xF9
};

static uint8_t cmux_frame_data_dlci2_ppp_52[] = {
	0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x21, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x38, 0x7D,
	0x22, 0x7D, 0x26, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x20, 0x7D, 0x23, 0x7D, 0x24,
	0xC0, 0x23, 0x7D, 0x25, 0x7D, 0x26, 0x53, 0x96, 0x7D, 0x38, 0xAA, 0x7D, 0x27, 0x7D, 0x22,
	0x7D, 0x28, 0x7D, 0x22, 0xD5, 0xA8, 0x7E
};

static uint8_t cmux_frame_dlci2_ppp_18[] = {
	0xF9, 0x09, 0xEF, 0x25, 0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x22, 0x7D, 0x21, 0x7D,
	0x20, 0x7D, 0x24, 0x7D, 0x3C, 0x90, 0x7E, 0xEE, 0xF9
};

static uint8_t cmux_frame_data_dlci2_ppp_18[] = {
	0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x22, 0x7D, 0x21, 0x7D, 0x20, 0x7D, 0x24, 0x7D,
	0x3C, 0x90, 0x7E
};

static void test_modem_cmux_event_handler(struct modem_cmux *cmux, struct modem_cmux_event event,
					    void *user_data)
{

	if (event.type == MODEM_CMUX_EVENT_CONNECTED) {
		k_event_post(&cmux_event, EVENT_CMUX_CONNECTED);
		return;
	}

	if ((event.type == MODEM_CMUX_EVENT_OPENED) &&
	    (event.dlci_address == 1)) {
		k_event_post(&cmux_event, EVENT_CMUX_DLCI1_OPEN);
		return;
	}

	if ((event.type == MODEM_CMUX_EVENT_OPENED) &&
	    (event.dlci_address == 2)) {
		k_event_post(&cmux_event, EVENT_CMUX_DLCI2_OPEN);
		return;
	}

	if ((event.type == MODEM_CMUX_EVENT_CLOSED) &&
	    (event.dlci_address == 1)) {
		k_event_post(&cmux_event, EVENT_CMUX_DLCI1_CLOSED);
		return;
	}

	if ((event.type == MODEM_CMUX_EVENT_CLOSED) &&
	    (event.dlci_address == 2)) {
		k_event_post(&cmux_event, EVENT_CMUX_DLCI2_CLOSED);
		return;
	}

	if (event.type == MODEM_CMUX_EVENT_DISCONNECTED) {
		k_event_post(&cmux_event, EVENT_CMUX_DISCONNECTED);
		return;
	}
}

static void test_modem_cmux_dlci1_pipe_event_handler(struct modem_pipe *pipe,
						     enum modem_pipe_event event, void *user_data)
{

}

static void test_modem_cmux_dlci2_pipe_event_handler(struct modem_pipe *pipe,
						     enum modem_pipe_event event, void *user_data)
{

}

static void *test_modem_cmux_setup(void)
{
	uint32_t events;

	struct modem_cmux_dlci_config dlci1_config = {
		.dlci_address = 1,
		.receive_buf = dlci1_receive_buf,
		.receive_buf_size = sizeof(dlci1_receive_buf),
	};

	struct modem_cmux_dlci_config dlci2_config = {
		.dlci_address = 2,
		.receive_buf = dlci2_receive_buf,
		.receive_buf_size = sizeof(dlci2_receive_buf),
	};

	k_event_init(&cmux_event);

	struct modem_cmux_config cmux_config = {
		.event_handler = test_modem_cmux_event_handler,
		.event_handler_user_data = NULL,
		.dlcis = dlcis,
		.dlcis_cnt = ARRAY_SIZE(dlcis),
		.receive_buf = cmux_receive_buf,
		.receive_buf_size = sizeof(cmux_receive_buf),
		.receive_timeout = K_MSEC(3),
	};

	zassert_true(modem_cmux_init(&cmux, &cmux_config) == 0,
		     "Failed to init modem CMUX");

	const struct modem_pipe_mock_config bus_mock_config = {
		.rx_buf = bus_mock_rx_buf,
		.rx_buf_size = sizeof(bus_mock_rx_buf),
		.tx_buf = bus_mock_tx_buf,
		.tx_buf_size = sizeof(bus_mock_tx_buf),
	};

	zassert_true(modem_pipe_mock_init(&bus_mock, &bus_mock_config) == 0,
		     "Failed to init modem bus pipe mock");

	zassert_true(modem_pipe_mock_open(&bus_mock, &bus_pipe) == 0,
		     "Failed to open modem bus pipe mock");

	/* Connect CMUX */
	zassert_true(modem_cmux_connect(&cmux, &bus_pipe) == 0,
		     "Failed to connect CMUX to bus pipe");

	modem_pipe_mock_put(&bus_mock, cmux_frame_control_open_ack,
			    sizeof(cmux_frame_control_open_ack));

	events = k_event_wait(&cmux_event, EVENT_CMUX_CONNECTED, false, K_MSEC(100));

	zassert_true(events == EVENT_CMUX_CONNECTED, "Connected event not raised");

	/* Open DLCI channels */
	zassert_true(modem_cmux_dlci_open(&cmux, &dlci1_config, &dlci1_pipe) == 0,
		     "Failed to open DLCI 1");

	zassert_true(modem_cmux_dlci_open(&cmux, &dlci2_config, &dlci2_pipe) == 0,
		     "Failed to open DLCI 2");

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_open_ack,
			    sizeof(cmux_frame_dlci1_open_ack));

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci2_open_ack,
			    sizeof(cmux_frame_dlci2_open_ack));

	events = k_event_wait_all(&cmux_event, (EVENT_CMUX_DLCI1_OPEN | EVENT_CMUX_DLCI2_OPEN),
				  false, K_MSEC(100));

	zassert_true((events & EVENT_CMUX_DLCI1_OPEN), "DLCI1 open event not raised");
	zassert_true((events & EVENT_CMUX_DLCI2_OPEN), "DLCI2 open event not raised");

	/* Set DLCI channel event handlers */
	modem_pipe_event_handler_set(&dlci1_pipe, test_modem_cmux_dlci1_pipe_event_handler,
				     NULL);

	modem_pipe_event_handler_set(&dlci2_pipe, test_modem_cmux_dlci1_pipe_event_handler,
				     NULL);

	return NULL;
}

static void test_modem_cmux_before(void *f)
{
	/* Reset events */
	k_event_clear(&cmux_event, UINT32_MAX);

	/* Reset mock pipes */
	modem_pipe_mock_reset(&bus_mock);
}

ZTEST(modem_cmux, modem_cmux_receive_dlci2_at)
{
	int ret;

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci2_at_cgdcont,
			    sizeof(cmux_frame_dlci2_at_cgdcont));

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci2_at_newline,
			    sizeof(cmux_frame_dlci2_at_newline));

	k_msleep(100);

	ret = modem_pipe_receive(&dlci2_pipe, buffer2, sizeof(buffer2));

	zassert_true(ret == (sizeof(cmux_frame_data_dlci2_at_cgdcont) +
		     sizeof(cmux_frame_data_dlci2_at_newline)),
		     "Incorrect number of bytes received");

	zassert_true(memcmp(buffer2, cmux_frame_data_dlci2_at_cgdcont,
		     sizeof(cmux_frame_data_dlci2_at_cgdcont)) == 0,
		     "Incorrect data received");

	zassert_true(memcmp(&buffer2[sizeof(cmux_frame_data_dlci2_at_cgdcont)],
		     cmux_frame_data_dlci2_at_newline,
		     sizeof(cmux_frame_data_dlci2_at_newline)) == 0,
		     "Incorrect data received");
}

ZTEST(modem_cmux, modem_cmux_receive_dlci1_at)
{
	int ret;

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_at_at,
			    sizeof(cmux_frame_dlci1_at_at));

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_at_newline,
			    sizeof(cmux_frame_dlci1_at_newline));

	k_msleep(100);

	ret = modem_pipe_receive(&dlci1_pipe, buffer1, sizeof(buffer1));

	zassert_true(ret == (sizeof(cmux_frame_data_dlci1_at_at) +
		     sizeof(cmux_frame_data_dlci1_at_newline)),
		     "Incorrect number of bytes received");

	zassert_true(memcmp(buffer1, cmux_frame_data_dlci1_at_at,
		     sizeof(cmux_frame_data_dlci1_at_at)) == 0,
		     "Incorrect data received");

	zassert_true(memcmp(&buffer1[sizeof(cmux_frame_data_dlci1_at_at)],
		     cmux_frame_data_dlci1_at_newline,
		     sizeof(cmux_frame_data_dlci1_at_newline)) == 0,
		     "Incorrect data received");
}

ZTEST(modem_cmux, modem_cmux_receive_dlci2_ppp)
{
	int ret;

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci2_ppp_52,
			    sizeof(cmux_frame_dlci2_ppp_52));

	modem_pipe_mock_put(&bus_mock, cmux_frame_dlci2_ppp_18,
			    sizeof(cmux_frame_dlci2_ppp_18));

	k_msleep(100);

	ret = modem_pipe_receive(&dlci2_pipe, buffer2, sizeof(buffer2));

	zassert_true(ret == (sizeof(cmux_frame_data_dlci2_ppp_52) +
		     sizeof(cmux_frame_data_dlci2_ppp_18)),
		     "Incorrect number of bytes received");

	zassert_true(memcmp(buffer2, cmux_frame_data_dlci2_ppp_52,
		     sizeof(cmux_frame_data_dlci2_ppp_52)) == 0,
		     "Incorrect data received");

	zassert_true(memcmp(&buffer2[sizeof(cmux_frame_data_dlci2_ppp_52)],
		     cmux_frame_data_dlci2_ppp_18,
		     sizeof(cmux_frame_data_dlci2_ppp_18)) == 0,
		     "Incorrect data received");
}


ZTEST(modem_cmux, modem_cmux_transmit_dlci2_ppp)
{
	int ret;

	ret = modem_pipe_transmit(&dlci2_pipe, cmux_frame_data_dlci2_ppp_52,
				  sizeof(cmux_frame_data_dlci2_ppp_52));

	zassert_true(ret == sizeof(cmux_frame_data_dlci2_ppp_52),
		     "Failed to send DLCI2 PPP 52");

	ret = modem_pipe_transmit(&dlci2_pipe, cmux_frame_data_dlci2_ppp_18,
				  sizeof(cmux_frame_data_dlci2_ppp_18));

	zassert_true(ret == sizeof(cmux_frame_data_dlci2_ppp_18),
		     "Failed to send DLCI2 PPP 18");

	k_msleep(100);

	ret = modem_pipe_mock_get(&bus_mock, buffer2, sizeof(buffer2));

	zassert_true(ret == (sizeof(cmux_frame_dlci2_ppp_52) +
		     sizeof(cmux_frame_dlci2_ppp_18)),
		     "Incorrect number of bytes transmitted");
}

ZTEST(modem_cmux, modem_cmux_resync)
{
	int ret;

	ret = modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_at_at_desync,
				  sizeof(cmux_frame_dlci1_at_at_desync));

	k_msleep(100);

	ret = modem_pipe_mock_get(&bus_mock, buffer1, sizeof(buffer1));

	zassert_true(ret == sizeof(cmux_frame_resync),
		     "Expected resync flags to be sent to bus");

	zassert_true(memcmp(buffer1, cmux_frame_resync, sizeof(cmux_frame_resync)) == 0,
		     "Expected resync flags to be sent to bus");

	ret = modem_pipe_mock_put(&bus_mock, cmux_frame_resync,
				  sizeof(cmux_frame_resync));

	ret = modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_at_at,
				  sizeof(cmux_frame_dlci1_at_at));

	ret = modem_pipe_mock_put(&bus_mock, cmux_frame_dlci1_at_newline,
				  sizeof(cmux_frame_dlci1_at_newline));

	k_msleep(100);

	ret = modem_pipe_receive(&dlci1_pipe, buffer1, sizeof(buffer1));

	zassert_true(ret == (sizeof(cmux_frame_data_dlci1_at_at) +
		     sizeof(cmux_frame_data_dlci1_at_newline)),
		     "Incorrect number of bytes received");

	zassert_true(memcmp(buffer1, cmux_frame_data_dlci1_at_at,
		     sizeof(cmux_frame_data_dlci1_at_at)) == 0,
		     "Incorrect data received");

	zassert_true(memcmp(&buffer1[sizeof(cmux_frame_data_dlci1_at_at)],
		     cmux_frame_data_dlci1_at_newline,
		     sizeof(cmux_frame_data_dlci1_at_newline)) == 0,
		     "Incorrect data received");
}

ZTEST_SUITE(modem_cmux, NULL, test_modem_cmux_setup, test_modem_cmux_before, NULL, NULL);