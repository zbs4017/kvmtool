#include "kvm/kvm-cpu.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include <asm/ptrace.h>
#include <linux/bitops.h>

#define COMPAT_PSR_F_BIT 0x00000040
#define COMPAT_PSR_I_BIT 0x00000080
#define COMPAT_PSR_E_BIT 0x00000200
#define COMPAT_PSR_MODE_SVC 0x00000013

#define SCTLR_EL1_E0E_MASK (1 << 24)
#define SCTLR_EL1_EE_MASK (1 << 25)

static __u64 __core_reg_id(__u64 offset) {
  __u64 id = KVM_REG_ARM64 | KVM_REG_ARM_CORE | offset;

  if (offset < KVM_REG_ARM_CORE_REG(fp_regs))
    id |= KVM_REG_SIZE_U64;
  else if (offset < KVM_REG_ARM_CORE_REG(fp_regs.fpsr))
    id |= KVM_REG_SIZE_U128;
  else
    id |= KVM_REG_SIZE_U32;

  return id;
}

#define ARM64_CORE_REG(x) __core_reg_id(KVM_REG_ARM_CORE_REG(x))

unsigned long kvm_cpu__get_vcpu_mpidr(struct kvm_cpu *vcpu) {
  struct kvm_one_reg reg;
  u64 mpidr;

  reg.id = ARM64_SYS_REG(ARM_CPU_ID, ARM_CPU_ID_MPIDR);
  reg.addr = (u64)&mpidr;
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (get_mpidr vcpu%ld", vcpu->cpu_id);

  return mpidr;
}

static void reset_vcpu_aarch32(struct kvm_cpu *vcpu) {
  struct kvm *kvm = vcpu->kvm;
  struct kvm_one_reg reg;
  u64 data;

  reg.addr = (u64)&data;

  /* pstate = all interrupts masked */
  data = COMPAT_PSR_I_BIT | COMPAT_PSR_F_BIT | COMPAT_PSR_MODE_SVC;
  reg.id = ARM64_CORE_REG(regs.pstate);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (spsr[EL1])");

  /* Secondary cores are stopped awaiting PSCI wakeup */
  if (vcpu->cpu_id != 0)
    return;

  /* r0 = 0 */
  data = 0;
  reg.id = ARM64_CORE_REG(regs.regs[0]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (r0)");

  /* r1 = machine type (-1) */
  data = -1;
  reg.id = ARM64_CORE_REG(regs.regs[1]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (r1)");

  /* r2 = physical address of the device tree blob */
  data = kvm->arch.dtb_guest_start;
  reg.id = ARM64_CORE_REG(regs.regs[2]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (r2)");

  /* pc = start of kernel image */
  data = kvm->arch.kern_guest_start;
  reg.id = ARM64_CORE_REG(regs.pc);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (pc)");
}

static void reset_vcpu_aarch64(struct kvm_cpu *vcpu) {
  struct kvm *kvm = vcpu->kvm;
  struct kvm_one_reg reg;
  u64 data;

  reg.addr = (u64)&data;

  /* pstate = all interrupts masked */
  data = PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h;
  reg.id = ARM64_CORE_REG(regs.pstate);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (spsr[EL1])");

  /* x1...x3 = 0 */
  data = 0;
  reg.id = ARM64_CORE_REG(regs.regs[1]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (x1)");

  reg.id = ARM64_CORE_REG(regs.regs[2]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (x2)");

  reg.id = ARM64_CORE_REG(regs.regs[3]);
  if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
    die_perror("KVM_SET_ONE_REG failed (x3)");

  /* Secondary cores are stopped awaiting PSCI wakeup */
  if (vcpu->cpu_id == 0) {
    /* x0 = physical address of the device tree blob */
    data = kvm->arch.dtb_guest_start;
    reg.id = ARM64_CORE_REG(regs.regs[0]);
    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
      die_perror("KVM_SET_ONE_REG failed (x0)");

    /* pc = start of kernel image */
    data = kvm->arch.kern_guest_start;
    reg.id = ARM64_CORE_REG(regs.pc);
    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
      die_perror("KVM_SET_ONE_REG failed (pc)");
  }
}

void kvm_cpu__select_features(struct kvm *kvm, struct kvm_vcpu_init *init) {
  if (kvm->cfg.arch.aarch32_guest) {
    if (!kvm__supports_extension(kvm, KVM_CAP_ARM_EL1_32BIT))
      die("32bit guests are not supported\n");
    init->features[0] |= 1UL << KVM_ARM_VCPU_EL1_32BIT;
  }

  if (kvm->cfg.arch.has_pmuv3) {
    if (!kvm__supports_extension(kvm, KVM_CAP_ARM_PMU_V3))
      die("PMUv3 is not supported");
    init->features[0] |= 1UL << KVM_ARM_VCPU_PMU_V3;
  }

  /* Enable pointer authentication if available */
  if (kvm__supports_extension(kvm, KVM_CAP_ARM_PTRAUTH_ADDRESS) &&
      kvm__supports_extension(kvm, KVM_CAP_ARM_PTRAUTH_GENERIC)) {
    init->features[0] |= 1UL << KVM_ARM_VCPU_PTRAUTH_ADDRESS;
    init->features[0] |= 1UL << KVM_ARM_VCPU_PTRAUTH_GENERIC;
  }

  /* Enable SVE if available */
  if (kvm__supports_extension(kvm, KVM_CAP_ARM_SVE))
    init->features[0] |= 1UL << KVM_ARM_VCPU_SVE;
}

int sve_vl_parser(const struct option *opt, const char *arg, int unset) {
  struct kvm *kvm = opt->ptr;
  unsigned long val;
  unsigned int vq;

  errno = 0;
  val = strtoull(arg, NULL, 10);
  if (errno == ERANGE)
    die("SVE vector length too large: %s", arg);

  if (!val || (val & (val - 1)))
    die("SVE vector length isn't power of 2: %s", arg);

  vq = val / 128;
  if (vq > KVM_ARM64_SVE_VQ_MAX || vq < KVM_ARM64_SVE_VQ_MIN)
    die("SVE vector length out of range: %s", arg);

  kvm->cfg.arch.sve_max_vq = vq;
  return 0;
}

static int vcpu_configure_sve(struct kvm_cpu *vcpu) {
  unsigned int max_vq = vcpu->kvm->cfg.arch.sve_max_vq;
  int feature = KVM_ARM_VCPU_SVE;

  if (max_vq) {
    unsigned long vls[KVM_ARM64_SVE_VLS_WORDS];
    struct kvm_one_reg reg = {
        .id = KVM_REG_ARM64_SVE_VLS,
        .addr = (u64)&vls,
    };
    unsigned int vq;

    if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg))
      die_perror("KVM_GET_ONE_REG failed (KVM_ARM64_SVE_VLS)");

    if (!test_bit(max_vq - KVM_ARM64_SVE_VQ_MIN, vls))
      die("SVE vector length (%u) not supported", max_vq * 128);

    for (vq = KVM_ARM64_SVE_VQ_MAX; vq > max_vq; vq--)
      clear_bit(vq - KVM_ARM64_SVE_VQ_MIN, vls);

    if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg))
      die_perror("KVM_SET_ONE_REG failed (KVM_ARM64_SVE_VLS)");
  }

  if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_FINALIZE, &feature)) {
    pr_err("KVM_ARM_VCPU_FINALIZE: %s", strerror(errno));
    return -1;
  }

  return 0;
}

int kvm_cpu__configure_features(struct kvm_cpu *vcpu) {
  if (kvm__supports_extension(vcpu->kvm, KVM_CAP_ARM_SVE))
    return vcpu_configure_sve(vcpu);

  return 0;
}

void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu) {
  struct kvm *kvm = vcpu->kvm;
  cpu_set_t *affinity;
  int ret;

  affinity = kvm->arch.vcpu_affinity_cpuset;
  if (affinity) {
    ret = sched_setaffinity(0, sizeof(cpu_set_t), affinity);
    if (ret == -1)
      die_perror("sched_setaffinity");
  }

  if (kvm->cfg.arch.aarch32_guest)
    return reset_vcpu_aarch32(vcpu);
  else
    return reset_vcpu_aarch64(vcpu);
}

int kvm_cpu__get_endianness(struct kvm_cpu *vcpu) {
  struct kvm_one_reg reg;
  u64 psr;
  u64 sctlr;

  /*
   * Quoting the definition given by Peter Maydell:
   *
   * "Endianness of the CPU which does the virtio reset at the
   * point when it does that reset"
   *
   * We first check for an AArch32 guest: its endianness can
   * change when using SETEND, which affects the CPSR.E bit.
   *
   * If we're AArch64, use SCTLR_EL1.E0E if access comes from
   * EL0, and SCTLR_EL1.EE if access comes from EL1.
   */
  reg.id = ARM64_CORE_REG(regs.pstate);
  reg.addr = (u64)&psr;
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (spsr[EL1])");

  if (psr & PSR_MODE32_BIT)
    return (psr & COMPAT_PSR_E_BIT) ? VIRTIO_ENDIAN_BE : VIRTIO_ENDIAN_LE;

  reg.id = ARM64_SYS_REG(ARM_CPU_CTRL, ARM_CPU_CTRL_SCTLR_EL1);
  reg.addr = (u64)&sctlr;
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (SCTLR_EL1)");

  if ((psr & PSR_MODE_MASK) == PSR_MODE_EL0t)
    sctlr &= SCTLR_EL1_E0E_MASK;
  else
    sctlr &= SCTLR_EL1_EE_MASK;
  return sctlr ? VIRTIO_ENDIAN_BE : VIRTIO_ENDIAN_LE;
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu) {
  struct kvm_one_reg reg;
  unsigned long data;
  int debug_fd = kvm_cpu__get_debug_fd();

  reg.addr = (u64)&data;

  dprintf(debug_fd, "\n*pc:\n");
  reg.id = ARM64_CORE_REG(regs.pc);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (show_code @ PC)");

  kvm__dump_mem(vcpu->kvm, data, 32, debug_fd);

  dprintf(debug_fd, "\n*lr:\n");
  reg.id = ARM64_CORE_REG(regs.regs[30]);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (show_code @ LR)");

  kvm__dump_mem(vcpu->kvm, data, 32, debug_fd);
}

void kvm_cpu__show_registers(struct kvm_cpu *vcpu) {
  struct kvm_one_reg reg;
  unsigned long data;
  int debug_fd = kvm_cpu__get_debug_fd();

  reg.addr = (u64)&data;
  dprintf(debug_fd, "\n Registers:\n");

  reg.id = ARM64_CORE_REG(regs.pc);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (pc)");
  dprintf(debug_fd, " PC:    0x%lx\n", data);

  reg.id = ARM64_CORE_REG(regs.pstate);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (pstate)");
  dprintf(debug_fd, " PSTATE:    0x%lx\n", data);

  reg.id = ARM64_CORE_REG(sp_el1);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (sp_el1)");
  dprintf(debug_fd, " SP_EL1:    0x%lx\n", data);

  reg.id = ARM64_CORE_REG(regs.regs[30]);
  if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
    die("KVM_GET_ONE_REG failed (lr)");
  dprintf(debug_fd, " LR:    0x%lx\n", data);
}
