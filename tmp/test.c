#include <fcntl.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// IOMMU映射管理结构
struct iommu_mapping {
  void *vaddr;        // 虚拟地址
  unsigned long iova; // IO虚拟地址
  size_t size;        // 映射大小
  int container_fd;   // VFIO容器fd
};

// 创建IOMMU映射
int create_iommu_mapping(struct iommu_mapping *mapping) {
  struct vfio_iommu_type1_info iommu_info = {.argsz = sizeof(iommu_info)};

  // 获取IOMMU信息
  if (ioctl(mapping->container_fd, VFIO_IOMMU_GET_INFO, &iommu_info) < 0) {
    perror("Failed to get IOMMU info");
    return -1;
  }

  // 检查页大小
  printf("IOMMU page size: %lu\n", iommu_info.page_size);

  // 分配对齐的内存
  size_t aligned_size =
      (mapping->size + iommu_info.page_size - 1) & ~(iommu_info.page_size - 1);

  mapping->vaddr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

  if (mapping->vaddr == MAP_FAILED) {
    perror("Failed to allocate memory");
    return -1;
  }

  // 设置DMA映射
  struct vfio_iommu_type1_dma_map dma_map = {.argsz = sizeof(dma_map),
                                             .flags = VFIO_DMA_MAP_FLAG_READ |
                                                      VFIO_DMA_MAP_FLAG_WRITE,
                                             .vaddr = (__u64)mapping->vaddr,
                                             .iova = mapping->iova,
                                             .size = aligned_size};

  // 建立映射
  if (ioctl(mapping->container_fd, VFIO_IOMMU_MAP_DMA, &dma_map) < 0) {
    perror("Failed to map DMA");
    munmap(mapping->vaddr, aligned_size);
    return -1;
  }

  return 0;
}

// 删除IOMMU映射
int remove_iommu_mapping(struct iommu_mapping *mapping) {
  struct vfio_iommu_type1_dma_unmap dma_unmap = {.argsz = sizeof(dma_unmap),
                                                 .flags = 0,
                                                 .iova = mapping->iova,
                                                 .size = mapping->size};

  // 解除映射
  if (ioctl(mapping->container_fd, VFIO_IOMMU_UNMAP_DMA, &dma_unmap) < 0) {
    perror("Failed to unmap DMA");
    return -1;
  }

  // 释放内存
  if (munmap(mapping->vaddr, mapping->size) < 0) {
    perror("Failed to unmap memory");
    return -1;
  }

  return 0;
}

int main() {
  struct iommu_mapping mapping = {
      .iova = 0x10000,
      .size = 4096 * 1024,      // 4MB
      .container_fd = container // 你的VFIO容器fd
  };

  // 创建映射
  if (create_iommu_mapping(&mapping) < 0) {
    fprintf(stderr, "Failed to create mapping\n");
    return 1;
  }

  // 使用映射的内存
  memset(mapping.vaddr, 0, mapping.size);

  // 设备可以通过IOVA(0x10000)访问这块内存

  // 清理
  remove_iommu_mapping(&mapping);

  return 0;
}