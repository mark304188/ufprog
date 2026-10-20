/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * CH552 USB-SPI bridge definitions
 */
#pragma once

#ifndef _UFPROG_CH552_H_
#define _UFPROG_CH552_H_

#include <stdint.h>
#include <stdbool.h>
#include <ufprog/osdef.h>
#include <ufprog/config.h>
#include <ufprog/libusb.h>

/* Device identity.  The bridge enumerates as a vendor specific interface, so
 * no system driver claims it and libusb can talk to it directly. */
#define CH552_USB_VID					0x1A86
#define CH552_USB_PID					0x5507

#define CH552_USB_INTERFACE				0
#define CH552_USB_BULK_EP_OUT				0x03
#define CH552_USB_BULK_EP_IN				0x82
#define CH552_USB_PACKET_SIZE				64

/* Endpoint 0 vendor request used by the firmware to report its own state.
 * It keeps working when the bulk endpoints are dead, so it is the only way to
 * tell an old firmware from broken wiring. */
#define CH552_VENDOR_REQ_STATUS				0x42
#define CH552_VENDOR_STATUS_LEN				32

#define CH552_STATUS_MAGIC_0				'C'
#define CH552_STATUS_MAGIC_1				'S'
#define CH552_STATUS_MAGIC_2				'P'

#define CH552_STATUS_OFF_PROTO_VER			3
#define CH552_STATUS_OFF_FW_MAJOR			4
#define CH552_STATUS_OFF_FW_MINOR			5

/* Wire protocol version this driver understands. */
#define CH552_PROTO_VERSION				1

/*---------------------------------------------------------------*
 * Commands                                                      *
 *---------------------------------------------------------------*/

#define CH552_CMD_PING					0x01
#define CH552_CMD_GET_INFO				0x02
#define CH552_CMD_GET_CAPS				0x03
#define CH552_CMD_SET_SPI_CFG				0x04
#define CH552_CMD_GET_SPI_CFG				0x05
#define CH552_CMD_CS					0x08
#define CH552_CMD_SPI_WRITE				0x10
#define CH552_CMD_SPI_XFER				0x11
#define CH552_CMD_SPI_READ				0x12
#define CH552_CMD_STREAM_WRITE				0x20
#define CH552_CMD_STREAM_READ				0x21
#define CH552_CMD_STREAM_XFER				0x22
#define CH552_CMD_NOR_PROGRAM				0x23
#define CH552_CMD_NOR_ERASE				0x24
#define CH552_CMD_NAND_SET_CFG				0x25
#define CH552_CMD_NAND_READ_PAGES			0x26
#define CH552_CMD_NAND_PROGRAM_PAGES			0x27
#define CH552_CMD_GPIO_WRITE				0x30
#define CH552_CMD_GPIO_READ				0x31
#define CH552_CMD_BOOTLOADER				0x40

/*---------------------------------------------------------------*
 * Status codes (first byte of every reply)                      *
 *---------------------------------------------------------------*/

#define CH552_ST_OK					0x00
#define CH552_ST_ERR					0x01
#define CH552_ST_BUSY					0x02
#define CH552_ST_PARAM					0x03
#define CH552_ST_LEN					0x04
#define CH552_ST_TIMEOUT				0x05
#define CH552_ST_UNKNOWN				0xFF

/*---------------------------------------------------------------*
 * Capability bits (little endian 16-bit field at reply offset 1) *
 *---------------------------------------------------------------*/

#define CH552_CAP_STREAM_MODE				0x0001
#define CH552_CAP_CS_CONTROL				0x0002
#define CH552_CAP_DUAL_BUFFER				0x0004
#define CH552_CAP_XFER					0x0008
#define CH552_CAP_SPI_CFG				0x0010
#define CH552_CAP_GPIO					0x0020
#define CH552_CAP_NOR_ACCEL				0x0040
#define CH552_CAP_NAND_ACCEL				0x0080
#define CH552_CAP_ERASE_ACCEL				0x0100

/* Accelerator commands we are able to use.  Anything missing here falls back
 * to the generic streaming path. */
#define CH552_ACCEL_NONE				0
#define CH552_ACCEL_NOR					0x01
#define CH552_ACCEL_NAND				0x02

/* SPI prefix of a stream command may carry at most this many bytes. */
#define CH552_MAX_PREFIX				8

/* The firmware is built for a fixed system clock.  The SPI divider is
 * relative to it, so the absolute SCK frequency follows from this. */
#define CH552_DEFAULT_FSYS_HZ				24000000

/* GPIO slots exposed by CMD_GPIO_*. */
#define CH552_GPIO_WP					0
#define CH552_GPIO_HOLD					1

#define CH552_RW_TIMEOUT_MS				5000

/* Data chunk submitted to libusb in one go.  The device consumes it as a
 * series of 64 byte frames, but a single large URB lets the host controller
 * retry inside the same USB frame, which is much faster than one call per
 * frame. */
#define CH552_DATA_CHUNK_SIZE				0x8000

/* Largest byte count accepted by a streaming command (24 bit field). */
#define CH552_MAX_STREAM_LEN				0x10000

/* Timeout used for commands that run a whole flash operation inside the
 * device: a 4 KiB NOR sector erase or a NAND block erase can take a few
 * hundred milliseconds on its own. */
#define CH552_ACCEL_TIMEOUT_MS			60000

/* Length of the [status][done24] reply shared by the accelerator commands. */
#define CH552_ACCEL_REPLY_LEN			4

struct ch552_handle {
	struct libusb_device_handle *handle;
};

ufprog_status ch552_init(struct ufprog_interface *ch552dev, bool thread_safe);
ufprog_status ch552_spi_init(struct ufprog_interface *ch552dev, struct json_object *config);

ufprog_status ch552_write(struct ch552_handle *handle, const void *buf, size_t len, size_t *retlen);
ufprog_status ch552_read(struct ch552_handle *handle, void *buf, size_t len, size_t *retlen);

ufprog_status ch552_send_cmd(struct ufprog_interface *ch552dev, uint8_t cmd, const void *payload,
			     size_t payload_len);
ufprog_status ch552_recv_reply(struct ufprog_interface *ch552dev, void *data, size_t len);
ufprog_status ch552_read_status(struct ufprog_interface *ch552dev, uint8_t *retstatus);

struct ufprog_interface {
	struct ch552_handle *handle;

	uint32_t spi_fsys_hz;
	uint8_t spi_div;
	uint8_t spi_mode3;

	uint8_t caps_lo, caps_hi;
	uint8_t accel_mask;
	uint8_t fw_major, fw_minor;
	uint8_t proto_version;

	/* NAND geometry programmed with CMD_NAND_SET_CFG. */
	uint32_t nand_page_size;
	uint32_t nand_oob_size;
	bool nand_geom_set;

	bool cs_asserted;

	mutex_handle lock;
};

/*---------------------------------------------------------------*
 * Accelerator API (ch552-accel.c)                              *
 *                                                               *
 * These run the per-page flash loop inside the device.  Each of *
 * them returns UFP_UNSUPPORTED when the firmware does not      *
 * advertise the corresponding capability, so the caller can     *
 * fall back to the generic SPI-MEM path.                        *
 *---------------------------------------------------------------*/

ufprog_bool UFPROG_API ch552_has_nor_accel(struct ufprog_interface *ch552dev);
ufprog_bool UFPROG_API ch552_has_nand_accel(struct ufprog_interface *ch552dev);
ufprog_status UFPROG_API ch552_nor_program(struct ufprog_interface *ch552dev, uint64_t addr, size_t len,
					   const void *data);
ufprog_status UFPROG_API ch552_nor_erase(struct ufprog_interface *ch552dev, uint64_t addr, size_t len);

ufprog_status UFPROG_API ch552_nand_set_geometry(struct ufprog_interface *ch552dev, uint32_t page_size,
						 uint32_t oob_size);
ufprog_bool UFPROG_API ch552_nand_geometry_matches(struct ufprog_interface *ch552dev, uint32_t page_size,
						   uint32_t oob_size);
ufprog_status UFPROG_API ch552_nand_read_pages(struct ufprog_interface *ch552dev, uint32_t page, uint32_t count,
					       void *buf);
ufprog_status UFPROG_API ch552_nand_program_pages(struct ufprog_interface *ch552dev, uint32_t page,
						  uint32_t count, const void *data);

#endif /* _UFPROG_CH552_H_ */
