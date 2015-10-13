/*
 * This software is contributed or developed by KYOCERA Corporation.
 * (C) 2012 KYOCERA Corporation
 *
 * kernel/include/linux/mipi_novatek_wxga_ext.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
*/
#ifndef MIPI_NOVATEK_WXGA_EXT_H
#define MIPI_NOVATEK_WXGA_EXT_H
#include <linux/disp_ext_blc.h>

#define mipi_novatek_wxga_cabc_mode_select(mode) \
            ( disp_ext_blc_mode_select(mode) )
#endif  /* MIPI_NOVATEK_WXGA_EXT_H */
