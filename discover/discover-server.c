
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <asm/byteorder.h>

#include <pb-config/pb-config.h>
#include <talloc/talloc.h>
#include <waiter/waiter.h>
#include <log/log.h>

#include "pb-protocol/pb-protocol.h"
#include "list/list.h"

#include "device-handler.h"
#include "discover-server.h"
#include "platform.h"
#include "sysinfo.h"

struct discover_server {
	int socket;
	struct waitset *waitset;
	struct waiter *waiter;
	struct list clients;
	struct device_handler *device_handler;
};

struct client {
	struct discover_server *server;
	struct list_item list;
	struct waiter *waiter;
	int fd;
	bool remote_closed;
};


static int server_destructor(void *arg)
{
	struct discover_server *server = arg;

	if (server->waiter)
		waiter_remove(server->waiter);

	if (server->socket >= 0)
		close(server->socket);

	return 0;
}

static int client_destructor(void *arg)
{
	struct client *client = arg;

	if (client->fd >= 0)
		close(client->fd);

	if (client->waiter)
		waiter_remove(client->waiter);

	list_remove(&client->list);

	return 0;

}

static void print_clients(struct discover_server *server)
	__attribute__((unused));

static void print_clients(struct discover_server *server)
{
	struct client *client;

	pb_debug("current clients [%p,%p,%p]:\n",
			&server->clients.head,
			server->clients.head.prev,
			server->clients.head.next);
	list_for_each_entry(&server->clients, client, list)
		pb_debug("\t[%p,%p,%p] client: %d\n", &client->list,
				client->list.prev, client->list.next,
				client->fd);
}

static int client_write_message(
		struct discover_server *server __attribute__((unused)),
		struct client *client, struct pb_protocol_message *message)
{
	int rc;

	if (client->remote_closed)
		return -1;

	rc = pb_protocol_write_message(client->fd, message);
	if (rc)
		client->remote_closed = true;

	return rc;
}

static int write_device_add_message(struct discover_server *server,
		struct client *client, const struct device *dev)
{
	struct pb_protocol_message *message;
	int len;

	len = pb_protocol_device_len(dev);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_DEVICE_ADD, len);
	if (!message)
		return -1;

	pb_protocol_serialise_device(dev, message->payload, len);

	return client_write_message(server, client, message);
}

static int write_boot_option_add_message(struct discover_server *server,
		struct client *client, const struct boot_option *opt)
{
	struct pb_protocol_message *message;
	int len;

	len = pb_protocol_boot_option_len(opt);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_BOOT_OPTION_ADD, len);
	if (!message)
		return -1;

	pb_protocol_serialise_boot_option(opt, message->payload, len);

	return client_write_message(server, client, message);
}

static int write_device_remove_message(struct discover_server *server,
		struct client *client, char *dev_id)
{
	struct pb_protocol_message *message;
	int len;

	len = strlen(dev_id) + sizeof(uint32_t);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_DEVICE_REMOVE, len);
	if (!message)
		return -1;

	pb_protocol_serialise_string(message->payload, dev_id);

	return client_write_message(server, client, message);
}

static int write_boot_status_message(struct discover_server *server,
		struct client *client, const struct status *status)
{
	struct pb_protocol_message *message;
	int len;

	len = pb_protocol_boot_status_len(status);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_STATUS, len);
	if (!message)
		return -1;

	pb_protocol_serialise_boot_status(status, message->payload, len);

	return client_write_message(server, client, message);
}

static int write_system_info_message(struct discover_server *server,
		struct client *client, const struct system_info *sysinfo)
{
	struct pb_protocol_message *message;
	int len;

	len = pb_protocol_system_info_len(sysinfo);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_SYSTEM_INFO, len);
	if (!message)
		return -1;

	pb_protocol_serialise_system_info(sysinfo, message->payload, len);

	return client_write_message(server, client, message);
}

static int write_config_message(struct discover_server *server,
		struct client *client, const struct config *config)
{
	struct pb_protocol_message *message;
	int len;

	len = pb_protocol_config_len(config);

	message = pb_protocol_create_message(client,
			PB_PROTOCOL_ACTION_CONFIG, len);
	if (!message)
		return -1;

	pb_protocol_serialise_config(config, message->payload, len);

	return client_write_message(server, client, message);
}

static int discover_server_process_message(void *arg)
{
	struct pb_protocol_message *message;
	struct boot_command *boot_command;
	struct client *client = arg;
	struct config *config;
	char *url;
	int rc;

	message = pb_protocol_read_message(client, client->fd);

	if (!message) {
		talloc_free(client);
		return 0;
	}


	switch (message->action) {
	case PB_PROTOCOL_ACTION_BOOT:
		boot_command = talloc(client, struct boot_command);

		rc = pb_protocol_deserialise_boot_command(boot_command,
				message);
		if (rc) {
			pb_log("%s: no boot command?", __func__);
			return 0;
		}

		device_handler_boot(client->server->device_handler,
				boot_command);
		break;

	case PB_PROTOCOL_ACTION_CANCEL_DEFAULT:
		device_handler_cancel_default(client->server->device_handler);
		break;

	case PB_PROTOCOL_ACTION_REINIT:
		device_handler_reinit(client->server->device_handler);
		break;

	case PB_PROTOCOL_ACTION_CONFIG:
		config = talloc_zero(client, struct config);

		rc = pb_protocol_deserialise_config(config, message);
		if (rc) {
			pb_log("%s: no config?", __func__);
			return 0;
		}

		device_handler_update_config(client->server->device_handler,
				config);
		break;

	case PB_PROTOCOL_ACTION_ADD_URL:
		url = pb_protocol_deserialise_string((void *) client, message);

		device_handler_process_url(client->server->device_handler,
				url, NULL, NULL);
		break;

	default:
		pb_log("%s: invalid action %d\n", __func__, message->action);
		return 0;
	}


	return 0;
}

static int discover_server_process_connection(void *arg)
{
	struct discover_server *server = arg;
	int fd, rc, i, n_devices;
	struct client *client;

	/* accept the incoming connection */
	fd = accept(server->socket, NULL, NULL);
	if (fd < 0) {
		pb_log("accept: %s\n", strerror(errno));
		return 0;
	}

	/* add to our list of clients */
	client = talloc_zero(server, struct client);
	list_add(&server->clients, &client->list);

	talloc_set_destructor(client, client_destructor);

	client->fd = fd;
	client->server = server;
	client->waiter = waiter_register_io(server->waitset, client->fd,
				WAIT_IN, discover_server_process_message,
				client);

	/* send sysinfo to client */
	rc = write_system_info_message(server, client, system_info_get());
	if (rc)
		return 0;

	/* send config to client */
	rc = write_config_message(server, client, config_get());
	if (rc)
		return 0;

	/* send existing devices to client */
	n_devices = device_handler_get_device_count(server->device_handler);
	for (i = 0; i < n_devices; i++) {
		const struct discover_boot_option *opt;
		const struct discover_device *device;

		device = device_handler_get_device(server->device_handler, i);
		rc = write_device_add_message(server, client, device->device);
		if (rc)
			return 0;

		list_for_each_entry(&device->boot_options, opt, list) {
			rc = write_boot_option_add_message(server, client,
					opt->option);
			if (rc)
				return 0;
		}
	}

	return 0;
}

void discover_server_notify_device_add(struct discover_server *server,
		struct device *device)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_device_add_message(server, client, device);

}

void discover_server_notify_boot_option_add(struct discover_server *server,
		struct boot_option *boot_option)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_boot_option_add_message(server, client, boot_option);
}

void discover_server_notify_device_remove(struct discover_server *server,
		struct device *device)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_device_remove_message(server, client, device->id);

}

void discover_server_notify_boot_status(struct discover_server *server,
		struct status *status)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_boot_status_message(server, client, status);
}

void discover_server_notify_system_info(struct discover_server *server,
		const struct system_info *sysinfo)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_system_info_message(server, client, sysinfo);
}

void discover_server_notify_config(struct discover_server *server,
		const struct config *config)
{
	struct client *client;

	list_for_each_entry(&server->clients, client, list)
		write_config_message(server, client, config);
}

void discover_server_set_device_source(struct discover_server *server,
		struct device_handler *handler)
{
	server->device_handler = handler;
}

struct discover_server *discover_server_init(struct waitset *waitset)
{
	struct discover_server *server;
	struct sockaddr_un addr;

	server = talloc(NULL, struct discover_server);
	if (!server)
		return NULL;

	server->waiter = NULL;
	server->waitset = waitset;
	list_init(&server->clients);

	unlink(PB_SOCKET_PATH);

	server->socket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server->socket < 0) {
		pb_log("error creating server socket: %s\n", strerror(errno));
		goto out_err;
	}

	talloc_set_destructor(server, server_destructor);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, PB_SOCKET_PATH);

	if (bind(server->socket, (struct sockaddr *)&addr, sizeof(addr))) {
		pb_log("error binding server socket: %s\n", strerror(errno));
		goto out_err;
	}

	if (listen(server->socket, 8)) {
		pb_log("server socket listen: %s\n", strerror(errno));
		goto out_err;
	}

	server->waiter = waiter_register_io(server->waitset, server->socket,
			WAIT_IN, discover_server_process_connection, server);

	return server;

out_err:
	talloc_free(server);
	return NULL;
}

void discover_server_destroy(struct discover_server *server)
{
	talloc_free(server);
}

