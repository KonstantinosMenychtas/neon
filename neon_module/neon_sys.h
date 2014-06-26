/***************************************************************************/
/*!
  \author  Konstantinos Menychtas --- kmenycht@cs.rochester.edu
  \brief  "NEON black-box GPU channel management"
*/
/***************************************************************************/

#ifndef __NEON_SYS_H__
#define __NEON_SYS_H__

// #include <linux/timer.h>
// #include "neon_control.h"

/****************************************************************************/
// Enable kernel-call accounting based on trace invariance for kepler
// devices (where kernel calls happen as triplets of requets, while
// there appear to also exist different types of devices)
// #define NEON_KERNEL_CALL_COUNTING

/****************************************************************************/
// By convention: (check neon_kern_probe @ nv.c), device info
// should include the following entries:
// buf[i+0] = (unsigned long long) nv->bars[NV_GPU_BAR_INDEX_REGS].address;
// buf[i+1] = (unsigned long long) nv->bars[NV_GPU_BAR_INDEX_REGS].size;
// buf[i+2] = (unsigned long long) nv->fb->addres;
// buf[i+3] = (unsigned long long) nv->fb->size;
// buf[i+4] = (unsigned long long) nv->vendor_id;
// buf[i+5] = (unsigned long long) nv->device_id;
// buf[i+6] = (unsigned long long) nv->subsystem_id;
#define NEON_DEV_INFO_ENTRIES 7

/**************************************************************************/
// hardware-related macros
#define NVIDIA_VENDOR     0x10de // pci-probe, lspci -v

// GTX670
#define ZOTAC_SUBSYSTEM   0x1265  // pci-probe, lspci -v
#define GTX670_DEVICE_ID  0x1189  // pci-probe, lspci -v
#define GTX670_CHANNELS   0x60    // via testing
// GTX275
#define EVGA_SUBSYSTEM    0x1171 // pci-probe, lspci -v
#define GTX275_DEVICE_ID  0x5e6  // pci-probe, lspci -v
#define GTX275_CHANNELS   0x40   //
// NVS295
#define NVIDIA_SUBSYSTEM  0x62e  // pci-probe, lspci -v
#define NVS295_DEVICE_ID  0x6fd  // pci-probe, lspci -v
#define NVS295_CHANNELS   0x20   //

/****************************************************************************/
// macros related to trace observations

// cmd_nr
#define NEON_RQST_CTX     0x2a
#define NEON_RQST_UPDT    0x57
#define NEON_RQST_MMAP    0x4e
#define NEON_RQST_MAPIN   0x27

// cmd_val idx
#define NEON_CMD_IDX_KEY_CTX        0
#define NEON_CMD_IDX_KEY_DEV_GET    1
#define NEON_CMD_IDX_KEY_MAP_PREP   2
#define NEON_CMD_IDX_KEY_MAP_UPDT   3
#define NEON_CMD_IDX_METHOD         2
#define NEON_CMD_IDX_MMIO_ADDR     10 // + 1 (long value)

#define NEON_CMD_IDX_MMAP_SIZE      6
#define NEON_CMD_IDX_MMAP_ADDR      8 // +1  (long value)
#define NEON_CMD_IDX_MAPIN_TYPE     3
#define NEON_CMD_IDX_MAPIN_SIZE     8
#define NEON_CMD_IDX_MAPIN_ADDR     6 // +1  (long value)

// cmd_val
#define NEON_ENABLE_GRAPHICS       0x204
#define NEON_ENABLE_COMPUTE        0x214
#define NEON_ENABLE_OTHER          0x201
#define NEON_PIN_USER_PAGES        0x71
#define NEON_MMAP_KERNEL_PAGES     0x3e

// command buffer sizes
#define NEON_RCB_SIZE_COMPUTE       0x00402000
#define NEON_RB_SIZE_COMPUTE        0x00002000
#define NEON_RB_SIZE_GRAPHICS       0x00040000
#define NEON_RB_PAGEOFS             0x8c

// dev channel offset
#define NEON_TESLA_CHANNEL_BASE    0xc00000
#define NEON_TESLA_CHANNEL_OFFSET  0x2000
#define NEON_KEPLER_CHANNEL_BASE   0x7d60000
#define NEON_KEPLER_CHANNEL_OFFSET 0x200

// refc eval (definition contain hard-coded re vals)
int tesla_refc_eval(const unsigned int cb_pid,
                    struct vm_area_struct * cb_vma,
                    const unsigned int workload,
                    const unsigned long * const cmd_tuple,
                    unsigned long * const refc_addr_val);
int kepler_refc_eval(const unsigned int cb_pid,
                     struct vm_area_struct * cb_vma,
                     const unsigned int workload,
                     const unsigned long * const cmd_tuple,
                     unsigned long * const refc_addr_val);

// defined in neon_sys.c
extern const unsigned long refc_addr_backoff[];
extern const unsigned long refc_val_backoff[];

/****************************************************************************/
// _pre: ioctl-request arriving before ioctl is handled by the blob
// new context identified through associated ioctl call
int neon_rqst_pre_context(void *cmd_val);
// new device object identified through associated ioctl call
int neon_rqst_pre_dev(void *pre_cmd_val);
// build neon-map
int neon_rqst_pre_mapin(int cmd_nr, void *pre_cmd_val);
int neon_rqst_post_mapin(int cmd_nr, void *pre_cmd_val, void *post_cmd_val);

// _post: ioctl-request arriving after  ioctl is handled by the blob
// update some neon-map info
int neon_rqst_post_mmap(int cmd_nr, void *pre_cmd_val, void *post_cmd_val);
// sets the neon-map GPU-view address
int neon_rqst_post_gpuview(int cmd_nr, void *pre_cmd_val, void *post_cmd_val);

// read some value from an arbitrary (GPU accessing process') vaddr
unsigned int neon_uptr_read(const unsigned int pid,
                            struct vm_area_struct * vma,
                            const unsigned long ptr);

#endif  // __NEON_SYS_H__
