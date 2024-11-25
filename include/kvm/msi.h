#ifndef LKVM_MSI_H
#define LKVM_MSI_H
#include <linux/types.h>
struct msi_msg {
  u32 address_lo; /* low 32 bits of msi message address中断目标地址低32位 */
  u32 address_hi; /* high 32 bits of msi message address中断目标地址高32位 */
  u32 data;       /* 16 bits of msi message data中断数据 */
};

#endif /* LKVM_MSI_H */
