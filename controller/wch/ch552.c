/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * Common implementation for the CH552 USB-SPI bridge
 */

#include <malloc.h>
#include <string.h>
#include <ufprog/api_controller.h>
#include <ufprog/config.h>
#include <ufprog/libusb.h>
#include <ufprog/log.h>
#include "ch552.h"

#define CH552_DRV_API_VER_MAJOR			1
#define CH552_DRV_API_VER_MINOR			0

ufprog_status ch552_init(struct ufprog_interface *ch552dev, bool thread_safe);

ufprog_status ch552_write(struct ch552_handle *handle, const void *buf, size_t len, size_t *retlen);
ufprog_status ch552_read(struct ch552_handle *handle, void *buf, size_t len, size_t *retlen);

uint32_t UFPROG_API ufprog_plugin_api_version(void)
{
	return MAKE_VERSION(CH552_DRV_API_VER_MAJOR, CH552_DRV_API_VER_MINOR);
}

uint32_t UFPROG_API ufprog_controller_supported_if(void)
{
	return IFM_SPI;
}

ufprog_status UFPROG_API ufprog_device_lock(struct ufprog_interface *ch552dev)
{
	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!ch552dev->lock)
		return UFP_OK;

	return os_mutex_lock(ch552dev->lock) ? UFP_OK : UFP_LOCK_FAIL;
}

ufprog_status UFPROG_API ufprog_device_unlock(struct ufprog_interface *ch552dev)
{
	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!ch552dev->lock)
		return UFP_OK;

	return os_mutex_unlock(ch552dev->lock) ? UFP_OK : UFP_LOCK_FAIL;
}

ufprog_status ch552_init(struct ufprog_interface *ch552dev, bool thread_safe)
{
	if (thread_safe) {
		if (!os_create_mutex(&ch552dev->lock)) {
			logm_err("Failed to create lock for thread-safe\n");
			return UFP_LOCK_FAIL;
		}
	}

	ch552dev->spi_fsys_hz = CH552_DEFAULT_FSYS_HZ;

	return UFP_OK;
}

ufprog_status ch552_write(struct ch552_handle *handle, const void *buf, size_t len, size_t *retlen)
{
	int ret, transferred;

	ret = libusb_bulk_transfer(handle->handle, CH552_USB_BULK_EP_OUT, (uint8_t *)buf, (int)len, &transferred,
				   CH552_RW_TIMEOUT_MS);
	if (ret) {
		logm_warn("Bulk OUT transfer failed: %s, %u of %zu bytes written\n", libusb_strerror(ret),
			  transferred, len);
		return UFP_DEVICE_IO_ERROR;
	}

	if (retlen)
		*retlen = (size_t)transferred;

	return UFP_OK;
}

ufprog_status ch552_read(struct ch552_handle *handle, void *buf, size_t len, size_t *retlen)
{
	int ret, transferred = 0;

	/* A timeout is not an error here: the device only sends a short packet
	 * when it is done, so a partially filled read is the normal case for
	 * the trailing status byte. */
	ret = libusb_bulk_transfer(handle->handle, CH552_USB_BULK_EP_IN, buf, (int)len, &transferred,
				   CH552_RW_TIMEOUT_MS);
	if (ret && ret != LIBUSB_ERROR_TIMEOUT) {
		logm_warn("Bulk IN transfer failed: %s, %u of %zu bytes read\n", libusb_strerror(ret),
			  transferred, len);
		return UFP_DEVICE_IO_ERROR;
	}

	if (retlen)
		*retlen = (size_t)transferred;

	return UFP_OK;
}
