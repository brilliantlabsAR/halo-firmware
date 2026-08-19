/* Copyright (c) 2025 Brilliant Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/slist.h>
#include <string.h>

#include "halo/ble_service.h"

LOG_MODULE_REGISTER(halo_ble_svc, CONFIG_HALO_LOG_LEVEL);

static struct {
	bool initialized;
	sys_slist_t services;
	struct k_mutex lock;
} svc_ctx;

int halo_ble_service_init(void)
{
	if (svc_ctx.initialized) {
		return 0;
	}

	k_mutex_init(&svc_ctx.lock);
	sys_slist_init(&svc_ctx.services);
	svc_ctx.initialized = true;

	return 0;
}

int halo_ble_service_register(struct halo_ble_service *svc)
{
	if (!svc || !svc->name) {
		return -EINVAL;
	}

	if (!svc_ctx.initialized) {
		int ret = halo_ble_service_init();
		if (ret < 0) {
			return ret;
		}
	}

	k_mutex_lock(&svc_ctx.lock, K_FOREVER);

	sys_snode_t *node;
	struct halo_ble_service *item;
	SYS_SLIST_FOR_EACH_NODE(&svc_ctx.services, node) {
		item = CONTAINER_OF(node, struct halo_ble_service, node);
		if (item == svc) {
			k_mutex_unlock(&svc_ctx.lock);
			LOG_WRN("Service %s already registered", svc->name);
			return -EALREADY;
		}
	}

	sys_slist_append(&svc_ctx.services, &svc->node);
	k_mutex_unlock(&svc_ctx.lock);

	if (svc->init) {
		int ret = svc->init();
		if (ret < 0) {
			LOG_ERR("Service %s init failed: %d", svc->name, ret);
			k_mutex_lock(&svc_ctx.lock, K_FOREVER);
			sys_slist_find_and_remove(&svc_ctx.services, &svc->node);
			k_mutex_unlock(&svc_ctx.lock);
			return ret;
		}
	}

	svc->registered = true;
	return 0;
}

int halo_ble_service_unregister(struct halo_ble_service *svc)
{
	if (!svc) {
		return -EINVAL;
	}

	if (!svc_ctx.initialized) {
		return -ENODEV;
	}

	if (svc->deinit) {
		int ret = svc->deinit();
		if (ret < 0) {
			LOG_WRN("Service %s deinit failed: %d", svc->name, ret);
		}
	}

	k_mutex_lock(&svc_ctx.lock, K_FOREVER);
	bool found = sys_slist_find_and_remove(&svc_ctx.services, &svc->node);
	k_mutex_unlock(&svc_ctx.lock);

	if (!found) {
		LOG_WRN("Service %s not found", svc->name);
		return -ENOENT;
	}

	svc->registered = false;
	return 0;
}

struct halo_ble_service *halo_ble_service_find_by_name(const char *name)
{
	if (!name || !svc_ctx.initialized) {
		return NULL;
	}

	struct halo_ble_service *found = NULL;
	k_mutex_lock(&svc_ctx.lock, K_FOREVER);

	sys_snode_t *node;
	struct halo_ble_service *svc;
	SYS_SLIST_FOR_EACH_NODE(&svc_ctx.services, node) {
		svc = CONTAINER_OF(node, struct halo_ble_service, node);
		if (strcmp(svc->name, name) == 0) {
			found = svc;
			break;
		}
	}

	k_mutex_unlock(&svc_ctx.lock);
	return found;
}

bool halo_ble_service_is_registered(const struct halo_ble_service *service)
{
	return service ? service->registered : false;
}

void halo_ble_service_on_connected(uint8_t conidx)
{
	if (!svc_ctx.initialized) {
		return;
	}

	k_mutex_lock(&svc_ctx.lock, K_FOREVER);

	sys_snode_t *node;
	struct halo_ble_service *svc;
	SYS_SLIST_FOR_EACH_NODE(&svc_ctx.services, node) {
		svc = CONTAINER_OF(node, struct halo_ble_service, node);
		if (svc->on_connected) {
			int ret = svc->on_connected(conidx);
			if (ret < 0) {
				LOG_WRN("Service %s on_connected failed: %d", svc->name, ret);
			}
		}
	}

	k_mutex_unlock(&svc_ctx.lock);
}

void halo_ble_service_on_disconnected(uint8_t conidx)
{
	if (!svc_ctx.initialized) {
		return;
	}

	k_mutex_lock(&svc_ctx.lock, K_FOREVER);

	sys_snode_t *node;
	struct halo_ble_service *svc;
	SYS_SLIST_FOR_EACH_NODE(&svc_ctx.services, node) {
		svc = CONTAINER_OF(node, struct halo_ble_service, node);
		if (svc->on_disconnected) {
			int ret = svc->on_disconnected(conidx);
			if (ret < 0) {
				LOG_WRN("Service %s on_disconnected failed: %d", svc->name, ret);
			}
		}
	}

	k_mutex_unlock(&svc_ctx.lock);
}

void halo_ble_service_on_paired(uint8_t conidx)
{
	if (!svc_ctx.initialized) {
		return;
	}

	k_mutex_lock(&svc_ctx.lock, K_FOREVER);

	sys_snode_t *node;
	struct halo_ble_service *svc;
	SYS_SLIST_FOR_EACH_NODE(&svc_ctx.services, node) {
		svc = CONTAINER_OF(node, struct halo_ble_service, node);
		if (svc->on_paired) {
			int ret = svc->on_paired(conidx);
			if (ret < 0) {
				LOG_WRN("Service %s on_paired failed: %d", svc->name, ret);
			}
		}
	}

	k_mutex_unlock(&svc_ctx.lock);
}
