/*
 * Datalogic Controls Header
 *
 * Copyright (C) 2016 Datalogic Automation Srl
 *
 * Contacts: Datalogic Automation Srl <info.automation.it@datalogic.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __UAPI_DATALOGIC_V4L2_CONTROLS_H__
#define __UAPI_DATALOGIC_V4L2_CONTROLS_H__

#include <linux/v4l2-controls.h>

// Custom controls base
#define V4L2_CID_DATALOGIC_OFFSET		0x2000
#define V4L2_CID_DATALOGIC_BASE			(V4L2_CID_USER_BASE + V4L2_CID_DATALOGIC_OFFSET)

/*
 * Custom controls keys
 */

/* Change operating mode */
#define V4L2_CID_DATALOGIC_OPERATIVE_MODE	(V4L2_CID_DATALOGIC_BASE + 0)
/* Trigger a new acquistion */
#define V4L2_CID_DATALOGIC_TRIGGER		(V4L2_CID_DATALOGIC_BASE + 1)
/* Change debug level */
#define V4L2_CID_DATALOGIC_DEBUG_PRINTS		(V4L2_CID_DATALOGIC_BASE + 2)
/* Show user statistics */
#define V4L2_CID_DATALOGIC_USER_STATS		(V4L2_CID_DATALOGIC_BASE + 3)
/* Set laser pointer */
#define V4L2_CID_DATALOGIC_LASER_POINTER	(V4L2_CID_DATALOGIC_BASE + 4)
/* Turn on/off laser pointer */
#define V4L2_CID_DATALOGIC_GREEN_SPOT		(V4L2_CID_DATALOGIC_BASE + 5)
/* Set digital gain */
#define V4L2_CID_DATALOGIC_DIGITAL_GAIN		(V4L2_CID_DATALOGIC_BASE + 6)
/* Get device information */
#define V4L2_CID_DATALOGIC_DEVICE_INFO		(V4L2_CID_DATALOGIC_BASE + 7)
/* Request a firmware upgrade */
#define V4L2_CID_DATALOGIC_FW_UPLOAD		(V4L2_CID_DATALOGIC_BASE + 8)
/* Adjust black level calibration */
#define V4L2_CID_DATALOGIC_BLACK_LEVEL_CUSTOM	(V4L2_CID_DATALOGIC_BASE + 9)  /**< Available only in H1 **/
/* Adjust pixel saturation */
#define V4L2_CID_DATALOGIC_PIXEL_SATURATION	(V4L2_CID_DATALOGIC_BASE + 10) /**< Available only in H2 **/

#endif /* __UAPI_DATALOGIC_V4L2_CONTROLS_H__ */
