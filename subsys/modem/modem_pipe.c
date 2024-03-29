#include <zephyr/modem/pipe.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modem_pipe);

void modem_pipe_init(struct modem_pipe *pipe, void *data, struct modem_pipe_api *api)
{
	__ASSERT_NO_MSG(pipe != NULL);
	__ASSERT_NO_MSG(data != NULL);
	__ASSERT_NO_MSG(api != NULL);

	pipe->data = data;
	pipe->api = api;
	pipe->callback = NULL;
	pipe->user_data = NULL;
	pipe->state = MODEM_PIPE_STATE_CLOSED;

	k_mutex_init(&pipe->lock);
	k_condvar_init(&pipe->condvar);
}

int modem_pipe_open(struct modem_pipe *pipe)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	ret = pipe->api->open(pipe->data);

	if (ret < 0) {
		k_mutex_unlock(&pipe->lock);

		return ret;
	}

	if (pipe->state == MODEM_PIPE_STATE_OPEN) {
		k_mutex_unlock(&pipe->lock);

		return 0;
	}

	k_condvar_wait(&pipe->condvar, &pipe->lock, K_MSEC(10000));

	ret = (pipe->state == MODEM_PIPE_STATE_OPEN) ? 0 : -EAGAIN;

	k_mutex_unlock(&pipe->lock);

	return ret;
}

int modem_pipe_open_async(struct modem_pipe *pipe)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	ret = pipe->api->open(pipe->data);

	k_mutex_unlock(&pipe->lock);

	return ret;
}

void modem_pipe_attach(struct modem_pipe *pipe, modem_pipe_api_callback callback, void *user_data)
{
	k_mutex_lock(&pipe->lock, K_FOREVER);

	pipe->callback = callback;
	pipe->user_data = user_data;

	k_mutex_unlock(&pipe->lock);
}

int modem_pipe_transmit(struct modem_pipe *pipe, const uint8_t *buf, size_t size)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	if (pipe->state == MODEM_PIPE_STATE_CLOSED) {
		k_mutex_unlock(&pipe->lock);

		return -EPERM;
	}

	ret = pipe->api->transmit(pipe->data, buf, size);

	k_mutex_unlock(&pipe->lock);

	return ret;
}

int modem_pipe_receive(struct modem_pipe *pipe, uint8_t *buf, size_t size)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	if (pipe->state == MODEM_PIPE_STATE_CLOSED) {
		k_mutex_unlock(&pipe->lock);

		return -EPERM;
	}

	ret = pipe->api->receive(pipe->data, buf, size);

	k_mutex_unlock(&pipe->lock);

	return ret;
}

void modem_pipe_release(struct modem_pipe *pipe)
{
	k_mutex_lock(&pipe->lock, K_FOREVER);

	pipe->callback = NULL;
	pipe->user_data = NULL;

	k_mutex_unlock(&pipe->lock);
}

int modem_pipe_close(struct modem_pipe *pipe)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	ret = pipe->api->close(pipe->data);

	if (ret < 0) {
		k_mutex_unlock(&pipe->lock);

		return ret;
	}

	if (pipe->state == MODEM_PIPE_STATE_CLOSED) {
		k_mutex_unlock(&pipe->lock);

		return 0;
	}

	k_condvar_wait(&pipe->condvar, &pipe->lock, K_MSEC(10000));

	ret = (pipe->state == MODEM_PIPE_STATE_CLOSED) ? 0 : -EAGAIN;

	k_mutex_unlock(&pipe->lock);

	k_mutex_unlock(&pipe->lock);

	return ret;
}

int modem_pipe_close_async(struct modem_pipe *pipe)
{
	int ret;

	k_mutex_lock(&pipe->lock, K_FOREVER);

	ret = pipe->api->close(pipe->data);

	k_mutex_unlock(&pipe->lock);

	return ret;
}

void modem_pipe_notify_opened(struct modem_pipe *pipe)
{
	k_mutex_lock(&pipe->lock, K_FOREVER);

	pipe->state = MODEM_PIPE_STATE_OPEN;

	if (pipe->callback != NULL) {
		pipe->callback(pipe, MODEM_PIPE_EVENT_OPENED, pipe->user_data);
	}

	k_condvar_signal(&pipe->condvar);

	k_mutex_unlock(&pipe->lock);
}

void modem_pipe_notify_closed(struct modem_pipe *pipe)
{
	k_mutex_lock(&pipe->lock, K_FOREVER);

	pipe->state = MODEM_PIPE_STATE_CLOSED;

	if (pipe->callback != NULL) {
		pipe->callback(pipe, MODEM_PIPE_EVENT_CLOSED, pipe->user_data);
	}

	k_condvar_signal(&pipe->condvar);

	k_mutex_unlock(&pipe->lock);
}

void modem_pipe_notify_receive_ready(struct modem_pipe *pipe)
{
	k_mutex_lock(&pipe->lock, K_FOREVER);

	if (pipe->callback != NULL) {
		pipe->callback(pipe, MODEM_PIPE_EVENT_RECEIVE_READY, pipe->user_data);
	}

	k_mutex_unlock(&pipe->lock);
}
