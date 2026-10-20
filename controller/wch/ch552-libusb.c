/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * Interface driver for the CH552 USB-SPI bridge using libusb
 */

#include <malloc.h>
#include <string.h>
#include <ufprog/api_controller.h>
#include <ufprog/config.h>
#include <ufprog/libusb.h>
#include <ufprog/log.h>
#include "ch552.h"

const char *UFPROG_API ufprog_plugin_desc(void)
{
	return "CH552 USB-SPI bridge (libusb)";
}

static int UFPROG_API ch552_libusb_try_match_open(void *priv, struct json_object *match, int index)
{
	struct libusb_device_handle **dev_handle = priv;
	struct libusb_match_info info;
	ufprog_status ret;

	ret = libusb_read_config(match, &info, true);
	if (ret) {
		if (index >= 0)
			logm_warn("libusb match#%u is invalid\n", index);
		else
			logm_warn("libusb matching data is invalid\n");
		return 0;
	}

	/* Only the index and the port path are honoured.  VID/PID are fixed by
	 * the bridge firmware and the serial/product strings are not stable
	 * enough to match on. */
	info.vid = CH552_USB_VID;
	info.pid = CH552_USB_PID;
	info.bcd_device = 0;
	info.match_bcd_device = false;

	if (info.serial) {
		free((void *)info.serial);
		info.serial = NULL;
	}

	if (info.product) {
		free((void *)info.product);
		info.product = NULL;
	}

	if (info.manufacturer) {
		free((void *)info.manufacturer);
		info.manufacturer = NULL;
	}

	ret = libusb_open_matched(ufprog_global_libusb_context(), &info, dev_handle);
	if (ret) {
		if (index >= 0)
			logm_dbg("Failed to open device specified by match#%u\n", index);
		else
			logm_dbg("Failed to open device specified by matching data\n");
		return 0;
	}

	return 1;
}

ufprog_status UFPROG_API ufprog_device_open(uint32_t if_type, struct json_object *config, ufprog_bool thread_safe,
					    struct ufprog_interface **outifdev)
{
	struct libusb_device_handle *dev_handle = NULL;
	struct ufprog_interface *ch552dev;
	ufprog_status ret = UFP_OK;
	int err;

	if (!outifdev)
		return UFP_INVALID_PARAMETER;

	*outifdev = NULL;

	if (if_type != IF_SPI)
		return UFP_UNSUPPORTED;

	if (!config) {
		logm_err("Device connection configuration required\n");
		return UFP_DEVICE_MISSING_CONFIG;
	}

	STATUS_CHECK_RET(json_array_foreach(config, "match", ch552_libusb_try_match_open, &dev_handle, NULL));

	if (!dev_handle) {
		logm_errdbg("No matched device opened\n");
		return UFP_DEVICE_NOT_FOUND;
	}

	libusb_set_auto_detach_kernel_driver(dev_handle, 1);

	ch552dev = calloc(1, sizeof(*ch552dev) + sizeof(struct ch552_handle));
	if (!ch552dev) {
		logm_err("No memory for device object\n");
		ret = UFP_NOMEM;
		goto cleanup;
	}

	ch552dev->handle = (struct ch552_handle *)((uintptr_t)ch552dev + sizeof(*ch552dev));
	ch552dev->handle->handle = dev_handle;

	STATUS_CHECK_GOTO_RET(ch552_init(ch552dev, thread_safe), ret, cleanup);

	err = libusb_claim_interface(dev_handle, CH552_USB_INTERFACE);
	if (err < 0) {
		logm_err("Unable to claim interface %u: %s\n", CH552_USB_INTERFACE, libusb_strerror(err));
		ret = UFP_DEVICE_IO_ERROR;
		goto cleanup;
	}

	*outifdev = ch552dev;
	return UFP_OK;

cleanup:
	libusb_close(dev_handle);

	if (ch552dev) {
		if (ch552dev->lock)
			os_free_mutex(ch552dev->lock);

		free(ch552dev);
	}

	return ret;
}

ufprog_status UFPROG_API ufprog_device_free(struct ufprog_interface *ch552dev)
{
	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (ch552dev->handle->handle) {
		libusb_release_interface(ch552dev->handle->handle, CH552_USB_INTERFACE);
		libusb_close(ch552dev->handle->handle);
	}

	if (ch552dev->lock)
		os_free_mutex(ch552dev->lock);

	free(ch552dev);

	return UFP_OK;
}
