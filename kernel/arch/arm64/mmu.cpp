// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Google Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/arm64/el2_state.h>
#include <arch/arm64/mmu.h>
#include <arch/aspace.h>
#include <arch/mmu.h>
#include <assert.h>
#include <bitmap/raw-bitmap.h>
#include <bitmap/storage.h>
#include <bits.h>
#include <debug.h>
#include <err.h>
#include <fbl/atomic.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <inttypes.h>
#include <kernel/mutex.h>
#include <lib/heap.h>
#include <lib/ktrace.h>
#include <rand.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <trace.h>
#include <vm/arch_vm_aspace.h>
#include <vm/physmap.h>
#include <vm/pmm.h>
#include <vm/vm.h>
#include <zircon/types.h>

#define LOCAL_TRACE 0
#define TRACE_CONTEXT_SWITCH 0

/* ktraces just local to this file */
#define LOCAL_KTRACE 0

#if LOCAL_KTRACE
#define LOCAL_KTRACE0(probe) ktrace_probe0(probe)
#define LOCAL_KTRACE2(probe, x, y) ktrace_probe2(probe, x, y)
#define LOCAL_KTRACE64(probe, x) ktrace_probe64(probe, x)
#else
#define LOCAL_KTRACE0(probe)
#define LOCAL_KTRACE2(probe, x, y)
#define LOCAL_KTRACE64(probe, x)
#endif

static_assert(((long)KERNEL_BASE >> MMU_KERNEL_SIZE_SHIFT) == -1, "");
static_assert(((long)KERNEL_ASPACE_BASE >> MMU_KERNEL_SIZE_SHIFT) == -1, "");
static_assert(MMU_KERNEL_SIZE_SHIFT <= 48, "");
static_assert(MMU_KERNEL_SIZE_SHIFT >= 25, "");

// Static relocated base to prepare for KASLR. Used at early boot and by gdb
// script to know the target relocated address.
// TODO(SEC-31): Choose it randomly.
//是否关闭内核地址空间分布随机化
//KASLR技术允许kernel image加载到VMALLOC区域的任何位置
//好像目前还没完成。。。所以是个固定值
//参考 http://www.wowotech.net/memory_management/441.html
#if DISABLE_KASLR
uint64_t kernel_relocated_base = KERNEL_BASE;
#else
uint64_t kernel_relocated_base = 0xffffffff10000000;
#endif

// The main translation table.
pte_t arm64_kernel_translation_table[MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP] __ALIGNED(MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP * 8);

pte_t* arm64_get_kernel_ptable() {
    return arm64_kernel_translation_table;
}

namespace {

class AsidAllocator {
public:
    // set bitmap 的大小
    AsidAllocator() { bitmap_.Reset(MMU_ARM64_MAX_USER_ASID + 1); }
    ~AsidAllocator() = default;

    zx_status_t Alloc(uint16_t* asid);
    zx_status_t Free(uint16_t asid);

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(AsidAllocator);

    fbl::Mutex lock_;
    uint16_t last_ TA_GUARDED(lock_) = MMU_ARM64_FIRST_USER_ASID - 1;
    
    bitmap::RawBitmapGeneric<bitmap::FixedStorage<MMU_ARM64_MAX_USER_ASID + 1>> bitmap_ TA_GUARDED(lock_);

    static_assert(MMU_ARM64_ASID_BITS <= 16, "");
};

zx_status_t AsidAllocator::Alloc(uint16_t* asid) {
    uint16_t new_asid;

    // use the bitmap allocator to allocate ids in the range of
    // [MMU_ARM64_FIRST_USER_ASID, MMU_ARM64_MAX_USER_ASID]
    // start the search from the last found id + 1 and wrap when hitting the end of the range
    {
        fbl::AutoLock al(&lock_);

        size_t val;
        bool notfound = bitmap_.Get(last_ + 1, MMU_ARM64_MAX_USER_ASID + 1, &val);
        if (unlikely(notfound)) {
            // search again from the start
            notfound = bitmap_.Get(MMU_ARM64_FIRST_USER_ASID, MMU_ARM64_MAX_USER_ASID + 1, &val);
            if (unlikely(notfound)) {
                // ASID 溢出未做处理？
                TRACEF("ARM64: out of ASIDs\n");
                return ZX_ERR_NO_MEMORY;
            }
        }
        bitmap_.SetOne(val);

        DEBUG_ASSERT(val <= UINT16_MAX);

        new_asid = (uint16_t)val;
        last_ = new_asid;
    }

    LTRACEF("new asid %#x\n", new_asid);

    *asid = new_asid;

    return ZX_OK;
}

zx_status_t AsidAllocator::Free(uint16_t asid) {
    LTRACEF("free asid %#x\n", asid);

    fbl::AutoLock al(&lock_);

    bitmap_.ClearOne(asid);

    return ZX_OK;
}

AsidAllocator asid;

} // namespace

// Convert user level mmu flags to flags that go in L1 descriptors.
static pte_t mmu_flags_to_s1_pte_attr(uint flags) {
    pte_t attr = MMU_PTE_ATTR_AF;

    switch (flags & ARCH_MMU_FLAG_CACHE_MASK) {
    case ARCH_MMU_FLAG_CACHED:
        attr |= MMU_PTE_ATTR_NORMAL_MEMORY | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
        break;
    case ARCH_MMU_FLAG_WRITE_COMBINING:
        attr |= MMU_PTE_ATTR_NORMAL_UNCACHED | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
        break;
    case ARCH_MMU_FLAG_UNCACHED:
        attr |= MMU_PTE_ATTR_STRONGLY_ORDERED;
        break;
    case ARCH_MMU_FLAG_UNCACHED_DEVICE:
        attr |= MMU_PTE_ATTR_DEVICE;
        break;
    default:
        PANIC_UNIMPLEMENTED;
    }

    switch (flags & (ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE)) {
    case 0:
        attr |= MMU_PTE_ATTR_AP_P_RO_U_NA;
        break;
    case ARCH_MMU_FLAG_PERM_WRITE:
        attr |= MMU_PTE_ATTR_AP_P_RW_U_NA;
        break;
    case ARCH_MMU_FLAG_PERM_USER:
        attr |= MMU_PTE_ATTR_AP_P_RO_U_RO;
        break;
    case ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE:
        attr |= MMU_PTE_ATTR_AP_P_RW_U_RW;
        break;
    }

    if (!(flags & ARCH_MMU_FLAG_PERM_EXECUTE)) {
        attr |= MMU_PTE_ATTR_UXN | MMU_PTE_ATTR_PXN;
    }
    if (flags & ARCH_MMU_FLAG_NS) {
        attr |= MMU_PTE_ATTR_NON_SECURE;
    }

    return attr;
}

static void s1_pte_attr_to_mmu_flags(pte_t pte, uint* mmu_flags) {
    switch (pte & MMU_PTE_ATTR_ATTR_INDEX_MASK) {
    case MMU_PTE_ATTR_STRONGLY_ORDERED:
        *mmu_flags |= ARCH_MMU_FLAG_UNCACHED;
        break;
    case MMU_PTE_ATTR_DEVICE:
        *mmu_flags |= ARCH_MMU_FLAG_UNCACHED_DEVICE;
        break;
    case MMU_PTE_ATTR_NORMAL_UNCACHED:
        *mmu_flags |= ARCH_MMU_FLAG_WRITE_COMBINING;
        break;
    case MMU_PTE_ATTR_NORMAL_MEMORY:
        *mmu_flags |= ARCH_MMU_FLAG_CACHED;
        break;
    default:
        PANIC_UNIMPLEMENTED;
    }

    *mmu_flags |= ARCH_MMU_FLAG_PERM_READ;
    switch (pte & MMU_PTE_ATTR_AP_MASK) {
    case MMU_PTE_ATTR_AP_P_RW_U_NA:
        *mmu_flags |= ARCH_MMU_FLAG_PERM_WRITE;
        break;
    case MMU_PTE_ATTR_AP_P_RW_U_RW:
        *mmu_flags |= ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_WRITE;
        break;
    case MMU_PTE_ATTR_AP_P_RO_U_NA:
        break;
    case MMU_PTE_ATTR_AP_P_RO_U_RO:
        *mmu_flags |= ARCH_MMU_FLAG_PERM_USER;
        break;
    }

    if (!((pte & MMU_PTE_ATTR_UXN) && (pte & MMU_PTE_ATTR_PXN))) {
        *mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }
    if (pte & MMU_PTE_ATTR_NON_SECURE) {
        *mmu_flags |= ARCH_MMU_FLAG_NS;
    }
}

static pte_t mmu_flags_to_s2_pte_attr(uint flags) {
    pte_t attr = MMU_PTE_ATTR_AF;

    switch (flags & ARCH_MMU_FLAG_CACHE_MASK) {
    case ARCH_MMU_FLAG_CACHED:
        attr |= MMU_S2_PTE_ATTR_NORMAL_MEMORY | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
        break;
    case ARCH_MMU_FLAG_WRITE_COMBINING:
        attr |= MMU_S2_PTE_ATTR_NORMAL_UNCACHED | MMU_PTE_ATTR_SH_INNER_SHAREABLE;
        break;
    case ARCH_MMU_FLAG_UNCACHED:
        attr |= MMU_S2_PTE_ATTR_STRONGLY_ORDERED;
        break;
    case ARCH_MMU_FLAG_UNCACHED_DEVICE:
        attr |= MMU_S2_PTE_ATTR_DEVICE;
        break;
    default:
        PANIC_UNIMPLEMENTED;
    }

    if (flags & ARCH_MMU_FLAG_PERM_WRITE) {
        attr |= MMU_S2_PTE_ATTR_S2AP_RW;
    } else {
        attr |= MMU_S2_PTE_ATTR_S2AP_RO;
    }
    if (!(flags & ARCH_MMU_FLAG_PERM_EXECUTE)) {
        attr |= MMU_S2_PTE_ATTR_XN;
    }

    return attr;
}

static void s2_pte_attr_to_mmu_flags(pte_t pte, uint* mmu_flags) {
    switch (pte & MMU_S2_PTE_ATTR_ATTR_INDEX_MASK) {
    case MMU_S2_PTE_ATTR_STRONGLY_ORDERED:
        *mmu_flags |= ARCH_MMU_FLAG_UNCACHED;
        break;
    case MMU_S2_PTE_ATTR_DEVICE:
        *mmu_flags |= ARCH_MMU_FLAG_UNCACHED_DEVICE;
        break;
    case MMU_S2_PTE_ATTR_NORMAL_UNCACHED:
        *mmu_flags |= ARCH_MMU_FLAG_WRITE_COMBINING;
        break;
    case MMU_S2_PTE_ATTR_NORMAL_MEMORY:
        *mmu_flags |= ARCH_MMU_FLAG_CACHED;
        break;
    default:
        PANIC_UNIMPLEMENTED;
    }

    *mmu_flags |= ARCH_MMU_FLAG_PERM_READ;
    switch (pte & MMU_PTE_ATTR_AP_MASK) {
    case MMU_S2_PTE_ATTR_S2AP_RO:
        break;
    case MMU_S2_PTE_ATTR_S2AP_RW:
        *mmu_flags |= ARCH_MMU_FLAG_PERM_WRITE;
        break;
    default:
        PANIC_UNIMPLEMENTED;
    }

    if (pte & MMU_S2_PTE_ATTR_XN) {
        *mmu_flags |= ARCH_MMU_FLAG_PERM_EXECUTE;
    }
}

// 由虚拟地址查询物理地址
zx_status_t ArmArchVmAspace::Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) {
    fbl::AutoLock a(&lock_);
    return QueryLocked(vaddr, paddr, mmu_flags);
}

// 模拟 MMU 工作
zx_status_t ArmArchVmAspace::QueryLocked(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) {
    ulong index;
    uint index_shift;
    uint page_size_shift;
    pte_t pte;
    pte_t pte_addr;
    uint descriptor_type;
    volatile pte_t* page_table;
    vaddr_t vaddr_rem;

    canary_.Assert();
    LTRACEF("aspace %p, vaddr 0x%lx\n", this, vaddr);

    DEBUG_ASSERT(tt_virt_);

    DEBUG_ASSERT(IsValidVaddr(vaddr));
    if (!IsValidVaddr(vaddr))
        return ZX_ERR_OUT_OF_RANGE;

    // Compute shift values based on if this address space is for kernel or user space.
    // 从 vaddr 中截取 L0 页表的 index
    if (flags_ & ARCH_ASPACE_FLAG_KERNEL) {
        // 如果是虚拟地址是内核页
        // L0 index 在虚拟地址中的 bit 偏移*
        // 等于 39
        index_shift = MMU_KERNEL_TOP_SHIFT;
        // 页内偏移 在虚拟地址中的位置
        // 等于 12
        page_size_shift = MMU_KERNEL_PAGE_SIZE_SHIFT;

        // kernel base 虚拟地址
        vaddr_t kernel_base = ~0UL << MMU_KERNEL_SIZE_SHIFT;
        // 消去虚拟地址高 16bit
        vaddr_rem = vaddr - kernel_base;
        // 获得 L0 的 index(9bit)
        index = vaddr_rem >> index_shift;
        ASSERT(index < MMU_KERNEL_PAGE_TABLE_ENTRIES_TOP);
    } else if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
        index_shift = MMU_GUEST_TOP_SHIFT;
        page_size_shift = MMU_GUEST_PAGE_SIZE_SHIFT;

        vaddr_rem = vaddr;
        index = vaddr_rem >> index_shift;
        ASSERT(index < MMU_GUEST_PAGE_TABLE_ENTRIES_TOP);
    } else {
        index_shift = MMU_USER_TOP_SHIFT;
        page_size_shift = MMU_USER_PAGE_SIZE_SHIFT;

        vaddr_rem = vaddr;
        index = vaddr_rem >> index_shift;
        ASSERT(index < MMU_USER_PAGE_TABLE_ENTRIES_TOP);
    }

    page_table = tt_virt_;

    while (true) {
        // L0 的 index(9bit)
        index = vaddr_rem >> index_shift;
        // 消去 L0 的那 9bit
        // 到现在整个 vaddr 64bit 只剩下 L1-L2-L3-页内偏移
        vaddr_rem -= (vaddr_t)index << index_shift;
        // 从 L0 页表中查询 L1 的地址
        pte = page_table[index];
        descriptor_type = pte & MMU_PTE_DESCRIPTOR_MASK;
        pte_addr = pte & MMU_PTE_OUTPUT_ADDR_MASK;

        LTRACEF("va %#" PRIxPTR ", index %lu, index_shift %u, rem %#" PRIxPTR
                ", pte %#" PRIx64 "\n",
                vaddr, index, index_shift, vaddr_rem, pte);

        if (descriptor_type == MMU_PTE_DESCRIPTOR_INVALID)
            return ZX_ERR_NOT_FOUND;

        //已经循环到业内偏移的低 12 bit，则可以退出循环了，paddr 已经组装完毕！
        if (descriptor_type == ((index_shift > page_size_shift) ? MMU_PTE_L012_DESCRIPTOR_BLOCK : MMU_PTE_L3_DESCRIPTOR_PAGE)) {
            break;
        }

        if (index_shift <= page_size_shift ||
            descriptor_type != MMU_PTE_L012_DESCRIPTOR_TABLE) {
            PANIC_UNIMPLEMENTED;
        }
        // 取下一个等级的页表
        page_table = static_cast<volatile pte_t*>(paddr_to_physmap(pte_addr));
        index_shift -= page_size_shift - 3;
    }

    //最后出来 vaddr_rem 仅有 vaddr 的低 12 bit 的页内偏移
    //还有对应的最后一级 L3 页表的 pte
    if (paddr)
        //则物理地址 = pte 内容(目标页基地址) + 页内偏移
        *paddr = pte_addr + vaddr_rem;

    //如果需要查 mmu flag 的话，填充 flag 返回    
    if (mmu_flags) {
        *mmu_flags = 0;
        if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
            s2_pte_attr_to_mmu_flags(pte, mmu_flags);
        } else {
            s1_pte_attr_to_mmu_flags(pte, mmu_flags);
        }
    }
    LTRACEF("va 0x%lx, paddr 0x%lx, flags 0x%x\n",
            vaddr, paddr ? *paddr : ~0UL, mmu_flags ? *mmu_flags : ~0U);
    return 0;
}

// 分配页表
zx_status_t ArmArchVmAspace::AllocPageTable(paddr_t* paddrp, uint page_size_shift) {
    LTRACEF("page_size_shift %u\n", page_size_shift);

    // currently we only support allocating a single page
    DEBUG_ASSERT(page_size_shift == PAGE_SIZE_SHIFT);

    vm_page_t* page;
    zx_status_t status = pmm_alloc_page(0, &page, paddrp);
    if (status != ZX_OK) {
        return status;
    }

    page->state = VM_PAGE_STATE_MMU;
    pt_pages_++;

    LOCAL_KTRACE0("page table alloc");

    LTRACEF("allocated 0x%lx\n", *paddrp);
    return 0;
}

void ArmArchVmAspace::FreePageTable(void* vaddr, paddr_t paddr, uint page_size_shift) {
    LTRACEF("vaddr %p paddr 0x%lx page_size_shift %u\n", vaddr, paddr, page_size_shift);

    // currently we only support freeing a single page
    DEBUG_ASSERT(page_size_shift == PAGE_SIZE_SHIFT);

    vm_page_t* page;

    LOCAL_KTRACE0("page table free");

    page = paddr_to_vm_page(paddr);
    if (!page) {
        panic("bad page table paddr 0x%lx\n", paddr);
    }
    pmm_free_page(page);

    pt_pages_--;
}

volatile pte_t* ArmArchVmAspace::GetPageTable(vaddr_t index, uint page_size_shift,
                                              volatile pte_t* page_table) {
    pte_t pte;
    paddr_t paddr;
    void* vaddr;

    DEBUG_ASSERT(page_size_shift <= MMU_MAX_PAGE_SIZE_SHIFT);

    pte = page_table[index];
    switch (pte & MMU_PTE_DESCRIPTOR_MASK) {
    case MMU_PTE_DESCRIPTOR_INVALID: {
        zx_status_t ret = AllocPageTable(&paddr, page_size_shift);
        if (ret) {
            TRACEF("failed to allocate page table\n");
            return NULL;
        }
        vaddr = paddr_to_physmap(paddr);

        LTRACEF("allocated page table, vaddr %p, paddr 0x%lx\n", vaddr, paddr);
        memset(vaddr, MMU_PTE_DESCRIPTOR_INVALID, 1U << page_size_shift);

        // ensure that the zeroing is observable from hardware page table walkers
        DMB_ISHST;

        pte = paddr | MMU_PTE_L012_DESCRIPTOR_TABLE;
        page_table[index] = pte;
        LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n",
                page_table, index, pte);
        return static_cast<volatile pte_t*>(vaddr);
    }
    case MMU_PTE_L012_DESCRIPTOR_TABLE:
        paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
        LTRACEF("found page table %#" PRIxPTR "\n", paddr);
        return static_cast<volatile pte_t*>(paddr_to_physmap(paddr));

    case MMU_PTE_L012_DESCRIPTOR_BLOCK:
        return NULL;

    default:
        PANIC_UNIMPLEMENTED;
    }
}

static bool page_table_is_clear(volatile pte_t* page_table, uint page_size_shift) {
    int i;
    int count = 1U << (page_size_shift - 3);
    pte_t pte;

    for (i = 0; i < count; i++) {
        pte = page_table[i];
        if (pte != MMU_PTE_DESCRIPTOR_INVALID) {
            LTRACEF("page_table at %p still in use, index %d is %#" PRIx64 "\n",
                    page_table, i, pte);
            return false;
        }
    }

    LTRACEF("page table at %p is clear\n", page_table);
    return true;
}

// 从缓存中 FLush TBL
// use the appropriate TLB flush instruction to globally flush the modified entry
// terminal is set when flushing at the final level of the page table.
void ArmArchVmAspace::FlushTLBEntry(vaddr_t vaddr, bool terminal) {
    if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
        paddr_t vttbr = arm64_vttbr(asid_, tt_phys_);
        __UNUSED zx_status_t status = arm64_el2_tlbi_ipa(vttbr, vaddr, terminal);
        DEBUG_ASSERT(status == ZX_OK);
    } else if (asid_ == MMU_ARM64_GLOBAL_ASID) {
        // 内核 ASID
        // flush this address on all ASIDs
        if (terminal) {
            ARM64_TLBI(vaale1is, vaddr >> 12);
        } else {
            ARM64_TLBI(vaae1is, vaddr >> 12);
        }
    } else {
        // flush this address for the specific asid
        // 某个用户进程
        if (terminal) {
            ARM64_TLBI(vale1is, vaddr >> 12 | (vaddr_t)asid_ << 48);
        } else {
            ARM64_TLBI(vae1is, vaddr >> 12 | (vaddr_t)asid_ << 48);
        }
    }
}

// 取消映射某段地址
// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
ssize_t ArmArchVmAspace::UnmapPageTable(vaddr_t vaddr, vaddr_t vaddr_rel,
                                        size_t size, uint index_shift,
                                        uint page_size_shift,
                                        volatile pte_t* page_table) {
    volatile pte_t* next_page_table;
    vaddr_t index;
    size_t chunk_size;
    vaddr_t vaddr_rem;
    vaddr_t block_size;
    vaddr_t block_mask;
    pte_t pte;
    paddr_t page_table_paddr;
    size_t unmap_size;

    LTRACEF("vaddr 0x%lx, vaddr_rel 0x%lx, size 0x%lx, index shift %u, page_size_shift %u, page_table %p\n",
            vaddr, vaddr_rel, size, index_shift, page_size_shift, page_table);

    unmap_size = 0;
    while (size) {
        block_size = 1UL << index_shift;
        block_mask = block_size - 1;
        vaddr_rem = vaddr_rel & block_mask;
        chunk_size = MIN(size, block_size - vaddr_rem);
        index = vaddr_rel >> index_shift;

        pte = page_table[index];

        if (index_shift > page_size_shift &&
            (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
            page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
            next_page_table = static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));
            UnmapPageTable(vaddr, vaddr_rem, chunk_size,
                           index_shift - (page_size_shift - 3),
                           page_size_shift, next_page_table);
            if (chunk_size == block_size ||
                page_table_is_clear(next_page_table, page_size_shift)) {
                LTRACEF("pte %p[0x%lx] = 0 (was page table)\n", page_table, index);
                page_table[index] = MMU_PTE_DESCRIPTOR_INVALID;

                // ensure that the update is observable from hardware page table walkers
                DMB_ISHST;

                // flush the non terminal TLB entry
                FlushTLBEntry(vaddr, false);

                FreePageTable(const_cast<pte_t*>(next_page_table), page_table_paddr,
                              page_size_shift);
            }
        } else if (pte) {
            LTRACEF("pte %p[0x%lx] = 0\n", page_table, index);
            page_table[index] = MMU_PTE_DESCRIPTOR_INVALID;

            // ensure that the update is observable from hardware page table walkers
            DMB_ISHST;

            // flush the terminal TLB entry
            FlushTLBEntry(vaddr, true);
        } else {
            LTRACEF("pte %p[0x%lx] already clear\n", page_table, index);
        }
        vaddr += chunk_size;
        vaddr_rel += chunk_size;
        size -= chunk_size;
        unmap_size += chunk_size;
    }

    return unmap_size;
}

// map 一页
// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
ssize_t ArmArchVmAspace::MapPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel_in,
                                      paddr_t paddr_in, size_t size_in,
                                      pte_t attrs, uint index_shift,
                                      uint page_size_shift,
                                      volatile pte_t* page_table) {
    ssize_t ret;
    volatile pte_t* next_page_table;
    vaddr_t index;
    vaddr_t vaddr = vaddr_in;
    vaddr_t vaddr_rel = vaddr_rel_in;
    paddr_t paddr = paddr_in;
    size_t size = size_in;
    size_t chunk_size;
    vaddr_t vaddr_rem;
    vaddr_t block_size;
    vaddr_t block_mask;
    pte_t pte;
    size_t mapped_size;

    LTRACEF("vaddr %#" PRIxPTR ", vaddr_rel %#" PRIxPTR ", paddr %#" PRIxPTR
            ", size %#zx, attrs %#" PRIx64
            ", index shift %u, page_size_shift %u, page_table %p\n",
            vaddr, vaddr_rel, paddr, size, attrs,
            index_shift, page_size_shift, page_table);

    if ((vaddr_rel | paddr | size) & ((1UL << page_size_shift) - 1)) {
        TRACEF("not page aligned\n");
        return ZX_ERR_INVALID_ARGS;
    }

    // 初始 index_shift = L0 的 shift = 39
    // vaddr_rel = vaddr 消去高 16 bit
    // size = 页大小

    mapped_size = 0;
    while (size) {
        //以第一次循环为例，下次循环以此类推
        //所能管理的内存块大小
        //初始值 2^39
        block_size = 1UL << index_shift;
        //2^39 - 1 = 000000000000111111111111111
        block_mask = block_size - 1;
        //消去 39 - 47 bit，现在 vaddr_rem 只有 0 - 38 bit的值了
        vaddr_rem = vaddr_rel & block_mask;
        //block_size - vaddr_rem = 对于 size 大小的内偏移 = 页内偏移
        chunk_size = MIN(size, block_size - vaddr_rem);

        //现在循环的等级页表的 index(9 bit)，现在是 L0,即 39 - 47 bit 的值
        index = vaddr_rel >> index_shift;

        //发现这个不是最后一级页表
        if (((vaddr_rel | paddr) & block_mask) ||
            (chunk_size != block_size) ||
            (index_shift > MMU_PTE_DESCRIPTOR_BLOCK_MAX_SHIFT)) {

            //从这一级页表中查询 index，得到下一级页表    
            next_page_table = GetPageTable(index, page_size_shift, page_table);
            if (!next_page_table)
                goto err;

            //如果还有下一级页面表，则递归 Map
            ret = MapPageTable(vaddr, vaddr_rem, paddr, chunk_size, attrs,
                               index_shift - (page_size_shift - 3),
                               page_size_shift, next_page_table);
            if (ret < 0)
                goto err;
        } else {

            // 最后一级页表对应的页表项
            pte = page_table[index];
            if (pte) {
                TRACEF("page table entry already in use, index %#" PRIxPTR ", %#" PRIx64 "\n",
                       index, pte);
                goto err;
            }

            //设置页表项为要 map 的物理地址
            pte = paddr | attrs;
            if (index_shift > page_size_shift)
                pte |= MMU_PTE_L012_DESCRIPTOR_BLOCK;
            else
                pte |= MMU_PTE_L3_DESCRIPTOR_PAGE;
            if (!(flags_ & ARCH_ASPACE_FLAG_GUEST))
                pte |= MMU_PTE_ATTR_NON_GLOBAL;
            LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n",
                    page_table, index, pte);
            //再次填充到页表        
            page_table[index] = pte;
        }
        vaddr += chunk_size;
        vaddr_rel += chunk_size;
        paddr += chunk_size;
        size -= chunk_size;
        mapped_size += chunk_size;
    }

    return mapped_size;

err:
    UnmapPageTable(vaddr_in, vaddr_rel_in, size_in - size, index_shift,
                   page_size_shift, page_table);
    return ZX_ERR_INTERNAL;
}

// NOTE: caller must DSB afterwards to ensure TLB entries are flushed
int ArmArchVmAspace::ProtectPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel_in,
                                      size_t size_in, pte_t attrs,
                                      uint index_shift, uint page_size_shift,
                                      volatile pte_t* page_table) {
    int ret;
    volatile pte_t* next_page_table;
    vaddr_t index;
    vaddr_t vaddr = vaddr_in;
    vaddr_t vaddr_rel = vaddr_rel_in;
    size_t size = size_in;
    size_t chunk_size;
    vaddr_t vaddr_rem;
    vaddr_t block_size;
    vaddr_t block_mask;
    paddr_t page_table_paddr;
    pte_t pte;

    LTRACEF("vaddr %#" PRIxPTR ", vaddr_rel %#" PRIxPTR ", size %#" PRIxPTR
            ", attrs %#" PRIx64
            ", index shift %u, page_size_shift %u, page_table %p\n",
            vaddr, vaddr_rel, size, attrs,
            index_shift, page_size_shift, page_table);

    if ((vaddr_rel | size) & ((1UL << page_size_shift) - 1)) {
        TRACEF("not page aligned\n");
        return ZX_ERR_INVALID_ARGS;
    }

    while (size) {
        block_size = 1UL << index_shift;
        block_mask = block_size - 1;
        vaddr_rem = vaddr_rel & block_mask;
        chunk_size = MIN(size, block_size - vaddr_rem);
        index = vaddr_rel >> index_shift;
        pte = page_table[index];

        if (index_shift > page_size_shift &&
            (pte & MMU_PTE_DESCRIPTOR_MASK) == MMU_PTE_L012_DESCRIPTOR_TABLE) {
            page_table_paddr = pte & MMU_PTE_OUTPUT_ADDR_MASK;
            next_page_table = static_cast<volatile pte_t*>(paddr_to_physmap(page_table_paddr));
            ret = ProtectPageTable(vaddr, vaddr_rem, chunk_size, attrs,
                                   index_shift - (page_size_shift - 3),
                                   page_size_shift, next_page_table);
            if (ret != 0) {
                goto err;
            }
        } else if (pte) {
            pte = (pte & ~MMU_PTE_PERMISSION_MASK) | attrs;
            LTRACEF("pte %p[%#" PRIxPTR "] = %#" PRIx64 "\n",
                    page_table, index, pte);
            page_table[index] = pte;

            // ensure that the update is observable from hardware page table walkers
            DMB_ISHST;

            // flush the terminal TLB entry
            FlushTLBEntry(vaddr, true);
        } else {
            LTRACEF("page table entry does not exist, index %#" PRIxPTR
                    ", %#" PRIx64 "\n",
                    index, pte);
        }
        vaddr += chunk_size;
        vaddr_rel += chunk_size;
        size -= chunk_size;
    }

    return 0;

err:
    // TODO: Unroll any changes we've made, though in practice if we've reached
    // here there's a programming bug since the higher level region abstraction
    // should guard against us trying to change permissions on an umapped page
    return ZX_ERR_INTERNAL;
}

// internal routine to map a run of pages

// map 一页
ssize_t ArmArchVmAspace::MapPages(vaddr_t vaddr, paddr_t paddr, size_t size,
                                  pte_t attrs, vaddr_t vaddr_base, uint top_size_shift,
                                  uint top_index_shift, uint page_size_shift) {
    vaddr_t vaddr_rel = vaddr - vaddr_base;
    vaddr_t vaddr_rel_max = 1UL << top_size_shift;

    LTRACEF("vaddr %#" PRIxPTR ", paddr %#" PRIxPTR ", size %#" PRIxPTR
            ", attrs %#" PRIx64 ", asid %#x\n",
            vaddr, paddr, size, attrs, asid_);

    if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
        TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR ", size %#" PRIxPTR "\n",
               vaddr, size, vaddr_base, vaddr_rel_max);
        return ZX_ERR_INVALID_ARGS;
    }

    LOCAL_KTRACE64("mmu map", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));
    ssize_t ret = MapPageTable(vaddr, vaddr_rel, paddr, size, attrs,
                               top_index_shift, page_size_shift, tt_virt_);
    DSB;
    return ret;
}

ssize_t ArmArchVmAspace::UnmapPages(vaddr_t vaddr, size_t size,
                                    vaddr_t vaddr_base,
                                    uint top_size_shift,
                                    uint top_index_shift,
                                    uint page_size_shift) {
    vaddr_t vaddr_rel = vaddr - vaddr_base;
    vaddr_t vaddr_rel_max = 1UL << top_size_shift;

    LTRACEF("vaddr 0x%lx, size 0x%lx, asid 0x%x\n", vaddr, size, asid_);

    if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
        TRACEF("vaddr 0x%lx, size 0x%lx out of range vaddr 0x%lx, size 0x%lx\n",
               vaddr, size, vaddr_base, vaddr_rel_max);
        return ZX_ERR_INVALID_ARGS;
    }

    LOCAL_KTRACE64("mmu unmap", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

    ssize_t ret = UnmapPageTable(vaddr, vaddr_rel, size, top_index_shift,
                                 page_size_shift, tt_virt_);
    DSB;
    return ret;
}

zx_status_t ArmArchVmAspace::ProtectPages(vaddr_t vaddr, size_t size, pte_t attrs,
                                          vaddr_t vaddr_base, uint top_size_shift,
                                          uint top_index_shift, uint page_size_shift) {
    vaddr_t vaddr_rel = vaddr - vaddr_base;
    vaddr_t vaddr_rel_max = 1UL << top_size_shift;

    LTRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR ", attrs %#" PRIx64
            ", asid %#x\n",
            vaddr, size, attrs, asid_);

    if (vaddr_rel > vaddr_rel_max - size || size > vaddr_rel_max) {
        TRACEF("vaddr %#" PRIxPTR ", size %#" PRIxPTR " out of range vaddr %#" PRIxPTR ", size %#" PRIxPTR "\n",
               vaddr, size, vaddr_base, vaddr_rel_max);
        return ZX_ERR_INVALID_ARGS;
    }

    LOCAL_KTRACE64("mmu protect", (vaddr & ~PAGE_MASK) | ((size >> PAGE_SIZE_SHIFT) & PAGE_MASK));

    zx_status_t ret = ProtectPageTable(vaddr, vaddr_rel, size, attrs,
                                       top_index_shift, page_size_shift,
                                       tt_virt_);
    DSB;
    return ret;
}

void ArmArchVmAspace::MmuParamsFromFlags(uint mmu_flags,
                                         pte_t* attrs, vaddr_t* vaddr_base,
                                         uint* top_size_shift, uint* top_index_shift,
                                         uint* page_size_shift) {

    if (flags_ & ARCH_ASPACE_FLAG_KERNEL) {
        if (attrs) {
            *attrs = mmu_flags_to_s1_pte_attr(mmu_flags);
        }
        *vaddr_base = ~0UL << MMU_KERNEL_SIZE_SHIFT;
        *top_size_shift = MMU_KERNEL_SIZE_SHIFT;
        *top_index_shift = MMU_KERNEL_TOP_SHIFT;
        *page_size_shift = MMU_KERNEL_PAGE_SIZE_SHIFT;
    } else if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
        if (attrs) {
            *attrs = mmu_flags_to_s2_pte_attr(mmu_flags);
        }
        *vaddr_base = 0;
        *top_size_shift = MMU_GUEST_SIZE_SHIFT;
        *top_index_shift = MMU_GUEST_TOP_SHIFT;
        *page_size_shift = MMU_GUEST_PAGE_SIZE_SHIFT;
    } else {
        if (attrs) {
            *attrs = mmu_flags_to_s1_pte_attr(mmu_flags);
        }
        *vaddr_base = 0;
        *top_size_shift = MMU_USER_SIZE_SHIFT;
        *top_index_shift = MMU_USER_TOP_SHIFT;
        *page_size_shift = MMU_USER_PAGE_SIZE_SHIFT;
    }
}

zx_status_t ArmArchVmAspace::MapContiguous(vaddr_t vaddr, paddr_t paddr, size_t count,
                                           uint mmu_flags, size_t* mapped) {
    canary_.Assert();
    LTRACEF("vaddr %#" PRIxPTR " paddr %#" PRIxPTR " count %zu flags %#x\n",
            vaddr, paddr, count, mmu_flags);

    DEBUG_ASSERT(tt_virt_);

    DEBUG_ASSERT(IsValidVaddr(vaddr));
    if (!IsValidVaddr(vaddr))
        return ZX_ERR_OUT_OF_RANGE;

    if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ))
        return ZX_ERR_INVALID_ARGS;

    // paddr and vaddr must be aligned.
    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
    if (!IS_PAGE_ALIGNED(vaddr) || !IS_PAGE_ALIGNED(paddr))
        return ZX_ERR_INVALID_ARGS;

    if (count == 0)
        return ZX_OK;

    ssize_t ret;
    {
        fbl::AutoLock a(&lock_);
        pte_t attrs;
        vaddr_t vaddr_base;
        uint top_size_shift, top_index_shift, page_size_shift;
        MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                           &page_size_shift);
        ret = MapPages(vaddr, paddr, count * PAGE_SIZE,
                       attrs, vaddr_base, top_size_shift,
                       top_index_shift, page_size_shift);
    }

    if (mapped) {
        *mapped = (ret > 0) ? (ret / PAGE_SIZE) : 0u;
        DEBUG_ASSERT(*mapped <= count);
    }

    return (ret < 0) ? (zx_status_t)ret : ZX_OK;
}

//Map 一段地址 paddr -> vaddr
zx_status_t ArmArchVmAspace::Map(vaddr_t vaddr, paddr_t* phys, size_t count, uint mmu_flags,
                                 size_t* mapped) {
    canary_.Assert();
    LTRACEF("vaddr %#" PRIxPTR " count %zu flags %#x\n",
            vaddr, count, mmu_flags);

    DEBUG_ASSERT(tt_virt_);

    DEBUG_ASSERT(IsValidVaddr(vaddr));
    if (!IsValidVaddr(vaddr))
        return ZX_ERR_OUT_OF_RANGE;

    // 检查 phys[] 是不是为页大小的数组即页大小的 count 倍    
    for (size_t i = 0; i < count; ++i) {
        DEBUG_ASSERT(IS_PAGE_ALIGNED(phys[i]));
        if (!IS_PAGE_ALIGNED(phys[i]))
            return ZX_ERR_INVALID_ARGS;
    }

    //如果这段内存被设置为不可读，那么 map 将无意义，直接返回报错
    if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ))
        return ZX_ERR_INVALID_ARGS;

    // vaddr must be aligned.
    // 虚拟地址也必须与页大小对齐
    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    if (!IS_PAGE_ALIGNED(vaddr))
        return ZX_ERR_INVALID_ARGS;

    if (count == 0)
        return ZX_OK;

    size_t total_mapped = 0;
    //同步块
    {   
        fbl::AutoLock a(&lock_);
        pte_t attrs;
        vaddr_t vaddr_base;
        // 初始化虚拟地址各个段偏移
        // 以内核空间为例
        // 页大小为 4k，所以页面偏移占用 12 bit
        // 有4级页表，每级页表占用 9 bit
        // 页表基地址 占用 16 bit
        // 16 + 4 * 9 + 12 = 64
        // top_size_shift 虚拟地址高 16 位，页表基地址
        // top_index_shift L0 页表偏移 64 - （top_size_shift + 9） = 39
        // page_size_shift = 12
        // vaddr_base = kernel base
        uint top_size_shift, top_index_shift, page_size_shift;
        MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                           &page_size_shift);

        ssize_t ret;
        size_t idx = 0;

        // 代码块结束的时候自动调用
        auto undo = fbl::MakeAutoCall([&]() TA_NO_THREAD_SAFETY_ANALYSIS {
            if (idx > 0) {
                UnmapPages(vaddr, idx * PAGE_SIZE, vaddr_base, top_size_shift,
                           top_index_shift, page_size_shift);
            }
        });

        vaddr_t v = vaddr;
        for (; idx < count; ++idx) {
            paddr_t paddr = phys[idx];
            DEBUG_ASSERT(IS_PAGE_ALIGNED(paddr));
            // TODO: optimize by not DSBing inside each of these calls
            // 一页一页的 map
            ret = MapPages(v, paddr, PAGE_SIZE,
                           attrs, vaddr_base, top_size_shift,
                           top_index_shift, page_size_shift);
            if (ret < 0) {
                return static_cast<zx_status_t>(ret);
            }

            v += PAGE_SIZE;
            total_mapped += ret / PAGE_SIZE;
        }
        undo.cancel();
    }
    DEBUG_ASSERT(total_mapped <= count);

    if (mapped) {
        *mapped = total_mapped;
    }

    return ZX_OK;
}

zx_status_t ArmArchVmAspace::Unmap(vaddr_t vaddr, size_t count, size_t* unmapped) {
    canary_.Assert();
    LTRACEF("vaddr %#" PRIxPTR " count %zu\n", vaddr, count);

    DEBUG_ASSERT(tt_virt_);

    DEBUG_ASSERT(IsValidVaddr(vaddr));

    if (!IsValidVaddr(vaddr))
        return ZX_ERR_OUT_OF_RANGE;

    DEBUG_ASSERT(IS_PAGE_ALIGNED(vaddr));
    if (!IS_PAGE_ALIGNED(vaddr))
        return ZX_ERR_INVALID_ARGS;

    fbl::AutoLock a(&lock_);

    ssize_t ret;
    {
        vaddr_t vaddr_base;
        uint top_size_shift, top_index_shift, page_size_shift;
        MmuParamsFromFlags(0, nullptr, &vaddr_base, &top_size_shift, &top_index_shift,
                           &page_size_shift);

        ret = UnmapPages(vaddr, count * PAGE_SIZE,
                         vaddr_base, top_size_shift,
                         top_index_shift, page_size_shift);
    }

    if (unmapped) {
        *unmapped = (ret > 0) ? (ret / PAGE_SIZE) : 0u;
        DEBUG_ASSERT(*unmapped <= count);
    }

    return (ret < 0) ? (zx_status_t)ret : 0;
}

zx_status_t ArmArchVmAspace::Protect(vaddr_t vaddr, size_t count, uint mmu_flags) {
    canary_.Assert();

    if (!IsValidVaddr(vaddr))
        return ZX_ERR_INVALID_ARGS;

    if (!IS_PAGE_ALIGNED(vaddr))
        return ZX_ERR_INVALID_ARGS;

    if (!(mmu_flags & ARCH_MMU_FLAG_PERM_READ))
        return ZX_ERR_INVALID_ARGS;

    fbl::AutoLock a(&lock_);

    int ret;
    {
        pte_t attrs;
        vaddr_t vaddr_base;
        uint top_size_shift, top_index_shift, page_size_shift;
        MmuParamsFromFlags(mmu_flags, &attrs, &vaddr_base, &top_size_shift, &top_index_shift,
                           &page_size_shift);

        ret = ProtectPages(vaddr, count * PAGE_SIZE,
                           attrs, vaddr_base,
                           top_size_shift, top_index_shift, page_size_shift);
    }

    return ret;
}

zx_status_t ArmArchVmAspace::Init(vaddr_t base, size_t size, uint flags) {
    canary_.Assert();
    LTRACEF("aspace %p, base %#" PRIxPTR ", size 0x%zx, flags 0x%x\n",
            this, base, size, flags);

    fbl::AutoLock a(&lock_);

    // Validate that the base + size is sane and doesn't wrap.
    DEBUG_ASSERT(size > PAGE_SIZE);
    DEBUG_ASSERT(base + size - 1 > base);

    flags_ = flags;
    // 内核空间
    if (flags & ARCH_ASPACE_FLAG_KERNEL) {
        // At the moment we can only deal with address spaces as globally defined.
        DEBUG_ASSERT(base == ~0UL << MMU_KERNEL_SIZE_SHIFT);
        DEBUG_ASSERT(size == 1UL << MMU_KERNEL_SIZE_SHIFT);

        base_ = base;
        size_ = size;
        tt_virt_ = arm64_kernel_translation_table;
        tt_phys_ = vaddr_to_paddr(const_cast<pte_t*>(tt_virt_));
        asid_ = (uint16_t)MMU_ARM64_GLOBAL_ASID;
    } else {
        
        if (flags & ARCH_ASPACE_FLAG_GUEST) {
            // Guest 内核空间
            // 无需 asid
            DEBUG_ASSERT(base + size <= 1UL << MMU_GUEST_SIZE_SHIFT);
        } else {
            // 用户空间
            DEBUG_ASSERT(base + size <= 1UL << MMU_USER_SIZE_SHIFT);
            // 分配该进程的 ASID
            // 分配失败则返回
            if (asid.Alloc(&asid_) != ZX_OK)
                return ZX_ERR_NO_MEMORY;
        }

        base_ = base;
        size_ = size;

        paddr_t pa;
        vm_page_t* page;
        zx_status_t status = pmm_alloc_page(0, &page, &pa);
        if (status != ZX_OK) {
            return status;
        }
        page->state = VM_PAGE_STATE_MMU;

        volatile pte_t* va = static_cast<volatile pte_t*>(paddr_to_physmap(pa));

        tt_virt_ = va;
        tt_phys_ = pa;

        // zero the top level translation table.
        // XXX remove when PMM starts returning pre-zeroed pages.
        arch_zero_page(const_cast<pte_t*>(tt_virt_));
    }
    pt_pages_ = 1;

    LTRACEF("tt_phys %#" PRIxPTR " tt_virt %p\n", tt_phys_, tt_virt_);

    return ZX_OK;
}

zx_status_t ArmArchVmAspace::Destroy() {
    canary_.Assert();
    LTRACEF("aspace %p\n", this);

    fbl::AutoLock a(&lock_);

    DEBUG_ASSERT((flags_ & ARCH_ASPACE_FLAG_KERNEL) == 0);

    // XXX make sure it's not mapped

    vm_page_t* page = paddr_to_vm_page(tt_phys_);
    DEBUG_ASSERT(page);
    pmm_free_page(page);

    // 需要 flush 所有 CPU 的缓存中该 ASID 空间残余的 TBL
    // ARM64 可以使用 tlbi 指令实现这一操作
    // X86 是不支持的，需要使用核间中断通知每个 CPU 自行操作
    if (flags_ & ARCH_ASPACE_FLAG_GUEST) {
        paddr_t vttbr = arm64_vttbr(asid_, tt_phys_);
        __UNUSED zx_status_t status = arm64_el2_tlbi_vmid(vttbr);
        DEBUG_ASSERT(status == ZX_OK);
    } else {
        ARM64_TLBI(ASIDE1IS, asid_);
        asid.Free(asid_);
        asid_ = MMU_ARM64_UNUSED_ASID;
    }

    return ZX_OK;
}

// 切换内存地址空间，即切换页表
void ArmArchVmAspace::ContextSwitch(ArmArchVmAspace* old_aspace, ArmArchVmAspace* aspace) {
    if (TRACE_CONTEXT_SWITCH)
        TRACEF("aspace %p\n", aspace);

    uint64_t tcr;
    uint64_t ttbr;
    if (aspace) {
        aspace->canary_.Assert();
        DEBUG_ASSERT((aspace->flags_ & (ARCH_ASPACE_FLAG_KERNEL | ARCH_ASPACE_FLAG_GUEST)) == 0);

        tcr = MMU_TCR_FLAGS_USER;
        ttbr = ((uint64_t)aspace->asid_ << 48) | aspace->tt_phys_;
        ARM64_WRITE_SYSREG(ttbr0_el1, ttbr);

        if (TRACE_CONTEXT_SWITCH)
            TRACEF("ttbr %#" PRIx64 ", tcr %#" PRIx64 "\n", ttbr, tcr);

    } else {
        tcr = MMU_TCR_FLAGS_KERNEL;

        if (TRACE_CONTEXT_SWITCH)
            TRACEF("tcr %#" PRIx64 "\n", tcr);
    }

    ARM64_WRITE_SYSREG(tcr_el1, tcr);
}

void arch_zero_page(void* _ptr) {
    uintptr_t ptr = (uintptr_t)_ptr;

    uint32_t zva_size = arm64_zva_size;
    uintptr_t end_ptr = ptr + PAGE_SIZE;
    do {
        __asm volatile("dc zva, %0" ::"r"(ptr));
        ptr += zva_size;
    } while (ptr != end_ptr);
}

zx_status_t arm64_mmu_translate(vaddr_t va, paddr_t* pa, bool user, bool write) {
    // disable interrupts around this operation to make the at/par instruction combination atomic
    spin_lock_saved_state_t state;
    arch_interrupt_save(&state, ARCH_DEFAULT_SPIN_LOCK_FLAG_INTERRUPTS);

    if (user) {
        if (write) {
            __asm__ volatile("at s1e0w, %0" ::"r"(va)
                             : "memory");
        } else {
            __asm__ volatile("at s1e0r, %0" ::"r"(va)
                             : "memory");
        }
    } else {
        if (write) {
            __asm__ volatile("at s1e1w, %0" ::"r"(va)
                             : "memory");
        } else {
            __asm__ volatile("at s1e1r, %0" ::"r"(va)
                             : "memory");
        }
    }

    uint64_t par;
    par = ARM64_READ_SYSREG(par_el1);

    arch_interrupt_restore(state, ARCH_DEFAULT_SPIN_LOCK_FLAG_INTERRUPTS);

    // if bit 0 is clear, the translation succeeded
    if (BIT(par, 0))
        return ZX_ERR_NO_MEMORY;

    // physical address is stored in bits [51..12], naturally aligned
    *pa = BITS(par, 51, 12) | (va & (PAGE_SIZE - 1));

    return ZX_OK;
}

ArmArchVmAspace::ArmArchVmAspace() {}

ArmArchVmAspace::~ArmArchVmAspace() {
    // TODO: check that we've destroyed the aspace
}

vaddr_t ArmArchVmAspace::PickSpot(vaddr_t base, uint prev_region_mmu_flags,
                                  vaddr_t end, uint next_region_mmu_flags,
                                  vaddr_t align, size_t size, uint mmu_flags) {
    canary_.Assert();
    return PAGE_ALIGN(base);
}
