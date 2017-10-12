/*
 * videobuf2-dma-nc.h - DMA contiousg and not-coherent memory allocator for videobuf2
 * 
 */

#include <linux/dma-buf.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-nc.h>
#include <media/videobuf2-memops.h>

struct vb2_dma_nc_conf
{
  struct device *dev;
  struct dma_attrs attrs;
};

struct vb2_dma_nc_buf
{
  struct device *dev;
  void *vaddr;
  unsigned long size;
  dma_addr_t dma_addr;
  struct dma_attrs attrs;
  enum dma_data_direction dma_dir;

  /* MMAP related */
  struct vb2_vmarea_handler handler;
  atomic_t refcount;
};

/*********************************************/
/*         callbacks for all buffers         */
/*********************************************/

static void *
vb2_dma_nc_cookie (void *buf_priv)
{
  struct vb2_dma_nc_buf *buf = buf_priv;
  return &buf->dma_addr;
}

static void *
vb2_dma_nc_vaddr (void *buf_priv)
{
  struct vb2_dma_nc_buf *buf = buf_priv;
  return buf->vaddr;
}

static unsigned int
vb2_dma_nc_num_users (void *buf_priv)
{
  struct vb2_dma_nc_buf *buf = buf_priv;
  return atomic_read (&buf->refcount);
}

static void
vb2_dma_nc_prepare (void *buf_priv)
{
  // ML: no cache write back here!
}

static void
vb2_dma_nc_finish (void *buf_priv)
{
  struct vb2_dma_nc_buf *buf = buf_priv;
  dma_sync_single_for_cpu (buf->dev, buf->dma_addr, buf->size, buf->dma_dir);
}

/*********************************************/
/*        callbacks for MMAP buffers         */
/*********************************************/

static void
vb2_dma_nc_put (void *buf_priv)
{
  struct vb2_dma_nc_buf *buf = buf_priv;

  if (!atomic_dec_and_test (&buf->refcount))
    return;

  free_pages (buf->vaddr, get_order(buf->size));

  put_device (buf->dev);
  kfree (buf);
}

static void *
vb2_dma_nc_alloc (void *alloc_ctx, unsigned long size,
		  enum dma_data_direction dma_dir, gfp_t gfp_flags)
{
  struct vb2_dma_nc_conf *conf = alloc_ctx;
  struct device *dev = conf->dev;
  struct vb2_dma_nc_buf *buf;

  buf = kzalloc (sizeof *buf, GFP_KERNEL);
  if (!buf)
    return ERR_PTR (-ENOMEM);

  buf->attrs = conf->attrs;

  /* allocate memory and fill the real buffer size variable */
  buf->vaddr =
    (void *) __get_free_pages(GFP_DMA32 | gfp_flags, get_order(size));
  if (buf->vaddr == NULL)
    {
      dev_err (dev, "kmalloc of size %ld failed\n", size);
      kfree (buf);
      return ERR_PTR (-ENOMEM);
    }

  /* map memory into DMA & give it to CPU */
  buf->dma_addr = dma_map_single (dev, buf->vaddr, size, dma_dir);
  if (dma_mapping_error (dev, buf->dma_addr))
    {
      dev_err (dev, "unable to map page to DMA\n");
      free_pages(buf->vaddr, get_order(size));
      kfree (buf);
      return ERR_PTR (-ENOMEM);
    }

  /* Prevent the device from being released while the buffer is used */
  buf->dev = get_device (dev);
  buf->size = size;
  buf->dma_dir = dma_dir;

  buf->handler.refcount = &buf->refcount;
  buf->handler.put = vb2_dma_nc_put;
  buf->handler.arg = buf;

  atomic_inc (&buf->refcount);

  return buf;
}

static int
vb2_dma_nc_mmap (void *buf_priv, struct vm_area_struct *vma)
{
  struct vb2_dma_nc_buf *buf = buf_priv;
  unsigned long aligned_paddress;
  int ret;

  if (!buf)
    {
      printk (KERN_ERR "No buffer to map\n");
      return -EINVAL;
    }

  /*
   * dma_mmap_* uses vm_pgoff as in-buffer offset, but we want to
   * map whole buffer
   */
  vma->vm_pgoff = 0;

  aligned_paddress = (virt_to_phys (buf->vaddr)) >> PAGE_SHIFT;
  ret =
    remap_pfn_range (vma, vma->vm_start, aligned_paddress, buf->size,
		     vma->vm_page_prot);


  if (ret)
    {
      pr_err ("Remapping memory failed, error: %d\n", ret);
      return ret;
    }

  vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
  vma->vm_private_data = &buf->handler;
  vma->vm_ops = &vb2_common_vm_ops;

  vma->vm_ops->open (vma);

  pr_debug ("%s: mapped dma addr 0x%08lx at 0x%08lx, size %ld\n",
	    __func__, (unsigned long) buf->dma_addr, vma->vm_start,
	    buf->size);

  return 0;
}

/*********************************************/
/*       DMA CONTIG exported functions       */
/*********************************************/

const struct vb2_mem_ops vb2_dma_nc_memops = {
  .alloc = vb2_dma_nc_alloc,
  .put = vb2_dma_nc_put,
  .cookie = vb2_dma_nc_cookie,
  .vaddr = vb2_dma_nc_vaddr,
  .mmap = vb2_dma_nc_mmap,
  .prepare = vb2_dma_nc_prepare,
  .finish = vb2_dma_nc_finish,
  .num_users = vb2_dma_nc_num_users,
};

EXPORT_SYMBOL_GPL (vb2_dma_nc_memops);

void *
vb2_dma_nc_init_ctx_attrs (struct device *dev, struct dma_attrs *attrs)
{
  struct vb2_dma_nc_conf *conf;

  conf = kzalloc (sizeof *conf, GFP_KERNEL);
  if (!conf)
    return ERR_PTR (-ENOMEM);

  conf->dev = dev;
  if (attrs)
    conf->attrs = *attrs;

  return conf;
}

EXPORT_SYMBOL_GPL (vb2_dma_nc_init_ctx_attrs);

void
vb2_dma_nc_cleanup_ctx (void *alloc_ctx)
{
  if (!IS_ERR_OR_NULL (alloc_ctx))
    kfree (alloc_ctx);
}

EXPORT_SYMBOL_GPL (vb2_dma_nc_cleanup_ctx);

MODULE_DESCRIPTION ("DMA-contig memory handling routines for videobuf2");
MODULE_AUTHOR ("Pawel Osciak <pawel@osciak.com>");
MODULE_LICENSE ("GPL");
