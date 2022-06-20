// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright 2022 NXP
 */

#include <arm.h>
#include <atomic.h>
#include <hse_abi.h>
#include <hse_core.h>
#include <hse_mu.h>
#include <hse_util.h>
#include <initcall.h>
#include <kernel/interrupt.h>
#include <kernel/spinlock.h>
#include <malloc.h>
#include <string.h>
#include <tee_api_types.h>
#include <tee/cache.h>
#include <trace.h>

/**
 * struct hse_drvdata - HSE driver private data
 * @srv_desc[n].ptr: service descriptor virtual address for channel n
 * @srv_desc[n].dma: service descriptor DMA address for channel n
 * @srv_desc[n].id: current service request ID for channel n
 * @mu: MU instance handle returned by lower abstraction layer
 * @type[n]: designated type of service channel n
 * @aes_key_ring: AES key slots currently available
 * @aes_key_ring_size: size of AES key ring
 * @tx_lock: lock used for service request transmission
 * @key_ring_lock: lock used for key slot acquisition
 * @firmware_version: firmware version
 */
struct hse_drvdata {
	struct {
		void  *ptr;
		paddr_t dma;
		uint32_t id;
	} srv_desc[HSE_NUM_CHANNELS];
	void *mu;
	bool channel_busy[HSE_NUM_CHANNELS];
	enum hse_ch_type type[HSE_NUM_CHANNELS];
	struct hse_key *aes_key_ring;
	size_t aes_key_ring_size;
	unsigned int tx_lock;
	unsigned int key_ring_lock;
	struct hse_attr_fw_version firmware_version;
};

static struct hse_drvdata *drv;

/**
 * hse_err_decode - HSE error code translation
 * @srv_rsp: HSE service response
 *
 * Return: 0 on service request success, error code otherwise
 */
static TEE_Result hse_err_decode(uint32_t srv_rsp)
{
	switch (srv_rsp) {
	case HSE_SRV_RSP_OK:
		return TEE_SUCCESS;
	case HSE_SRV_RSP_VERIFY_FAILED:
		return TEE_ERROR_COMMUNICATION;
	case HSE_SRV_RSP_INVALID_ADDR:
	case HSE_SRV_RSP_INVALID_PARAM:
		return TEE_ERROR_BAD_PARAMETERS;
	case HSE_SRV_RSP_NOT_SUPPORTED:
		return TEE_ERROR_NOT_SUPPORTED;
	case HSE_SRV_RSP_NOT_ALLOWED:
		return TEE_ERROR_ACCESS_DENIED;
	case HSE_SRV_RSP_NOT_ENOUGH_SPACE:
		return TEE_ERROR_OUT_OF_MEMORY;
	case HSE_SRV_RSP_CANCELED:
		return TEE_ERROR_CANCEL;
	case HSE_SRV_RSP_KEY_NOT_AVAILABLE:
	case HSE_SRV_RSP_KEY_EMPTY:
	case HSE_SRV_RSP_KEY_INVALID:
	case HSE_SRV_RSP_KEY_WRITE_PROTECTED:
	case HSE_SRV_RSP_KEY_UPDATE_ERROR:
		return TEE_ERROR_BAD_STATE;
	default:
		return TEE_ERROR_GENERIC;
	}
}

/**
 * hse_sync_srv_desc - sync service descriptor
 * @channel: service channel
 * @desc: service descriptor address
 *
 * Copy descriptor to the dedicated space and cache service ID internally.
 */
static inline void hse_sync_srv_desc(uint8_t channel,
				     const struct hse_srv_desc *srv_desc)
{
	if (channel >= HSE_NUM_CHANNELS || !srv_desc)
		return;

	memcpy(drv->srv_desc[channel].ptr, srv_desc, sizeof(*srv_desc));
	drv->srv_desc[channel].id = srv_desc->srv_id;
}

/**
 * hse_next_free_channel - find the next available shared channel
 * @type: channel type
 *
 * Return: channel index, HSE_CHANNEL_INV if none available
 */
static uint8_t hse_next_free_channel(void)
{
	uint8_t channel;

	for (channel = ARRAY_SIZE(drv->type) - 1; channel > 0; channel--)
		if (drv->type[channel] == HSE_CH_TYPE_SHARED &&
		    !drv->channel_busy[channel])
			return channel;

	return HSE_CHANNEL_INV;
}

TEE_Result hse_srv_req_sync(uint8_t channel, const void *srv_desc)
{
	TEE_Result ret;
	uint32_t srv_rsp, exceptions;
	void *mu = drv->mu;

	if (!srv_desc)
		return TEE_ERROR_BAD_PARAMETERS;

	if (channel != HSE_CHANNEL_ANY && channel >= HSE_NUM_CHANNELS)
		return TEE_ERROR_BAD_PARAMETERS;

	exceptions = cpu_spin_lock_xsave(&drv->tx_lock);

	if (channel == HSE_CHANNEL_ANY) {
		channel = hse_next_free_channel();
		if (channel == HSE_CHANNEL_INV) {
			cpu_spin_unlock_xrestore(&drv->tx_lock, exceptions);
			DMSG("No channel available\n");
			return TEE_ERROR_BUSY;
		}
	} else if (drv->channel_busy[channel]) {
		cpu_spin_unlock_xrestore(&drv->tx_lock, exceptions);
		DMSG("channel %d busy\n", channel);
		return TEE_ERROR_BUSY;
	}

	drv->channel_busy[channel] = true;

	cpu_spin_unlock_xrestore(&drv->tx_lock, exceptions);

	hse_sync_srv_desc(channel, srv_desc);

	ret = hse_mu_msg_send(mu, channel, drv->srv_desc[channel].dma);
	if (ret)
		return ret;

	while (!hse_mu_msg_pending(mu, channel))
		;

	ret = hse_mu_msg_recv(mu, channel, &srv_rsp);
	if (ret)
		return ret;

	drv->channel_busy[channel] = false;

	ret = hse_err_decode(srv_rsp);
	return ret;
}

/**
 * hse_key_ring_init - initialize all keys in a specific key group
 * @key_ring: output key ring
 * @group_id: key group ID
 * @group_size: key group size
 *
 * Return: allocated key ring array, NULL for failed key ring allocation
 */
static struct hse_key *hse_key_ring_init(enum hse_key_type type,
					 uint8_t group_id, uint8_t group_size)
{
	struct hse_key *ring;
	unsigned int i;

	if (!group_size)
		return NULL;

	ring = malloc(group_size * sizeof(*ring));
	if (!ring)
		return NULL;

	for (i = 0; i < group_size; i++) {
		ring[i].handle = HSE_KEY_HANDLE(group_id, i);
		ring[i].type = type;
		ring[i].acquired = false;
	}

	DMSG("key ring: group id %d, size %d\n", group_id, group_size);

	return ring;
}

/**
 * hse_key_ring_free - remove all keys in a specific key group
 * @key_ring: input key ring
 */
static inline void hse_key_ring_free(struct hse_key *key_ring)
{
	if (!key_ring)
		return;

	free(key_ring);
}

/**
 * hse_key_slot_acquire - acquire a HSE key slot
 * @type: key type
 *
 * Return: key slot of specified type if available, NULL otherwise
 */
struct hse_key *hse_key_slot_acquire(enum hse_key_type type)
{
	struct hse_key *key_ring, *slot = NULL;
	size_t key_ring_size, i;
	uint32_t exceptions;

	switch (type) {
	case HSE_KEY_TYPE_AES:
		key_ring = drv->aes_key_ring;
		key_ring_size = drv->aes_key_ring_size;
		break;
	default:
		return NULL;
	}

	/* remove key slot from ring */
	exceptions = cpu_spin_lock_xsave(&drv->key_ring_lock);
	for (i = 0; i < key_ring_size;  i++) {
		if (!key_ring[i].acquired) {
			key_ring[i].acquired = true;
			slot = &key_ring[i];
			break;
		}
	}
	cpu_spin_unlock_xrestore(&drv->key_ring_lock, exceptions);

	return slot;
}

/**
 * hse_key_slot_release - release a HSE key slot
 * @slot: key slot
 */
void hse_key_slot_release(struct hse_key *slot)
{
	struct hse_key *key_ring;
	size_t key_ring_size, i;
	uint32_t exceptions;

	switch (slot->type) {
	case HSE_KEY_TYPE_AES:
		key_ring = drv->aes_key_ring;
		key_ring_size = drv->aes_key_ring_size;
		break;
	default:
		return;
	}

	/* add key slot back to ring */
	exceptions = cpu_spin_lock_xsave(&drv->key_ring_lock);
	for (i = 0; i < key_ring_size; i++) {
		if (slot == &key_ring[i]) {
			slot->acquired = false;
			break;
		}
	}
	cpu_spin_unlock_xrestore(&drv->key_ring_lock, exceptions);
}

/**
 * hse_config_channels - configure channels and manage descriptor space
 *
 * HSE firmware restricts channel zero to administrative services, all the rest
 * are usable for crypto operations. Driver reserves the last HSE_STREAM_COUNT
 * channels for streaming mode use and marks the remaining as shared channels.
 */
static inline void hse_config_channels(void)
{
	unsigned int offset;
	uint8_t channel;
	vaddr_t ch_addr;

	drv->type[0] = HSE_CH_TYPE_ADMIN;
	drv->srv_desc[0].ptr = hse_mu_desc_base_ptr(drv->mu);
	drv->srv_desc[0].dma = hse_mu_desc_base_dma(drv->mu);

	for (channel = 1; channel < HSE_NUM_CHANNELS; channel++) {
		if (channel >= HSE_NUM_CHANNELS - HSE_STREAM_COUNT)
			drv->type[channel] = HSE_CH_TYPE_STREAM;
		else
			drv->type[channel] = HSE_CH_TYPE_SHARED;

		offset = channel * HSE_SRV_DESC_MAX_SIZE;
		ch_addr = (vaddr_t)drv->srv_desc[0].ptr + offset;

		drv->srv_desc[channel].ptr = (void *)ch_addr;
		drv->srv_desc[channel].dma = drv->srv_desc[0].dma + offset;
	}
}

/**
 * hse_check_fw_version - retrieve firmware version
 *
 * Issues a service request for retrieving the HSE Firmware version
 */
static TEE_Result hse_check_fw_version(void)
{
	TEE_Result err;
	struct hse_srv_desc srv_desc;
	struct hse_buf buf;

	err = hse_buf_alloc(&buf, sizeof(struct hse_attr_fw_version));

	srv_desc.srv_id = HSE_SRV_ID_GET_ATTR;
	srv_desc.get_attr_req.attr_id = HSE_FW_VERSION_ATTR_ID;
	srv_desc.get_attr_req.attr_len = buf.size;
	srv_desc.get_attr_req.attr = buf.paddr;

	err = hse_srv_req_sync(HSE_CHANNEL_ADM, &srv_desc);
	if (err) {
		DMSG("request failed: %d", err);
		hse_buf_free(&buf);
		return err;
	}

	cache_operation(TEE_CACHEINVALIDATE, buf.data, buf.size);

	memcpy(&drv->firmware_version, buf.data, buf.size);

	hse_buf_free(&buf);

	return TEE_SUCCESS;
}

static TEE_Result crypto_driver_init(void)
{
	TEE_Result ret = TEE_ERROR_GENERIC;
	uint16_t status;

	drv = malloc(sizeof(*drv));
	if (!drv) {
		EMSG("Could not malloc drv instance");
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	drv->mu = hse_mu_init();
	if (!drv->mu) {
		EMSG("Could not get MU Instance");
		return TEE_ERROR_GENERIC;
	}

	status = hse_mu_check_status(drv->mu);
	if (!(status & HSE_STATUS_INIT_OK)) {
		EMSG("Firmware not found");
		return TEE_ERROR_BAD_STATE;
	}

	hse_config_channels();

	drv->tx_lock = SPINLOCK_UNLOCK;
	drv->key_ring_lock = SPINLOCK_UNLOCK;

	ret = hse_check_fw_version();
	if (ret)
		return ret;
	DMSG("%s firmware, version %d.%d.%d\n",
	     drv->firmware_version.fw_type == 0 ? "standard" :
	     (drv->firmware_version.fw_type == 1 ? "premium" : "custom"),
	     drv->firmware_version.major, drv->firmware_version.minor,
	     drv->firmware_version.patch);

	drv->aes_key_ring = hse_key_ring_init(HSE_KEY_TYPE_AES,
					      CFG_HSE_AES_KEY_GROUP_ID,
					      CFG_HSE_AES_KEY_GROUP_SIZE);
	drv->aes_key_ring_size = CFG_HSE_AES_KEY_GROUP_SIZE;

	IMSG("HSE is successfully initialized");

	return TEE_SUCCESS;
}

early_init(crypto_driver_init);