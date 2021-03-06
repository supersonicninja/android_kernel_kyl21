






/*
 SiI8334 Linux Driver

 Copyright (C) 2011 Silicon Image Inc.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation version 2.

 This program is distributed .as is. WITHOUT ANY WARRANTY of any
 kind, whether express or implied; without even the implied warranty
 of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE.  See the
 GNU General Public License for more details.
*/
/*
 This software is contributed or developed by KYOCERA Corporation.
 (C) 2012 KYOCERA Corporation
*/
/*
   @file si_drv_mhl_tx.h
 */

// DEVCAP we will initialize to
#define	MHL_LOGICAL_DEVICE_MAP		(MHL_DEV_LD_GUI )
 

typedef void (*mhl_event_func)(int mhlinfo);

struct mhl_event_callback {
	mhl_event_func fn;
};

extern int32_t mhl_reg_cbfunc(struct mhl_event_callback* cb); 
extern int32_t mhl_unreg_cbfunc(struct mhl_event_callback* cb); 
