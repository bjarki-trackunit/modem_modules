#include <zephyr/net/ppp.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include <zephyr/modem/modem_ppp.h>

LOG_MODULE_REGISTER(modem_ppp);
/*********************************************************
 * DEFINES / MACROS
 *********************************************************/
#define MODEM_PPP_FRAME_TAIL_SIZE (2)

/*********************************************************
 * LOCAL FUNCTIONS
 *********************************************************/
static uint16_t modem_ppp_fcs_init(uint8_t byte)
{
	return crc16_ccitt(0xFFFF, &byte, 1);
}

static uint16_t modem_ppp_fcs_update(uint16_t fcs, uint8_t byte)
{
	return crc16_ccitt(fcs, &byte, 1);
}

static uint16_t modem_ppp_fcs_final(uint16_t fcs)
{
	return fcs ^ 0xFFFF;
}

static uint16_t modem_ppp_ppp_protocol(struct net_pkt *pkt)
{
	if (net_pkt_family(pkt) == AF_INET) {
		return PPP_IP;
	}

	if (net_pkt_family(pkt) == AF_INET6) {
		return PPP_IPV6;
	}

	LOG_WRN("Unsupported protocol");

	return 0;
}

static uint8_t modem_ppp_wrap_net_pkt_byte(struct modem_ppp *ppp)
{
	uint8_t byte;

	switch (ppp->transmit_state) {
	case MODEM_PPP_TRANSMIT_STATE_IDLE:
		LOG_WRN("Invalid transmit state");

		return 0;

	/* Writing header */
	case MODEM_PPP_TRANSMIT_STATE_SOF:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_HDR_FF;

		return 0x7E;

	case MODEM_PPP_TRANSMIT_STATE_HDR_FF:
		net_pkt_cursor_init(ppp->tx_pkt);

		ppp->tx_pkt_fcs = modem_ppp_fcs_init(0xFF);

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_HDR_7D;

		return 0xFF;

	case MODEM_PPP_TRANSMIT_STATE_HDR_7D:
		ppp->tx_pkt_fcs = modem_ppp_fcs_update(ppp->tx_pkt_fcs, 0x03);

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_HDR_23;

		return 0x7D;

	case MODEM_PPP_TRANSMIT_STATE_HDR_23:
		if (net_pkt_is_ppp(ppp->tx_pkt) == true) {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_DATA;
		} else {
			ppp->tx_pkt_protocol = modem_ppp_ppp_protocol(ppp->tx_pkt);

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_PROTOCOL_HIGH;
		}

		return 0x23;

	/* Writing protocol */
	case MODEM_PPP_TRANSMIT_STATE_PROTOCOL_HIGH:
		byte = (ppp->tx_pkt_protocol >> 8) & 0xFF;

		ppp->tx_pkt_fcs = modem_ppp_fcs_update(ppp->tx_pkt_fcs, byte);

		if ((byte == 0x7E) || (byte == 0x7D) || (byte < 0x20)) {
			ppp->tx_pkt_escaped = byte ^ 0x20;

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_HIGH;

			return 0x7D;
		}

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_PROTOCOL_LOW;

		return byte;

	case MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_HIGH:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_PROTOCOL_LOW;

		return ppp->tx_pkt_escaped;

	case MODEM_PPP_TRANSMIT_STATE_PROTOCOL_LOW:
		byte = ppp->tx_pkt_protocol & 0xFF;

		ppp->tx_pkt_fcs = modem_ppp_fcs_update(ppp->tx_pkt_fcs, byte);

		if ((byte == 0x7E) || (byte == 0x7D) || (byte < 0x20)) {
			ppp->tx_pkt_escaped = byte ^ 0x20;

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_LOW;

			return 0x7D;
		}

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_DATA;

		return byte;

	case MODEM_PPP_TRANSMIT_STATE_ESCAPING_PROTOCOL_LOW:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_DATA;

		return ppp->tx_pkt_escaped;

	/* Writing data */
	case MODEM_PPP_TRANSMIT_STATE_DATA:
		net_pkt_read_u8(ppp->tx_pkt, &byte);

		ppp->tx_pkt_fcs = modem_ppp_fcs_update(ppp->tx_pkt_fcs, byte);

		if ((byte == 0x7E) || (byte == 0x7D) || (byte < 0x20)) {
			ppp->tx_pkt_escaped = byte ^ 0x20;

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_ESCAPING_DATA;

			return 0x7D;
		}

		if (net_pkt_remaining_data(ppp->tx_pkt) == 0) {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_FCS_LOW;
		}

		return byte;

	case MODEM_PPP_TRANSMIT_STATE_ESCAPING_DATA:
		if (net_pkt_remaining_data(ppp->tx_pkt) == 0) {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_FCS_LOW;
		} else {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_DATA;
		}

		return ppp->tx_pkt_escaped;

	/* Writing FCS */
	case MODEM_PPP_TRANSMIT_STATE_FCS_LOW:
		ppp->tx_pkt_fcs = modem_ppp_fcs_final(ppp->tx_pkt_fcs);

		byte = ppp->tx_pkt_fcs & 0xFF;

		if ((byte == 0x7E) || (byte == 0x7D) || (byte < 0x20)) {
			ppp->tx_pkt_escaped = byte ^ 0x20;

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_LOW;

			return 0x7D;
		}

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_FCS_HIGH;

		return byte;

	case MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_LOW:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_FCS_HIGH;

		return ppp->tx_pkt_escaped;

	case MODEM_PPP_TRANSMIT_STATE_FCS_HIGH:
		byte = (ppp->tx_pkt_fcs >> 8) & 0xFF;

		if ((byte == 0x7E) || (byte == 0x7D) || (byte < 0x20)) {
			ppp->tx_pkt_escaped = byte ^ 0x20;

			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_HIGH;

			return 0x7D;
		}

		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_EOF;

		return byte;

	case MODEM_PPP_TRANSMIT_STATE_ESCAPING_FCS_HIGH:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_EOF;

		return ppp->tx_pkt_escaped;

	/* Writing end of frame */
	case MODEM_PPP_TRANSMIT_STATE_EOF:
		ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_IDLE;

		return 0x7E;
	}

	return 0;
}

static void modem_ppp_process_received_byte(struct modem_ppp *ppp, uint8_t byte)
{
	switch (ppp->receive_state) {
	case MODEM_PPP_RECEIVE_STATE_HDR_SOF:
		if (byte == 0x7E) {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_FF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_HDR_FF:
		if (byte == 0x7E) {
			break;
		}

		if (byte == 0xFF) {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_7D;

		} else {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_HDR_7D:
		if (byte == 0x7D) {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_23;

		} else {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_HDR_23:
		if (byte == 0x23) {
			ppp->pkt = net_pkt_rx_alloc_with_buffer(ppp->net_iface, 256, AF_UNSPEC, 0,
								K_NO_WAIT);

			if (ppp->pkt == NULL) {
				LOG_WRN("Dropped frame, no net_pkt available");

				ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

				break;
			}

			LOG_DBG("Receiving PPP frame -> net_pkt(0x%08x)", (size_t)ppp->pkt);

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_WRITING;

			net_pkt_cursor_init(ppp->pkt);

		} else {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_WRITING:
		if (byte == 0x7E) {
			LOG_DBG("Received PPP frame -> net_pkt(0x%08x)", (size_t)ppp->pkt);

			/* Remove FCS */
			net_pkt_remove_tail(ppp->pkt, MODEM_PPP_FRAME_TAIL_SIZE);

			net_pkt_cursor_init(ppp->pkt);
			net_pkt_set_ppp(ppp->pkt, true);

			/* Receive on network interface */
			net_recv_data(ppp->net_iface, ppp->pkt);

			/* Remove reference to network packet */
			ppp->pkt = NULL;

			/* Reset state */
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

			break;
		}

		if (byte == 0x7D) {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_UNESCAPING;

			break;
		}

		if (net_pkt_write_u8(ppp->pkt, byte) < 0) {
			LOG_WRN("Dropped PPP frame -> net_pkt(0x%08x)", (size_t)ppp->pkt);

			net_pkt_unref(ppp->pkt);

			ppp->pkt = NULL;

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_UNESCAPING:
		if (net_pkt_write_u8(ppp->pkt, (byte ^ 0x20)) < 0) {
			LOG_WRN("Dropped PPP frame -> net_pkt(0x%08x)", (size_t)ppp->pkt);

			net_pkt_unref(ppp->pkt);

			ppp->pkt = NULL;

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

			break;
		}

		ppp->receive_state = MODEM_PPP_RECEIVE_STATE_WRITING;

		break;
	}
}

static void modem_ppp_process_received_data(struct modem_ppp *ppp)
{
	for (size_t i = 0; i < ppp->rx_buf_len; i++) {
		modem_ppp_process_received_byte(ppp, ppp->rx_buf[i]);
	}
}

static void modem_ppp_pipe_event_handler(struct modem_pipe *pipe, enum modem_pipe_event event,
					 void *user_data)
{
	struct modem_ppp *ppp = (struct modem_ppp *)user_data;

	/* Validate receive ready */
	if (event != MODEM_PIPE_EVENT_RECEIVE_READY) {
		return;
	}

	/* Submit processing work */
	k_work_submit(&ppp->process_work.work);
}

static void modem_ppp_send_submit_handler(struct k_work *item)
{
	struct modem_ppp_send_work_item *ppp_work_item = (struct modem_ppp_send_work_item *)item;
	struct modem_ppp *ppp = ppp_work_item->ppp;

	/* Validate memory available */
	if (ppp->tx_pkt != NULL) {
		LOG_WRN("Dropped net pkt");

		return;
	}

	/* Submit network packet */
	ppp->tx_pkt = ppp_work_item->pkt;

	/* Submit send work */
	k_work_submit(&ppp->send_work.work);
}

static void modem_ppp_send_handler(struct k_work *item)
{
	struct modem_ppp_work_item *ppp_work_item = (struct modem_ppp_work_item *)item;
	struct modem_ppp *ppp = ppp_work_item->ppp;
	uint8_t byte;
	uint8_t *reserved;
	uint32_t reserved_size;
	int ret;

	if (ppp->tx_pkt != NULL) {
		/* Initialize wrap */
		if (ppp->transmit_state == MODEM_PPP_TRANSMIT_STATE_IDLE) {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_SOF;
		}

		/* Fill transmit ring buffer */
		while (ring_buf_space_get(&ppp->tx_rb) > 0) {
			byte = modem_ppp_wrap_net_pkt_byte(ppp);

			ring_buf_put(&ppp->tx_rb, &byte, 1);

			if (ppp->transmit_state == MODEM_PPP_TRANSMIT_STATE_IDLE) {
				net_pkt_unref(ppp->tx_pkt);

				ppp->tx_pkt = NULL;

				break;
			}
		}
	}

	/* Reserve data to transmit from transmit ring buffer */
	reserved_size = ring_buf_get_claim(&ppp->tx_rb, &reserved, UINT32_MAX);

	/* Transmit reserved data */
	ret = modem_pipe_transmit(ppp->pipe, reserved, reserved_size);

	/* Release remaining reserved data */
	ring_buf_get_finish(&ppp->tx_rb, (uint32_t)ret);

	/* Resubmit send work if data remains */
	if ((ring_buf_is_empty(&ppp->tx_rb) == false) || (ppp->tx_pkt != NULL)) {
		k_work_submit(&ppp->send_work.work);
	}
}

static void modem_ppp_process_handler(struct k_work *item)
{
	struct modem_ppp_work_item *ppp_work_item = (struct modem_ppp_work_item *)item;
	struct modem_ppp *ppp = ppp_work_item->ppp;
	int ret;

	/* Receive data from pipe */
	ret = modem_pipe_receive(ppp->pipe, ppp->rx_buf, ppp->rx_buf_size);

	/* Validate data received */
	if (ret < 1) {
		return;
	}

	/* Save received data length */
	ppp->rx_buf_len = (uint16_t)ret;

	/* Process received data */
	modem_ppp_process_received_data(ppp);

	/* Schedule receive handler if data remains */
	if (ppp->rx_buf_size == ppp->rx_buf_len) {
		k_work_submit(&ppp->process_work.work);
	}
}

/*********************************************************
 * GLOBAL FUNCTIONS
 *********************************************************/
int modem_ppp_init(struct modem_ppp *ppp, const struct modem_ppp_config *config)
{
	/* Validate arguments */
	if ((ppp == NULL) || (config == NULL)) {
		return -EINVAL;
	}

	/* Validate configuration */
	if ((config->rx_buf == NULL) || (config->rx_buf_size == 0) || (config->tx_buf == NULL) ||
	    (config->tx_buf_size == 0)) {
		return -EINVAL;
	}

	/* Clear PPP instance */
	memset(ppp, 0x00, sizeof(*ppp));

	/* Copy configuration */
	ppp->rx_buf = config->rx_buf;
	ppp->rx_buf_size = config->rx_buf_size;

	/* Initialize ring buffer */
	ring_buf_init(&ppp->tx_rb, config->tx_buf_size, config->tx_buf);

	/* Initialize send submit work */
	ppp->send_submit_work.ppp = ppp;
	k_work_init(&ppp->send_submit_work.work, modem_ppp_send_submit_handler);

	/* Initialize send work */
	ppp->send_work.ppp = ppp;
	k_work_init(&ppp->send_work.work, modem_ppp_send_handler);

	/* Initialize process work */
	ppp->process_work.ppp = ppp;
	k_work_init(&ppp->process_work.work, modem_ppp_process_handler);

	return 0;
}

int modem_ppp_attach(struct modem_ppp *ppp, struct modem_pipe *pipe, struct net_if *iface)
{
	int ret;

	/* Associate command handler with pipe and network interface */
	ppp->net_iface = iface;
	ppp->pipe = pipe;

	/* Set pipe event handler */
	ret = modem_pipe_event_handler_set(pipe, modem_ppp_pipe_event_handler, ppp);

	if (ret < 0) {
		ppp->net_iface = NULL;
		ppp->pipe = NULL;

		return ret;
	}

	return 0;
}

int modem_ppp_send(struct modem_ppp *ppp, struct net_pkt *pkt)
{
	/* Validate packet protocol */
	if ((net_pkt_is_ppp(pkt) == false) && (net_pkt_family(pkt) != AF_INET) &&
	    (net_pkt_family(pkt) != AF_INET6)) {
		return -EPROTONOSUPPORT;
	}

	/* Validate packet data length */
	if (((net_pkt_get_len(pkt) < 2) && (net_pkt_is_ppp(pkt) == true)) ||
	    ((net_pkt_get_len(pkt) < 1))) {
		return -ENODATA;
	}

	/* Validate send submit work idle */
	if (k_work_is_pending(&ppp->send_submit_work.work) == true) {
		return -EBUSY;
	}

	/* Initialize send submit work */
	ppp->send_submit_work.pkt = pkt;

	/* Reference pkt */
	net_pkt_ref(pkt);

	/* Submit send submit work */
	k_work_submit(&ppp->send_submit_work.work);

	return 0;
}

int modem_ppp_release(struct modem_ppp *ppp)
{
	struct k_work_sync sync;

	/* Cancel work */
	k_work_cancel_sync(&ppp->send_work.work, &sync);

	modem_pipe_event_handler_set(ppp->pipe, NULL, NULL);

	/* Clear references to pipe and network interface */
	ppp->net_iface = NULL;
	ppp->pipe = NULL;

	/* Unreference network packet if exists */
	if (ppp->pkt != NULL) {
		net_pkt_unref(ppp->pkt);
		ppp->pkt = NULL;
	}

	return 0;
}