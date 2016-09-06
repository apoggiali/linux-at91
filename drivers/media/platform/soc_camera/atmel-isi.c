/*
 * Copyright (c) 2015 Atmel Corporation
 * Josh Wu, <josh.wu@atmel.com>
 *
 * Add SAMA5D2 chip's ISC (Image Sensor Controller) hardware support.
 *
 * Based on previous work by Lars Haring, <lars.haring@atmel.com>
 * and Sedji Gaouaou
 * Based on the bttv driver for Bt848 with respective copyright holders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/soc_camera.h>
#include <media/soc_mediabus.h>
#include <media/v4l2-of.h>
#include <media/videobuf2-dma-nc.h>

#include "atmel-isi.h"
#include "atmel-isc.h"

#define MAX_BUFFER_NUM			32
#define MAX_SUPPORT_WIDTH		2048
#define MAX_SUPPORT_HEIGHT		2048
#define VID_LIMIT_BYTES			(16 * 1024 * 1024)
#define MIN_FRAME_RATE			15
#define FRAME_INTERVAL_MILLI_SEC	(1000 / MIN_FRAME_RATE)

/* Frame buffer descriptor */
struct fbd_isi_v2 {
	/* Physical address of the frame buffer */
	u32 fb_address;
	/* DMA Control Register(only in HISI2) */
	u32 dma_ctrl;
	/* Physical address of the next fbd */
	u32 next_fbd_address;
};

struct fbd_view {
	/* DMA Control Register */
	u32 dma_ctrl;
	/* Physical address of the next fbd */
	u32 next_fbd_address;
	/* Physical address of the frame buffer 0 */
	u32 fb_address;
	/* stride 0 */
	u32 fb_stride;
};

union fbd {
	struct fbd_isi_v2 fbd_isi;
	struct fbd_view fbd_isc;
};

struct isi_dma_desc {
	struct list_head list;
	union fbd *p_fbd;
	dma_addr_t fbd_phys;
};

/* Frame buffer data */
struct frame_buffer {
	struct vb2_buffer vb;
	struct isi_dma_desc *p_dma_desc;
	struct list_head list;
};

struct atmel_isi {
	/* Protects the access of variables shared with the ISR */
	spinlock_t			lock;
	void __iomem			*regs;

	int				sequence;

	struct vb2_alloc_ctx		*alloc_ctx;

	/* Allocate descriptors for dma buffer use */
	union fbd			*p_fb_descriptors;
	dma_addr_t			fb_descriptors_phys;
	struct				list_head dma_desc_head;
	struct isi_dma_desc		dma_desc[MAX_BUFFER_NUM];
	bool				enable_preview_path;

	struct completion		complete;
	/* ISI peripherial clock */
	struct clk			*pclk;
	/* ISC clock */
	struct clk			*iscck;

	unsigned int			irq;

	struct isi_platform_data	pdata;
	u16				width_flags;	/* max 12 bits */
	u32				bus_param;

	struct list_head		video_buffer_list;
	struct frame_buffer		*active;

	struct soc_camera_host		soc_host;
	struct at91_camera_hw_ops	*hw_ops;
	struct at91_camera_caps		*caps;
};

static void isi_writel(struct atmel_isi *isi, u32 reg, u32 val)
{
	writel(val, isi->regs + reg);
}
static u32 isi_readl(struct atmel_isi *isi, u32 reg)
{
	return readl(isi->regs + reg);
}

struct at91_camera_hw_ops {
	void (*start_dma)(struct atmel_isi *isi, struct frame_buffer *buffer,
			  bool enable_irq);
	void (*hw_initialize)(struct atmel_isi *isi);
	void (*hw_uninitialize)(struct atmel_isi *isi);
	void (*hw_configure)(struct atmel_isi *isi, u32 width, u32 height,
			     const struct soc_camera_format_xlate *xlate);
	irqreturn_t (*interrupt)(int irq, void *dev_id);
	void (*init_dma_desc)(union fbd *p_fdb, u32 fb_addr,
			      u32 next_fbd_addr);
	void (*hw_enable_interrupt)(struct atmel_isi *isi, int type);
	void (*hw_set_clock)(struct atmel_isi *isi, bool enable_clk);
	bool (*host_fmt_supported)(const u32 pixformat);
};

struct at91_camera_caps {
	struct at91_camera_hw_ops hw_ops;
	struct soc_mbus_pixelfmt yuv_support_formats[];
};

static u32 setup_cfg2_yuv_swap(struct atmel_isi *isi,
		const struct soc_camera_format_xlate *xlate)
{
	if (xlate->host_fmt->fourcc == V4L2_PIX_FMT_YUYV) {
		/* all convert to YUYV */
		switch (xlate->code) {
		case MEDIA_BUS_FMT_VYUY8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_3;
		case MEDIA_BUS_FMT_UYVY8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_2;
		case MEDIA_BUS_FMT_YVYU8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_1;
		}
	} else if (xlate->host_fmt->fourcc == V4L2_PIX_FMT_RGB565) {
		/*
		 * Preview path is enabled, it will convert UYVY to RGB format.
		 * But if sensor output format is not UYVY, we need to set
		 * YCC_SWAP_MODE to convert it as UYVY.
		 */
		switch (xlate->code) {
		case MEDIA_BUS_FMT_VYUY8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_1;
		case MEDIA_BUS_FMT_YUYV8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_2;
		case MEDIA_BUS_FMT_YVYU8_2X8:
			return ISI_CFG2_YCC_SWAP_MODE_3;
		}
	}

	/*
	 * By default, no swap for the codec path of Atmel ISI. So codec
	 * output is same as sensor's output.
	 * For instance, if sensor's output is YUYV, then codec outputs YUYV.
	 * And if sensor's output is UYVY, then codec outputs UYVY.
	 */
	return ISI_CFG2_YCC_SWAP_DEFAULT;
}

static void configure_geometry(struct atmel_isi *isi, u32 width,
		u32 height, const struct soc_camera_format_xlate *xlate)
{
	u32 cfg2, psize;
	u32 fourcc = xlate->host_fmt->fourcc;

	isi->enable_preview_path = (fourcc == V4L2_PIX_FMT_RGB565 ||
				    fourcc == V4L2_PIX_FMT_RGB32);

	/* According to sensor's output format to set cfg2 */
	switch (xlate->code) {
	default:
	/* Grey */
	case MEDIA_BUS_FMT_Y8_1X8:
		cfg2 = ISI_CFG2_GRAYSCALE | ISI_CFG2_COL_SPACE_YCbCr;
		break;
	/* YUV */
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
		cfg2 = ISI_CFG2_COL_SPACE_YCbCr |
				setup_cfg2_yuv_swap(isi, xlate);
		break;
	/* RGB, TODO */
	}

	isi_writel(isi, ISI_CTRL, ISI_CTRL_DIS);
	/* Set width */
	cfg2 |= ((width - 1) << ISI_CFG2_IM_HSIZE_OFFSET) &
			ISI_CFG2_IM_HSIZE_MASK;
	/* Set height */
	cfg2 |= ((height - 1) << ISI_CFG2_IM_VSIZE_OFFSET)
			& ISI_CFG2_IM_VSIZE_MASK;
	isi_writel(isi, ISI_CFG2, cfg2);

	/* No down sampling, preview size equal to sensor output size */
	psize = ((width - 1) << ISI_PSIZE_PREV_HSIZE_OFFSET) &
		ISI_PSIZE_PREV_HSIZE_MASK;
	psize |= ((height - 1) << ISI_PSIZE_PREV_VSIZE_OFFSET) &
		ISI_PSIZE_PREV_VSIZE_MASK;
	isi_writel(isi, ISI_PSIZE, psize);
	isi_writel(isi, ISI_PDECF, ISI_PDECF_NO_SAMPLING);

	return;
}

static bool isi_fmt_supported(const u32 pixformat)
{
	switch (pixformat) {
	/* YUV, including grey */
	case V4L2_PIX_FMT_GREY:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_VYUY:
	/* RGB */
	case V4L2_PIX_FMT_RGB565:
		return true;
	default:
		return false;
	}
}

static irqreturn_t atmel_isi_handle_streaming(struct atmel_isi *isi)
{
	if (isi->active) {
		struct vb2_buffer *vb = &isi->active->vb;
		struct frame_buffer *buf = isi->active;

		list_del_init(&buf->list);
		v4l2_get_timestamp(&vb->v4l2_buf.timestamp);
		vb->v4l2_buf.sequence = isi->sequence++;
		vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
	}

	if (list_empty(&isi->video_buffer_list)) {
		isi->active = NULL;
	} else {
		/* start next dma frame. */
		isi->active = list_entry(isi->video_buffer_list.next,
					struct frame_buffer, list);

		(*isi->hw_ops->start_dma)(isi, isi->active, false);
	}
	return IRQ_HANDLED;
}

/* ISI interrupt service routine */
static irqreturn_t isi_interrupt(int irq, void *dev_id)
{
	struct atmel_isi *isi = dev_id;
	u32 status, mask, pending;
	irqreturn_t ret = IRQ_NONE;

	spin_lock(&isi->lock);

	status = isi_readl(isi, ISI_STATUS);
	mask = isi_readl(isi, ISI_INTMASK);
	pending = status & mask;

	if (pending & ISI_CTRL_SRST) {
		complete(&isi->complete);
		isi_writel(isi, ISI_INTDIS, ISI_CTRL_SRST);
		ret = IRQ_HANDLED;
	} else if (pending & ISI_CTRL_DIS) {
		complete(&isi->complete);
		isi_writel(isi, ISI_INTDIS, ISI_CTRL_DIS);
		ret = IRQ_HANDLED;
	} else {
		if (likely(pending & ISI_SR_CXFR_DONE) ||
				likely(pending & ISI_SR_PXFR_DONE))
			ret = atmel_isi_handle_streaming(isi);
	}

	spin_unlock(&isi->lock);
	
	return ret;
}

#define	WAIT_HW_RESET		1
#define	WAIT_HW_DISABLE		0
static void isi_hw_enable_interrupt(struct atmel_isi *isi, int type)
{
	if (type == WAIT_HW_RESET) {
		isi_writel(isi, ISI_INTEN, ISI_CTRL_SRST);
		isi_writel(isi, ISI_CTRL, ISI_CTRL_SRST);
	} else {
		isi_writel(isi, ISI_INTEN, ISI_CTRL_DIS);
		isi_writel(isi, ISI_CTRL, ISI_CTRL_DIS);
	}
}

static int atmel_isi_wait_status(struct atmel_isi *isi, int wait_reset)
{
	unsigned long timeout;
	/*
	 * The reset or disable will only succeed if we have a
	 * pixel clock from the camera.
	 */
	init_completion(&isi->complete);

	(*isi->hw_ops->hw_enable_interrupt)(isi, wait_reset);

	timeout = wait_for_completion_timeout(&isi->complete,
			msecs_to_jiffies(500));
	if (timeout == 0)
		return -ETIMEDOUT;

	return 0;
}

/* ------------------------------------------------------------------
	Videobuf operations
   ------------------------------------------------------------------*/
static int queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
				unsigned int *nbuffers, unsigned int *nplanes,
				unsigned int sizes[], void *alloc_ctxs[])
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	unsigned long size;

	size = icd->sizeimage;

	if (!*nbuffers || *nbuffers > MAX_BUFFER_NUM)
		*nbuffers = MAX_BUFFER_NUM;

	if (size * *nbuffers > VID_LIMIT_BYTES)
		*nbuffers = VID_LIMIT_BYTES / size;

	*nplanes = 1;
	sizes[0] = size;
	alloc_ctxs[0] = isi->alloc_ctx;

	isi->sequence = 0;
	isi->active = NULL;

	dev_dbg(icd->parent, "%s, count=%d, size=%ld\n", __func__,
		*nbuffers, size);

	return 0;
}

static int buffer_init(struct vb2_buffer *vb)
{
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);

	buf->p_dma_desc = NULL;
	INIT_LIST_HEAD(&buf->list);

	return 0;
}

static void isi_hw_init_dma_desc(union fbd *p_fdb, u32 fb_addr, u32 next_fbd_addr)
{
	struct fbd_isi_v2 *p = &(p_fdb->fbd_isi);
	p->fb_address = fb_addr;
	p->next_fbd_address = next_fbd_addr;
	p->dma_ctrl = ISI_DMA_CTRL_WB;
}

static int buffer_prepare(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	unsigned long size;
	struct isi_dma_desc *desc;
	u32 vb_addr;

	size = icd->sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(icd->parent, "%s data will not fit into plane (%lu < %lu)\n",
				__func__, vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(&buf->vb, 0, size);

	if (!buf->p_dma_desc) {
		if (list_empty(&isi->dma_desc_head)) {
			dev_err(icd->parent, "Not enough dma descriptors.\n");
			return -EINVAL;
		} else {
			/* Get an available descriptor */
			desc = list_entry(isi->dma_desc_head.next,
						struct isi_dma_desc, list);
			/* Delete the descriptor since now it is used */
			list_del_init(&desc->list);

			/* Initialize the dma descriptor */
			vb_addr = vb2_dma_nc_plane_dma_addr(vb, 0);
			(*isi->hw_ops->init_dma_desc)(desc->p_fbd, vb_addr, 0);

			buf->p_dma_desc = desc;
		}
	}
	return 0;
}

static void buffer_cleanup(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);

	/* This descriptor is available now and we add to head list */
	if (buf->p_dma_desc)
		list_add(&buf->p_dma_desc->list, &isi->dma_desc_head);
}

static void start_dma(struct atmel_isi *isi, struct frame_buffer *buffer,
		      bool enable_irq)
{
	u32 ctrl;

	if (enable_irq)
		/* Enable irq: cxfr for the codec path, pxfr for the preview path */
		isi_writel(isi, ISI_INTEN, ISI_SR_CXFR_DONE | ISI_SR_PXFR_DONE);

	/* Check if already in a frame */
	if (!isi->enable_preview_path) {
		isi_writel(isi, ISI_DMA_C_DSCR,
				(u32)buffer->p_dma_desc->fbd_phys);
		isi_writel(isi, ISI_DMA_C_CTRL,
				ISI_DMA_CTRL_FETCH | ISI_DMA_CTRL_DONE);
		isi_writel(isi, ISI_DMA_CHER, ISI_DMA_CHSR_C_CH);
	} else {
		isi_writel(isi, ISI_DMA_P_DSCR,
				(u32)buffer->p_dma_desc->fbd_phys);
		isi_writel(isi, ISI_DMA_P_CTRL,
				ISI_DMA_CTRL_FETCH | ISI_DMA_CTRL_DONE);
		isi_writel(isi, ISI_DMA_CHER, ISI_DMA_CHSR_P_CH);
	}

	/* Enable ISI */
	ctrl = ISI_CTRL_EN;

	if (!isi->enable_preview_path)
		ctrl |= ISI_CTRL_CDC;

	isi_writel(isi, ISI_CTRL, ctrl);
}

static void buffer_queue(struct vb2_buffer *vb)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vb->vb2_queue);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct frame_buffer *buf = container_of(vb, struct frame_buffer, vb);
	unsigned long flags = 0;

	spin_lock_irqsave(&isi->lock, flags);
	list_add_tail(&buf->list, &isi->video_buffer_list);

	if (isi->active == NULL) {
		isi->active = buf;
		if (vb2_is_streaming(vb->vb2_queue))
			(*isi->hw_ops->start_dma)(isi, buf, true);
	}
	spin_unlock_irqrestore(&isi->lock, flags);
}

static void isi_hw_initialize(struct atmel_isi *isi)
{
	u32 common_flags = isi->bus_param;
	u32 cfg1 = 0;

	/* Disable all interrupts */
	isi_writel(isi, ISI_INTDIS, (u32)~0UL);

	/* Clear any pending interrupt */
	isi_readl(isi, ISI_STATUS);

	/* set bus param for ISI */
	if (common_flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		cfg1 |= ISI_CFG1_HSYNC_POL_ACTIVE_LOW;
	if (common_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		cfg1 |= ISI_CFG1_VSYNC_POL_ACTIVE_LOW;
	if (common_flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		cfg1 |= ISI_CFG1_PIXCLK_POL_ACTIVE_FALLING;

	if (isi->pdata.has_emb_sync)
		cfg1 |= ISI_CFG1_EMB_SYNC;
	if (isi->pdata.full_mode)
		cfg1 |= ISI_CFG1_FULL_MODE;

	cfg1 |= ISI_CFG1_THMASK_BEATS_16;

	cfg1 |= isi->pdata.frate & ISI_CFG1_FRATE_DIV_MASK;

	cfg1 |= ISI_CFG1_DISCR;

	isi_writel(isi, ISI_CTRL, ISI_CTRL_DIS);
	isi_writel(isi, ISI_CFG1, cfg1);
}

static void isi_hw_uninitialize(struct atmel_isi *isi)
{
	unsigned long timeout;

	if (!isi->enable_preview_path) {
		timeout = jiffies + FRAME_INTERVAL_MILLI_SEC * HZ;
		/* Wait until the end of the current frame. */
		while ((isi_readl(isi, ISI_STATUS) & ISI_CTRL_CDC) &&
				time_before(jiffies, timeout))
			msleep(1);

		if (time_after(jiffies, timeout))
			dev_err(isi->soc_host.v4l2_dev.dev,
				"Timeout waiting for finishing codec request\n");
	}

	/* Disable interrupts */
	isi_writel(isi, ISI_INTDIS,
			ISI_SR_CXFR_DONE | ISI_SR_PXFR_DONE);
}

static int start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	int ret;

	pm_runtime_get_sync(ici->v4l2_dev.dev);

	/* Reset ISI */
	ret = atmel_isi_wait_status(isi, WAIT_HW_RESET);
	if (ret < 0) {
		dev_err(icd->parent, "Reset ISI timed out\n");
		pm_runtime_put(ici->v4l2_dev.dev);
		return ret;
	}

	(*isi->hw_ops->hw_initialize)(isi);

	(*isi->hw_ops->hw_configure)(isi, icd->user_width, icd->user_height,
				icd->current_fmt);

	spin_lock_irq(&isi->lock);

	if (count)
		(*isi->hw_ops->start_dma)(isi, isi->active, true);

	spin_unlock_irq(&isi->lock);

	return 0;
}

/* abort streaming and wait for last buffer */
static void stop_streaming(struct vb2_queue *vq)
{
	struct soc_camera_device *icd = soc_camera_from_vb2q(vq);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct frame_buffer *buf, *node;
	int ret = 0;
	struct v4l2_ctrl *ctrl;
	int32_t val;
	
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct v4l2_ctrl_handler *ctrl_handler = sd->ctrl_handler;

	spin_lock_irq(&isi->lock);
	isi->active = NULL;
	/* Release all active buffers */
	list_for_each_entry_safe(buf, node, &isi->video_buffer_list, list) {
		list_del_init(&buf->list);
		if (buf != isi->active)
			vb2_buffer_done(&buf->vb, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irq(&isi->lock);

	(*isi->hw_ops->hw_uninitialize)(isi);

	/* Disable ISI and (if we aren't in triggered mode) wait for it is done */
	if (ctrl_handler==NULL) 
	{
		// we are in continuos mode: wait.
		ret = atmel_isi_wait_status(isi, WAIT_HW_DISABLE);
		if (ret < 0)
			dev_err(icd->parent, "Disable ISI timed out\n");
	}
	else
	{
		ctrl = v4l2_ctrl_find(ctrl_handler, 0x00982900);
	
		if (ctrl==NULL)
		{
			// we are in continuos mode: wait.
			ret = atmel_isi_wait_status(isi, WAIT_HW_DISABLE);
			if (ret < 0)
				dev_err(icd->parent, "Disable ISI timed out\n");
		}	
		else
		{
			val = (int32_t) v4l2_ctrl_g_ctrl(ctrl);

			if (val==0)
			{
				// we are in continuos mode: wait.
				ret = atmel_isi_wait_status(isi, WAIT_HW_DISABLE);
				if (ret < 0)
					dev_err(icd->parent, "Disable ISI timed out\n");
			}
			else
			{
				// we are in the triggered mode: don't wait!
				// dev_err(icd->parent, "Triggered mode: don't wait!\n");
				;
			}
		}
	}
	
	pm_runtime_put(ici->v4l2_dev.dev);
}

static struct vb2_ops isi_video_qops = {
	.queue_setup		= queue_setup,
	.buf_init		= buffer_init,
	.buf_prepare		= buffer_prepare,
	.buf_cleanup		= buffer_cleanup,
	.buf_queue		= buffer_queue,
	.start_streaming	= start_streaming,
	.stop_streaming		= stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

/* ------------------------------------------------------------------
	ISC hardware operations
   ------------------------------------------------------------------*/
static void isc_hw_enable_interrupt(struct atmel_isi *isc, int type)
{
	if (type == WAIT_HW_RESET) {
		isi_writel(isc, ISC_INTEN, ISC_INT_SWRST_COMPLETE);
		isi_writel(isc, ISC_CTRLDIS, ISC_CTRLDIS_SWRST);
	} else {
		isi_writel(isc, ISC_INTEN, ISC_INT_DISABLE_COMPLETE);
		isi_writel(isc, ISC_CTRLDIS, ISC_CTRLDIS_CAPTURE);
	}
}

static void isc_hw_init_dma_desc(union fbd *p_fbd, u32 fb_addr, u32 next_fbd_addr)
{
	struct fbd_view *p = &(p_fbd->fbd_isc);
	p->fb_address = fb_addr;
	p->next_fbd_address = 0;
	p->fb_stride = 0;
	p->dma_ctrl = ISC_DCTRL_DESC_ENABLE | ISC_DCTRL_DVIEW_PACKED;
}

static void isc_start_dma(struct atmel_isi *isc, struct frame_buffer *buffer,
		bool enable_irq)
{
	if (enable_irq)
		isi_writel(isc, ISC_INTEN, ISC_INT_DMA_DONE);

	isi_writel(isc, ISC_DNDA, (u32)buffer->p_dma_desc->fbd_phys);
	isi_writel(isc, ISC_DCTRL, ISC_DCTRL_DESC_ENABLE | ISC_DCTRL_DVIEW_PACKED |
				ISC_DCTRL_DMA_DONE_INT_ENABLE | ISC_DCTRL_WRITE_BACK_ENABLE);
	isi_writel(isc, ISC_DAD0, buffer->p_dma_desc->p_fbd->fbd_isc.fb_address);

	isi_writel(isc, ISC_CTRLEN, ISC_CTRLEN_CAPTURE);
}

static void isc_hw_initialize(struct atmel_isi *isc)
{
	u32 pfe_cfg0 = 0;

	if (isc->bus_param & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		pfe_cfg0 |= ISC_PFE_HSYNC_ACTIVE_LOW;
	if (isc->bus_param & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		pfe_cfg0 |= ISC_PFE_VSYNC_ACTIVE_LOW;
	if (isc->bus_param & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		pfe_cfg0 |= ISC_PFE_PIX_CLK_FALLING_EDGE;

	pfe_cfg0 |= ISC_PFE_MODE_PROGRESSIVE | ISC_PFE_CONT_VIDEO;

	/* TODO: need to revisit. */
	pfe_cfg0 |= ISC_PFE_BPS_8_BIT;

	isi_writel(isc, ISC_PFE_CFG0, pfe_cfg0);
}

static void isc_hw_uninitialize(struct atmel_isi *isc)
{
	unsigned long timeout;

	timeout = jiffies + FRAME_INTERVAL_MILLI_SEC * HZ;
	/* Wait until the end of the current frame. */
	while ((isi_readl(isc, ISC_CTRLSR) & ISC_CTRLSR_CAPTURE) && time_before(jiffies, timeout))
		msleep(1);

	if (time_after(jiffies, timeout))
		dev_err(isc->soc_host.v4l2_dev.dev,
			"Timeout waiting for finishing codec request\n");

	/* Disable interrupts */
	isi_writel(isc, ISC_INTDIS, ISC_INT_DMA_DONE);
}

static void isc_hw_set_clock(struct atmel_isi *isc, bool enable_clk)
{
	if (enable_clk)
		/* as the clock (ISC_MCK) is provided by peripheral clock, so just resume pm */
		pm_runtime_get_sync(isc->soc_host.v4l2_dev.dev);
	else
		/* as the clock (ISC_MCK) is provided by peripheral clock, so just suspend pm */
		pm_runtime_put(isc->soc_host.v4l2_dev.dev);
}

static void isc_configure_geometry(struct atmel_isi *isc, u32 width,
		u32 height, const struct soc_camera_format_xlate *xlate)
{
	/* According to sensor's output format to set cfg2 */
	switch (xlate->code) {
	/* YUV, including grey */
	case MEDIA_BUS_FMT_Y8_1X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	default:
		isi_writel(isc, ISC_CFA_CTRL, 0);
		isi_writel(isc, ISC_GAM_CTRL, 0);
		isi_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_DAT8);
		isi_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED8);
		break;
	/* Bayer RGB */
	case MEDIA_BUS_FMT_SBGGR8_1X8:
		if (xlate->host_fmt->fourcc == V4L2_PIX_FMT_RGB565) {
			isi_writel(isc, ISC_CFA_CTRL, 1);
			isi_writel(isc, ISC_CFA_CFG, 3 | 1 << 4);
			isi_writel(isc, ISC_GAM_CTRL, ISC_GAM_CTRL_ENABLE | ISC_GAM_CTRL_ENABLE_ALL_CHAN);
			isi_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_RGB565);
			isi_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED16);
		} else {
			/* output to Bayer RGB */
			isi_writel(isc, ISC_CFA_CTRL, 0);
			isi_writel(isc, ISC_GAM_CTRL, 0);
			isi_writel(isc, ISC_RLP_CFG, ISC_RLP_CFG_MODE_DAT8);
			isi_writel(isc, ISC_DCFG, ISC_DCFG_IMODE_PACKED8);
		}
		break;
	}
}

static irqreturn_t isc_interrupt(int irq, void *dev_id)
{
	struct atmel_isi *isc = dev_id;
	u32 status, mask, pending;
	irqreturn_t ret = IRQ_NONE;

	spin_lock(&isc->lock);

	status = isi_readl(isc, ISC_INTSR);
	mask = isi_readl(isc, ISC_INTMASK);
	pending = status & mask;
	
	if (pending & ISC_INT_SWRST_COMPLETE) {
		complete(&isc->complete);
		isi_writel(isc, ISC_INTEN, ISC_INT_SWRST_COMPLETE);
		ret = IRQ_HANDLED;
	} else if (pending & ISC_INT_DISABLE_COMPLETE) {
		complete(&isc->complete);
		isi_writel(isc, ISC_INTEN, ISC_INT_DISABLE_COMPLETE);
		ret = IRQ_HANDLED;
	} else if (likely(pending & ISC_INT_DMA_DONE)) {
		ret = atmel_isi_handle_streaming(isc);
	}

	spin_unlock(&isc->lock);
	
	return ret;
}

static void isc_enable_clock(struct atmel_isi *isc)
{
	u32 cfg;

	pm_runtime_get_sync(isc->soc_host.v4l2_dev.dev);

	/*Config the MCK div and select it to isc_clk(hclock) */
	cfg = ISC_CLKCFG_MCDIV(6) & ISC_CLKCFG_MCDIV_MASK;
	cfg |= ISC_CLKCFG_MASTER_SEL_HCLOCK;

	isi_writel(isc, ISC_CLKCFG, cfg);
	while ((isi_readl(isc, ISC_CLKSR) & ISC_CLK_SIP) == ISC_CLK_SIP);
		isi_writel(isc, ISC_CLKEN, ISC_CLK_MASTER);

	/* keep original clock config */
	// AP+SC increase pixel clock sampling from 1 fifth to full frequency
	// This is required to support halogen2 pixel clock that is higher than halogen 1
	// Atmel ......., ma ti sembra che una roba cosi' la devi annegare dentro il codice e non esporre a DTB?
	cfg |= ISC_CLKCFG_ICDIV(1) & ISC_CLKCFG_ICDIV_MASK;
	cfg |= ISC_CLKCFG_ISP_SEL_HCLOCK;

	isi_writel(isc, ISC_CLKCFG, cfg);
	while ((isi_readl(isc, ISC_CLKSR) & ISC_CLK_SIP) == ISC_CLK_SIP);
	/* Enable isp clock */
	isi_writel(isc, ISC_CLKEN, ISC_CLK_ISP);

	pm_runtime_put(isc->soc_host.v4l2_dev.dev);
}

static bool isc_fmt_supported(const u32 pixformat)
{
	switch (pixformat) {
	/* YUV, including grey */
	case V4L2_PIX_FMT_GREY:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_VYUY:
	/* Bayer RGB */
	case V4L2_PIX_FMT_SBGGR8:
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------
	SOC camera operations for the device
   ------------------------------------------------------------------*/
static int isi_camera_init_videobuf(struct vb2_queue *q,
				     struct soc_camera_device *icd)
{
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP;
	q->drv_priv = icd;
	q->buf_struct_size = sizeof(struct frame_buffer);
	q->ops = &isi_video_qops;
	q->mem_ops = &vb2_dma_nc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &ici->host_lock;

	return vb2_queue_init(q);
}

static int try_or_set_fmt(struct soc_camera_device *icd,
		   struct v4l2_format *f,
		   struct v4l2_subdev_format *format)
{
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct soc_camera_format_xlate *xlate;
	struct v4l2_subdev_pad_config pad_cfg;

	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct v4l2_mbus_framefmt *mf = &format->format;
	int ret;

	/* check with atmel-isi support format, if not support use YUYV */
	if (!(*isi->hw_ops->host_fmt_supported)(pix->pixelformat))
		pix->pixelformat = V4L2_PIX_FMT_YUYV;

	xlate = soc_camera_xlate_by_fourcc(icd, pix->pixelformat);
	if (!xlate) {
		dev_warn(icd->parent, "Format %x not found\n",
			 pix->pixelformat);
		return -EINVAL;
	}

	/* limit to Atmel ISI hardware capabilities */
	if (pix->height > MAX_SUPPORT_HEIGHT)
		pix->height = MAX_SUPPORT_HEIGHT;
	if (pix->width > MAX_SUPPORT_WIDTH)
		pix->width = MAX_SUPPORT_WIDTH;

	mf->width	= pix->width;
	mf->height	= pix->height;
	mf->field	= pix->field;
	mf->colorspace	= pix->colorspace;
	mf->code	= xlate->code;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, format);
	else
		ret = v4l2_subdev_call(sd, pad, set_fmt, &pad_cfg, format);

	if (ret < 0)
		return ret;

	if (mf->code != xlate->code)
		return -EINVAL;

	pix->width		= mf->width;
	pix->height		= mf->height;
	pix->field		= mf->field;
	pix->colorspace		= mf->colorspace;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		icd->current_fmt = xlate;

	switch (mf->field) {
	case V4L2_FIELD_ANY:
	case V4L2_FIELD_NONE:
		pix->field = V4L2_FIELD_NONE;
		break;
	default:
		dev_err(icd->parent, "Field type %d unsupported.\n",
			mf->field);
		ret = -EINVAL;
	}

	return ret;
}

static int isi_camera_set_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	return try_or_set_fmt(icd, f, &format);
}

static int isi_camera_try_fmt(struct soc_camera_device *icd,
			      struct v4l2_format *f)
{
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};

	return try_or_set_fmt(icd, f, &format);
}

/* This will be corrected as we get more formats */
static bool isi_camera_packing_supported(const struct soc_mbus_pixelfmt *fmt)
{
	return	fmt->packing == SOC_MBUS_PACKING_NONE ||
		(fmt->bits_per_sample == 8 &&
		 fmt->packing == SOC_MBUS_PACKING_2X8_PADHI) ||
		(fmt->bits_per_sample > 8 &&
		 fmt->packing == SOC_MBUS_PACKING_EXTEND16);
}

#define ISI_BUS_PARAM (V4L2_MBUS_MASTER |	\
		V4L2_MBUS_HSYNC_ACTIVE_HIGH |	\
		V4L2_MBUS_HSYNC_ACTIVE_LOW |	\
		V4L2_MBUS_VSYNC_ACTIVE_HIGH |	\
		V4L2_MBUS_VSYNC_ACTIVE_LOW |	\
		V4L2_MBUS_PCLK_SAMPLE_RISING |	\
		V4L2_MBUS_PCLK_SAMPLE_FALLING |	\
		V4L2_MBUS_DATA_ACTIVE_HIGH)

static int isi_camera_try_bus_param(struct soc_camera_device *icd,
				    unsigned char buswidth)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct v4l2_mbus_config cfg = {.type = V4L2_MBUS_PARALLEL,};
	unsigned long common_flags;
	int ret;

	ret = v4l2_subdev_call(sd, video, g_mbus_config, &cfg);
	if (!ret) {
		common_flags = soc_mbus_config_compatible(&cfg,
							  ISI_BUS_PARAM);
		if (!common_flags) {
			dev_warn(icd->parent,
				 "Flags incompatible: camera 0x%x, host 0x%x\n",
				 cfg.flags, ISI_BUS_PARAM);
			return -EINVAL;
		}
	} else if (ret != -ENOIOCTLCMD) {
		return ret;
	}

	if ((1 << (buswidth - 1)) & isi->width_flags)
		return 0;
	return -EINVAL;
}


static int isi_camera_get_formats(struct soc_camera_device *icd,
				  unsigned int idx,
				  struct soc_camera_format_xlate *xlate)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	int formats = 0, ret, i, n;
	/* sensor format */
	struct v4l2_subdev_mbus_code_enum code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.index = idx,
	};
	/* soc camera host format */
	const struct soc_mbus_pixelfmt *fmt;

	ret = v4l2_subdev_call(sd, pad, enum_mbus_code, NULL, &code);
	if (ret < 0)
		/* No more formats */
		return 0;

	fmt = soc_mbus_get_fmtdesc(code.code);
	if (!fmt) {
		dev_err(icd->parent,
			"Invalid format code #%u: %d\n", idx, code.code);
		return 0;
	}

	/* This also checks support for the requested bits-per-sample */
	ret = isi_camera_try_bus_param(icd, fmt->bits_per_sample);
	if (ret < 0) {
		dev_err(icd->parent,
			"Fail to try the bus parameters.\n");
		return 0;
	}

	switch (code.code) {
	case MEDIA_BUS_FMT_UYVY8_2X8:
	case MEDIA_BUS_FMT_VYUY8_2X8:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YVYU8_2X8:
		for (n = 0; isi->caps->yuv_support_formats[n].name != NULL; n++)
			/* Empty! */;

		formats += n;
		for (i = 0; xlate && i < n; i++, xlate++) {
			xlate->host_fmt	= isi->caps->yuv_support_formats + i;
			xlate->code	= code.code;
			dev_dbg(icd->parent, "Providing format %s using code %d\n",
				xlate->host_fmt->name, xlate->code);
		}
		break;
	default:
		if (!isi_camera_packing_supported(fmt))
			return 0;
		if (xlate)
			dev_dbg(icd->parent,
				"Providing format %s in pass-through mode\n",
				fmt->name);
	}

	/* Generic pass-through */
	formats++;
	if (xlate) {
		xlate->host_fmt	= fmt;
		xlate->code	= code.code;
		xlate++;
	}

	return formats;
}

static int isi_camera_add_device(struct soc_camera_device *icd)
{
	dev_dbg(icd->parent, "Atmel ISI Camera driver attached to camera %d\n",
		 icd->devnum);

	return 0;
}

static void isi_camera_remove_device(struct soc_camera_device *icd)
{
	dev_dbg(icd->parent, "Atmel ISI Camera driver detached from camera %d\n",
		 icd->devnum);
}

static unsigned int isi_camera_poll(struct file *file, poll_table *pt)
{
	struct soc_camera_device *icd = file->private_data;

	return vb2_poll(&icd->vb2_vidq, file, pt);
}

static int isi_camera_querycap(struct soc_camera_host *ici,
			       struct v4l2_capability *cap)
{
	strcpy(cap->driver, "atmel-isi");
	strcpy(cap->card, "Atmel Image Sensor Interface");
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int isi_camera_set_bus_param(struct soc_camera_device *icd)
{
	struct v4l2_subdev *sd = soc_camera_to_subdev(icd);
	struct soc_camera_host *ici = to_soc_camera_host(icd->parent);
	struct atmel_isi *isi = ici->priv;
	struct v4l2_mbus_config cfg = {.type = V4L2_MBUS_PARALLEL,};
	unsigned long common_flags;
	int ret;

	ret = v4l2_subdev_call(sd, video, g_mbus_config, &cfg);
	if (!ret) {
		common_flags = soc_mbus_config_compatible(&cfg,
							  ISI_BUS_PARAM);
		if (!common_flags) {
			dev_warn(icd->parent,
				 "Flags incompatible: camera 0x%x, host 0x%x\n",
				 cfg.flags, ISI_BUS_PARAM);
			return -EINVAL;
		}
	} else if (ret != -ENOIOCTLCMD) {
		return ret;
	} else {
		common_flags = ISI_BUS_PARAM;
	}
	dev_dbg(icd->parent, "Flags cam: 0x%x host: 0x%x common: 0x%lx\n",
		cfg.flags, ISI_BUS_PARAM, common_flags);

	/* Make choises, based on platform preferences */
	if ((common_flags & V4L2_MBUS_HSYNC_ACTIVE_HIGH) &&
	    (common_flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)) {
		if (isi->pdata.hsync_act_low)
			common_flags &= ~V4L2_MBUS_HSYNC_ACTIVE_HIGH;
		else
			common_flags &= ~V4L2_MBUS_HSYNC_ACTIVE_LOW;
	}

	if ((common_flags & V4L2_MBUS_VSYNC_ACTIVE_HIGH) &&
	    (common_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)) {
		if (isi->pdata.vsync_act_low)
			common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_HIGH;
		else
			common_flags &= ~V4L2_MBUS_VSYNC_ACTIVE_LOW;
	}

	if ((common_flags & V4L2_MBUS_PCLK_SAMPLE_RISING) &&
	    (common_flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)) {
		if (isi->pdata.pclk_act_falling)
			common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_RISING;
		else
			common_flags &= ~V4L2_MBUS_PCLK_SAMPLE_FALLING;
	}

	cfg.flags = common_flags;
	ret = v4l2_subdev_call(sd, video, s_mbus_config, &cfg);
	if (ret < 0 && ret != -ENOIOCTLCMD) {
		dev_dbg(icd->parent, "camera s_mbus_config(0x%lx) returned %d\n",
			common_flags, ret);
		return ret;
	}

	dev_dbg(icd->parent, "vsync active %s, hsync active %s, sampling on pix clock %s edge\n",
		common_flags & V4L2_MBUS_VSYNC_ACTIVE_LOW ? "low" : "high",
		common_flags & V4L2_MBUS_HSYNC_ACTIVE_LOW ? "low" : "high",
		common_flags & V4L2_MBUS_PCLK_SAMPLE_FALLING ? "falling" : "rising");

	isi->bus_param = common_flags;

	return 0;
}

static int isi_camera_set_parm(struct soc_camera_device *icd, struct v4l2_streamparm *parm)
{
	return 0;
}

static int clock_start(struct soc_camera_host *ici)
{
	struct atmel_isi *isi = ici->priv;

	if (isi->hw_ops->hw_set_clock)
		(*isi->hw_ops->hw_set_clock)(isi, true);

	return 0;
}

static void clock_stop(struct soc_camera_host *ici)
{
	struct atmel_isi *isi = ici->priv;

	if (isi->hw_ops->hw_set_clock)
		(*isi->hw_ops->hw_set_clock)(isi, false);
}

static struct soc_camera_host_ops isi_soc_camera_host_ops = {
	.owner		= THIS_MODULE,
	.add		= isi_camera_add_device,
	.remove		= isi_camera_remove_device,
	.set_fmt	= isi_camera_set_fmt,
	.try_fmt	= isi_camera_try_fmt,
	.get_formats	= isi_camera_get_formats,
	.init_videobuf2	= isi_camera_init_videobuf,
	.poll		= isi_camera_poll,
	.querycap	= isi_camera_querycap,
	.set_bus_param	= isi_camera_set_bus_param,
	.set_parm	= isi_camera_set_parm,
	.get_parm	= isi_camera_set_parm,
	.clock_start	= clock_start,
	.clock_stop	= clock_stop,
};

/* -----------------------------------------------------------------------*/
static int atmel_isi_remove(struct platform_device *pdev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(&pdev->dev);
	struct atmel_isi *isi = container_of(soc_host,
					struct atmel_isi, soc_host);

	soc_camera_host_unregister(soc_host);
	vb2_dma_nc_cleanup_ctx(isi->alloc_ctx);
	dma_free_coherent(&pdev->dev,
			sizeof(union fbd) * MAX_BUFFER_NUM,
			isi->p_fb_descriptors,
			isi->fb_descriptors_phys);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int atmel_isi_parse_dt(struct atmel_isi *isi,
			struct platform_device *pdev)
{
	struct device_node *np= pdev->dev.of_node;
	struct v4l2_of_endpoint ep;
	int err;

	/* Default settings for ISI */
	isi->pdata.full_mode = 1;
	isi->pdata.frate = ISI_CFG1_FRATE_CAPTURE_ALL;

	np = of_graph_get_next_endpoint(np, NULL);
	if (!np) {
		dev_err(&pdev->dev, "Could not find the endpoint\n");
		return -EINVAL;
	}

	err = v4l2_of_parse_endpoint(np, &ep);
	of_node_put(np);
	if (err) {
		dev_err(&pdev->dev, "Could not parse the endpoint\n");
		return err;
	}

	switch (ep.bus.parallel.bus_width) {
	case 8:
		isi->pdata.data_width_flags = ISI_DATAWIDTH_8;
		break;
	case 10:
		isi->pdata.data_width_flags =
				ISI_DATAWIDTH_8 | ISI_DATAWIDTH_10;
		break;
	default:
		dev_err(&pdev->dev, "Unsupported bus width: %d\n",
				ep.bus.parallel.bus_width);
		return -EINVAL;
	}

	if (ep.bus.parallel.flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		isi->pdata.hsync_act_low = true;
	if (ep.bus.parallel.flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		isi->pdata.vsync_act_low = true;
	if (ep.bus.parallel.flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		isi->pdata.pclk_act_falling = true;

	if (ep.bus_type == V4L2_MBUS_BT656)
		isi->pdata.has_emb_sync = true;

	return 0;
}

static const struct of_device_id atmel_isi_of_match[];
static int atmel_isi_probe(struct platform_device *pdev)
{
	unsigned int irq;
	struct atmel_isi *isi;
	struct resource *regs;
	int ret, i;
	struct soc_camera_host *soc_host;

	isi = devm_kzalloc(&pdev->dev, sizeof(struct atmel_isi), GFP_KERNEL);
	if (!isi) {
		dev_err(&pdev->dev, "Can't allocate interface!\n");
		return -ENOMEM;
	}

	isi->pclk = devm_clk_get(&pdev->dev, "isi_clk");
	if (IS_ERR(isi->pclk))
		return PTR_ERR(isi->pclk);

	isi->iscck = devm_clk_get(&pdev->dev, "iscck");
	if (IS_ERR(isi->iscck))
		isi->iscck = NULL;

	ret = atmel_isi_parse_dt(isi, pdev);
	if (ret)
		return ret;

	isi->caps = (struct at91_camera_caps *)
		of_match_device(atmel_isi_of_match, &pdev->dev)->data;
	isi->hw_ops = &isi->caps->hw_ops;

	isi->active = NULL;
	spin_lock_init(&isi->lock);
	INIT_LIST_HEAD(&isi->video_buffer_list);
	INIT_LIST_HEAD(&isi->dma_desc_head);

	isi->p_fb_descriptors = dma_alloc_coherent(&pdev->dev,
				sizeof(union fbd) * MAX_BUFFER_NUM,
				&isi->fb_descriptors_phys,
				GFP_KERNEL);
	if (!isi->p_fb_descriptors) {
		dev_err(&pdev->dev, "Can't allocate descriptors!\n");
		return -ENOMEM;
	}

	for (i = 0; i < MAX_BUFFER_NUM; i++) {
		isi->dma_desc[i].p_fbd = isi->p_fb_descriptors + i;
		isi->dma_desc[i].fbd_phys = isi->fb_descriptors_phys +
					i * sizeof(union fbd);
		list_add(&isi->dma_desc[i].list, &isi->dma_desc_head);
	}

	isi->alloc_ctx = vb2_dma_nc_init_ctx(&pdev->dev);
	if (IS_ERR(isi->alloc_ctx)) {
		ret = PTR_ERR(isi->alloc_ctx);
		goto err_alloc_ctx;
	}

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	isi->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(isi->regs)) {
		ret = PTR_ERR(isi->regs);
		goto err_ioremap;
	}

	if (isi->pdata.data_width_flags & ISI_DATAWIDTH_8)
		isi->width_flags = 1 << 7;
	if (isi->pdata.data_width_flags & ISI_DATAWIDTH_10)
		isi->width_flags |= 1 << 9;

	irq = platform_get_irq(pdev, 0);
	if (IS_ERR_VALUE(irq)) {
		ret = irq;
		goto err_req_irq;
	}

	ret = devm_request_irq(&pdev->dev, irq, isi->hw_ops->interrupt, 0,
			       "isi", isi);
	if (ret) {
		dev_err(&pdev->dev, "Unable to request irq %d\n", irq);
		goto err_req_irq;
	}
	isi->irq = irq;

	soc_host		= &isi->soc_host;
	soc_host->drv_name	= "isi-camera";
	soc_host->ops		= &isi_soc_camera_host_ops;
	soc_host->priv		= isi;
	soc_host->v4l2_dev.dev	= &pdev->dev;
	soc_host->nr		= pdev->id;

	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	ret = soc_camera_host_register(soc_host);
	if (ret) {
		dev_err(&pdev->dev, "Unable to register soc camera host\n");
		goto err_register_soc_camera_host;
	}

	if (of_device_is_compatible(pdev->dev.of_node, "atmel,sama5d2-isc"))
		isc_enable_clock(isi);

	return 0;

err_register_soc_camera_host:
	pm_runtime_disable(&pdev->dev);
err_req_irq:
err_ioremap:
	vb2_dma_nc_cleanup_ctx(isi->alloc_ctx);
err_alloc_ctx:
	dma_free_coherent(&pdev->dev,
			sizeof(union fbd) * MAX_BUFFER_NUM,
			isi->p_fb_descriptors,
			isi->fb_descriptors_phys);

	return ret;
}

#ifdef CONFIG_PM
static int atmel_isi_runtime_suspend(struct device *dev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(dev);
	struct atmel_isi *isi = container_of(soc_host,
					struct atmel_isi, soc_host);

	if (isi->iscck)
		clk_disable_unprepare(isi->iscck);
	clk_disable_unprepare(isi->pclk);

	return 0;
}
static int atmel_isi_runtime_resume(struct device *dev)
{
	struct soc_camera_host *soc_host = to_soc_camera_host(dev);
	struct atmel_isi *isi = container_of(soc_host,
					struct atmel_isi, soc_host);

	if (isi->iscck)
		clk_prepare_enable(isi->iscck);
	return clk_prepare_enable(isi->pclk);
}
#endif /* CONFIG_PM */

static struct at91_camera_caps at91sam9g45_caps = {
	.hw_ops = {
		.hw_initialize = isi_hw_initialize,
		.hw_uninitialize = isi_hw_uninitialize,
		.hw_configure = configure_geometry,
		.start_dma = start_dma,
		.interrupt = isi_interrupt,
		.init_dma_desc = isi_hw_init_dma_desc,
		.hw_enable_interrupt = isi_hw_enable_interrupt,
		.host_fmt_supported = isi_fmt_supported,
	},

	.yuv_support_formats = {
		{
			.fourcc			= V4L2_PIX_FMT_YUYV,
			.name			= "Packed YUV422 16 bit",
			.bits_per_sample	= 8,
			.packing		= SOC_MBUS_PACKING_2X8_PADHI,
			.order			= SOC_MBUS_ORDER_LE,
			.layout			= SOC_MBUS_LAYOUT_PACKED,
		},
		{
			.fourcc			= V4L2_PIX_FMT_RGB565,
			.name			= "RGB565",
			.bits_per_sample	= 8,
			.packing		= SOC_MBUS_PACKING_2X8_PADHI,
			.order			= SOC_MBUS_ORDER_LE,
			.layout			= SOC_MBUS_LAYOUT_PACKED,
		},
		{ /* terminator */ },
	},
};

static struct at91_camera_caps sama5d2_caps = {
	.hw_ops = {
		.hw_initialize = isc_hw_initialize,
		.hw_uninitialize = isc_hw_uninitialize,
		.hw_configure = isc_configure_geometry,
		.start_dma = isc_start_dma,
		.init_dma_desc = isc_hw_init_dma_desc,
		.interrupt = isc_interrupt,
		.hw_enable_interrupt = isc_hw_enable_interrupt,
		.host_fmt_supported = isc_fmt_supported,
		.hw_set_clock = isc_hw_set_clock,
	},

	.yuv_support_formats = {
		/* use default pass through */
		{ /* terminator */ },
	},
};

static const struct dev_pm_ops atmel_isi_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(atmel_isi_runtime_suspend,
				atmel_isi_runtime_resume, NULL)
};

static const struct of_device_id atmel_isi_of_match[] = {
	{ .compatible = "atmel,at91sam9g45-isi", .data = &at91sam9g45_caps},
	{ .compatible = "atmel,sama5d2-isc", .data = &sama5d2_caps},
	{ }
};
MODULE_DEVICE_TABLE(of, atmel_isi_of_match);

static struct platform_driver atmel_isi_driver = {
	.remove		= atmel_isi_remove,
	.driver		= {
		.name = "atmel_isi",
		.of_match_table = of_match_ptr(atmel_isi_of_match),
		.pm	= &atmel_isi_dev_pm_ops,
	},
};

module_platform_driver_probe(atmel_isi_driver, atmel_isi_probe);

MODULE_AUTHOR("Josh Wu <josh.wu@atmel.com>");
MODULE_DESCRIPTION("The V4L2 driver for Atmel Linux");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("video");
