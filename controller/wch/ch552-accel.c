/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * Flash accelerators for the CH552 USB-SPI bridge
 *
 * The generic SPI-MEM path issues one command per flash phase, so a single
 * NOR page costs WREN, PP and one WIP poll as three USB round trips.  On a
 * full-speed device that dominates the programming time even though the SPI
 * clock itself is only 12 MHz.
 *
 * The bridge firmware implements the whole per-page loop, so the host streams
 * a whole block with one command and the device does the rest:
 *
 *   CMD_NOR_PROGRAM        [addr24][len24] + data
 *                          -> [status][done24]
 *   CMD_NOR_ERASE          [addr24][len24]
 *                          -> [status][done24]
 *   CMD_NAND_SET_CFG       [page16][oob16][flags]
 *                          -> [status]
 *   CMD_NAND_READ_PAGES    [page24][npages16]
 *                          -> data + [status]
 *   CMD_NAND_PROGRAM_PAGES [page24][npages16] + data
 *                          -> [status][done24]
 *
 * All of them drive CS themselves, so they must not be mixed with a host-held
 * chip select (the device replies ST_BUSY in that case).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ufprog/api_spi.h>
#include <ufprog/config.h>
#include <ufprog/endian.h>
#include <ufprog/log.h>
#include "ch552.h"

#define CH552_ACCEL_IF_MAJOR			1
#define CH552_ACCEL_IF_MINOR			0

/*---------------------------------------------------------------*
 * Accelerator capability queries                               *
 *---------------------------------------------------------------*/

ufprog_bool UFPROG_API ch552_has_nor_accel(struct ufprog_interface *ch552dev)
{
	if (!ch552dev)
		return false;

	return !!(ch552dev->accel_mask & CH552_ACCEL_NOR);
}

ufprog_bool UFPROG_API ch552_has_nand_accel(struct ufprog_interface *ch552dev)
{
	if (!ch552dev)
		return false;

	return !!(ch552dev->accel_mask & CH552_ACCEL_NAND);
}

/*---------------------------------------------------------------*
 * Command plumbing                                             *
 *---------------------------------------------------------------*/

/* Send the command frame, the data phase (if any) and read the reply. */
static ufprog_status ch552_accel_send(struct ufprog_interface *ch552dev, uint8_t cmd, const void *payload,
				      size_t payload_len, const void *data, size_t data_len)
{
	uint32_t off, chunk;

	if (ch552dev->cs_asserted) {
		logm_dbg("Accelerator command rejected: CS is held by the host\n");
		return UFP_DEVICE_IO_ERROR;
	}

	STATUS_CHECK_RET(ch552_send_cmd(ch552dev, cmd, payload, payload_len));

	for (off = 0; off < data_len; off += chunk) {
		chunk = data_len - off;
		if (chunk > CH552_DATA_CHUNK_SIZE)
			chunk = CH552_DATA_CHUNK_SIZE;

		STATUS_CHECK_RET(ch552_write(ch552dev->handle, (const uint8_t *)data + off, chunk, NULL));
	}

	return UFP_OK;
}

/* Read the common [status][done24] reply. */
static ufprog_status ch552_accel_read_done(struct ufprog_interface *ch552dev, uint32_t *retdone)
{
	uint8_t buf[CH552_ACCEL_REPLY_LEN];
	uint32_t done;

	STATUS_CHECK_RET(ch552_recv_reply(ch552dev, buf, sizeof(buf)));

	if (buf[0] != CH552_ST_OK) {
		done = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);

		logm_warn("Accelerator command failed with status 0x%02x, %u units completed\n", buf[0], done);

		if (retdone)
			*retdone = done;

		return UFP_DEVICE_IO_ERROR;
	}

	if (retdone) {
		done = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) | ((uint32_t)buf[3] << 16);
		*retdone = done;
	}

	return UFP_OK;
}

/*---------------------------------------------------------------*
 * NOR page program                                             *
 *---------------------------------------------------------------*/

ufprog_status UFPROG_API ch552_nor_program(struct ufprog_interface *ch552dev, uint64_t addr, size_t len,
					   const void *data)
{
	uint8_t payload[6];
	ufprog_status ret;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!ch552_has_nor_accel(ch552dev))
		return UFP_UNSUPPORTED;

	if (!len || len > 0xffffff || addr > 0xffffff)
		return UFP_INVALID_PARAMETER;

	os_mutex_lock(ch552dev->lock);

	payload[0] = addr & 0xff;
	payload[1] = (addr >> 8) & 0xff;
	payload[2] = (addr >> 16) & 0xff;
	payload[3] = len & 0xff;
	payload[4] = (len >> 8) & 0xff;
	payload[5] = (len >> 16) & 0xff;

	ret = ch552_accel_send(ch552dev, CH552_CMD_NOR_PROGRAM, payload, sizeof(payload), data, len);
	if (ret)
		goto out;

	ret = ch552_accel_read_done(ch552dev, NULL);

out:
	os_mutex_unlock(ch552dev->lock);

	return ret;
}

/*---------------------------------------------------------------*
 * NOR sector erase                                             *
 *---------------------------------------------------------------*/

ufprog_status UFPROG_API ch552_nor_erase(struct ufprog_interface *ch552dev, uint64_t addr, size_t len)
{
	uint8_t payload[6];
	ufprog_status ret;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!ch552_has_nor_accel(ch552dev))
		return UFP_UNSUPPORTED;

	/* The device erases whole 4 KiB sectors and needs both fields to be
	 * sector aligned. */
	if (!len || len > 0xffffff || addr > 0xffffff || (addr & 0xfff) || (len & 0xfff))
		return UFP_INVALID_PARAMETER;

	os_mutex_lock(ch552dev->lock);

	payload[0] = addr & 0xff;
	payload[1] = (addr >> 8) & 0xff;
	payload[2] = (addr >> 16) & 0xff;
	payload[3] = len & 0xff;
	payload[4] = (len >> 8) & 0xff;
	payload[5] = (len >> 16) & 0xff;

	ret = ch552_accel_send(ch552dev, CH552_CMD_NOR_ERASE, payload, sizeof(payload), NULL, 0);
	if (ret)
		goto out;

	ret = ch552_accel_read_done(ch552dev, NULL);

out:
	os_mutex_unlock(ch552dev->lock);

	return ret;
}

/*---------------------------------------------------------------*
 * NAND geometry                                                *
 *---------------------------------------------------------------*/

ufprog_status UFPROG_API ch552_nand_set_geometry(struct ufprog_interface *ch552dev, uint32_t page_size,
						 uint32_t oob_size)
{
	uint8_t payload[5];
	ufprog_status ret;

	if (!ch552dev)
		return UFP_INVALID_PARAMETER;

	if (!ch552_has_nand_accel(ch552dev))
		return UFP_UNSUPPORTED;

	if (page_size < 256 || page_size > 4096 || oob_size > 512)
		return UFP_INVALID_PARAMETER;

	os_mutex_lock(ch552dev->lock);

	/* flags bit0 selects READ FROM CACHE 0x0B (one dummy byte) over 0x03.
	 * The generic SPI-MEM core builds the dummy itself, so keep the plain
	 * 0x03 form and let the caller decide which one to use. */
	payload[0] = page_size & 0xff;
	payload[1] = (page_size >> 8) & 0xff;
	payload[2] = oob_size & 0xff;
	payload[3] = (oob_size >> 8) & 0xff;
	payload[4] = 0;

	ret = ch552_accel_send(ch552dev, CH552_CMD_NAND_SET_CFG, payload, sizeof(payload), NULL, 0);
	if (ret)
		goto out;

	ret = ch552_read_status(ch552dev, NULL);
	if (ret)
		goto out;

	ch552dev->nand_page_size = page_size;
	ch552dev->nand_oob_size = oob_size;
	ch552dev->nand_geom_set = true;

out:
	os_mutex_unlock(ch552dev->lock);

	return ret;
}

ufprog_bool UFPROG_API ch552_nand_geometry_matches(struct ufprog_interface *ch552dev, uint32_t page_size,
						   uint32_t oob_size)
{
	if (!ch552dev || !ch552dev->nand_geom_set)
		return false;

	return ch552dev->nand_page_size == page_size && ch552dev->nand_oob_size == oob_size;
}

/*---------------------------------------------------------------*
 * NAND page read                                               *
 *---------------------------------------------------------------*/

ufprog_status UFPROG_API ch552_nand_read_pages(struct ufprog_interface *ch552dev, uint32_t page, uint32_t count,
					       void *buf)
{
	uint8_t payload[5];
	ufprog_status ret;
	size_t len;

	if (!ch552dev || !buf)
		return UFP_INVALID_PARAMETER;

	if (!ch552_has_nand_accel(ch552dev))
		return UFP_UNSUPPORTED;

	if (!ch552dev->nand_geom_set)
		return UFP_UNSUPPORTED;

	if (!count || page > 0xffffff || count > 0xffff)
		return UFP_INVALID_PARAMETER;

	len = (size_t)count * (ch552dev->nand_page_size + ch552dev->nand_oob_size);

	os_mutex_lock(ch552dev->lock);

	payload[0] = page & 0xff;
	payload[1] = (page >> 8) & 0xff;
	payload[2] = (page >> 16) & 0xff;
	payload[3] = count & 0xff;
	payload[4] = (count >> 8) & 0xff;

	ret = ch552_accel_send(ch552dev, CH552_CMD_NAND_READ_PAGES, payload, sizeof(payload), NULL, 0);
	if (ret)
		goto out;

	/* Data first, then the status byte. */
	ret = ch552_recv_reply(ch552dev, buf, len);
	if (ret)
		goto out;

	ret = ch552_read_status(ch552dev, NULL);

out:
	os_mutex_unlock(ch552dev->lock);

	return ret;
}

/*---------------------------------------------------------------*
 * NAND page program                                            *
 *---------------------------------------------------------------*/

ufprog_status UFPROG_API ch552_nand_program_pages(struct ufprog_interface *ch552dev, uint32_t page, uint32_t count,
						  const void *data)
{
	uint8_t payload[5];
	ufprog_status ret;
	size_t len;

	if (!ch552dev || !data)
		return UFP_INVALID_PARAMETER;

	if (!ch552_has_nand_accel(ch552dev))
		return UFP_UNSUPPORTED;

	if (!ch552dev->nand_geom_set)
		return UFP_UNSUPPORTED;

	if (!count || page > 0xffffff || count > 0xffff)
		return UFP_INVALID_PARAMETER;

	len = (size_t)count * (ch552dev->nand_page_size + ch552dev->nand_oob_size);

	os_mutex_lock(ch552dev->lock);

	payload[0] = page & 0xff;
	payload[1] = (page >> 8) & 0xff;
	payload[2] = (page >> 16) & 0xff;
	payload[3] = count & 0xff;
	payload[4] = (count >> 8) & 0xff;

	ret = ch552_accel_send(ch552dev, CH552_CMD_NAND_PROGRAM_PAGES, payload, sizeof(payload), data, len);
	if (ret)
		goto out;

	ret = ch552_accel_read_done(ch552dev, NULL);

out:
	os_mutex_unlock(ch552dev->lock);

	return ret;
}
