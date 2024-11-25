#include "linux/sizes.h"

#include "kvm/irq.h"
#include "kvm/kvm-cpu.h"
#include "kvm/kvm.h"
#include "kvm/vfio.h"

#include <assert.h>

#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/time.h>

/* Some distros don't have the define. */
#ifndef PCI_CAP_EXP_RC_ENDPOINT_SIZEOF_V1
#define PCI_CAP_EXP_RC_ENDPOINT_SIZEOF_V1 12
#endif

/* Wrapper around UAPI vfio_irq_set */
union vfio_irq_eventfd {
  struct vfio_irq_set irq;
  u8 buffer[sizeof(struct vfio_irq_set) + sizeof(int)];
};

static void set_vfio_irq_eventd_payload(union vfio_irq_eventfd *evfd, int fd) {
  memcpy(&evfd->irq.data, &fd, sizeof(fd));
}

/*
 * To support MSI and MSI-X with common code, track the host and guest states of
 * the MSI/MSI-X capability, and of individual vectors.
 *
 * Both MSI and MSI-X capabilities are enabled and disabled through registers.
 * Vectors cannot be individually disabled.
 */
#define msi_is_enabled(state) ((state) & VFIO_PCI_MSI_STATE_ENABLED)

/*
 * MSI-X: the control register allows to mask all vectors, and the table allows
 * to mask each vector individually.
 *
 * MSI: if the capability supports Per-Vector Masking then the Mask Bit register
 * allows to mask each vector individually. Otherwise there is no masking for
 * MSI.
 */
#define msi_is_masked(state) ((state) & VFIO_PCI_MSI_STATE_MASKED)

/*
 * A capability is empty when no vector has been registered with SET_IRQS
 * yet. It's an optimization specific to kvmtool to avoid issuing lots of
 * SET_IRQS ioctls when the guest configures the MSI-X table while the
 * capability is masked.
 */
#define msi_is_empty(state) ((state) & VFIO_PCI_MSI_STATE_EMPTY)

#define msi_update_state(state, val, bit)                                      \
  (state) = (val) ? (state) | bit : (state) & ~bit;
#define msi_set_enabled(state, val)                                            \
  msi_update_state(state, val, VFIO_PCI_MSI_STATE_ENABLED)
#define msi_set_masked(state, val)                                             \
  msi_update_state(state, val, VFIO_PCI_MSI_STATE_MASKED)
#define msi_set_empty(state, val)                                              \
  msi_update_state(state, val, VFIO_PCI_MSI_STATE_EMPTY)

static void vfio_pci_disable_intx(struct kvm *kvm, struct vfio_device *vdev);
static int vfio_pci_enable_intx(struct kvm *kvm, struct vfio_device *vdev);

static int vfio_pci_enable_msis(struct kvm *kvm, struct vfio_device *vdev,
                                bool msix) {
  size_t i;
  int ret = 0;
  int *eventfds;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_pci_msi_common *msis = msix ? &pdev->msix : &pdev->msi;
  union vfio_irq_eventfd single = {
      .irq =
          {
              .argsz = sizeof(single),
              .flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
              .index = msis->info.index,
              .count = 1,
          },
  };

  if (!msi_is_enabled(msis->guest_state))
    return 0;

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX)
    /*
     * PCI (and VFIO) forbids enabling INTx, MSI or MSIX at the same
     * time. Since INTx has to be enabled from the start (we don't
     * have a reliable way to know when the guest starts using it),
     * disable it now.
     */
    vfio_pci_disable_intx(kvm, vdev);

  eventfds = (void *)msis->irq_set + sizeof(struct vfio_irq_set);

  /*
   * Initial registration of the full range. This enables the physical
   * MSI/MSI-X capability, which might have side effects. For instance
   * when assigning virtio legacy devices, enabling the MSI capability
   * modifies the config space layout!
   *
   * As an optimization, only update MSIs when guest unmasks the
   * capability. This greatly reduces the initialization time for Linux
   * guest with 2048+ MSIs. Linux guest starts by enabling the MSI-X cap
   * masked, then fills individual vectors, then unmasks the whole
   * function. So we only do one VFIO ioctl when enabling for the first
   * time, and then one when unmasking.
   */
  if (!msi_is_enabled(msis->host_state) ||
      (!msi_is_masked(msis->guest_state) && msi_is_empty(msis->host_state))) {
    bool empty = true;

    for (i = 0; i < msis->nr_entries; i++) {
      eventfds[i] = msis->entries[i].gsi >= 0 ? msis->entries[i].eventfd : -1;

      if (eventfds[i] >= 0)
        empty = false;
    }

    ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, msis->irq_set);
    if (ret < 0) {
      perror("VFIO_DEVICE_SET_IRQS(multi)");
      return ret;
    }

    msi_set_enabled(msis->host_state, true);
    msi_set_empty(msis->host_state, empty);

    return 0;
  }

  if (msi_is_masked(msis->guest_state)) {
    /* TODO: if host_state is not empty nor masked, mask all vectors */
    return 0;
  }

  /* Update individual vectors to avoid breaking those in use */
  for (i = 0; i < msis->nr_entries; i++) {
    struct vfio_pci_msi_entry *entry = &msis->entries[i];
    int fd = entry->gsi >= 0 ? entry->eventfd : -1;

    if (fd == eventfds[i])
      continue;

    single.irq.start = i;
    set_vfio_irq_eventd_payload(&single, fd);

    ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &single);
    if (ret < 0) {
      perror("VFIO_DEVICE_SET_IRQS(single)");
      break;
    }

    eventfds[i] = fd;

    if (msi_is_empty(msis->host_state) && fd >= 0)
      msi_set_empty(msis->host_state, false);
  }

  return ret;
}

static int vfio_pci_disable_msis(struct kvm *kvm, struct vfio_device *vdev,
                                 bool msix) {
  int ret;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_pci_msi_common *msis = msix ? &pdev->msix : &pdev->msi;
  struct vfio_irq_set irq_set = {
      .argsz = sizeof(irq_set),
      .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
      .index = msis->info.index,
      .start = 0,
      .count = 0,
  };

  if (!msi_is_enabled(msis->host_state))
    return 0;

  ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);
  if (ret < 0) {
    perror("VFIO_DEVICE_SET_IRQS(NONE)");
    return ret;
  }

  msi_set_enabled(msis->host_state, false);
  msi_set_empty(msis->host_state, true);

  /*
   * When MSI or MSIX is disabled, this might be called when
   * PCI driver detects the MSI interrupt failure and wants to
   * rollback to INTx mode.  Thus enable INTx if the device
   * supports INTx mode in this case.
   */
  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX)
    ret = vfio_pci_enable_intx(kvm, vdev);

  return ret >= 0 ? 0 : ret;
}

static int vfio_pci_update_msi_entry(struct kvm *kvm, struct vfio_device *vdev,
                                     struct vfio_pci_msi_entry *entry) {
  int ret;
  // 创建eventfd（如果还没有）
  if (entry->eventfd < 0) {
    // 系统调用：eventfd(0, 0)
    entry->eventfd = eventfd(0, 0);
    if (entry->eventfd < 0) {
      ret = -errno;
      vfio_dev_err(vdev, "cannot create eventfd");
      return ret;
    }
  }

  /* Allocate IRQ if necessary */
  // 分配或更新IRQ路由
  if (entry->gsi < 0) {
    // 通过KVM添加MSI-X路由
    int ret = irq__add_msix_route(kvm, &entry->config.msg,
                                  vdev->dev_hdr.dev_num << 3);
    if (ret < 0) {
      vfio_dev_err(vdev, "cannot create MSI-X route");
      return ret;
    }
    entry->gsi = ret;
  } else {
    // 更新现有路由
    irq__update_msix_route(kvm, entry->gsi, &entry->config.msg);
  }

  /*
   * MSI masking is unimplemented in VFIO, so we have to handle it by
   * disabling/enabling IRQ route instead. We do it on the KVM side rather
   * than VFIO, because:
   * - it is 8x faster
   * - it allows to decouple masking logic from capability state.
   * - in masked state, after removing irqfd route, we could easily plug
   *   the eventfd in a local handler, in order to serve Pending Bit reads
   *   to the guest.
   *
   * So entry->host_state is masked when there is no active irqfd route.
   */
  
  if (msi_is_masked(entry->guest_state) == msi_is_masked(entry->host_state))
    return 0;

  if (msi_is_masked(entry->host_state)) {
    ret = irq__add_irqfd(kvm, entry->gsi, entry->eventfd, -1);
    if (ret < 0) {
      vfio_dev_err(vdev, "cannot setup irqfd");
      return ret;
    }
  } else {
    irq__del_irqfd(kvm, entry->gsi, entry->eventfd);
  }

  msi_set_masked(entry->host_state, msi_is_masked(entry->guest_state));

  return 0;
}

static void vfio_pci_msix_pba_access(struct kvm_cpu *vcpu, u64 addr, u8 *data,
                                     u32 len, u8 is_write, void *ptr) {
  struct vfio_pci_device *pdev = ptr;
  struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
  u64 offset = addr - pba->guest_phys_addr;
  struct vfio_device *vdev = container_of(pdev, struct vfio_device, pci);

  if (offset >= pba->size) {
    vfio_dev_err(vdev, "access outside of the MSIX PBA");
    return;
  }

  if (is_write)
    return;

  /*
   * TODO: emulate PBA. Hardware MSI-X is never masked, so reading the PBA
   * is completely useless here. Note that Linux doesn't use PBA.
   */
  if (pread(vdev->fd, data, len, pba->fd_offset + offset) != (ssize_t)len)
    vfio_dev_err(vdev, "cannot access MSIX PBA\n");
}
/**
 *     struct kvm_cpu *vcpu,     // 当前正在执行的虚拟CPU，kvm提供
 *     u64 addr,                 // 当前正在执行的虚拟CPU
 *     u8 *data,                 // 数据缓冲区
 *     u32 len,                  // 访问长度
 *     u8 is_write,             // 是否为写操作
 *     void *ptr                 // 注册MMIO时传入的私有数据(设备结构)
 */
static void vfio_pci_msix_table_access(struct kvm_cpu *vcpu, u64 addr, u8 *data,
                                       u32 len, u8 is_write, void *ptr) {
  struct kvm *kvm = vcpu->kvm;
  struct vfio_pci_msi_entry *entry;
  struct vfio_pci_device *pdev = ptr;
  struct vfio_device *vdev = container_of(pdev, struct vfio_device, pci);
  // 获取相对于MSI-X表起始的偏移量
  u64 offset = addr - pdev->msix_table.guest_phys_addr;
  if (offset >= pdev->msix_table.size) {
    vfio_dev_err(vdev, "access outside of the MSI-X table");
    return;
  }

  /**
   * MSI-X表布局：
+------------------+ <-- guest_phys_addr (表基地址)
| Vector 0         |     每个表项16字节
|   - Address Low  |     0-3   字节：目标地址低32位
|   - Address High |     4-7   字节：目标地址高32位
|   - Data         |     8-11  字节：中断数据
|   - Control      |     12-15 字节：控制字段
+------------------+
| Vector 1         |
|   ...            |
+------------------+
      ...
   */
  // 计算访问的是第几个向量
  size_t vector = offset / PCI_MSIX_ENTRY_SIZE;
  // 计算在向量内的偏移量
  off_t field = offset % PCI_MSIX_ENTRY_SIZE;

  /*
   * PCI spec says that software must use aligned 4 or 8 bytes accesses
   * for the MSI-X tables.
   */
  // 验证访问的合法性（PCI规范要求4或8字节对齐访问）
  if ((len != 4 && len != 8) || addr & (len - 1)) {
    vfio_dev_warn(vdev, "invalid MSI-X table access");
    return;
  }

  // 获取对应的MSI-X表项
  entry = &pdev->msix.entries[vector];

  mutex_lock(&pdev->msix.mutex);

  if (!is_write) {
    memcpy(data, (void *)&entry->config + field, len);
    goto out_unlock;
  }

  memcpy((void *)&entry->config + field, data, len);

  /*
   * Check if access touched the vector control register, which is at the
   * end of the MSI-X entry.
   */
  if (field + len <= PCI_MSIX_ENTRY_VECTOR_CTRL)
    goto out_unlock;

  msi_set_masked(entry->guest_state,
                 entry->config.ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT);

  if (vfio_pci_update_msi_entry(kvm, vdev, entry) < 0)
    /* Not much we can do here. */
    vfio_dev_err(vdev, "failed to configure MSIX vector %zu", vector);

  /* Update the physical capability if necessary */
  if (vfio_pci_enable_msis(kvm, vdev, true))
    vfio_dev_err(vdev, "cannot enable MSIX");

out_unlock:
  mutex_unlock(&pdev->msix.mutex);
}

static void vfio_pci_msix_cap_write(struct kvm *kvm, struct vfio_device *vdev,
                                    u16 off, void *data, int sz) {
  struct vfio_pci_device *pdev = &vdev->pci;
  off_t enable_pos = PCI_MSIX_FLAGS + 1;
  bool enable;
  u16 flags;

  off -= pdev->msix.pos;

  /* Check if access intersects with the MSI-X Enable bit */
  if (off > enable_pos || off + sz <= enable_pos)
    return;

  /* Read byte that contains the Enable bit */
  flags = *(u8 *)(data + enable_pos - off) << 8;

  mutex_lock(&pdev->msix.mutex);

  msi_set_masked(pdev->msix.guest_state, flags & PCI_MSIX_FLAGS_MASKALL);
  enable = flags & PCI_MSIX_FLAGS_ENABLE;
  msi_set_enabled(pdev->msix.guest_state, enable);

  if (enable && vfio_pci_enable_msis(kvm, vdev, true))
    vfio_dev_err(vdev, "cannot enable MSIX");
  else if (!enable && vfio_pci_disable_msis(kvm, vdev, true))
    vfio_dev_err(vdev, "cannot disable MSIX");

  mutex_unlock(&pdev->msix.mutex);
}

static int vfio_pci_msi_vector_write(struct kvm *kvm, struct vfio_device *vdev,
                                     u16 off, u8 *data, u32 sz) {
  size_t i;
  u32 mask = 0;
  size_t mask_pos, start, limit;
  struct vfio_pci_msi_entry *entry;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct msi_cap_64 *msi_cap_64 = PCI_CAP(&pdev->hdr, pdev->msi.pos);

  if (!(msi_cap_64->ctrl & PCI_MSI_FLAGS_MASKBIT))
    return 0;

  if (msi_cap_64->ctrl & PCI_MSI_FLAGS_64BIT)
    mask_pos = PCI_MSI_MASK_64;
  else
    mask_pos = PCI_MSI_MASK_32;

  if (off >= mask_pos + 4 || off + sz <= mask_pos)
    return 0;

  /* Set mask to current state */
  for (i = 0; i < pdev->msi.nr_entries; i++) {
    entry = &pdev->msi.entries[i];
    mask |= !!msi_is_masked(entry->guest_state) << i;
  }

  /* Update mask following the intersection of access and register */
  start = max_t(size_t, off, mask_pos);
  limit = min_t(size_t, off + sz, mask_pos + 4);

  memcpy((void *)&mask + start - mask_pos, data + start - off, limit - start);

  /* Update states if necessary */
  for (i = 0; i < pdev->msi.nr_entries; i++) {
    bool masked = mask & (1 << i);

    entry = &pdev->msi.entries[i];
    if (masked != msi_is_masked(entry->guest_state)) {
      msi_set_masked(entry->guest_state, masked);
      vfio_pci_update_msi_entry(kvm, vdev, entry);
    }
  }

  return 1;
}

static void vfio_pci_msi_cap_write(struct kvm *kvm, struct vfio_device *vdev,
                                   u16 off, u8 *data, u32 sz) {
  u8 ctrl;
  struct msi_msg msg;
  size_t i, nr_vectors;
  struct vfio_pci_msi_entry *entry;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct msi_cap_64 *msi_cap_64 = PCI_CAP(&pdev->hdr, pdev->msi.pos);

  off -= pdev->msi.pos;

  mutex_lock(&pdev->msi.mutex);

  /* Check if the guest is trying to update mask bits */
  if (vfio_pci_msi_vector_write(kvm, vdev, off, data, sz))
    goto out_unlock;

  /* Only modify routes when guest pokes the enable bit */
  if (off > PCI_MSI_FLAGS || off + sz <= PCI_MSI_FLAGS)
    goto out_unlock;

  ctrl = *(u8 *)(data + PCI_MSI_FLAGS - off);

  msi_set_enabled(pdev->msi.guest_state, ctrl & PCI_MSI_FLAGS_ENABLE);

  if (!msi_is_enabled(pdev->msi.guest_state)) {
    vfio_pci_disable_msis(kvm, vdev, false);
    goto out_unlock;
  }

  /* Create routes for the requested vectors */
  nr_vectors = 1 << ((ctrl & PCI_MSI_FLAGS_QSIZE) >> 4);

  msg.address_lo = msi_cap_64->address_lo;
  if (msi_cap_64->ctrl & PCI_MSI_FLAGS_64BIT) {
    msg.address_hi = msi_cap_64->address_hi;
    msg.data = msi_cap_64->data;
  } else {
    struct msi_cap_32 *msi_cap_32 = (void *)msi_cap_64;
    msg.address_hi = 0;
    msg.data = msi_cap_32->data;
  }

  for (i = 0; i < nr_vectors; i++) {
    entry = &pdev->msi.entries[i];

    /*
     * Set the MSI data value as required by the PCI local
     * bus specifications, MSI capability, "Message Data".
     */
    msg.data &= ~(nr_vectors - 1);
    msg.data |= i;

    entry->config.msg = msg;
    vfio_pci_update_msi_entry(kvm, vdev, entry);
  }

  /* Update the physical capability if necessary */
  if (vfio_pci_enable_msis(kvm, vdev, false))
    vfio_dev_err(vdev, "cannot enable MSI");

out_unlock:
  mutex_unlock(&pdev->msi.mutex);
}

static int vfio_pci_bar_activate(struct kvm *kvm,
                                 struct pci_device_header *pci_hdr, int bar_num,
                                 void *data) {
  struct vfio_device *vdev = data;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
  struct vfio_pci_msix_table *table = &pdev->msix_table;
  struct vfio_region *region;
  u32 bar_addr;
  bool has_msix;
  int ret;

  assert((u32)bar_num < vdev->info.num_regions);

  region = &vdev->regions[bar_num];
  has_msix = pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX;

  bar_addr = pci__bar_address(pci_hdr, bar_num);
  if (pci__bar_is_io(pci_hdr, bar_num))
    region->port_base = bar_addr;
  else
    region->guest_phys_addr = bar_addr;
  // 特殊处理MSI-X相关的BAR
  if (has_msix && (u32)bar_num == table->bar) {
    table->guest_phys_addr = region->guest_phys_addr;
    // 注册MSI-X表的MMIO处理
    ret = kvm__register_mmio(kvm, table->guest_phys_addr, table->size, false,
                             vfio_pci_msix_table_access, pdev);
    /*
     * The MSIX table and the PBA structure can share the same BAR,
     * but for convenience we register different regions for mmio
     * emulation. We want to we update both if they share the same
     * BAR.
     */
    if (ret < 0 || table->bar != pba->bar)
      goto out;
  }

  if (has_msix && (u32)bar_num == pba->bar) {
    if (pba->bar == table->bar)
      pba->guest_phys_addr = table->guest_phys_addr + pba->bar_offset;
    else
      pba->guest_phys_addr = region->guest_phys_addr;
    ret = kvm__register_mmio(kvm, pba->guest_phys_addr, pba->size, false,
                             vfio_pci_msix_pba_access, pdev);
    goto out;
  }

  ret = vfio_map_region(kvm, vdev, region);
out:
  return ret;
}

static int vfio_pci_bar_deactivate(struct kvm *kvm,
                                   struct pci_device_header *pci_hdr,
                                   int bar_num, void *data) {
  struct vfio_device *vdev = data;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
  struct vfio_pci_msix_table *table = &pdev->msix_table;
  struct vfio_region *region;
  bool has_msix, success;
  int ret;

  assert((u32)bar_num < vdev->info.num_regions);

  region = &vdev->regions[bar_num];
  has_msix = pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX;

  if (has_msix && (u32)bar_num == table->bar) {
    success = kvm__deregister_mmio(kvm, table->guest_phys_addr);
    /* kvm__deregister_mmio fails when the region is not found. */
    ret = (success ? 0 : -ENOENT);
    /* See vfio_pci_bar_activate(). */
    if (ret < 0 || table->bar != pba->bar)
      goto out;
  }

  if (has_msix && (u32)bar_num == pba->bar) {
    success = kvm__deregister_mmio(kvm, pba->guest_phys_addr);
    ret = (success ? 0 : -ENOENT);
    goto out;
  }

  vfio_unmap_region(kvm, region);
  ret = 0;

out:
  return ret;
}

static void vfio_pci_cfg_read(struct kvm *kvm,
                              struct pci_device_header *pci_hdr, u16 offset,
                              void *data, int sz) {
  struct vfio_region_info *info;
  struct vfio_pci_device *pdev;
  struct vfio_device *vdev;
  char base[sz];

  pdev = container_of(pci_hdr, struct vfio_pci_device, hdr);
  vdev = container_of(pdev, struct vfio_device, pci);
  info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;

  /* Dummy read in case of side-effects */
  if (pread(vdev->fd, base, sz, info->offset + offset) != sz)
    vfio_dev_warn(vdev,
                  "failed to read %d bytes from Configuration Space at 0x%x",
                  sz, offset);
}

static void vfio_pci_cfg_write(struct kvm *kvm,
                               struct pci_device_header *pci_hdr, u16 offset,
                               void *data, int sz) {
  struct vfio_region_info *info;
  struct vfio_pci_device *pdev;
  struct vfio_device *vdev;
  u32 tmp;

  /* Make sure a larger size will not overrun tmp on the stack. */
  assert(sz <= 4);

  if (offset == PCI_ROM_ADDRESS)
    return;

  pdev = container_of(pci_hdr, struct vfio_pci_device, hdr);
  vdev = container_of(pdev, struct vfio_device, pci);
  info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;

  if (pwrite(vdev->fd, data, sz, info->offset + offset) != sz)
    vfio_dev_warn(vdev,
                  "Failed to write %d bytes to Configuration Space at 0x%x", sz,
                  offset);

  /* Handle MSI write now, since it might update the hardware capability */
  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX)
    vfio_pci_msix_cap_write(kvm, vdev, offset, data, sz);

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI)
    vfio_pci_msi_cap_write(kvm, vdev, offset, data, sz);

  if (pread(vdev->fd, &tmp, sz, info->offset + offset) != sz)
    vfio_dev_warn(vdev,
                  "Failed to read %d bytes from Configuration Space at 0x%x",
                  sz, offset);
}

static ssize_t vfio_pci_msi_cap_size(struct msi_cap_64 *cap_hdr) {
  size_t size = 10;

  if (cap_hdr->ctrl & PCI_MSI_FLAGS_64BIT)
    size += 4;
  if (cap_hdr->ctrl & PCI_MSI_FLAGS_MASKBIT)
    size += 10;

  return size;
}

static ssize_t vfio_pci_cap_size(struct pci_cap_hdr *cap_hdr) {
  switch (cap_hdr->type) {
  case PCI_CAP_ID_MSIX:
    return PCI_CAP_MSIX_SIZEOF;
  case PCI_CAP_ID_MSI:
    return vfio_pci_msi_cap_size((void *)cap_hdr);
  case PCI_CAP_ID_EXP:
    /*
     * We don't emulate any of the link, slot and root complex
     * properties, so ignore them.
     */
    return PCI_CAP_EXP_RC_ENDPOINT_SIZEOF_V1;
  default:
    pr_err("unknown PCI capability 0x%x", cap_hdr->type);
    return 0;
  }
}

static int vfio_pci_add_cap(struct vfio_device *vdev, u8 *virt_hdr,
                            struct pci_cap_hdr *cap, off_t pos) {
  struct pci_cap_hdr *last;
  struct pci_device_header *hdr = &vdev->pci.hdr;

  cap->next = 0;
  // 如果是第一个能力，那么就设置能力指针
  if (!hdr->capabilities) {
    hdr->capabilities = pos;            // 设置能力指针
    hdr->status |= PCI_STATUS_CAP_LIST; // 设置状态位，表示存在能力链表
  } else {
    // 找到链表末尾
    last = PCI_CAP(virt_hdr, hdr->capabilities);

    while (last->next)
      last = PCI_CAP(virt_hdr, last->next);
    // 将新能力添加到链表末尾
    last->next = pos;
  }
  // 将能力结构复制到虚拟配置空间中
  memcpy(virt_hdr + pos, cap, vfio_pci_cap_size(cap));

  return 0;
}
/**
 *
配置空间
+------------------+ 0x00
|   标准头部       |
|                 |
+------------------+ 0x34
| 能力指针 (0x40) |--+
+------------------+  |
|                 |  |
+------------------+ 0x40
| 能力1(MSI)      |<-+
| next = 0x50     |--+
+------------------+  |
|                 |  |
+------------------+ 0x50
| 能力2(MSI-X)    |<-+
| next = 0x60     |--+
+------------------+  |
|                 |  |
+------------------+ 0x60
| 能力3(PM)       |<-+
| next = 0x00     |
+------------------+

就是整个能力空间按照一个链表的形式组织，表示了这个设备支持了那些东西
 */

/**
 * 原始设备配置空间          临时缓冲区(virt_hdr)         最终配置空间
+----------------+       +----------------+        +----------------+
|   设备头       |       |                |        |    设备头      |
|   能力A        |  -->  |    能力A'      |  -->   |    能力A'      |
|   能力B        |       |    能力B'      |        |    能力B'      |
|   能力C        |       |    能力C'      |        |    能力C'      |
+----------------+       +----------------+        +----------------+
 */
static int vfio_pci_parse_caps(struct vfio_device *vdev) {
  int ret;
  size_t size;
  u16 pos, next;
  struct pci_cap_hdr *cap;
  u8 virt_hdr[PCI_DEV_CFG_SIZE_LEGACY]; // 256字节的临时缓冲区
  struct vfio_pci_device *pdev = &vdev->pci;

  if (!(pdev->hdr.status & PCI_STATUS_CAP_LIST))
    return 0;

  memset(virt_hdr, 0, PCI_DEV_CFG_SIZE_LEGACY);
  // 能力就是就是设备支持的额外功能，比如msi，msix，pci-exp
  // 清除低2位，因为能力指针必须4字节对齐，这个是一个偏移量
  pos = pdev->hdr.capabilities & ~3;
  pdev->hdr.status &= ~PCI_STATUS_CAP_LIST;
  pdev->hdr.capabilities = 0;

  for (; pos; pos = next) {
    cap = PCI_CAP(&pdev->hdr, pos);
    next = cap->next;

    switch (cap->type) {
    case PCI_CAP_ID_MSIX:
      ret = vfio_pci_add_cap(vdev, virt_hdr, cap, pos);
      if (ret)
        return ret;

      pdev->msix.pos = pos;
      pdev->irq_modes |= VFIO_PCI_IRQ_MODE_MSIX;
      break;
    case PCI_CAP_ID_MSI:
      ret = vfio_pci_add_cap(vdev, virt_hdr, cap, pos);
      if (ret)
        return ret;

      pdev->msi.pos = pos;
      pdev->irq_modes |= VFIO_PCI_IRQ_MODE_MSI;
      break;
    case PCI_CAP_ID_EXP:
      if (!arch_has_pci_exp())
        continue;
      ret = vfio_pci_add_cap(vdev, virt_hdr, cap, pos);
      if (ret)
        return ret;
      break;
    }
  }

  /* Wipe remaining capabilities */
  pos = PCI_STD_HEADER_SIZEOF;
  size = PCI_DEV_CFG_SIZE_LEGACY - PCI_STD_HEADER_SIZEOF;
  memcpy((void *)&pdev->hdr + pos, virt_hdr + pos, size);

  return 0;
}

static int vfio_pci_parse_cfg_space(struct vfio_device *vdev) {
  ssize_t sz = PCI_DEV_CFG_SIZE_LEGACY;
  struct vfio_region_info *info;
  struct vfio_pci_device *pdev = &vdev->pci;

  if (vdev->info.num_regions < VFIO_PCI_CONFIG_REGION_INDEX) {
    vfio_dev_err(vdev, "Config Space not found");
    return -ENODEV;
  }

  info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
  *info = (struct vfio_region_info){
      .argsz = sizeof(*info),
      .index = VFIO_PCI_CONFIG_REGION_INDEX,
  };
  // 获取PCIe设备的配置空间
  ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
  if (!info->size) {
    vfio_dev_err(vdev, "Config Space has size zero?!");
    return -EINVAL;
  }

  /* Read standard headers and capabilities */
  if (pread(vdev->fd, &pdev->hdr, sz, info->offset) != sz) {
    vfio_dev_err(vdev, "failed to read %zd bytes of Config Space", sz);
    return -EIO;
  }

  /* Strip bit 7, that indicates multifunction */
  pdev->hdr.header_type &= 0x7f;

  if (pdev->hdr.header_type != PCI_HEADER_TYPE_NORMAL) {
    vfio_dev_err(vdev, "unsupported header type %u", pdev->hdr.header_type);
    return -EOPNOTSUPP;
  }

  if (pdev->hdr.irq_pin)
    pdev->irq_modes |= VFIO_PCI_IRQ_MODE_INTX;
  // 解析能力空间，根据标准PCIe配置空间，解析能力，然后添加到我们的头空间
  vfio_pci_parse_caps(vdev);

  return 0;
}

static int vfio_pci_fixup_cfg_space(struct vfio_device *vdev) {
  int i;
  u64 base;
  ssize_t hdr_sz;
  struct msix_cap *msix;
  struct vfio_region_info *info;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_region *region;

  /* Initialise the BARs */
  for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
    if ((u32)i == vdev->info.num_regions)
      break;

    region = &vdev->regions[i];
    /* Construct a fake reg to match what we've mapped. */
    // 根据类型(IO或内存)构造BAR寄存器的值
    if (region->is_ioport) {
      base = (region->port_base & PCI_BASE_ADDRESS_IO_MASK) |
             PCI_BASE_ADDRESS_SPACE_IO;
    } else {
      base = (region->guest_phys_addr & PCI_BASE_ADDRESS_MEM_MASK) |
             PCI_BASE_ADDRESS_SPACE_MEMORY;
    }
    // 将构造的值写入到PCI头部的BAR寄存器
    pdev->hdr.bar[i] = base;
    // 这个是处理64位的情况，如果是64位，那么这个base就是空的
    if (!base)
      continue;
    // 设置BAR的大小
    pdev->hdr.bar_size[i] = region->info.size;
  }

  /* I really can't be bothered to support cardbus. */
  pdev->hdr.card_bus = 0;

  /*
   * Nuke the expansion ROM for now. If we want to do this properly,
   * we need to save its size somewhere and map into the guest.
   */
  pdev->hdr.exp_rom_bar = 0;

  /* Plumb in our fake MSI-X capability, if we have it. */
  msix = pci_find_cap(&pdev->hdr, PCI_CAP_ID_MSIX);
  if (msix) {
    /* Add a shortcut to the PBA region for the MMIO handler */
    int pba_index = VFIO_PCI_BAR0_REGION_INDEX + pdev->msix_pba.bar;
    u32 pba_bar_offset = msix->pba_offset & PCI_MSIX_PBA_OFFSET;
    // 设置PBA(Pending Bit Array)区域的偏移
    /**
     * vdev->regions[pba_index].info.offset是这个region对应的偏移量
     * pba_bar_offset是PBA在BAR中的偏移量
     */
    pdev->msix_pba.fd_offset =
        vdev->regions[pba_index].info.offset + pba_bar_offset;

    /* Tidy up the capability */
    msix->table_offset &= PCI_MSIX_TABLE_BIR;
    if (pdev->msix_table.bar == pdev->msix_pba.bar) {
      /* Keep the same offset as the MSIX cap. */
      pdev->msix_pba.bar_offset = pba_bar_offset;
    } else {
      /* PBA is at the start of the BAR. */
      msix->pba_offset &= PCI_MSIX_PBA_BIR;
      pdev->msix_pba.bar_offset = 0;
    }
  }

  /* Install our fake Configuration Space */
  info = &vdev->regions[VFIO_PCI_CONFIG_REGION_INDEX].info;
  /*
   * We don't touch the extended configuration space, let's be cautious
   * and not overwrite it all with zeros, or bad things might happen.
   */
  hdr_sz = PCI_DEV_CFG_SIZE_LEGACY;
  if (pwrite(vdev->fd, &pdev->hdr, hdr_sz, info->offset) != hdr_sz) {
    vfio_dev_err(vdev, "failed to write %zd bytes to Config Space", hdr_sz);
    return -EIO;
  }

  /* Register callbacks for cfg accesses */
  pdev->hdr.cfg_ops = (struct pci_config_operations){
      .read = vfio_pci_cfg_read,
      .write = vfio_pci_cfg_write,
  };

  pdev->hdr.irq_type = IRQ_TYPE_LEVEL_HIGH;

  return 0;
}

static int vfio_pci_get_region_info(struct vfio_device *vdev, u32 index,
                                    struct vfio_region_info *info) {
  int ret;

  *info = (struct vfio_region_info){
      .argsz = sizeof(*info),
      .index = index,
  };

  ret = ioctl(vdev->fd, VFIO_DEVICE_GET_REGION_INFO, info);
  if (ret) {
    ret = -errno;
    vfio_dev_err(vdev, "cannot get info for BAR %u", index);
    return ret;
  }

  if (info->size && !is_power_of_two(info->size)) {
    vfio_dev_err(vdev, "region is not power of two: 0x%llx", info->size);
    return -EINVAL;
  }

  return 0;
}

static int vfio_pci_create_msix_table(struct kvm *kvm,
                                      struct vfio_device *vdev) {
  int ret;
  size_t i;
  size_t map_size;
  size_t nr_entries;
  struct vfio_pci_msi_entry *entries;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_pci_msix_pba *pba = &pdev->msix_pba;
  struct vfio_pci_msix_table *table = &pdev->msix_table;
  struct msix_cap *msix =
      PCI_CAP(&pdev->hdr, pdev->msix.pos); // 通过这个偏移，得到msi-x的信息
  struct vfio_region_info info;

  table->bar = msix->table_offset & PCI_MSIX_TABLE_BIR; // 得到他的那个bar中
  pba->bar = msix->pba_offset & PCI_MSIX_TABLE_BIR;

  nr_entries = (msix->ctrl & PCI_MSIX_FLAGS_QSIZE) + 1;

  /* MSIX table and PBA must support QWORD accesses. */
  table->size = ALIGN(nr_entries * PCI_MSIX_ENTRY_SIZE, 8);
  pba->size = ALIGN(DIV_ROUND_UP(nr_entries, 64), 8);
  // 分配MSI-X表和PBA的内存
  entries = calloc(nr_entries, sizeof(struct vfio_pci_msi_entry));
  if (!entries)
    return -ENOMEM;
  // 初始化时所有中断都是masked状态，
  // mask机制允许软件临时禁用某个中断向量，而不需要重新配置整个中断系统
  for (i = 0; i < nr_entries; i++)
    entries[i].config.ctrl = PCI_MSIX_ENTRY_CTRL_MASKBIT;

  ret = vfio_pci_get_region_info(vdev, table->bar, &info);
  if (ret)
    return ret;
  if (!info.size)
    return -EINVAL;

  map_size = ALIGN(info.size, MAX_PAGE_SIZE);
  table->guest_phys_addr = pci_get_mmio_block(map_size);
  if (!table->guest_phys_addr) {
    pr_err("cannot allocate MMIO space");
    ret = -ENOMEM;
    goto out_free;
  }

  /*
   * We could map the physical PBA directly into the guest, but it's
   * likely smaller than a page, and we can only hand full pages to the
   * guest. Even though the PCI spec disallows sharing a page used for
   * MSI-X with any other resource, it allows to share the same page
   * between MSI-X table and PBA. For the sake of isolation, create a
   * virtual PBA.
   */
  // 如果MSI-X表和PBA在同一个BAR中，那么就共享同一个物理地址
  if (table->bar == pba->bar) {
    u32 pba_bar_offset = msix->pba_offset & PCI_MSIX_PBA_OFFSET;

    /* Sanity checks. */
    if (table->size > pba_bar_offset)
      die("MSIX table overlaps with PBA");
    if (pba_bar_offset + pba->size > info.size)
      die("PBA exceeds the size of the region");
    pba->guest_phys_addr = table->guest_phys_addr + pba_bar_offset;
  } else {
    ret = vfio_pci_get_region_info(vdev, pba->bar, &info);
    if (ret)
      return ret;
    if (!info.size)
      return -EINVAL;

    map_size = ALIGN(info.size, MAX_PAGE_SIZE);
    pba->guest_phys_addr = pci_get_mmio_block(map_size);
    if (!pba->guest_phys_addr) {
      pr_err("cannot allocate MMIO space");
      ret = -ENOMEM;
      goto out_free;
    }
  }

  pdev->msix.entries = entries;
  pdev->msix.nr_entries = nr_entries;

  return 0;

out_free:
  free(entries);

  return ret;
}

static int vfio_pci_create_msi_cap(struct kvm *kvm,
                                   struct vfio_pci_device *pdev) {
  struct msi_cap_64 *cap = PCI_CAP(&pdev->hdr, pdev->msi.pos);

  pdev->msi.nr_entries = 1 << ((cap->ctrl & PCI_MSI_FLAGS_QMASK) >> 1),
  pdev->msi.entries =
      calloc(pdev->msi.nr_entries, sizeof(struct vfio_pci_msi_entry));
  if (!pdev->msi.entries)
    return -ENOMEM;

  return 0;
}

static int vfio_pci_configure_bar(struct kvm *kvm, struct vfio_device *vdev,
                                  size_t nr) {
  int ret;
  u32 bar;
  size_t map_size;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_region *region;

  if (nr >= vdev->info.num_regions)
    return 0;

  region = &vdev->regions[nr];
  bar = pdev->hdr.bar[nr];

  region->vdev = vdev;
  region->is_ioport = !!(bar & PCI_BASE_ADDRESS_SPACE_IO); // 判断是否是io端口

  ret = vfio_pci_get_region_info(vdev, nr, &region->info);
  if (ret)
    return ret;

  /* Ignore invalid or unimplemented regions */
  if (!region->info.size)
    return 0;
  // 2. 特殊处理MSI-X相关的BAR
  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
    /* Trap and emulate MSI-X table */
    // MSI-X表所在的BAR需要特殊处理，陷入和模拟
    if (nr == pdev->msix_table.bar) {
      region->guest_phys_addr = pdev->msix_table.guest_phys_addr;
      return 0;
    } else if (nr == pdev->msix_pba.bar) {
      region->guest_phys_addr = pdev->msix_pba.guest_phys_addr;
      return 0;
    }
  }

  if (region->is_ioport) {
    // 如果是IO端口，分配IO端口空间
    region->port_base = pci_get_io_port_block(region->info.size);
  } else {
    /* Grab some MMIO space in the guest */
    // 如果是内存空间，分配MMIO空间
    map_size = ALIGN(region->info.size, PAGE_SIZE);
    region->guest_phys_addr = pci_get_mmio_block(map_size);
  }

  return 0;
}

static int vfio_pci_configure_dev_regions(struct kvm *kvm,
                                          struct vfio_device *vdev) {
  int ret;
  u32 bar;
  size_t i;
  bool is_64bit = false;
  struct vfio_pci_device *pdev = &vdev->pci;
  // 解析配置空间，根据标准PCIe配置空间，解析能力，然后添加到我们的头空间
  ret = vfio_pci_parse_cfg_space(vdev);
  if (ret)
    return ret;

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
    ret = vfio_pci_create_msix_table(kvm, vdev);
    if (ret)
      return ret;
  }

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI) {
    ret = vfio_pci_create_msi_cap(kvm, pdev);
    if (ret)
      return ret;
  }

  for (i = VFIO_PCI_BAR0_REGION_INDEX; i <= VFIO_PCI_BAR5_REGION_INDEX; ++i) {
    /* Ignore top half of 64-bit BAR */
    if (is_64bit) {
      is_64bit = false;
      continue;
    }
    // 遍历所有的bar，给他们配置相应的guest物理内存，用于之后的mmio
    ret = vfio_pci_configure_bar(kvm, vdev, i);
    if (ret)
      return ret;

    bar = pdev->hdr.bar[i];
    is_64bit =
        (bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY &&
        bar & PCI_BASE_ADDRESS_MEM_TYPE_64;
  }

  /* We've configured the BARs, fake up a Configuration Space */
  ret = vfio_pci_fixup_cfg_space(vdev);
  if (ret)
    return ret;

  return pci__register_bar_regions(kvm, &pdev->hdr, vfio_pci_bar_activate,
                                   vfio_pci_bar_deactivate, vdev);
}

/*
 * Attempt to update the FD limit, if opening an eventfd for each IRQ vector
 * would hit the limit. Which is likely to happen when a device uses 2048 MSIs.
 */
static int vfio_pci_reserve_irq_fds(size_t num) {
  /*
   * I counted around 27 fds under normal load. Let's add 100 for good
   * measure.
   */
  static size_t needed = 128;
  struct rlimit fd_limit, new_limit;

  needed += num;

  if (getrlimit(RLIMIT_NOFILE, &fd_limit)) {
    perror("getrlimit(RLIMIT_NOFILE)");
    return 0;
  }

  if (fd_limit.rlim_cur >= needed)
    return 0;

  new_limit.rlim_cur = needed;

  if (fd_limit.rlim_max < needed)
    /* Try to bump hard limit (root only) */
    new_limit.rlim_max = needed;
  else
    new_limit.rlim_max = fd_limit.rlim_max;

  if (setrlimit(RLIMIT_NOFILE, &new_limit)) {
    perror("setrlimit(RLIMIT_NOFILE)");
    pr_warning("not enough FDs for full MSI-X support (estimated need: %zu)",
               (size_t)(needed - fd_limit.rlim_cur));
  }

  return 0;
}

static int vfio_pci_init_msis(struct kvm *kvm, struct vfio_device *vdev,
                              struct vfio_pci_msi_common *msis) {
  int ret;
  size_t i;
  int *eventfds;
  size_t irq_set_size;
  struct vfio_pci_msi_entry *entry;
  size_t nr_entries = msis->nr_entries;
  // 获取中断信息
  ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &msis->info);
  if (ret || msis->info.count == 0) {
    vfio_dev_err(vdev, "no MSI reported by VFIO");
    return -ENODEV;
  }

  if (!(msis->info.flags & VFIO_IRQ_INFO_EVENTFD)) {
    vfio_dev_err(vdev, "interrupt not EVENTFD capable");
    return -EINVAL;
  }

  if (msis->info.count != nr_entries) {
    vfio_dev_err(vdev, "invalid number of MSIs reported by VFIO");
    return -EINVAL;
  }

  mutex_init(&msis->mutex);
  // 检测是否文件描述符的数量是否够用，假设有2048个中断，并且你之前也使用了一些文件描述符，这个时候可能达到了单个进程使用的文件描述符上限，这个时候会出错
  vfio_pci_reserve_irq_fds(nr_entries);
  // 分配内存`
  irq_set_size = sizeof(struct vfio_irq_set) + nr_entries * sizeof(int);
  msis->irq_set = malloc(irq_set_size);
  if (!msis->irq_set)
    return -ENOMEM;
  // 初始化中断设置，设置为eventfd，并且设置为触发
  *msis->irq_set = (struct vfio_irq_set){
      .argsz = irq_set_size,
      .flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
      .index = msis->info.index,
      .start = 0,
      .count = nr_entries,
  };

  eventfds = (void *)msis->irq_set + sizeof(struct vfio_irq_set);

  for (i = 0; i < nr_entries; i++) {
    entry = &msis->entries[i];
    entry->gsi = -1;
    entry->eventfd = -1;
    msi_set_masked(entry->guest_state, false);
    msi_set_masked(entry->host_state, true);
    eventfds[i] = -1;
  }

  return 0;
}

static void vfio_pci_disable_intx(struct kvm *kvm, struct vfio_device *vdev) {
  struct vfio_pci_device *pdev = &vdev->pci;
  int gsi = pdev->intx_gsi;
  struct vfio_irq_set irq_set = {
      .argsz = sizeof(irq_set),
      .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
      .index = VFIO_PCI_INTX_IRQ_INDEX,
  };

  if (pdev->intx_fd == -1)
    return;

  pr_debug("user requested MSI, disabling INTx %d", gsi);

  ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &irq_set);
  irq__del_irqfd(kvm, gsi, pdev->intx_fd);

  close(pdev->intx_fd);
  close(pdev->unmask_fd);
  pdev->intx_fd = -1;
}

static int vfio_pci_enable_intx(struct kvm *kvm, struct vfio_device *vdev) {
  int ret;
  int trigger_fd, unmask_fd;
  union vfio_irq_eventfd trigger;
  union vfio_irq_eventfd unmask;
  struct vfio_pci_device *pdev = &vdev->pci;
  int gsi = pdev->intx_gsi;

  if (pdev->intx_fd != -1)
    return 0;

  /*
   * PCI IRQ is level-triggered, so we use two eventfds. trigger_fd
   * signals an interrupt from host to guest, and unmask_fd signals the
   * deassertion of the line from guest to host.
   */
  trigger_fd = eventfd(0, 0);
  if (trigger_fd < 0) {
    vfio_dev_err(vdev, "failed to create trigger eventfd");
    return trigger_fd;
  }

  unmask_fd = eventfd(0, 0);
  if (unmask_fd < 0) {
    vfio_dev_err(vdev, "failed to create unmask eventfd");
    close(trigger_fd);
    return unmask_fd;
  }

  ret = irq__add_irqfd(kvm, gsi, trigger_fd, unmask_fd);
  if (ret)
    goto err_close;

  trigger.irq = (struct vfio_irq_set){
      .argsz = sizeof(trigger),
      .flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
      .index = VFIO_PCI_INTX_IRQ_INDEX,
      .start = 0,
      .count = 1,
  };
  set_vfio_irq_eventd_payload(&trigger, trigger_fd);

  ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);
  if (ret < 0) {
    vfio_dev_err(vdev, "failed to setup VFIO IRQ");
    goto err_delete_line;
  }

  unmask.irq = (struct vfio_irq_set){
      .argsz = sizeof(unmask),
      .flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_UNMASK,
      .index = VFIO_PCI_INTX_IRQ_INDEX,
      .start = 0,
      .count = 1,
  };
  set_vfio_irq_eventd_payload(&unmask, unmask_fd);

  ret = ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &unmask);
  if (ret < 0) {
    vfio_dev_err(vdev, "failed to setup unmask IRQ");
    goto err_remove_event;
  }

  pdev->intx_fd = trigger_fd;
  pdev->unmask_fd = unmask_fd;

  return 0;

err_remove_event:
  /* Remove trigger event */
  trigger.irq.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
  trigger.irq.count = 0;
  ioctl(vdev->fd, VFIO_DEVICE_SET_IRQS, &trigger);

err_delete_line:
  irq__del_irqfd(kvm, gsi, trigger_fd);

err_close:
  close(trigger_fd);
  close(unmask_fd);
  return ret;
}

static int vfio_pci_init_intx(struct kvm *kvm, struct vfio_device *vdev) {
  int ret;
  struct vfio_pci_device *pdev = &vdev->pci;
  struct vfio_irq_info irq_info = {
      .argsz = sizeof(irq_info),
      .index = VFIO_PCI_INTX_IRQ_INDEX,
  };

  vfio_pci_reserve_irq_fds(2);

  ret = ioctl(vdev->fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
  if (ret || irq_info.count == 0) {
    vfio_dev_err(vdev, "no INTx reported by VFIO");
    return -ENODEV;
  }

  if (!(irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
    vfio_dev_err(vdev, "interrupt not eventfd capable");
    return -EINVAL;
  }

  if (!(irq_info.flags & VFIO_IRQ_INFO_AUTOMASKED)) {
    vfio_dev_err(vdev, "INTx interrupt not AUTOMASKED");
    return -EINVAL;
  }

  /* Guest is going to ovewrite our irq_line... */
  pdev->intx_gsi = pdev->hdr.irq_line - KVM_IRQ_OFFSET;

  pdev->intx_fd = -1;

  return 0;
}

static int vfio_pci_configure_dev_irqs(struct kvm *kvm,
                                       struct vfio_device *vdev) {
  int ret = 0;
  struct vfio_pci_device *pdev = &vdev->pci;

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSIX) {
    pdev->msix.info = (struct vfio_irq_info){
        .argsz = sizeof(pdev->msix.info),
        .index = VFIO_PCI_MSIX_IRQ_INDEX,
    };
    ret = vfio_pci_init_msis(kvm, vdev, &pdev->msix);
    if (ret)
      return ret;
  }

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_MSI) {
    pdev->msi.info = (struct vfio_irq_info){
        .argsz = sizeof(pdev->msi.info),
        .index = VFIO_PCI_MSI_IRQ_INDEX,
    };
    ret = vfio_pci_init_msis(kvm, vdev, &pdev->msi);
    if (ret)
      return ret;
  }

  if (pdev->irq_modes & VFIO_PCI_IRQ_MODE_INTX) {
    pci__assign_irq(&vdev->pci.hdr);

    ret = vfio_pci_init_intx(kvm, vdev);
    if (ret)
      return ret;

    ret = vfio_pci_enable_intx(kvm, vdev);
  }

  return ret;
}

int vfio_pci_setup_device(struct kvm *kvm, struct vfio_device *vdev) {
  int ret;
  // 配置内存区域，获取bar的空间，并且设置mmio的处理程序
  ret = vfio_pci_configure_dev_regions(kvm, vdev);
  if (ret) {
    vfio_dev_err(vdev, "failed to configure regions");
    return ret;
  }

  vdev->dev_hdr = (struct device_header){
      .bus_type = DEVICE_BUS_PCI,
      .data = &vdev->pci.hdr,
  };
  // 在用户态使用一个红黑树来管理设备，没有使用系统调用和系统交互
  ret = device__register(&vdev->dev_hdr);
  if (ret) {
    vfio_dev_err(vdev, "failed to register VFIO device");
    return ret;
  }
  // 配置中断，读取设备信息以确定中断数量，并在用户空间为这些中断分配内存，但不使用系统调用
  ret = vfio_pci_configure_dev_irqs(kvm, vdev);
  if (ret) {
    vfio_dev_err(vdev, "failed to configure IRQs");
    return ret;
  }

  return 0;
}

void vfio_pci_teardown_device(struct kvm *kvm, struct vfio_device *vdev) {
  size_t i;
  struct vfio_pci_device *pdev = &vdev->pci;

  for (i = 0; i < vdev->info.num_regions; i++)
    vfio_unmap_region(kvm, &vdev->regions[i]);

  device__unregister(&vdev->dev_hdr);

  free(pdev->msix.irq_set);
  free(pdev->msix.entries);
  free(pdev->msi.irq_set);
  free(pdev->msi.entries);
}
