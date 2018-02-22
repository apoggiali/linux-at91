/*
 * videobuf2-dma-nc.h - DMA contiousg and not-coherent memory allocator for videobuf2
 * 
 * This allocator provides only MMAP method for V4L2 buffers.
 * Buffers are allocate contigously (no scatter-gather).
 * Buffers are mapped in user space in not-coherent mode (cacheable).
 * 
 */

#ifndef _MEDIA_VIDEOBUF2_DMA_NC_H
#define _MEDIA_VIDEOBUF2_DMA_NC_H

#include <media/videobuf2-core.h>
#include <linux/dma-mapping.h>

struct dma_attrs;

static inline dma_addr_t
vb2_dma_nc_plane_dma_addr (struct vb2_buffer *vb, unsigned int plane_no)
{
  dma_addr_t *addr = vb2_plane_cookie (vb, plane_no);

  return *addr;
}

void *vb2_dma_nc_init_ctx_attrs (struct device *dev, struct dma_attrs *attrs);

static inline void *
vb2_dma_nc_init_ctx (struct device *dev)
{
  return vb2_dma_nc_init_ctx_attrs (dev, NULL);
}

void vb2_dma_nc_cleanup_ctx (void *alloc_ctx);

int
vb2_dma_nc_set_valid_size (struct vb2_buffer *vb, unsigned int plane_no,
							unsigned long valid_size);

extern const struct vb2_mem_ops vb2_dma_nc_memops;

#endif
