#include <zephyr/net/ppp.h>
#include <zephyr/sys/crc.h>
#include <zephyr/modem/ppp.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_ppp);

#define MODEM_PPP_STATE_ATTACHED_BIT	(0)
#define MODEM_PPP_FRAME_TAIL_SIZE	(2)

static bool modem_ppp_tx_net_pkt_buf_put(struct modem_ppp *ppp, struct net_pkt *pkt)
{
	k_mutex_lock(&ppp->tx_pkt_buf_lock, K_FOREVER);

	uint16_t next = ppp->tx_pkt_buf_tail;

	do {
		if (ppp->tx_pkt_buf[next] == NULL) {
			ppp->tx_pkt_buf[next] = pkt;

			k_mutex_unlock(&ppp->tx_pkt_buf_lock);

			return true;
		}

		next = ((next + 1) == ppp->tx_pkt_buf_size) ? 0 : (next + 1);

	} while (next != ppp->tx_pkt_buf_tail);

	k_mutex_unlock(&ppp->tx_pkt_buf_lock);

	return false;
}

static bool modem_ppp_tx_net_pkt_buf_get(struct modem_ppp *ppp, struct net_pkt **pkt)
{
	k_mutex_lock(&ppp->tx_pkt_buf_lock, K_FOREVER);

	(*pkt) = ppp->tx_pkt_buf[ppp->tx_pkt_buf_tail];

	if (*pkt == NULL) {
		k_mutex_unlock(&ppp->tx_pkt_buf_lock);
		return false;
	}

	ppp->tx_pkt_buf[ppp->tx_pkt_buf_tail] = NULL;

	ppp->tx_pkt_buf_tail =
		(ppp->tx_pkt_buf_tail + 1 == ppp->tx_pkt_buf_size ? 0 : ppp->tx_pkt_buf_tail + 1);

	k_mutex_unlock(&ppp->tx_pkt_buf_lock);

	return true;
}

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
			ppp->rx_pkt = net_pkt_rx_alloc_with_buffer(
				ppp->iface, CONFIG_NET_BUF_DATA_SIZE, AF_UNSPEC, 0, K_NO_WAIT);

			if (ppp->rx_pkt == NULL) {
				LOG_WRN("Dropped frame, no net_pkt available");

				ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

				break;
			}

			LOG_DBG("Receiving PPP frame -> net_pkt(0x%08x)", (size_t)ppp->rx_pkt);

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_WRITING;

			net_pkt_cursor_init(ppp->rx_pkt);

		} else {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_WRITING:
		if (byte == 0x7E) {
			LOG_DBG("Received PPP frame -> net_pkt(0x%08x)", (size_t)ppp->rx_pkt);

			/* Remove FCS */
			net_pkt_remove_tail(ppp->rx_pkt, MODEM_PPP_FRAME_TAIL_SIZE);

			net_pkt_cursor_init(ppp->rx_pkt);
			net_pkt_set_ppp(ppp->rx_pkt, true);

			net_recv_data(ppp->iface, ppp->rx_pkt);

			ppp->rx_pkt = NULL;

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

			break;
		}

		if (net_pkt_available_buffer(ppp->rx_pkt) == 1) {
			if (net_pkt_alloc_buffer(ppp->rx_pkt, CONFIG_NET_BUF_DATA_SIZE, AF_INET,
						 K_NO_WAIT) < 0) {
				LOG_WRN("Failed to alloc buffer -> net_pkt(0x%08x)",
					(size_t)ppp->rx_pkt);

				net_pkt_unref(ppp->rx_pkt);

				ppp->rx_pkt = NULL;

				ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

				break;
			}
		}

		if (byte == 0x7D) {
			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_UNESCAPING;

			break;
		}

		if (net_pkt_write_u8(ppp->rx_pkt, byte) < 0) {
			LOG_WRN("Dropped PPP frame -> net_pkt(0x%08x)", (size_t)ppp->rx_pkt);

			net_pkt_unref(ppp->rx_pkt);

			ppp->rx_pkt = NULL;

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;
		}

		break;

	case MODEM_PPP_RECEIVE_STATE_UNESCAPING:
		if (net_pkt_write_u8(ppp->rx_pkt, (byte ^ 0x20)) < 0) {
			LOG_WRN("Dropped PPP frame -> net_pkt(0x%08x)", (size_t)ppp->rx_pkt);

			net_pkt_unref(ppp->rx_pkt);

			ppp->rx_pkt = NULL;

			ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

			break;
		}

		ppp->receive_state = MODEM_PPP_RECEIVE_STATE_WRITING;

		break;
	}
}

static void modem_ppp_pipe_callback(struct modem_pipe *pipe, enum modem_pipe_event event,
				    void *user_data)
{
	struct modem_ppp *ppp = (struct modem_ppp *)user_data;

	if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
		k_work_submit(&ppp->process_work.work);
	}
}

static void modem_ppp_send_handler(struct k_work *item)
{
	struct modem_ppp_work_item *ppp_work_item = (struct modem_ppp_work_item *)item;
	struct modem_ppp *ppp = ppp_work_item->ppp;
	uint8_t byte;
	uint8_t *reserved;
	uint32_t reserved_size;
	int ret;

	if (ppp->tx_pkt == NULL) {
		modem_ppp_tx_net_pkt_buf_get(ppp, &ppp->tx_pkt);
	}

	if (ppp->tx_pkt != NULL) {
		/* Initialize wrap */
		if (ppp->transmit_state == MODEM_PPP_TRANSMIT_STATE_IDLE) {
			ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_SOF;
		}

		/* Fill transmit ring buffer */
		while (ring_buf_space_get(&ppp->transmit_rb) > 0) {
			byte = modem_ppp_wrap_net_pkt_byte(ppp);

			ring_buf_put(&ppp->transmit_rb, &byte, 1);

			if (ppp->transmit_state == MODEM_PPP_TRANSMIT_STATE_IDLE) {
				net_pkt_unref(ppp->tx_pkt);

				modem_ppp_tx_net_pkt_buf_get(ppp, &ppp->tx_pkt);

				break;
			}
		}
	}

	reserved_size = ring_buf_get_claim(&ppp->transmit_rb, &reserved, UINT32_MAX);

	if (reserved_size == 0) {
		ring_buf_get_finish(&ppp->transmit_rb, 0);

		return;
	}

	ret = modem_pipe_transmit(ppp->pipe, reserved, reserved_size);

	if (ret < 0) {
		ring_buf_get_finish(&ppp->transmit_rb, 0);
	} else {
		ring_buf_get_finish(&ppp->transmit_rb, (uint32_t)ret);
	}

	/* Resubmit send work if data remains */
	if ((ring_buf_is_empty(&ppp->transmit_rb) == false) || (ppp->tx_pkt != NULL)) {
		k_work_submit(&ppp->send_work.work);
	}
}

static void modem_ppp_process_handler(struct k_work *item)
{
	struct modem_ppp_work_item *ppp_work_item = (struct modem_ppp_work_item *)item;
	struct modem_ppp *ppp = ppp_work_item->ppp;
	int ret;

	ret = modem_pipe_receive(ppp->pipe, ppp->receive_buf, ppp->buf_size);

	if (ret < 1) {
		return;
	}

	for (int i = 0; i < ret; i++) {
		modem_ppp_process_received_byte(ppp, ppp->receive_buf[i]);
	}

	k_work_submit(&ppp->process_work.work);
}

static void modem_ppp_ppp_api_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);

	struct modem_ppp *ppp = (struct modem_ppp *)dev->data;

	net_ppp_init(iface);

	net_if_flag_set(iface, NET_IF_NO_AUTO_START);

	if (ppp->init_iface != NULL) {
		ppp->init_iface(iface);
	}

	ppp->iface = iface;
}

static int modem_ppp_ppp_api_start(const struct device *dev)
{
	return 0;
}

static int modem_ppp_ppp_api_stop(const struct device *dev)
{
	return 0;
}

static int modem_ppp_ppp_api_send(const struct device *dev, struct net_pkt *pkt)
{
	struct modem_ppp *ppp = (struct modem_ppp *)dev->data;

	if (atomic_test_bit(&ppp->state, MODEM_PPP_STATE_ATTACHED_BIT) == false) {
		return -EPERM;
	}

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

	net_pkt_ref(pkt);

	if (modem_ppp_tx_net_pkt_buf_put(ppp, pkt) == false) {
		LOG_WRN("tx pkt buf overrun");

		net_pkt_unref(pkt);

		return -ENOMEM;
	}

	k_work_submit(&ppp->send_work.work);

	return 0;
}

const struct ppp_api modem_ppp_ppp_api = {
	.iface_api.init = modem_ppp_ppp_api_init,
	.start = modem_ppp_ppp_api_start,
	.stop = modem_ppp_ppp_api_stop,
	.send = modem_ppp_ppp_api_send,
};

int modem_ppp_attach(struct modem_ppp *ppp, struct modem_pipe *pipe)
{
	if (atomic_test_and_set_bit(&ppp->state, MODEM_PPP_STATE_ATTACHED_BIT) == true) {
		return 0;
	}

	modem_pipe_attach(pipe, modem_ppp_pipe_callback, ppp);

	ppp->pipe = pipe;

	return 0;
}

struct net_if *modem_ppp_get_iface(struct modem_ppp *ppp)
{
	return ppp->iface;
}

void modem_ppp_release(struct modem_ppp *ppp)
{
	struct k_work_sync sync;
	struct net_pkt *pkt;

	if (atomic_test_and_clear_bit(&ppp->state, MODEM_PPP_STATE_ATTACHED_BIT) == false) {
		return;
	}

	modem_pipe_release(ppp->pipe);

	k_work_cancel_sync(&ppp->send_work.work, &sync);
	k_work_cancel_sync(&ppp->process_work.work, &sync);

	ppp->pipe = NULL;

	ppp->receive_state = MODEM_PPP_RECEIVE_STATE_HDR_SOF;

	if (ppp->rx_pkt != NULL) {
		net_pkt_unref(ppp->rx_pkt);
		ppp->rx_pkt = NULL;
	}

	ppp->transmit_state = MODEM_PPP_TRANSMIT_STATE_IDLE;

	if (ppp->tx_pkt != NULL) {
		net_pkt_unref(ppp->tx_pkt);
		ppp->tx_pkt = NULL;
	}

	while (modem_ppp_tx_net_pkt_buf_get(ppp, &pkt) == true) {
		net_pkt_unref(pkt);
	}
}

int modem_ppp_init_internal(const struct device *dev)
{
	struct modem_ppp *ppp = (struct modem_ppp *)dev->data;

	atomic_set(&ppp->state, 0);

	ring_buf_init(&ppp->transmit_rb, ppp->buf_size, ppp->transmit_buf);

	k_mutex_init(&ppp->tx_pkt_buf_lock);

	ppp->send_work.ppp = ppp;
	k_work_init(&ppp->send_work.work, modem_ppp_send_handler);

	ppp->process_work.ppp = ppp;
	k_work_init(&ppp->process_work.work, modem_ppp_process_handler);

	return 0;
}
