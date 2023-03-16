/*
 * Copyright (c) 2022 Trackunit Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_chat);

#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/modem/modem_chat.h>

#define MODEM_CHAT_MATCHES_INDEX_RESPONSE (0)
#define MODEM_CHAT_MATCHES_INDEX_ABORT	  (1)
#define MODEM_CHAT_MATCHES_INDEX_UNSOL	  (2)

#define MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT (0)

static void modem_chat_script_stop(struct modem_chat *chat, enum modem_chat_script_result result)
{
	/* Handle result */
	if (result == MODEM_CHAT_SCRIPT_RESULT_SUCCESS) {
		LOG_DBG("%s: complete", chat->script->name);
	} else if (result == MODEM_CHAT_SCRIPT_RESULT_ABORT) {
		LOG_WRN("%s: aborted", chat->script->name);
	} else {
		LOG_WRN("%s: timed out", chat->script->name);
	}

	/* Clear script running state */
	atomic_clear_bit(&chat->script_state, MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT);

	/* Call back with result */
	if (chat->script->callback != NULL) {
		chat->script->callback(chat, result, chat->user_data);
	}

	/* Clear reference to script */
	chat->script = NULL;

	/* Clear response and abort commands */
	chat->matches[MODEM_CHAT_MATCHES_INDEX_ABORT] = NULL;
	chat->matches_size[MODEM_CHAT_MATCHES_INDEX_ABORT] = 0;
	chat->matches[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = NULL;
	chat->matches_size[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = 0;

	/* Cancel timeout work */
	k_work_cancel_delayable(&chat->script_timeout_work.dwork);
}

static void modem_chat_script_send(struct modem_chat *chat)
{
	/* Initialize script send work */
	chat->script_send_request_pos = 0;
	chat->script_send_delimiter_pos = 0;

	/* Schedule script send work */
	k_work_schedule(&chat->script_send_work.dwork, K_NO_WAIT);
}

static void modem_chat_script_next(struct modem_chat *chat, bool initial)
{
	const struct modem_chat_script_chat *script_chat;

	/* Advance iterator if not initial */
	if (initial == true) {
		/* Reset iterator */
		chat->script_chat_it = 0;
	} else {
		/* Advance iterator */
		chat->script_chat_it++;
	}

	/* Check if end of script reached */
	if (chat->script_chat_it == chat->script->script_chats_size) {
		modem_chat_script_stop(chat, MODEM_CHAT_SCRIPT_RESULT_SUCCESS);

		return;
	}

	LOG_DBG("%s: step: %u", chat->script->name, chat->script_chat_it);

	script_chat = &chat->script->script_chats[chat->script_chat_it];

	/* Set response command handlers */
	chat->matches[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = script_chat->response_matches;
	chat->matches_size[MODEM_CHAT_MATCHES_INDEX_RESPONSE] = script_chat->response_matches_size;

	/* Check if work must be sent */
	if (strlen(script_chat->request) > 0) {
		modem_chat_script_send(chat);
	}
}

static void modem_chat_script_start(struct modem_chat *chat, const struct modem_chat_script *script)
{
	/* Save script */
	chat->script = script;

	/* Set abort matches */
	chat->matches[MODEM_CHAT_MATCHES_INDEX_ABORT] = script->abort_matches;
	chat->matches_size[MODEM_CHAT_MATCHES_INDEX_ABORT] = script->abort_matches_size;

	LOG_DBG("%s", chat->script->name);

	/* Set first script command */
	modem_chat_script_next(chat, true);

	/* Start timeout work if script started */
	if (chat->script != NULL) {
		k_work_schedule(&chat->script_timeout_work.dwork, K_SECONDS(chat->script->timeout));
	}
}

static void modem_chat_script_run_handler(struct k_work *item)
{
	struct modem_chat_script_run_work_item *script_run_work =
		(struct modem_chat_script_run_work_item *)item;

	struct modem_chat *chat = script_run_work->chat;
	const struct modem_chat_script *script = script_run_work->script;

	/* Start script */
	modem_chat_script_start(chat, script);
}

static void modem_chat_script_timeout_handler(struct k_work *item)
{
	struct modem_chat_work_item *script_timeout_work = (struct modem_chat_work_item *)item;
	struct modem_chat *chat = script_timeout_work->chat;

	/* Abort script */
	modem_chat_script_stop(chat, MODEM_CHAT_SCRIPT_RESULT_TIMEOUT);
}

static void modem_chat_script_abort_handler(struct k_work *item)
{
	struct modem_chat_script_abort_work_item *script_abort_work =
		(struct modem_chat_script_abort_work_item *)item;

	struct modem_chat *chat = script_abort_work->chat;

	/* Validate script is currently running */
	if (chat->script == NULL) {
		return;
	}

	/* Abort script */
	modem_chat_script_stop(chat, MODEM_CHAT_SCRIPT_RESULT_ABORT);
}

static bool modem_chat_script_send_request(struct modem_chat *chat)
{
	const struct modem_chat_script_chat *script_chat =
		&chat->script->script_chats[chat->script_chat_it];

	uint16_t script_chat_request_size = strlen(script_chat->request);
	uint8_t *script_chat_request_start;
	uint16_t script_chat_request_remaining;
	int ret;

	/* Validate data to send */
	if (script_chat_request_size == chat->script_send_request_pos) {
		return true;
	}

	script_chat_request_start = (uint8_t *)&script_chat->request[chat->script_send_request_pos];
	script_chat_request_remaining = script_chat_request_size - chat->script_send_request_pos;

	/* Send data through pipe */
	ret = modem_pipe_transmit(chat->pipe, script_chat_request_start,
				  script_chat_request_remaining);

	/* Validate transmit successful */
	if (ret < 1) {
		return false;
	}

	/* Update script send position */
	chat->script_send_request_pos += (uint16_t)ret;

	/* Check if data remains */
	if (chat->script_send_request_pos < script_chat_request_size) {
		return false;
	}

	return true;
}

static bool modem_chat_script_send_delimiter(struct modem_chat *chat)
{
	uint8_t *script_chat_delimiter_start;
	uint8_t script_chat_delimiter_remaining;
	int ret;

	/* Validate data to send */
	if (chat->delimiter_size == chat->script_send_delimiter_pos) {
		return true;
	}

	script_chat_delimiter_start = (uint8_t *)&chat->delimiter[chat->script_send_delimiter_pos];
	script_chat_delimiter_remaining = chat->delimiter_size - chat->script_send_delimiter_pos;

	/* Send data through pipe */
	ret = modem_pipe_transmit(chat->pipe, script_chat_delimiter_start,
				  script_chat_delimiter_remaining);

	/* Validate transmit successful */
	if (ret < 1) {
		return false;
	}

	/* Update script send position */
	chat->script_send_delimiter_pos += (uint8_t)ret;

	/* Check if data remains */
	if (chat->script_send_delimiter_pos < chat->delimiter_size) {
		return false;
	}

	return true;
}

static bool modem_chat_script_chat_is_no_response(struct modem_chat *chat)
{
	const struct modem_chat_script_chat *script_chat =
		&chat->script->script_chats[chat->script_chat_it];

	return (script_chat->response_matches_size == 0) ? true : false;
}

static uint16_t modem_chat_script_chat_get_send_timeout(struct modem_chat *chat)
{
	const struct modem_chat_script_chat *script_chat =
		&chat->script->script_chats[chat->script_chat_it];

	return script_chat->timeout;
}

static void modem_chat_script_send_handler(struct k_work *item)
{
	struct modem_chat_work_item *send_work = (struct modem_chat_work_item *)item;
	struct modem_chat *chat = send_work->chat;
	uint16_t timeout;

	/* Validate script running */
	if (chat->script == NULL) {
		return;
	}

	/* Send request */
	if (modem_chat_script_send_request(chat) == false) {
		k_work_schedule(&chat->script_send_work.dwork, chat->process_timeout);

		return;
	}

	/* Send delimiter */
	if (modem_chat_script_send_delimiter(chat) == false) {
		k_work_schedule(&chat->script_send_work.dwork, chat->process_timeout);

		return;
	}

	/* Check if script command is no response */
	if (modem_chat_script_chat_is_no_response(chat)) {
		timeout = modem_chat_script_chat_get_send_timeout(chat);

		if (timeout == 0) {
			modem_chat_script_next(chat, false);
		} else {
			k_work_schedule(&chat->script_send_timeout_work.dwork, K_MSEC(timeout));
		}
	}
}

static void modem_chat_script_send_timeout_handler(struct k_work *item)
{
	struct modem_chat_work_item *timeout_work = (struct modem_chat_work_item *)item;
	struct modem_chat *chat = timeout_work->chat;

	/* Validate script is currently running */
	if (chat->script == NULL) {
		return;
	}

	modem_chat_script_next(chat, false);
}

static void modem_chat_parse_reset(struct modem_chat *chat)
{
	/* Reset parameters used for parsing */
	chat->receive_buf_len = 0;
	chat->delimiter_match_len = 0;
	chat->argc = 0;
	chat->parse_match = NULL;
}

/* Exact match is stored at end of receive buffer */
static void modem_chat_parse_save_match(struct modem_chat *chat)
{
	uint8_t *argv;

	/* Store length of match including NULL to avoid overwriting it if buffer overruns */
	chat->parse_match_len = chat->receive_buf_len + 1;

	/* Copy match to end of receive buffer */
	argv = &chat->receive_buf[chat->receive_buf_size - chat->parse_match_len];

	/* Copy match to end of receive buffer (excluding NULL) */
	memcpy(argv, &chat->receive_buf[0], chat->parse_match_len - 1);

	/* Save match */
	chat->argv[chat->argc] = argv;

	/* Terminate match */
	chat->receive_buf[chat->receive_buf_size - 1] = '\0';

	/* Increment argument count */
	chat->argc++;
}

static bool modem_chat_match_matches_received(struct modem_chat *chat,
					      const struct modem_chat_match *match)
{
	for (uint16_t i = 0; i < match->match_size; i++) {
		if ((match->match[i] == chat->receive_buf[i]) ||
		    (match->wildcards == true && match->match[i] == '?')) {
			continue;
		}

		return false;
	}

	return true;
}

static bool modem_chat_parse_find_match(struct modem_chat *chat)
{
	/* Find in all matches types */
	for (uint16_t i = 0; i < ARRAY_SIZE(chat->matches); i++) {
		/* Find in all matches of matches type */
		for (uint16_t u = 0; u < chat->matches_size[i]; u++) {
			/* Validate match size matches received data length */
			if (chat->matches[i][u].match_size != chat->receive_buf_len) {
				continue;
			}

			/* Validate match */
			if (modem_chat_match_matches_received(chat, &chat->matches[i][u]) ==
			    false) {
				continue;
			}

			/* Complete match found */
			chat->parse_match = &chat->matches[i][u];
			chat->parse_match_type = i;

			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_is_separator(struct modem_chat *chat)
{
	for (uint16_t i = 0; i < chat->parse_match->separators_size; i++) {
		if ((chat->parse_match->separators[i]) ==
		    (chat->receive_buf[chat->receive_buf_len - 1])) {
			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_end_del_start(struct modem_chat *chat)
{
	for (uint8_t i = 0; i < chat->delimiter_size; i++) {
		if (chat->receive_buf[chat->receive_buf_len - 1] == chat->delimiter[i]) {
			return true;
		}
	}

	return false;
}

static bool modem_chat_parse_end_del_complete(struct modem_chat *chat)
{
	/* Validate length of end delimiter */
	if (chat->receive_buf_len < chat->delimiter_size) {
		return false;
	}

	/* Compare end delimiter with receive buffer content */
	return (memcmp(&chat->receive_buf[chat->receive_buf_len - chat->delimiter_size],
		       chat->delimiter, chat->delimiter_size) == 0)
		       ? true
		       : false;
}

static void modem_chat_on_command_received_unsol(struct modem_chat *chat)
{
	/* Callback */
	if (chat->parse_match->callback != NULL) {
		chat->parse_match->callback(chat, (char **)chat->argv, chat->argc, chat->user_data);
	}
}

static void modem_chat_on_command_received_abort(struct modem_chat *chat)
{
	/* Callback */
	if (chat->parse_match->callback != NULL) {
		chat->parse_match->callback(chat, (char **)chat->argv, chat->argc, chat->user_data);
	}

	/* Abort script */
	modem_chat_script_stop(chat, MODEM_CHAT_SCRIPT_RESULT_ABORT);
}

static void modem_chat_on_command_received_resp(struct modem_chat *chat)
{
	/* Callback */
	if (chat->parse_match->callback != NULL) {
		chat->parse_match->callback(chat, (char **)chat->argv, chat->argc, chat->user_data);
	}

	/* Advance script */
	modem_chat_script_next(chat, false);
}

static bool modem_chat_parse_find_catch_all_match(struct modem_chat *chat)
{
	/* Find in all matches types */
	for (uint16_t i = 0; i < ARRAY_SIZE(chat->matches); i++) {
		/* Find in all matches of matches type */
		for (uint16_t u = 0; u < chat->matches_size[i]; u++) {
			/* Validate match config is matching previous bytes */
			if (chat->matches[i][u].match_size == 0) {
				chat->parse_match = &chat->matches[i][u];
				chat->parse_match_type = i;

				return true;
			}
		}
	}

	return false;
}

static void modem_chat_on_command_received(struct modem_chat *chat)
{
	LOG_DBG("\"%s\"", chat->argv[0]);

	switch (chat->parse_match_type) {
	case MODEM_CHAT_MATCHES_INDEX_UNSOL:
		modem_chat_on_command_received_unsol(chat);
		break;

	case MODEM_CHAT_MATCHES_INDEX_ABORT:
		modem_chat_on_command_received_abort(chat);
		break;

	case MODEM_CHAT_MATCHES_INDEX_RESPONSE:
		modem_chat_on_command_received_resp(chat);
		break;
	}
}

static void modem_chat_on_unknown_command_received(struct modem_chat *chat)
{
	/* Try to find catch all match */
	if (modem_chat_parse_find_catch_all_match(chat) == false) {
		return;
	}

	/* Terminate received command */
	chat->receive_buf[chat->receive_buf_len - chat->delimiter_size] = '\0';

	/* Parse command */
	chat->argv[0] = "";
	chat->argv[1] = chat->receive_buf;
	chat->argc = 2;

	/* Invoke on response received */
	modem_chat_on_command_received(chat);
}

static void modem_chat_process_byte(struct modem_chat *chat, uint8_t byte)
{
	/* Validate receive buffer not overrun */
	if (chat->receive_buf_size == chat->receive_buf_len) {
		LOG_WRN("Receive buffer overrun");
		modem_chat_parse_reset(chat);

		return;
	}

	/* Validate argv buffer not overrun */
	if (chat->argc == chat->argv_size) {
		LOG_WRN("Argv buffer overrun");
		modem_chat_parse_reset(chat);

		return;
	}

	/* Copy byte to receive buffer */
	chat->receive_buf[chat->receive_buf_len] = byte;
	chat->receive_buf_len++;

	/* Validate end delimiter not complete */
	if (modem_chat_parse_end_del_complete(chat) == true) {
		/* Filter out empty lines */
		if (chat->receive_buf_len == chat->delimiter_size) {
			/* Reset parser */
			modem_chat_parse_reset(chat);

			return;
		}

		/* Check if match exists */
		if (chat->parse_match == NULL) {
			/* Handle unknown command */
			modem_chat_on_unknown_command_received(chat);

			/* Reset parser */
			modem_chat_parse_reset(chat);

			return;
		}

		/* Check if trailing argument exists */
		if (chat->parse_arg_len > 0) {
			chat->argv[chat->argc] =
				&chat->receive_buf[chat->receive_buf_len - chat->delimiter_size -
						   chat->parse_arg_len];
			chat->receive_buf[chat->receive_buf_len - chat->delimiter_size] = '\0';
			chat->argc++;
		}

		/* Handle received command */
		modem_chat_on_command_received(chat);

		/* Reset parser */
		modem_chat_parse_reset(chat);

		return;
	}

	/* Validate end delimiter not started */
	if (modem_chat_parse_end_del_start(chat) == true) {
		return;
	}

	/* Find matching command if missing */
	if (chat->parse_match == NULL) {
		/* Find matching command */
		if (modem_chat_parse_find_match(chat) == false) {
			return;
		}

		/* Save match */
		modem_chat_parse_save_match(chat);

		/* Prepare argument parser */
		chat->parse_arg_len = 0;

		return;
	}

	/* Check if separator reached */
	if (modem_chat_parse_is_separator(chat) == true) {
		/* Check if argument is empty */
		if (chat->parse_arg_len == 0) {
			/* Save empty argument */
			chat->argv[chat->argc] = "";
		} else {
			/* Save pointer to start of argument */
			chat->argv[chat->argc] =
				&chat->receive_buf[chat->receive_buf_len - chat->parse_arg_len - 1];

			/* Replace separator with string terminator */
			chat->receive_buf[chat->receive_buf_len - 1] = '\0';
		}

		/* Increment argument count */
		chat->argc++;

		/* Reset parse argument length */
		chat->parse_arg_len = 0;

		return;
	}

	/* Increment argument length */
	chat->parse_arg_len++;
}

static bool modem_chat_discard_byte(struct modem_chat *chat, uint8_t byte)
{
	for (uint8_t i = 0; i < chat->filter_size; i++) {
		if (byte == chat->filter[i]) {
			return true;
		}
	}

	return false;
}

/* Process chunk of received bytes */
static void modem_chat_process_bytes(struct modem_chat *chat)
{
	for (uint16_t i = 0; i < chat->work_buf_len; i++) {
		if (modem_chat_discard_byte(chat, chat->work_buf[i])) {
			continue;
		}

		modem_chat_process_byte(chat, chat->work_buf[i]);
	}
}

static void modem_chat_process_handler(struct k_work *item)
{
	struct modem_chat_work_item *process_work = (struct modem_chat_work_item *)item;
	struct modem_chat *chat = process_work->chat;
	int ret;

	/* Fill work buffer */
	ret = modem_pipe_receive(chat->pipe, chat->work_buf, sizeof(chat->work_buf));

	/* Validate data received */
	if (ret < 1) {
		return;
	}

	/* Save received data length */
	chat->work_buf_len = (size_t)ret;

	/* Process data */
	modem_chat_process_bytes(chat);

	k_work_schedule(&chat->process_work.dwork, K_NO_WAIT);
}

static void modem_chat_pipe_callback(struct modem_pipe *pipe, enum modem_pipe_event event,
				     void *user_data)
{
	struct modem_chat *chat = (struct modem_chat *)user_data;

	if (event == MODEM_PIPE_EVENT_RECEIVE_READY) {
		k_work_schedule(&chat->process_work.dwork, chat->process_timeout);
	}
}

/*********************************************************
 * GLOBAL FUNCTIONS
 *********************************************************/
int modem_chat_init(struct modem_chat *chat, const struct modem_chat_config *config)
{
	__ASSERT_NO_MSG(chat != NULL);
	__ASSERT_NO_MSG(config != NULL);
	__ASSERT_NO_MSG(config->receive_buf != NULL);
	__ASSERT_NO_MSG(config->receive_buf_size > 0);
	__ASSERT_NO_MSG(config->argv != NULL);
	__ASSERT_NO_MSG(config->argv_size > 0);
	__ASSERT_NO_MSG(config->delimiter != NULL);
	__ASSERT_NO_MSG(config->delimiter_size > 0);
	__ASSERT_NO_MSG(!((config->filter == NULL) && (config->filter > 0)));
	__ASSERT_NO_MSG(!((config->unsol_matches == NULL) && (config->unsol_matches_size > 0)));

	memset(chat, 0x00, sizeof(*chat));

	chat->pipe = NULL;
	chat->user_data = config->user_data;
	chat->receive_buf = config->receive_buf;
	chat->receive_buf_size = config->receive_buf_size;
	chat->argv = config->argv;
	chat->argv_size = config->argv_size;
	chat->delimiter = config->delimiter;
	chat->delimiter_size = config->delimiter_size;
	chat->filter = config->filter;
	chat->filter_size = config->filter_size;
	chat->matches[MODEM_CHAT_MATCHES_INDEX_UNSOL] = config->unsol_matches;
	chat->matches_size[MODEM_CHAT_MATCHES_INDEX_UNSOL] = config->unsol_matches_size;
	chat->process_timeout = config->process_timeout;

	atomic_set(&chat->script_state, 0);

	chat->process_work.chat = chat;
	k_work_init_delayable(&chat->process_work.dwork, modem_chat_process_handler);

	chat->script_run_work.chat = chat;
	k_work_init(&chat->script_run_work.work, modem_chat_script_run_handler);

	chat->script_timeout_work.chat = chat;
	k_work_init_delayable(&chat->script_timeout_work.dwork, modem_chat_script_timeout_handler);

	chat->script_abort_work.chat = chat;
	k_work_init(&chat->script_abort_work.work, modem_chat_script_abort_handler);

	chat->script_send_work.chat = chat;
	k_work_init_delayable(&chat->script_send_work.dwork, modem_chat_script_send_handler);

	chat->script_send_timeout_work.chat = chat;
	k_work_init_delayable(&chat->script_send_timeout_work.dwork,
			      modem_chat_script_send_timeout_handler);

	return 0;
}

int modem_chat_attach(struct modem_chat *chat, struct modem_pipe *pipe)
{
	chat->pipe = pipe;

	modem_chat_parse_reset(chat);

	modem_pipe_attach(chat->pipe, modem_chat_pipe_callback, chat);

	return 0;
}

int modem_chat_script_run(struct modem_chat *chat, const struct modem_chat_script *script)
{
	bool script_is_running;

	if (chat->pipe == NULL) {
		return -EPERM;
	}

	/* Validate script */
	if ((script->script_chats == NULL) || (script->script_chats_size == 0) ||
	    ((script->abort_matches != NULL) && (script->abort_matches_size == 0))) {
		return -EINVAL;
	}

	/* Validate script commands */
	for (uint16_t i = 0; i < script->script_chats_size; i++) {
		if ((strlen(script->script_chats[i].request) == 0) &&
		    (script->script_chats[i].response_matches_size == 0)) {
			return -EINVAL;
		}
	}

	script_is_running =
		atomic_test_and_set_bit(&chat->script_state, MODEM_CHAT_SCRIPT_STATE_RUNNING_BIT);

	if (script_is_running == true) {
		return -EBUSY;
	}

	chat->script_run_work.script = script;

	k_work_submit(&chat->script_run_work.work);

	return 0;
}

void modem_chat_script_abort(struct modem_chat *chat)
{
	k_work_submit(&chat->script_abort_work.work);
}

void modem_chat_release(struct modem_chat *chat)
{
	struct k_work_sync sync;

	modem_pipe_release(chat->pipe);

	k_work_cancel_sync(&chat->script_run_work.work, &sync);
	k_work_cancel_sync(&chat->script_abort_work.work, &sync);
	k_work_cancel_delayable_sync(&chat->process_work.dwork, &sync);
	k_work_cancel_delayable_sync(&chat->script_send_work.dwork, &sync);

	chat->pipe = NULL;
	chat->receive_buf_len = 0;
	chat->work_buf_len = 0;
	chat->argc = 0;
	chat->script = NULL;
	chat->script_chat_it = 0;
	atomic_set(&chat->script_state, 0);
	chat->script_send_request_pos = 0;
	chat->script_send_delimiter_pos = 0;
	chat->parse_match = NULL;
	chat->parse_match_len = 0;
	chat->parse_arg_len = 0;
}
