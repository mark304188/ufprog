/* SPDX-License-Identifier: LGPL-2.1-only */
/*
 * Author: Weijie Gao <hackpascal@gmail.com>
 *
 * Optional controller-side flash accelerators
 *
 * Some controllers can run the per-page flash loop themselves, so the host
 * hands over a whole block with one command instead of issuing WREN, program
 * and status-poll as separate transfers.  On a full-speed device that saves
 * several USB round trips per page, which is usually the dominant cost.
 *
 * The functions live in the controller plugin and are looked up by name, so
 * a controller that does not provide them simply leaves the pointers NULL
 * and the flash layers fall back to the generic SPI-MEM path.
 */
#pragma once

#ifndef _UFPROG_CONTROLLER_ACCEL_H_
#define _UFPROG_CONTROLLER_ACCEL_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ufprog/device.h>
#include <ufprog/driver.h>
#include <ufprog/osdef.h>
#include <ufprog/spi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define API_NAME_CTRL_NOR_ACCEL_SUPPORTED	"ch552_has_nor_accel"
typedef bool (UFPROG_API *api_ctrl_nor_accel_supported)(struct ufprog_interface *ifdev);

#define API_NAME_CTRL_NAND_ACCEL_SUPPORTED	"ch552_has_nand_accel"
typedef bool (UFPROG_API *api_ctrl_nand_accel_supported)(struct ufprog_interface *ifdev);

#define API_NAME_CTRL_NOR_PROGRAM		"ch552_nor_program"
typedef ufprog_status (UFPROG_API *api_ctrl_nor_program)(struct ufprog_interface *ifdev, uint64_t addr,
							 size_t len, const void *data);

#define API_NAME_CTRL_NOR_ERASE			"ch552_nor_erase"
typedef ufprog_status (UFPROG_API *api_ctrl_nor_erase)(struct ufprog_interface *ifdev, uint64_t addr,
						       size_t len);

#define API_NAME_CTRL_NAND_SET_GEOMETRY		"ch552_nand_set_geometry"
typedef ufprog_status (UFPROG_API *api_ctrl_nand_set_geometry)(struct ufprog_interface *ifdev,
							       uint32_t page_size, uint32_t oob_size);

#define API_NAME_CTRL_NAND_GEOMETRY_MATCHES	"ch552_nand_geometry_matches"
typedef bool (UFPROG_API *api_ctrl_nand_geometry_matches)(struct ufprog_interface *ifdev, uint32_t page_size,
							  uint32_t oob_size);

#define API_NAME_CTRL_NAND_READ_PAGES		"ch552_nand_read_pages"
typedef ufprog_status (UFPROG_API *api_ctrl_nand_read_pages)(struct ufprog_interface *ifdev, uint32_t page,
							     uint32_t count, void *buf);

#define API_NAME_CTRL_NAND_PROGRAM_PAGES		"ch552_nand_program_pages"
typedef ufprog_status (UFPROG_API *api_ctrl_nand_program_pages)(struct ufprog_interface *ifdev, uint32_t page,
								uint32_t count, const void *data);

struct ufprog_controller_accel {
	struct ufprog_interface *ifdev;

	api_ctrl_nor_accel_supported nor_accel_supported;
	api_ctrl_nor_program nor_program;
	api_ctrl_nor_erase nor_erase;

	api_ctrl_nand_accel_supported nand_accel_supported;
	api_ctrl_nand_set_geometry nand_set_geometry;
	api_ctrl_nand_geometry_matches nand_geometry_matches;
	api_ctrl_nand_read_pages nand_read_pages;
	api_ctrl_nand_program_pages nand_program_pages;
};

/* Look up the accelerator entry points of the controller behind a SPI
 * interface device.  Missing symbols leave the corresponding field NULL. */
static inline void ufprog_controller_accel_init(struct ufprog_controller_accel *accel, struct ufprog_spi *spi)
{
	struct ufprog_controller_driver *drv;
	struct ufprog_controller_device *dev;

	struct symbol_find_entry symbols[] = {
		FIND_MODULE(API_NAME_CTRL_NOR_ACCEL_SUPPORTED, accel->nor_accel_supported),
		FIND_MODULE(API_NAME_CTRL_NOR_PROGRAM, accel->nor_program),
		FIND_MODULE(API_NAME_CTRL_NOR_ERASE, accel->nor_erase),
		FIND_MODULE(API_NAME_CTRL_NAND_ACCEL_SUPPORTED, accel->nand_accel_supported),
		FIND_MODULE(API_NAME_CTRL_NAND_SET_GEOMETRY, accel->nand_set_geometry),
		FIND_MODULE(API_NAME_CTRL_NAND_GEOMETRY_MATCHES, accel->nand_geometry_matches),
		FIND_MODULE(API_NAME_CTRL_NAND_READ_PAGES, accel->nand_read_pages),
		FIND_MODULE(API_NAME_CTRL_NAND_PROGRAM_PAGES, accel->nand_program_pages),
	};

	memset(accel, 0, sizeof(*accel));

	dev = ufprog_spi_get_device(spi);
	if (!dev)
		return;

	drv = ufprog_controller_device_get_driver(dev);
	if (!drv)
		return;

	accel->ifdev = ufprog_controller_device_get_interface(dev);

	ufprog_controller_find_symbols(drv, symbols, ARRAY_SIZE(symbols), false);
}

#ifdef __cplusplus
}
#endif

#endif /* _UFPROG_CONTROLLER_ACCEL_H_ */
