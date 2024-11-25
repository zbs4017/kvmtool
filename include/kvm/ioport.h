#ifndef KVM__IOPORT_H
#define KVM__IOPORT_H

#include "kvm/kvm-cpu.h"

#include <asm/types.h>
#include <linux/byteorder.h>
#include <linux/types.h>

/* some ports we reserve for own use */
#define IOPORT_DBG 0xe0

void ioport__map_irq(u8 *irq);

static inline u8 ioport__read8(u8 *data) { return *data; }
/* On BE platforms, PCI I/O is byteswapped, i.e. LE, so swap back. */
static inline u16 ioport__read16(u16 *data) { return le16_to_cpu(*data); }

static inline u32 ioport__read32(u32 *data) { return le32_to_cpu(*data); }

static inline void ioport__write8(u8 *data, u8 value) { *data = value; }

static inline void ioport__write16(u16 *data, u16 value) {
  *data = cpu_to_le16(value);
}

static inline void ioport__write32(u32 *data, u32 value) {
  *data = cpu_to_le32(value);
}

#endif /* KVM__IOPORT_H */
