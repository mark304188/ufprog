/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * SPI master interface driver for the CH552 USB-SPI bridge
 *
 * The bridge speaks a small vendor specific protocol on two bulk endpoints.
 * On top of the generic single I/O transfers it offers three commands that
 * keep the whole per-page loop inside the device:
 *
 *   CMD_NOR_PROGRAM       WREN + PP + poll WIP, over a byte range
 *   CMD_NOR_ERASE         WREN + SE + poll WIP, over a byte range
 *   CMD_NAND_READ_PAGES   PAGE READ + poll OIP + READ FROM CACHE, N pages
 *   CMD_NAND_PROGRAM_PAGES  WREN + LOAD + EXECUTE + poll OIP, N pages
 *
 * Using them removes 3..5 USB round trips per page, which is what actually
 * limits the bridge (it is a full-speed device).  Whether they are available
 * is decided at attach time from CMD_GET_CAPS, and every caller of this file
 * falls back to the generic path when they are not.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ufprog/api_spi.h>
#include <ufprog/config.h>
#include <ufprog/endian.h>
#include <ufprog/log.h>
#include "ch552.h"

#define CH552_SPI_IF_MAJOR			1
#define CH552_SPI_IF_MINOR			0

/*---------------------------------------------------------------*
 * Transport helpers                                             *
 *---------------------------------------------------------------*/

/* Send [cmd][len][payload] in one OUT frame.  The device buffers it as a
 * whole, so no partial frame can be observed. */
ufprog_status ch552_send_cmd(struct ufprog_interface *ch552dev, uint8_t cmd, const void *payload,
			     size_t payload_len)
{
	uint8_t frame[CH552_USB_PACKET_SIZE];

	if (payload_len > sizeof(frame) - 2)
		return UFP_INVALID_PARAMETER;

	frame[0] = cmd;
	frame[1] = (uint8_t)payload_len;

	if (payload_len)
		memcpy(frame + 2, payload, payload_len);

	return ch552_write(ch552dev->handle, frame, payload_len + 2, NULL);
}

/* Read exactly len bytes of reply, tolerating short packets: the device
 * fills the IN endpoint in 64 byte frames and only signals the end with a
 * short one. */
ufprog_status ch552_recv_reply(struct ufprog_interface *ch552dev, void *data, size_t len)
{
	uint8_t *p = data;
	size_t got, remain = len;

	while (remain) {
		STATUS_CHECK_RET(ch552_read(ch552dev->handle, p, remain, &got));

		if (!got) {
			logm_err("Bridge reply ended early: %zu of %zu bytes received\n", len - remain, len);
			return UFP_DEVICE_IO_ERROR;
		}

		p += got;
		remain -= got;
	}

	return UFP_OK;
}

ufprog_status ch552_read_status(struct ufprog_interface *ch552dev, uint8_t *retstatus)
{
	uint8_t st;

	STATUS_CHECK_RET(ch552_recv_reply(ch552dev, &st, 1));

	if (retstatus)
		*retstatus = st;

	if (st != CH552_ST_OK) {
		logm_dbg("Bridge command failed with status 0x%02x\n", st);
		return UFP_DEVICE_IO_ERROR;
	}

	return UFP_OK;
}

/*---------------------------------------------------------------*
 * Device identification and configuration                      *
 *---------------------------------------------------------------*/

static ufprog_status ch552_read_fw_status(struct ufprog_interface *ch552dev)
{
	uint8_t buf[CH552_VENDOR_STATUS_LEN];
	int ret;

	ret = libusb_control_transfer(ch552dev->handle->handle,
				      LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
				      CH552_VENDOR_REQ_STATUS, 0, 0, buf, sizeof(buf), CH552_RW_TIMEOUT_MS);
	if (ret < (int)sizeof(buf)) {
		logm_errdbg("Firmware status request failed: %s\n",
			    ret < 0 ? libusb_strerror(ret) : "incomplete reply");
		return UFP_DEVICE_IO_ERROR;
	}

	if (buf[0] != CH552_STATUS_MAGIC_0 || buf[1] != CH552_STATUS_MAGIC_1 || buf[2] != CH552_STATUS_MAGIC_2) {
		logm_err("Firmware status reply carries a wrong magic\n");
		return UFP_DEVICE_IO_ERROR;
	}

	ch552dev->proto_version = buf[CH552_STATUS_OFF_PROTO_VER];
	ch552dev->fw_major = buf[CH552_STATUS_OFF_FW_MAJOR];
	ch552dev->fw_minor = buf[CH552_STATUS_OFF_FW_MINOR];

	if (ch552dev->proto_version != CH552_PROTO_VERSION) {
		logm_err("Bridge firmware speaks protocol v%u, this driver needs v%u\n", ch552dev->proto_version,
			 CH552_PROTO_VERSION);
		return UFP_UNSUPPORTED;
	}

	return UFP_OK;
}

static ufprog_status ch552_get_caps(struct ufprog_interface *ch552dev)
{
	uint8_t buf[16];
	uint16_t caps;

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_GET_CAPS, NULL, 0));
	STATUS_CHECK_RET(ch552_recv_reply(ch552dev, buf, sizeof(buf)));

	if (buf[0] != CH552_ST_OK)
		return UFP_DEVICE_IO_ERROR;

	caps = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);

	ch552dev->caps_lo = buf[1];
	ch552dev->caps_hi = buf[2];

	if (caps & CH552_CAP_NOR_ACCEL)
		ch552dev->accel_mask |= CH552_ACCEL_NOR;

	if (caps & CH552_CAP_NAND_ACCEL)
		ch552dev->accel_mask |= CH552_ACCEL_NAND;

	if (!(caps & CH552_CAP_STREAM_MODE)) {
		logm_err("Bridge firmware does not support streaming transfers\n");
		return UFP_UNSUPPORTED;
	}

	if (!(caps & CH552_CAP_XFER)) {
		logm_err("Bridge firmware does not support buffered transfers\n");
		return UFP_UNSUPPORTED;
	}

	return UFP_OK;
}

static ufprog_status ch552_ping(struct ufprog_interface *ch552dev)
{
	uint8_t buf[5];

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_PING, NULL, 0));
	STATUS_CHECK_RET(ch552_recv_reply(ch552dev, buf, sizeof(buf)));

	if (buf[0] != CH552_ST_OK || buf[1] != 'C' || buf[2] != 'S' || buf[3] != 'P') {
		logm_err("Bridge replied to PING with unexpected data\n");
		return UFP_DEVICE_IO_ERROR;
	}

	if (buf[4] != CH552_PROTO_VERSION) {
		logm_err("Bridge speaks protocol v%u, this driver needs v%u\n", buf[4], CH552_PROTO_VERSION);
		return UFP_UNSUPPORTED;
	}

	return UFP_OK;
}

ufprog_status ch552_spi_init(struct ufprog_interface *ch552dev, struct json_object *config)
{
	uint32_t fsys_hz;

	/* Optional: a firmware built for 12 MHz (3.3 V only boards) reports a
	 * different system clock, and the SPI divider is relative to it. */
	if (config && !json_read_uint32(config, "sysclk-hz", &fsys_hz, 0)) {
		if (fsys_hz < 1000000 || fsys_hz > 48000000) {
			logm_err("Invalid 'sysclk-hz' %u in SPI configuration\n", fsys_hz);
			return UFP_DEVICE_INVALID_CONFIG;
		}

		ch552dev->spi_fsys_hz = fsys_hz;
	}

	/* A dead bulk endpoint is reported by the EP0 vendor request, so read
	 * it first: it is the only diagnostic that still works then. */
	STATUS_CHECK_RET(ch552_read_fw_status(ch552dev));

	STATUS_CHECK_RET(ch552_ping(ch552dev));
	STATUS_CHECK_RET(ch552_get_caps(ch552dev));

	logm_info("Bridge firmware v%u.%u, protocol v%u, capabilities 0x%02x%02x\n", ch552dev->fw_major,
		  ch552dev->fw_minor, ch552dev->proto_version, ch552dev->caps_hi, ch552dev->caps_lo);

	if (!ch552dev->accel_mask)
		logm_info("Bridge has no flash accelerators, using the generic path only\n");

	/* The firmware leaves CS released after a reset.  Make sure the host
	 * view agrees. */
	ch552dev->cs_asserted = false;

	return UFP_OK;
}

/*---------------------------------------------------------------*
 * SPI interface API                                            *
 *---------------------------------------------------------------*/

uint32_t UFPROG_API ufprog_spi_if_version(void)
{
	return MAKE_VERSION(CH552_SPI_IF_MAJOR, CH552_SPI_IF_MINOR);
}

uint32_t UFPROG_API ufprog_spi_if_caps(void)
{
	return 0;
}

size_t UFPROG_API ufprog_spi_max_read_granularity(void)
{
	return CH552_MAX_STREAM_LEN;
}

ufprog_status UFPROG_API ufprog_spi_set_cs_pol(struct ufprog_interface *ch552dev, ufprog_bool positive)
{
	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	/* The bridge drives CS in software with a fixed polarity. */
	if (positive)
		return UFP_UNSUPPORTED;

	return UFP_OK;
}

/* The hardware supports mode 0 and mode 3 only (the master samples MISO on
 * the rising edge of SCK). */
ufprog_status UFPROG_API ufprog_spi_set_mode(struct ufprog_interface *ch552dev, uint32_t mode)
{
	uint8_t payload[2];
	ufprog_status ret;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (mode != SPI_MODE_0 && mode != SPI_MODE_3) {
		logm_err("Only SPI mode 0 and 3 are supported by the bridge\n");
		return UFP_UNSUPPORTED;
	}

	payload[0] = ch552dev->spi_div;
	payload[1] = mode == SPI_MODE_3 ? 1 : 0;

	ret = ch552_send_cmd(ch552dev, CH552_CMD_SET_SPI_CFG, payload, sizeof(payload));
	if (ret)
		return ret;

	ret = ch552_read_status(ch552dev, NULL);
	if (ret)
		return ret;

	ch552dev->spi_mode3 = payload[1];

	/* The command re-initialises the SPI pins, which releases CS. */
	ch552dev->cs_asserted = false;

	return UFP_OK;
}

static uint32_t ch552_div_for_freq(struct ufprog_interface *ch552dev, uint32_t hz)
{
	uint32_t div, want;

	if (!hz)
		return 0;

	/* Pick the smallest divider that does not exceed the requested
	 * frequency.  The hardware only accepts even dividers >= 2. */
	want = (ch552dev->spi_fsys_hz + hz - 1) / hz;
	if (want < 2)
		want = 2;

	for (div = 2; div < 256; div += 2) {
		if (div >= want)
			return div;
	}

	return 254;
}

ufprog_status UFPROG_API ufprog_spi_set_speed(struct ufprog_interface *ch552dev, uint32_t hz, uint32_t *rethz)
{
	uint8_t payload[2];
	uint32_t div;
	ufprog_status ret;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	div = ch552_div_for_freq(ch552dev, hz);
	if (!div)
		return UFP_INVALID_PARAMETER;

	if (div == ch552dev->spi_div) {
		if (rethz)
			*rethz = ch552dev->spi_fsys_hz / div;

		return UFP_OK;
	}

	payload[0] = (uint8_t)div;
	payload[1] = ch552dev->spi_mode3;

	ret = ch552_send_cmd(ch552dev, CH552_CMD_SET_SPI_CFG, payload, sizeof(payload));
	if (ret)
		return ret;

	ret = ch552_read_status(ch552dev, NULL);
	if (ret)
		return ret;

	ch552dev->spi_div = (uint8_t)div;

	if (rethz)
		*rethz = ch552dev->spi_fsys_hz / div;

	return UFP_OK;
}

uint32_t UFPROG_API ufprog_spi_get_speed(struct ufprog_interface *ch552dev)
{
	if (!ch552dev || !ch552dev->spi_div)
		return 0;

	return ch552dev->spi_fsys_hz / ch552dev->spi_div;
}

ufprog_status UFPROG_API ufprog_spi_get_speed_range(struct ufprog_interface *ch552dev, uint32_t *retlowhz,
						    uint32_t *rethighhz)
{
	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (retlowhz)
		*retlowhz = ch552dev->spi_fsys_hz / 254;

	if (rethighhz)
		*rethighhz = ch552dev->spi_fsys_hz / 2;

	return UFP_OK;
}

uint32_t UFPROG_API ufprog_spi_get_speed_list(struct ufprog_interface *ch552dev, uint32_t *retlist, uint32_t count)
{
	uint32_t div, num = 0;

	if (!ch552dev)
		return 0;

	for (div = 2; div <= 254; div += 2) {
		if (num < count && retlist)
			retlist[num] = ch552dev->spi_fsys_hz / div;

		num++;
	}

	return num;
}

ufprog_status UFPROG_API ufprog_spi_set_wp(struct ufprog_interface *ch552dev, ufprog_bool high)
{
	uint8_t payload[2];

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!(ch552dev->caps_lo & CH552_CAP_GPIO))
		return UFP_UNSUPPORTED;

	payload[0] = CH552_GPIO_WP;
	payload[1] = high ? 1 : 0;

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_GPIO_WRITE, payload, sizeof(payload)));

	return ch552_read_status(ch552dev, NULL);
}

ufprog_status UFPROG_API ufprog_spi_set_hold(struct ufprog_interface *ch552dev, ufprog_bool high)
{
	uint8_t payload[2];

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!(ch552dev->caps_lo & CH552_CAP_GPIO))
		return UFP_UNSUPPORTED;

	payload[0] = CH552_GPIO_HOLD;
	payload[1] = high ? 1 : 0;

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_GPIO_WRITE, payload, sizeof(payload)));

	return ch552_read_status(ch552dev, NULL);
}

/*---------------------------------------------------------------*
 * Chip select                                                  *
 *---------------------------------------------------------------*/

/* CS is software driven on the bridge, so it survives across commands.  The
 * generic transfer API expects the opposite (it drives CS per message), which
 * is why every transfer ends with an explicit release here. */
static ufprog_status ch552_spi_cs(struct ufprog_interface *ch552dev, bool assert)
{
	uint8_t payload = assert ? 0 : 1;

	/* Silent command by protocol: the host learns about completion from the
	 * next accepted OUT transfer. */
	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_CS, &payload, 1));

	ch552dev->cs_asserted = assert;

	return UFP_OK;
}

/*---------------------------------------------------------------*
 * Generic transfers                                            *
 *---------------------------------------------------------------*/

/* Streaming write: [cmd][len=4+pre][count24][pre_len][pre...] followed by
 * `count` raw data frames.  CS is managed by the device because it is not
 * held by the host. */
static ufprog_status ch552_stream_write(struct ufprog_interface *ch552dev, const uint8_t *prefix, uint32_t pre_len,
					const void *data, size_t len)
{
	uint8_t desc[4 + CH552_MAX_PREFIX];
	uint32_t off, chunk;

	if (pre_len > CH552_MAX_PREFIX)
		return UFP_INVALID_PARAMETER;

	desc[0] = len & 0xff;
	desc[1] = (len >> 8) & 0xff;
	desc[2] = (len >> 16) & 0xff;
	desc[3] = (uint8_t)pre_len;

	if (pre_len)
		memcpy(desc + 4, prefix, pre_len);

	if (!ch552dev->cs_asserted)
		STATUS_CHECK_RET(ch552_spi_cs(ch552dev, true));

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_STREAM_WRITE, desc, 4 + pre_len));

	for (off = 0; off < len; off += chunk) {
		chunk = len - off;
		if (chunk > CH552_DATA_CHUNK_SIZE)
			chunk = CH552_DATA_CHUNK_SIZE;

		STATUS_CHECK_RET(ch552_write(ch552dev->handle, (const uint8_t *)data + off, chunk, NULL));
	}

	STATUS_CHECK_RET(ch552_read_status(ch552dev, NULL));

	if (!ch552dev->cs_asserted)
		STATUS_CHECK_RET(ch552_spi_cs(ch552dev, false));

	return UFP_OK;
}

/* Streaming read: same descriptor, then `count` bytes of data followed by a
 * single status byte. */
static ufprog_status ch552_stream_read(struct ufprog_interface *ch552dev, const uint8_t *prefix, uint32_t pre_len,
				       void *data, size_t len)
{
	uint8_t desc[4 + CH552_MAX_PREFIX];
	ufprog_status ret;

	if (pre_len > CH552_MAX_PREFIX)
		return UFP_INVALID_PARAMETER;

	desc[0] = len & 0xff;
	desc[1] = (len >> 8) & 0xff;
	desc[2] = (len >> 16) & 0xff;
	desc[3] = (uint8_t)pre_len;

	if (pre_len)
		memcpy(desc + 4, prefix, pre_len);

	if (!ch552dev->cs_asserted)
		STATUS_CHECK_RET(ch552_spi_cs(ch552dev, true));

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, CH552_CMD_STREAM_READ, desc, 4 + pre_len));

	/* The data phase must be drained before the status byte, otherwise the
	 * status would be interpreted as data. */
	STATUS_CHECK_GOTO_RET(ch552_recv_reply(ch552dev, data, len), ret, out);

	ret = ch552_read_status(ch552dev, NULL);

out:
	if (!ch552dev->cs_asserted)
		STATUS_CHECK_RET(ch552_spi_cs(ch552dev, false));

	return ret;
}

static ufprog_status ch552_spi_generic_xfer_one(struct ufprog_interface *ch552dev,
						const struct ufprog_spi_transfer *xfer)
{
	uint8_t prefix[CH552_MAX_PREFIX];

	if (xfer->buswidth > 1 || xfer->dtr) {
		logm_err("Only single I/O single rate is supported\n");
		return UFP_UNSUPPORTED;
	}

	if (xfer->speed)
		STATUS_CHECK_RET(ufprog_spi_set_speed(ch552dev, xfer->speed, NULL));

	memset(prefix, 0xff, sizeof(prefix));

	if (xfer->dir == SPI_DATA_IN)
		return ch552_stream_read(ch552dev, prefix, 0, xfer->buf.rx, xfer->len);

	return ch552_stream_write(ch552dev, prefix, 0, xfer->buf.tx, xfer->len);
}

ufprog_status UFPROG_API ufprog_spi_generic_xfer(struct ufprog_interface *ch552dev,
						 const struct ufprog_spi_transfer *xfers, uint32_t count)
{
	ufprog_status ret = UFP_OK;
	uint32_t i;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	os_mutex_lock(ch552dev->lock);

	/* The two-phase form used by the SPI-MEM core (command then data) is
	 * folded into a single stream command when possible: that saves one USB
	 * round trip per transfer, which matters a lot at full speed. */
	if (count == 2 && xfers[0].dir == SPI_DATA_OUT && xfers[1].dir == SPI_DATA_IN &&
	    xfers[0].len <= CH552_MAX_PREFIX && !(xfers[0].dtr || xfers[1].dtr) &&
	    xfers[0].buswidth == 1 && xfers[1].buswidth == 1) {
		if (xfers[0].speed || xfers[1].speed) {
			uint32_t speed = xfers[0].speed > xfers[1].speed ? xfers[0].speed : xfers[1].speed;

			STATUS_CHECK_GOTO_RET(ufprog_spi_set_speed(ch552dev, speed, NULL), ret, out);
		}

		STATUS_CHECK_GOTO_RET(ch552_stream_read(ch552dev, xfers[0].buf.tx, xfers[0].len, xfers[1].buf.rx,
						       xfers[1].len), ret, out);

		goto out;
	}

	for (i = 0; i < count; i++) {
		ret = ch552_spi_generic_xfer_one(ch552dev, &xfers[i]);
		if (ret)
			goto out;
	}

out:
	/* Whatever happened, leave the bus in a well defined state: CS released
	 * and the host view of it in sync. */
	if (ch552dev->cs_asserted) {
		ufprog_status cret = ch552_spi_cs(ch552dev, false);
		if (!ret)
			ret = cret;
	}

	os_mutex_unlock(ch552dev->lock);

	return ret;
}

/*---------------------------------------------------------------*
 * SPI-MEM operations                                           *
 *                                                               *
 * These are the hot path.  Everything that can be folded into a *
 * stream command is, so a page read costs one command frame    *
 * plus the data frames and nothing else.                       *
 *---------------------------------------------------------------*/

static ufprog_status ch552_mem_fill_prefix(const struct ufprog_spi_mem_op *op, uint8_t *prefix, uint32_t *pre_len)
{
	uint32_t n = 0;
	uint32_t i;

	if (op->cmd.len) {
		if (op->cmd.len > 2 || op->cmd.buswidth != 1 || op->cmd.dtr)
			return UFP_UNSUPPORTED;

		for (i = 0; i < op->cmd.len; i++)
			prefix[n++] = (op->cmd.opcode >> (8 * i)) & 0xff;
	}

	if (op->addr.len) {
		if (op->addr.len > 4 || op->addr.buswidth != 1 || op->addr.dtr)
			return UFP_UNSUPPORTED;

		for (i = 0; i < op->addr.len; i++)
			prefix[n++] = (op->addr.val >> (8 * (op->addr.len - i - 1))) & 0xff;
	}

	if (op->dummy.len) {
		if (op->dummy.buswidth != 1 || op->dummy.dtr)
			return UFP_UNSUPPORTED;

		for (i = 0; i < op->dummy.len; i++)
			prefix[n++] = 0xff;
	}

	*pre_len = n;

	return UFP_OK;
}

ufprog_status UFPROG_API ufprog_spi_mem_supports_op(struct ufprog_interface *ch552dev,
						    const struct ufprog_spi_mem_op *op)
{
	uint8_t prefix[CH552_MAX_PREFIX];
	uint32_t pre_len;
	size_t total;

	if (!ch552dev || !op)
		return false;

	if (op->cmd.buswidth > 1 || op->addr.buswidth > 1 || op->dummy.buswidth > 1 || op->data.buswidth > 1)
		return false;

	if (op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr)
		return false;

	if (ch552_mem_fill_prefix(op, prefix, &pre_len))
		return false;

	total = pre_len + op->data.len;

	if (op->data.dir == SPI_DATA_OUT)
		return total <= CH552_MAX_STREAM_LEN;

	return op->data.len <= CH552_MAX_STREAM_LEN;
}

ufprog_status UFPROG_API ufprog_spi_mem_adjust_op_size(struct ufprog_interface *ch552dev,
						       struct ufprog_spi_mem_op *op)
{
	uint8_t prefix[CH552_MAX_PREFIX];
	uint32_t pre_len;
	size_t room;

	if (!ch552dev || !op)
		return UFP_INVALID_PARAMETER;

	if (ch552_mem_fill_prefix(op, prefix, &pre_len))
		return UFP_UNSUPPORTED;

	if (op->data.dir == SPI_DATA_OUT)
		room = CH552_MAX_STREAM_LEN - pre_len;
	else
		room = CH552_MAX_STREAM_LEN;

	if (op->data.len > room)
		op->data.len = room;

	return UFP_OK;
}

ufprog_status UFPROG_API ufprog_spi_mem_exec_op(struct ufprog_interface *ch552dev,
						const struct ufprog_spi_mem_op *op)
{
	uint8_t prefix[CH552_MAX_PREFIX];
	uint32_t pre_len;
	ufprog_status ret;

	if (!ch552dev || !op)
		return UFP_INVALID_PARAMETER;

	os_mutex_lock(ch552dev->lock);

	STATUS_CHECK_GOTO_RET(ch552_mem_fill_prefix(op, prefix, &pre_len), ret, out);

	if (op->data.dir == SPI_DATA_IN) {
		if (op->data.len)
			ret = ch552_stream_read(ch552dev, prefix, pre_len, op->data.buf.rx, op->data.len);
		else
			ret = ch552_stream_write(ch552dev, prefix, pre_len, NULL, 0);
	} else {
		ret = ch552_stream_write(ch552dev, prefix, pre_len, op->data.buf.tx, op->data.len);
	}

out:
	if (ch552dev->cs_asserted) {
		ufprog_status cret = ch552_spi_cs(ch552dev, false);
		if (!ret)
			ret = cret;
	}

	os_mutex_unlock(ch552dev->lock);

	return ret;
}

ufprog_status UFPROG_API ufprog_spi_mem_poll_status(struct ufprog_interface *ch552dev,
						    const struct ufprog_spi_mem_op *op, uint16_t mask,
						    uint16_t match, uint32_t initial_delay_us,
						    uint32_t polling_rate_us, uint32_t timeout_ms)
{
	uint64_t end_us;
	uint16_t val;

	if (!ch552dev || !op)
		return UFP_INVALID_PARAMETER;

	if (op->data.len < 1 || op->data.len > 2 || op->data.dir != SPI_DATA_IN)
		return UFP_UNSUPPORTED;

	os_udelay(initial_delay_us);

	end_us = os_get_timer_us() + (uint64_t)timeout_ms * 1000;

	do {
		STATUS_CHECK_RET(ufprog_spi_mem_exec_op(ch552dev, op));

		if (op->data.len == 2)
			val = ((uint16_t)((const uint8_t *)op->data.buf.rx)[0] << 8) |
			      ((const uint8_t *)op->data.buf.rx)[1];
		else
			val = ((const uint8_t *)op->data.buf.rx)[0];

		if ((val & mask) == match)
			return UFP_OK;

		if (polling_rate_us)
			os_udelay(polling_rate_us);
	} while (os_get_timer_us() <= end_us);

	return UFP_TIMEOUT;
}
