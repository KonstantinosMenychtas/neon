/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/***************************************************************************/

#include <linux/list.h>     // lists
#include <linux/spinlock.h> // lock/unlock
#include <linux/slab.h>     // kmalloc/kzalloc
#include <asm/io.h>         // ioremap
#include <nv.h>             // nvidia module
#include "neon_help.h"
#include "neon_core.h"
#include "neon_control.h"
#include "neon_sys.h"

/**************************************************************************/
// neon_chan_init
/**************************************************************************/
// initialize a new channel entry
static inline int
neon_chan_init(neon_dev_t * const dev,
               const unsigned int cid)
{
  neon_chan_t *chan = NULL;
  unsigned long ir_paddr = 0;

  chan              = &dev->chan[cid];
  ir_paddr          = dev->reg_base + cid * dev->reg_ofs + NEON_RB_PAGEOFS;
  chan->id          = cid;
  chan->pid         = 0;
  chan->ir_kvaddr   = ioremap_nocache(ir_paddr, sizeof(long));
  if(chan->ir_kvaddr == NULL)
    return -1;
  chan->refc_kvaddr = 0;
  chan->refc_target = 0;
  chan->pdt         = 0;
  spin_lock_init(&chan->lock);

  neon_debug("did %d : cid %d : ir p 0x%lx --> kv 0x%p",
            dev->id, cid, ir_paddr, chan->ir_kvaddr);

  return 0;
}

/**************************************************************************/
// neon_chan_fini
/**************************************************************************/
// clean up channel
static inline int
neon_chan_fini(neon_dev_t * const dev,
               const unsigned int cid)
{
  neon_chan_t * const chan = &dev->chan[cid];

  if(chan->ir_kvaddr != 0) {
    iounmap(chan->ir_kvaddr);
    chan->ir_kvaddr = NULL;
  }
  
  if(chan->refc_kvaddr != 0) {
    neon_warning("task %d : chan %d : refc [0x%lx, 0x%p] :"
                 " alive @ fini, pdt = %d ...",
                 chan->pid, chan->id, chan->refc_kvaddr,
                 chan->refc_target, chan->pdt);
    chan->refc_kvaddr = 0;
    return -1;
  } else
    neon_warning("chan %d : fini", chan->id);
  
  return 0;
}

/**************************************************************************/
// neon_chan_print
/**************************************************************************/
// finilize and cleanup channel entry
inline void
neon_chan_print(const neon_chan_t * const chan)
{
  // all entries here should be initialized properly
  neon_warning("task %d : chan %d : refc [0x%lx, 0x%p] , pdt = %d",
               chan->pid, chan->id, chan->refc_kvaddr,
               chan->refc_target, chan->pdt);
  
  return;
}

/**************************************************************************/
// neon_dev_init
/**************************************************************************/
// initialize a new device management struct
static int
neon_dev_init(unsigned int id,
              unsigned long long * const dev_info,
              neon_dev_t    *dev)
{
  // the preceeding type-castings have been found to be correct
  // in our traces;
  unsigned long bar0_addr    = (unsigned long) dev_info[0];
  unsigned long bar1_addr    = (unsigned long) dev_info[2];
  unsigned int  vendor_id    = (unsigned int)  dev_info[4];
  unsigned int  device_id    = (unsigned int)  dev_info[5];
  unsigned int  subsystem_id = (unsigned int)  dev_info[6];

  might_sleep();

  dev->id = id;
  spin_lock_init(&dev->lock);

  // the number of channels for every device is a feature
  // that is device dependent; we map to our system below
  if(vendor_id == NVIDIA_VENDOR &&
     device_id == GTX670_DEVICE_ID &&
     subsystem_id  == ZOTAC_SUBSYSTEM) {
    dev->nchan     = GTX670_CHANNELS;
    dev->reg_base  = bar1_addr + NEON_KEPLER_CHANNEL_BASE;
    dev->reg_ofs   = NEON_KEPLER_CHANNEL_OFFSET;
    dev->refc_eval = kepler_refc_eval;
  }
  else if(vendor_id == NVIDIA_VENDOR &&
          device_id == GTX275_DEVICE_ID &&
          subsystem_id == EVGA_SUBSYSTEM) {
    dev->nchan = GTX275_CHANNELS;
    dev->reg_base = bar0_addr + NEON_TESLA_CHANNEL_BASE;
    dev->reg_ofs  = NEON_TESLA_CHANNEL_OFFSET;
    dev->refc_eval = tesla_refc_eval;
  }
  else if(vendor_id == NVIDIA_VENDOR &&
          device_id == NVS295_DEVICE_ID &&
          subsystem_id == NVIDIA_SUBSYSTEM) {
    dev->nchan = NVS295_CHANNELS;
    dev->reg_base = bar0_addr + NEON_TESLA_CHANNEL_BASE;
    dev->reg_ofs  = NEON_TESLA_CHANNEL_OFFSET;
    dev->refc_eval = tesla_refc_eval;
  }
  else {
    neon_error("Vendor:Dev:Subsystem 0x%lx:0x%lx:0x%lx not supported",
               vendor_id, device_id, subsystem_id);
    return -1;
  }
  
  // init channel alive bmp_sub2comp
  dev->bmp_sub2comp = (long *) kzalloc(BITS_TO_LONGS(dev->nchan) *       \
                              sizeof(long), GFP_KERNEL);
  if(dev->bmp_sub2comp == NULL) {
    neon_error("%s : dev 0x%lx/0x%lx bmp_sub2comp kalloc failed",
               __func__, bar0_addr, bar1_addr);
    return -1;
  }

  // init channels array
  dev->chan = (neon_chan_t *) kzalloc(dev->nchan * sizeof(neon_chan_t),
                                      GFP_KERNEL);
  if(dev->chan == NULL) {
    neon_error("%s : dev bar0 0x%lx : bar1 0x%lx : alloc failed",
               __func__, bar0_addr, bar1_addr);
    kfree(dev->bmp_sub2comp);
    return -1;
  } else {
    unsigned int  i = 0;
    for(i = 0; i < dev->nchan; i++) {
      if(unlikely(neon_chan_init(dev, i) != 0)) {
        neon_error("%s : dev bar0 0x%lx : bar1 0x%lx : chan init failed",
                   __func__, bar0_addr, bar1_addr);
        break;
      }
    }
    if(i != dev->nchan) {
      while(i-- >= 0) 
        neon_chan_fini(dev, i);
      kfree(dev->chan);
      kfree(dev->bmp_sub2comp);
      return -1;
    }
  }

  neon_info("init dev : id %x : VDS 0x%x/0x%x/0x%x : "
            "bar0 @ 0x%lx : bar1 @ 0x%lx ",
            vendor_id, device_id, subsystem_id, bar0_addr, bar1_addr);

  return 0;
}

/**************************************************************************/
// neon_dev_fini
/**************************************************************************/
// finalize and cleanup a device management struct
static int
neon_dev_fini(neon_dev_t * const dev)
{
  unsigned int i    = 0;
  int          ret  = 0;
  neon_chan_t *chan = NULL;

  // clean up all channels of the device
  for(i = 0; i < dev->nchan; i++) {
    if(test_bit(i, dev->bmp_sub2comp) != 0)
      ret = -1;
    chan = &dev->chan[i];
    spin_lock_irq(&chan->lock);
    ret |= neon_chan_fini(dev, i);
    spin_unlock_irq(&chan->lock);
    if(ret != 0)
      neon_warning("dev %d : reg base 0x%lx : "
                   "ref ofs 0x%lx : chan %d still busy",
                   dev->id, dev->reg_base, dev->reg_ofs, chan->id);
  }
  if(ret == 0) {
    kfree(dev->bmp_sub2comp);
    kfree(dev->chan);
  } else
    neon_warning("dev %d : busy at fini", dev->id);

  return ret;
}

/**************************************************************************/
// neon_dev_print
/**************************************************************************/
// print out neon_dev struct
void
neon_dev_print(const neon_dev_t * const dev)
{
  unsigned int  i         = 0;

  neon_info("dev : id 0x%x : nchan %d : reg base 0x%lx : "
            "reg ofs 0x%lx : chan ...",
            dev->id, dev->nchan, dev->reg_base, dev->reg_ofs);

  for_each_set_bit(i, dev->bmp_sub2comp, dev->nchan) {
    unsigned long  flags = 0;
    neon_chan_t   *chan  = &dev->chan[i];
    spin_lock_irqsave(&chan->lock, flags);
    neon_chan_print(chan);
    spin_unlock_irqrestore(&chan->lock, flags);
  }

  return;
}

/**************************************************************************/
// neon_global_init
/**************************************************************************/
// initialize globals
int
neon_global_init(void)
{
  unsigned long long *dev_info = NULL;
  unsigned int        size     = 0;
  unsigned int        i        = 0;
  unsigned int        bi       = 0;
  int                 ret      = 0;

  might_sleep();

  neon_global.ndev = 0;
  atomic_set(&neon_global.ctx_ever, 0);
  atomic_set(&neon_global.ctx_live, 0);

  // ask the NVIDIA module about device info
  size = NV_MAX_DEVICES * NEON_DEV_INFO_ENTRIES * sizeof(unsigned long long);
  dev_info = (unsigned long long *) kmalloc(size, GFP_KERNEL);
  if(dev_info == NULL) {
    neon_error("%s : kalloc blob-proped info buffer failed",
               __func__);
    return -1;
  }
  neon_kern_probe(dev_info); // nvidia module export

  // count registered devices
  for(i = 0; i < NV_MAX_DEVICES; i++) {
    unsigned int vendor_id = 0;
    bi = i * NEON_DEV_INFO_ENTRIES;
    vendor_id = (unsigned int) dev_info[bi+4];
    if (vendor_id == NVIDIA_VENDOR)
      neon_global.ndev++;
  }
  if(neon_global.ndev == 0) {
    neon_error("%s : no GPUs found", __func__);
    return -1;
  }

  // initialize devices
  neon_global.dev = (neon_dev_t *) \
    kmalloc(neon_global.ndev * sizeof(neon_dev_t), GFP_KERNEL);
  for(i = 0; i < neon_global.ndev; i++) {
    bi = i * NEON_DEV_INFO_ENTRIES;
    if(neon_dev_init(i, &dev_info[bi], &neon_global.dev[i]) != 0) {
      neon_error("%s : failed to init GPU %d", __func__, i);
      break;
    }
  }
  if(i < neon_global.ndev) {
    for(i = 0; i < neon_global.ndev; i++)
      neon_dev_fini(&neon_global.dev[i]);
    kfree(neon_global.dev);
    ret = -1;
  }
  
  // done with probe buffer
  kfree(dev_info);

  return ret;
}

/**************************************************************************/
// neon_global_fini
/**************************************************************************/
// clean up NEON globals before module exit
int
neon_global_fini(void)
{
  unsigned int i   = 0;
  int          ret = 0;

  // this should not be possible given module task use count
  // but double checking never hurts
  if (atomic_read(&neon_global.ctx_live) > 0) {
    neon_error("%s : active contexts/devices exist", __func__);
    return -1;
  } else
    neon_global.ndev = 0;

  for(i = 0; i < neon_global.ndev; i++) {
    neon_dev_t *dev = &neon_global.dev[i];
    ret |= neon_dev_fini(dev);
    if(ret != 0)
      neon_error("%s : prob removing GPU dev %d", __func__, i);
  }

  if(ret == 0)
    kfree(neon_global.dev);
  else
    return -1;

  return 0;
}

/**************************************************************************/
// neon_global_print
/**************************************************************************/
// print out neon_global struct
void
neon_global_print(void)
{
  unsigned int  i   = 0;
  neon_dev_t   *dev = NULL;

  neon_info("global : ctx_ever %u : ctx_live %u : dev ...",
            atomic_read(&neon_global.ctx_ever),
            atomic_read(&neon_global.ctx_live));

  for(i = 0; i < neon_global.ndev; i++) {
    dev = &neon_global.dev[i];
    neon_dev_print(dev);
  }

  return;
}
