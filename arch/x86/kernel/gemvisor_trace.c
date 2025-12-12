// SPDX-License-Identifier: GPL-2.0
/*
 * Gemvisor guest-to-hypervisor trace subsystem
 *
 * This module provides a low-overhead mechanism for sending structured
 * binary events from the guest kernel to the hypervisor. Events are written
 * to a shared memory ring buffer, minimizing perturbation to guest execution.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/panic.h>
#include <linux/stacktrace.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <asm/early_ioremap.h>
#include <asm/gemvisor.h>
#include <asm/gemvisor_trace.h>
#include <asm/irq_regs.h>
#include <asm/msr.h>
#include <asm/pvclock.h>
#include <asm/kvmclock.h>
#include <asm/processor.h>
#include <asm/fpu/xcr.h>
#include <asm/pgtable.h>
#include <asm/pgtable_types.h>

#define MIN_EVENT_SIZE 32

static struct gem_trace_header __iomem *trace_header;
static void __iomem *trace_buffer;
static bool trace_enabled;
static bool gemvisor_detected;
static u32 ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;

/* Trace hypercall commands */
#define TRACE_CMD_INIT		1
#define TRACE_CMD_FLUSH		2

struct gem_trace_reg_snapshot {
	u64 gs_base;
	u64 kernel_gs_base;
	u64 fs_base;
	u64 cr3;
	u64 xcr0;
	u64 cr2;		/* Fault linear address */
	u64 cr4;		/* PCID, SMEP, SMAP flags */
	u64 rsp;		/* Stack pointer from pt_regs */
	u64 rflags;		/* Flags from pt_regs */
	u64 hash;
} __packed;

#define GEM_TRACE_REG_BLOCK_SIZE ((u8)sizeof(struct gem_trace_reg_snapshot))

static u64 gem_trace_read_vtime(void)
{
	struct pvclock_vsyscall_time_info *hvclock = this_cpu_hvclock();

	if (!hvclock)
		return 0;

	return pvclock_clocksource_read_nowd(&hvclock->pvti);
}

static u64 gem_trace_fnv64(const void *data, size_t len)
{
	const u8 *bytes = data;
	u64 hash = 0xcbf29ce484222325ULL;

	while (len--) {
		hash ^= *bytes++;
		hash *= 0x100000001b3ULL;
	}

	return hash;
}

static struct gem_trace_reg_snapshot gem_trace_collect_regs(struct pt_regs *regs)
{
	struct gem_trace_reg_snapshot snap = { 0 };

	rdmsrl(MSR_GS_BASE, snap.gs_base);
	rdmsrl(MSR_KERNEL_GS_BASE, snap.kernel_gs_base);
	rdmsrl(MSR_FS_BASE, snap.fs_base);
	snap.cr3 = __read_cr3();
	snap.cr4 = __read_cr4();
	/* XGETBV requires CR4.OSXSAVE (bit 18) to be set */
	if (snap.cr4 & X86_CR4_OSXSAVE)
		snap.xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
	snap.cr2 = read_cr2();

	/* Extract rsp and rflags from pt_regs if available */
	if (regs) {
		snap.rsp = regs->sp;
		snap.rflags = regs->flags;
	}

	/*
	 * Hash the pt_regs (when available) together with the raw register snapshot
	 * to provide a compact, deterministic fingerprint without bloating payloads.
	 */
	snap.hash = gem_trace_fnv64(&snap, sizeof(snap) - sizeof(snap.hash));
	if (regs)
		snap.hash = gem_trace_fnv64(regs, sizeof(*regs)) ^ snap.hash;

	return snap;
}

static u8 gem_trace_capture_stack(struct pt_regs *regs, u64 *out, u8 max_depth)
{
	unsigned long entries[GEM_TRACE_MAX_STACK_DEPTH] = { 0 };
	u8 depth, i;

	if (max_depth > GEM_TRACE_MAX_STACK_DEPTH)
		max_depth = GEM_TRACE_MAX_STACK_DEPTH;

	/* Prefer kernel-provided unwinders for consistency with stacktrace.h helpers. */
	if (regs)
		depth = stack_trace_save_regs(regs, entries, max_depth, 0);
	else
		depth = stack_trace_save(entries, max_depth, 0);

	/* Fallback: if unwinder returned very few frames (common on user faults), grab current kernel stack. */
	if (depth < 4)
		depth = stack_trace_save(entries, max_depth, 1); /* skip this helper frame */

	for (i = 0; i < depth; i++)
		out[i] = (u64)entries[i];

	return depth;
}

void __init gemvisor_trace_init(void)
{
	void __iomem *ptr;
	u32 magic;
	struct gem_trace_header __iomem *hdr;

	/*
	 * Only map a single page during early boot. early_ioremap has a limit
	 * of 64 pages (256KB) per mapping, but the full trace buffer is 16MB.
	 * We only need to read the header here; the full buffer is mapped
	 * later via ioremap in gemvisor_trace_remap().
	 */
	ptr = early_ioremap(GEMVISOR_TRACE_PAGE_GPA, PAGE_SIZE);
	if (!ptr) {
		pr_info("gemvisor-trace: early_ioremap failed\n");
		return;
	}

	/* Check for gemvisor magic */
	magic = readl(ptr);
	if (magic != GEMVISOR_TRACE_MAGIC) {
		early_iounmap(ptr, PAGE_SIZE);
		pr_info("gemvisor-trace: not running under gemvisor (magic=%#x)\n", magic);
		return;
	}

	/* Read ring_size from header before unmapping */
	hdr = (struct gem_trace_header __iomem *)ptr;
	ring_buffer_size = readl(&hdr->ring_size);
	if (!ring_buffer_size)
		ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;

	/* Initialize hypervisor trace subsystem via I/O port */
	outl(TRACE_CMD_INIT, GEMVISOR_TRACE_PORT);

	/* Unmap the early mapping - full mapping happens in gemvisor_trace_remap */
	early_iounmap(ptr, PAGE_SIZE);

	gemvisor_detected = true;
	pr_info("gemvisor-trace: detected gemvisor at GPA %#lx, deferring full mapping\n",
		(unsigned long)GEMVISOR_TRACE_PAGE_GPA);
}

static int __init gemvisor_trace_remap(void)
{
	void __iomem *ptr;

	if (!gemvisor_detected)
		return 0;

	ptr = ioremap(GEMVISOR_TRACE_PAGE_GPA, GEMVISOR_TRACE_PAGE_SIZE);
	if (!ptr) {
		pr_warn("gemvisor-trace: ioremap failed; tracing disabled\n");
		return 0;
	}

	trace_header = (struct gem_trace_header __iomem *)ptr;
	trace_buffer = (void __iomem *)trace_header + GEMVISOR_TRACE_HEADER_SIZE;

	/* Re-read ring_size from the permanent mapping */
	ring_buffer_size = readl(&trace_header->ring_size);
	if (!ring_buffer_size)
		ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;

	trace_enabled = true;
	pr_info("gemvisor-trace: initialized with permanent mapping at GPA %#lx\n",
		(unsigned long)GEMVISOR_TRACE_PAGE_GPA);
	return 0;
}
core_initcall(gemvisor_trace_remap);

void gemvisor_trace_emit_regs(u16 event_type, u32 flags, const void *payload, u8 payload_len,
				 struct pt_regs *regs)
{
	struct gem_trace_event evt;
	struct gem_trace_payload_hdr payload_hdr;
	u64 stack_entries[GEM_TRACE_MAX_STACK_DEPTH];
	struct gem_trace_reg_snapshot reg_snapshot;
	u32 write_ptr, event_size, avail;
	u32 payload_total;
	u8 stack_depth;
	unsigned int flush_tries = 0;
	unsigned long irq_flags;
	u64 vtime_ns;

	if (!trace_enabled || !trace_header)
		return;

	/* Prevent IRQ handlers from interleaving writes into the trace ring */
	local_irq_save(irq_flags);

	vtime_ns = gem_trace_read_vtime();
	if (!regs)
		regs = get_irq_regs();

	reg_snapshot = gem_trace_collect_regs(regs);
	stack_depth = gem_trace_capture_stack(regs, stack_entries,
					       GEM_TRACE_MAX_STACK_DEPTH);

	payload_hdr.version = GEM_TRACE_PAYLOAD_VERSION;
	payload_hdr.body_len = payload_len;
	payload_hdr.stack_depth = stack_depth;
	payload_hdr.reserved = GEM_TRACE_REG_BLOCK_SIZE;

	/* Calculate event size (header + payload header + body + reg block + stack frames), aligned to 8 bytes */
	payload_total = sizeof(payload_hdr) + payload_len + payload_hdr.reserved +
		(u32)stack_depth * (u32)sizeof(u64);
	event_size = MIN_EVENT_SIZE + payload_total;
	event_size = (event_size + 7) & ~7;

	while (event_size > 255 && stack_depth > 0) {
		stack_depth--;
		payload_hdr.stack_depth = stack_depth;
		payload_total -= sizeof(u64);
		event_size = MIN_EVENT_SIZE + payload_total;
		event_size = (event_size + 7) & ~7;
	}

	if (event_size > ring_buffer_size || event_size > 255) {
		local_irq_restore(irq_flags);
		panic("gemvisor-trace: event larger than trace ring");
	}

	/* Read current write pointer */
	write_ptr = readl(&trace_header->write_ptr);

	while (1) {
		avail = ring_buffer_size - write_ptr;
		if (avail >= event_size)
			break;

		/* Buffer is full: ask host to drain and force a VM-exit. */
		outl(TRACE_CMD_FLUSH, GEMVISOR_TRACE_PORT);
		asm volatile("vmcall" ::: "memory");

		/* Zero trailing bytes so the host doesn't parse stale data after wrap. */
		memset_io(trace_buffer + write_ptr, 0, ring_buffer_size - write_ptr);

		/* After a flush we start a new ring lap to avoid overwrite without wrap. */
		writel(0, &trace_header->write_ptr);
		write_ptr = 0;
		writel(readl(&trace_header->wrap_count) + 1, &trace_header->wrap_count);

		flush_tries++;
		if (flush_tries > 1) {
			panic("gemvisor-trace: host failed to drain trace buffer after flush");
		}
	}

	/* Build event header */
	evt.size = event_size;
	evt.type_hi = (event_type >> 8) & 0xFF;
	evt.type_lo = event_type & 0xFF;
	evt.flags = flags;
	evt.vtime_ns = vtime_ns;
	evt.retired = vtime_ns;
	evt.rip = (u64)__builtin_return_address(0);

	/* Write event header */
	memcpy_toio(trace_buffer + write_ptr, &evt, MIN_EVENT_SIZE);

	/* Write payload header + body + register snapshot + stack frames */
	memcpy_toio(trace_buffer + write_ptr + MIN_EVENT_SIZE,
		    &payload_hdr, sizeof(payload_hdr));

	if (payload_len > 0 && payload != NULL) {
		memcpy_toio(trace_buffer + write_ptr + MIN_EVENT_SIZE + sizeof(payload_hdr),
			    payload, payload_len);
	}

	if (payload_hdr.reserved) {
		memcpy_toio(trace_buffer + write_ptr + MIN_EVENT_SIZE + sizeof(payload_hdr) +
			    payload_len,
			    &reg_snapshot, payload_hdr.reserved);
	}

	if (stack_depth > 0) {
		memcpy_toio(trace_buffer + write_ptr + MIN_EVENT_SIZE +
			    sizeof(payload_hdr) + payload_len + payload_hdr.reserved,
			    stack_entries, (size_t)stack_depth * sizeof(u64));
	}

	/* Zero any alignment padding to keep the buffer deterministic */
	if (event_size > MIN_EVENT_SIZE + payload_total) {
		memset_io(trace_buffer + write_ptr + MIN_EVENT_SIZE + payload_total, 0,
			  event_size - (MIN_EVENT_SIZE + payload_total));
	}

	/* Advance write pointer and sequence */
	/* Ensure header/payload stores are visible before updating pointers */
	wmb();
	writel(write_ptr + event_size, &trace_header->write_ptr);
	writel(readl(&trace_header->sequence) + 1, &trace_header->sequence);

	local_irq_restore(irq_flags);
}
EXPORT_SYMBOL_GPL(gemvisor_trace_emit_regs);

/* Backward-compatible shim when callers don't have pt_regs handy. */
void gemvisor_trace_emit(u16 event_type, u32 flags, const void *payload, u8 payload_len)
{
	gemvisor_trace_emit_regs(event_type, flags, payload, payload_len, NULL);
}

/*
 * Collect extended page fault context for debugging.
 *
 * This function attempts to gather VMA and PTE information at the time of
 * a page fault. It uses lockless/speculative reads where possible to avoid
 * deadlocks or sleeping in the fault path.
 *
 * Note: VMA info may be stale or unavailable if we can't safely read it.
 * PTE walk is lockless but may race with concurrent modifications.
 */
void gem_trace_collect_pf_context(struct gem_pf_payload *pl, unsigned long address,
				  struct pt_regs *regs)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/* Always capture PID */
	pl->pid = current->pid;

	/* Initialize extended fields to zero (no info available) */
	pl->vm_start = 0;
	pl->vm_end = 0;
	pl->vm_flags = 0;
	pl->pte_val = 0;

	/*
	 * Try to get VMA info. We use a speculative read approach:
	 * - Check if we have an mm
	 * - Try lock_vma_under_rcu if CONFIG_PER_VMA_LOCK is enabled
	 * - Otherwise skip VMA info (too risky to take mmap lock here)
	 */
	mm = current->mm;
	if (!mm)
		goto walk_pte;

	/*
	 * Try RCU-protected VMA lookup if available.
	 * This is safe in the fault path and doesn't sleep.
	 */
	vma = lock_vma_under_rcu(mm, address);
	if (vma) {
		pl->vm_start = vma->vm_start;
		pl->vm_end = vma->vm_end;
		pl->vm_flags = vma->vm_flags;
		vma_end_read(vma);
	}

walk_pte:
	/*
	 * Walk the page table to get PTE value.
	 * This is a lockless read - the PTE may change concurrently,
	 * but we capture a snapshot for debugging purposes.
	 *
	 * For user addresses, use the mm's pgd; for kernel addresses,
	 * use the kernel's pgd.
	 */
	if (address >= TASK_SIZE) {
		/* Kernel address - use kernel page tables */
		pgd = pgd_offset_k(address);
	} else if (mm) {
		/* User address - use process page tables */
		pgd = pgd_offset(mm, address);
	} else {
		return;
	}

	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud) || pud_bad(*pud))
		return;

	/* Check for huge page at PUD level */
	if (pud_large(*pud)) {
		pl->pte_val = pud_val(*pud);
		return;
	}

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return;

	/* Check for huge page at PMD level */
	if (pmd_large(*pmd)) {
		pl->pte_val = pmd_val(*pmd);
		return;
	}

	pte = pte_offset_kernel(pmd, address);
	if (pte) {
		/*
		 * GEMVISOR DETERMINISM: Use ptep_get() for safe atomic PTE read.
		 *
		 * This lockless read may race with concurrent PTE modifications.
		 * ptep_get() provides the proper memory barrier and atomic read
		 * to capture a consistent snapshot, avoiding torn reads that
		 * could produce non-deterministic trace data.
		 */
		pl->pte_val = pte_val(ptep_get(pte));
	}
}
EXPORT_SYMBOL_GPL(gem_trace_collect_pf_context);
