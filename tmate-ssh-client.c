#include <sys/socket.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <event.h>
#include <assert.h>

#include "tmate.h"
#include "window-copy.h"

static void on_ssh_client_event(struct tmate_ssh_client *client);
static void __on_ssh_client_event(evutil_socket_t fd, short what, void *arg);

static void printflike(2, 3) kill_ssh_client(struct tmate_ssh_client *client,
					     const char *fmt, ...);
static void printflike(2, 3) reconnect_ssh_client(struct tmate_ssh_client *client,
						  const char *fmt, ...);

static void read_channel(struct tmate_ssh_client *client)
{
	struct tmate_decoder *decoder = &client->tmate_session->decoder;
	char *buf;
	ssize_t len;

	for (;;) {
		tmate_decoder_get_buffer(decoder, &buf, &len);
		len = ssh_channel_read_nonblocking(client->channel, buf, len, 0);
		if (len < 0) {
			reconnect_ssh_client(client, "Error reading from channel: %s",
					     ssh_get_error(client->session));
			break;
		}

		if (len == 0)
			break;

		tmate_decoder_commit(decoder, len);
	}
}

static void on_decoder_read(void *userdata, struct tmate_unpacker *uk)
{
	struct tmate_ssh_client *client = userdata;
	tmate_dispatch_slave_message(client->tmate_session, uk);
}

static void on_encoder_write(void *userdata, struct evbuffer *buffer)
{
	struct tmate_ssh_client *client = userdata;
	ssize_t len, written;
	unsigned char *buf;

	for(;;) {
		len = evbuffer_get_length(buffer);
		if (!len)
			break;

		buf = evbuffer_pullup(buffer, -1);

		written = ssh_channel_write(client->channel, buf, len);
		if (written < 0) {
			reconnect_ssh_client(client, "Error writing to channel: %s",
					     ssh_get_error(client->session));
			break;
		}

		evbuffer_drain(buffer, written);
	}
}

static void on_ssh_auth_server_complete(struct tmate_ssh_client *connected_client)
{
	/*
	 * The first ssh connection succeeded. Hopefully this one offers the
	 * best latency. We can now kill the other ssh clients that are trying
	 * to connect.
	 */
	struct tmate_session *session = connected_client->tmate_session;
	struct tmate_ssh_client *client, *tmp_client;

	TAILQ_FOREACH_SAFE(client, &session->clients, node, tmp_client) {
		if (client == connected_client)
			continue;

		assert(!client->has_encoder);
		kill_ssh_client(client, NULL);
	}
}

static char *get_identity(void)
{
	char *identity;

	identity = options_get_string(global_options, "tmate-identity");
	if (!strlen(identity))
		return NULL;

	if (strchr(identity, '/'))
		identity = xstrdup(identity);
	else
		xasprintf(&identity, "%%d/%s", identity);

	return identity;
}

static int passphrase_callback(__unused const char *prompt, char *buf, size_t len,
			       __unused int echo, __unused int verify, void *userdata)
{
	struct tmate_ssh_client *client = userdata;

	client->tmate_session->need_passphrase = 1;

	if (client->tmate_session->passphrase)
		strncpy(buf, client->tmate_session->passphrase, len);
	else
		strcpy(buf, "");

	return 0;
}

static void on_passphrase_read(const char *passphrase, void *private)
{
	struct tmate_ssh_client *client = private;

	client->tmate_session->passphrase = xstrdup(passphrase);
	on_ssh_client_event(client);
}

static void request_passphrase(struct tmate_ssh_client *client)
{
	struct window_pane *wp;
	struct window_copy_mode_data *data;

	/*
	 * We'll display the prompt on the first pane.
	 * It doesn't make much sense, but it's simpler to reuse the copy mode
	 * and its key parsing logic compared to rolling something on our own.
	 */
	wp = RB_MIN(window_pane_tree, &all_window_panes);

	if (wp->mode) {
		data = wp->modedata;
		if (data->inputtype == WINDOW_COPY_PASSWORD) {
			/* We are already requesting the passphrase */
			return;
		}
		window_pane_reset_mode(wp);
	}

	window_pane_set_mode(wp, &window_copy_mode);
	window_copy_init_from_pane(wp, 0);
	data = wp->modedata;

	data->inputtype = WINDOW_COPY_PASSWORD;
	data->inputprompt = "SSH key passphrase";

	mode_key_init(&data->mdata, &mode_key_tree_vi_edit);

	window_copy_update_selection(wp, 1);
	window_copy_redraw_screen(wp);

	data->password_cb = on_passphrase_read;
	data->password_cb_private = client;
}

static void register_session_fd_event(struct tmate_ssh_client *client)
{
	if (!event_initialized(&client->ev_ssh)) {
		int flag = 1;
		setsockopt(ssh_get_fd(client->session), IPPROTO_TCP,
			   TCP_NODELAY, &flag, sizeof(flag));

		event_assign(&client->ev_ssh, client->tmate_session->ev_base,
			     ssh_get_fd(client->session),
			     EV_READ | EV_PERSIST, __on_ssh_client_event, client);
		event_add(&client->ev_ssh, NULL);
	}
}

static void on_ssh_client_event(struct tmate_ssh_client *client)
{
	char *identity;
	ssh_key pubkey;
	int key_type;
	unsigned char *hash;
	ssize_t hash_len;
	char *hash_str;
	const char *server_hash_str;
	int match;

	int verbosity = SSH_LOG_NOLOG + log_get_level();
	int port = options_get_number(global_options, "tmate-server-port");

	ssh_session session = client->session;
	ssh_channel channel = client->channel;

	switch (client->state) {
	case SSH_INIT:
		client->session = session = ssh_new();
		if (!session) {
			tmate_fatal("cannot initialize");
			return;
		}

		ssh_set_callbacks(session, &client->ssh_callbacks);

		client->channel = channel = ssh_channel_new(session);
		if (!channel) {
			tmate_fatal("cannot initialize");
			return;
		}

		ssh_set_blocking(session, 0);
		ssh_options_set(session, SSH_OPTIONS_HOST, client->server_ip);
		ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
		ssh_options_set(session, SSH_OPTIONS_PORT, &port);
		ssh_options_set(session, SSH_OPTIONS_USER, "tmate");
		ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "yes");

		if ((identity = get_identity())) {
			/*
			 * FIXME libssh will continue with the next set of
			 * keys if the identity has a passphrase and the
			 * regular one doesn't.
			 */
			ssh_options_set(session, SSH_OPTIONS_IDENTITY, identity);
			free(identity);
		}

		client->state = SSH_CONNECT;
		/* fall through */

	case SSH_CONNECT:
		switch (ssh_connect(session)) {
		case SSH_AGAIN:
			register_session_fd_event(client);
			return;
		case SSH_ERROR:
			reconnect_ssh_client(client, "Error connecting: %s",
					     ssh_get_error(session));
			return;
		case SSH_OK:
			register_session_fd_event(client);
			tmate_debug("Establishing connection to %s", client->server_ip);
			client->state = SSH_AUTH_SERVER;
			/* fall through */
		}

	case SSH_AUTH_SERVER:
		if (ssh_get_publickey(session, &pubkey) < 0)
			tmate_fatal("ssh_get_publickey");

		if (ssh_get_publickey_hash(pubkey, SSH_PUBLICKEY_HASH_MD5, &hash, &hash_len) < 0) {
			kill_ssh_client(client, "Cannot authenticate server");
			return;
		}

		hash_str = ssh_get_hexa(hash, hash_len);
		if (!hash_str)
			tmate_fatal("malloc failed");

		key_type = ssh_key_type(pubkey);

		switch (key_type) {
		case SSH_KEYTYPE_RSA:
			server_hash_str = options_get_string(global_options,
						"tmate-server-rsa-fingerprint");
			break;
		case SSH_KEYTYPE_ECDSA:
			server_hash_str = options_get_string(global_options,
						"tmate-server-ecdsa-fingerprint");
			break;
		default:
			server_hash_str = "";
		}

		match = !strcmp(hash_str, server_hash_str);

		ssh_key_free(pubkey);
		ssh_clean_pubkey_hash(&hash);
		free(hash_str);

		if (!match) {
			kill_ssh_client(client, "Cannot authenticate server");
			return;
		}

		/*
		 * At this point, we abort other connection attempts to the
		 * other tmate servers, since we have reached the fastest one.
		 * We need to do it before we ask the user its passphrase,
		 * otherwise the speed test would be biased.
		 */
		tmate_debug("Connected to %s", client->server_ip);
		on_ssh_auth_server_complete(client);
		client->state = SSH_AUTH_CLIENT;

		/* fall through */

	case SSH_AUTH_CLIENT:
		client->tried_passphrase = client->tmate_session->passphrase;
		switch (ssh_userauth_autopubkey(session, client->tried_passphrase)) {
		case SSH_AUTH_AGAIN:
			return;
		case SSH_AUTH_PARTIAL:
		case SSH_AUTH_INFO:
		case SSH_AUTH_DENIED:
			if (client->tmate_session->need_passphrase)
				request_passphrase(client);
			else
				kill_ssh_client(client, "SSH keys not found."
				" Run 'ssh-keygen' to create keys and try again.");

			if (client->tried_passphrase)
				tmate_status_message("Can't load SSH key."
				" Try typing passphrase again in case of typo. ctrl-c to abort.");
			return;
		case SSH_AUTH_ERROR:
			reconnect_ssh_client(client, "Auth error: %s",
					     ssh_get_error(session));
			return;
		case SSH_AUTH_SUCCESS:
			tmate_debug("Auth successful");
			client->state = SSH_OPEN_CHANNEL;
			/* fall through */
		}

	case SSH_OPEN_CHANNEL:
		switch (ssh_channel_open_session(channel)) {
		case SSH_AGAIN:
			return;
		case SSH_ERROR:
			reconnect_ssh_client(client, "Error opening channel: %s",
					     ssh_get_error(session));
			return;
		case SSH_OK:
			tmate_debug("Session opened, initalizing tmate");
			client->state = SSH_BOOTSTRAP;
			/* fall through */
		}

	case SSH_BOOTSTRAP:
		switch (ssh_channel_request_subsystem(channel, "tmate")) {
		case SSH_AGAIN:
			return;
		case SSH_ERROR:
			reconnect_ssh_client(client, "Error initializing tmate: %s",
					     ssh_get_error(session));
			return;
		case SSH_OK:
			tmate_debug("Ready");

			/* Writes are now performed in a blocking fashion */
			ssh_set_blocking(session, 1);

			client->state = SSH_READY;

			tmate_encoder_set_ready_callback(&client->tmate_session->encoder,
							 on_encoder_write, client);
			tmate_decoder_init(&client->tmate_session->decoder,
					   on_decoder_read, client);
			/* fall through */
		}

	case SSH_READY:
		read_channel(client);
		if (!ssh_is_connected(session)) {
			reconnect_ssh_client(client, "Disconnected");
			return;
		}
	}
}

static void __on_ssh_client_event(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	on_ssh_client_event(arg);
}

static void __kill_ssh_client(struct tmate_ssh_client *client,
			      const char *fmt, va_list va)
{
	if (fmt && TAILQ_EMPTY(&client->tmate_session->clients))
		__tmate_status_message(fmt, va);
	else
		tmate_debug("Disconnecting %s", client->server_ip);

	if (event_initialized(&client->ev_ssh)) {
		event_del(&client->ev_ssh);
	}

	if (client->session) {
		/* ssh_free() also frees the associated channels. */
		ssh_free(client->session);
		client->session = NULL;
		client->channel = NULL;
	}

	client->state = SSH_NONE;
}

static void kill_ssh_client(struct tmate_ssh_client *client,
			    const char *fmt, ...)
{
	va_list ap;

	TAILQ_REMOVE(&client->tmate_session->clients, client, node);

	va_start(ap, fmt);
	__kill_ssh_client(client, fmt, ap);
	va_end(ap);

	free(client->server_ip);
	free(client);
}

static void connect_ssh_client(struct tmate_ssh_client *client)
{
	if (!client->session) {
		client->state = SSH_INIT;
		on_ssh_client_event(client);
	}
}

static void on_reconnect_timer(__unused evutil_socket_t fd, __unused short what, void *arg)
{
	connect_ssh_client(arg);
}

static void reconnect_ssh_client(struct tmate_ssh_client *client,
				 const char *fmt, ...)
{
	/* struct timeval tv; */
	va_list ap;

#if 1
	TAILQ_REMOVE(&client->tmate_session->clients, client, node);
#endif

	va_start(ap, fmt);
	__kill_ssh_client(client, fmt, ap);
	va_end(ap);

	/* Not yet implemented... */
#if 0
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	evtimer_add(&client->ev_ssh_reconnect, &tv);
#endif
}

static void ssh_log_function(int priority, const char *function,
			     const char *buffer, __unused void *userdata)
{
	tmate_debug("[%d] [%s] %s", priority, function, buffer);
}

struct tmate_ssh_client *tmate_ssh_client_alloc(struct tmate_session *session,
						const char *server_ip)
{
	struct tmate_ssh_client *client;
	client = xmalloc(sizeof(*client));

	ssh_set_log_callback(ssh_log_function);

	memset(&client->ssh_callbacks, 0, sizeof(client->ssh_callbacks));
	ssh_callbacks_init(&client->ssh_callbacks);
	client->ssh_callbacks.userdata = client;
	client->ssh_callbacks.auth_function = passphrase_callback;

	client->tmate_session = session;
	TAILQ_INSERT_TAIL(&session->clients, client, node);

	client->server_ip = xstrdup(server_ip);
	client->state = SSH_NONE;
	client->session = NULL;
	client->channel = NULL;
	client->has_encoder = 0;

	evtimer_assign(&client->ev_ssh_reconnect, session->ev_base,
		       on_reconnect_timer, client);

	connect_ssh_client(client);

	return client;
}
