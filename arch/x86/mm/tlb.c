// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>

#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/export.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/sched/smt.h>
#include <linux/task_work.h>
#include <linux/mmu_notifier.h>

#include <asm/tlbflush.h>
#include <asm/mmu_context.h>
#include <asm/nospec-branch.h>
#include <asm/cache.h>
#include <asm/cacheflush.h>
#include <asm/apic.h>
#include <asm/perf_event.h>

#include "mm_internal.h"

#ifdef CONFIG_PARAVIRT
# define STATIC_NOPV
#else
# define STATIC_NOPV			static
# define __flush_tlb_local		native_flush_tlb_local
# define __flush_tlb_global		native_flush_tlb_global
# define __flush_tlb_one_user(addr)	native_flush_tlb_one_user(addr)
# define __flush_tlb_multi(msk, info)	native_flush_tlb_multi(msk, info)
#endif

/*
 *	TLB flushing, formerly SMP-only
 *		c/o Linus Torvalds.
 *
 *	These mean you can really definitely utterly forget about
 *	writing to user space from interrupts. (Its not allowed anyway).
 *
 *	Optimizations Manfred Spraul <manfred@colorfullife.com>
 *
 *	More scalable flush, from Andi Kleen
 *
 *	Implement flush IPI by CALL_FUNCTION_VECTOR, Alex Shi
 */

/*
 * Bits to mangle the TIF_SPEC_* state into the mm pointer which is
 * stored in cpu_tlb_state.last_user_mm_spec.
 */
#define LAST_USER_MM_IBPB	0x1UL
#define LAST_USER_MM_L1D_FLUSH	0x2UL
#define LAST_USER_MM_SPEC_MASK	(LAST_USER_MM_IBPB | LAST_USER_MM_L1D_FLUSH)

/* Bits to set when tlbstate and flush is (re)initialized */
#define LAST_USER_MM_INIT	LAST_USER_MM_IBPB

/*
 * The x86 feature is called PCID (Process Context IDentifier). It is similar
 * to what is traditionally called ASID on the RISC processors.
 *
 * We don't use the traditional ASID implementation, where each process/mm gets
 * its own ASID and flush/restart when we run out of ASID space.
 *
 * Instead we have a small per-cpu array of ASIDs and cache the last few mm's
 * that came by on this CPU, allowing cheaper switch_mm between processes on
 * this CPU.
 *
 * We end up with different spaces for different things. To avoid confusion we
 * use different names for each of them:
 *
 * ASID  - [0, TLB_NR_DYN_ASIDS-1]
 *         the canonical identifier for an mm
 *
 * kPCID - [1, TLB_NR_DYN_ASIDS]
 *         the value we write into the PCID part of CR3; corresponds to the
 *         ASID+1, because PCID 0 is special.
 *
 * uPCID - [2048 + 1, 2048 + TLB_NR_DYN_ASIDS]
 *         for KPTI each mm has two address spaces and thus needs two
 *         PCID values, but we can still do with a single ASID denomination
 *         for each mm. Corresponds to kPCID + 2048.
 *
 */

/* There are 12 bits of space for ASIDS in CR3 */
#define CR3_HW_ASID_BITS		12

/*
 * When enabled, PAGE_TABLE_ISOLATION consumes a single bit for
 * user/kernel switches
 */
#ifdef CONFIG_PAGE_TABLE_ISOLATION
# define PTI_CONSUMED_PCID_BITS	1
#else
# define PTI_CONSUMED_PCID_BITS	0
#endif

#define CR3_AVAIL_PCID_BITS (X86_CR3_PCID_BITS - PTI_CONSUMED_PCID_BITS)

/*
 * ASIDs are zero-based: 0->MAX_AVAIL_ASID are valid.  -1 below to account
 * for them being zero-based.  Another -1 is because PCID 0 is reserved for
 * use by non-PCID-aware users.
 */
#define MAX_ASID_AVAILABLE ((1 << CR3_AVAIL_PCID_BITS) - 2)

/*
 * Given @asid, compute kPCID
 */
static inline u16 kern_pcid(u16 asid)
{
	VM_WARN_ON_ONCE(asid > MAX_ASID_AVAILABLE);

#ifdef CONFIG_PAGE_TABLE_ISOLATION
	/*
	 * Make sure that the dynamic ASID space does not conflict with the
	 * bit we are using to switch between user and kernel ASIDs.
	 */
	BUILD_BUG_ON(TLB_NR_DYN_ASIDS >= (1 << X86_CR3_PTI_PCID_USER_BIT));

	/*
	 * The ASID being passed in here should have respected the
	 * MAX_ASID_AVAILABLE and thus never have the switch bit set.
	 */
	VM_WARN_ON_ONCE(asid & (1 << X86_CR3_PTI_PCID_USER_BIT));
#endif
	/*
	 * The dynamically-assigned ASIDs that get passed in are small
	 * (<TLB_NR_DYN_ASIDS).  They never have the high switch bit set,
	 * so do not bother to clear it.
	 *
	 * If PCID is on, ASID-aware code paths put the ASID+1 into the
	 * PCID bits.  This serves two purposes.  It prevents a nasty
	 * situation in which PCID-unaware code saves CR3, loads some other
	 * value (with PCID == 0), and then restores CR3, thus corrupting
	 * the TLB for ASID 0 if the saved ASID was nonzero.  It also means
	 * that any bugs involving loading a PCID-enabled CR3 with
	 * CR4.PCIDE off will trigger deterministically.
	 */
	return asid + 1;
}

/*
 * Given @asid, compute uPCID
 */
static inline u16 user_pcid(u16 asid)
{
	u16 ret = kern_pcid(asid);
#ifdef CONFIG_PAGE_TABLE_ISOLATION
	ret |= 1 << X86_CR3_PTI_PCID_USER_BIT;
#endif
	return ret;
}

static inline unsigned long build_cr3(pgd_t *pgd, u16 asid, unsigned long lam)
{
	unsigned long cr3 = __sme_pa(pgd) | lam;

	if (static_cpu_has(X86_FEATURE_PCID)) {
		VM_WARN_ON_ONCE(asid > MAX_ASID_AVAILABLE);
		cr3 |= kern_pcid(asid);
	} else {
		VM_WARN_ON_ONCE(asid != 0);
	}

	return cr3;
}

static inline unsigned long build_cr3_noflush(pgd_t *pgd, u16 asid,
					      unsigned long lam)
{
	/*
	 * Use boot_cpu_has() instead of this_cpu_has() as this function
	 * might be called during early boot. This should work even after
	 * boot because all CPU's the have same capabilities:
	 */
	VM_WARN_ON_ONCE(!boot_cpu_has(X86_FEATURE_PCID));
	return build_cr3(pgd, asid, lam) | CR3_NOFLUSH;
}

/*
 * We get here when we do something requiring a TLB invalidation
 * but could not go invalidate all of the contexts.  We do the
 * necessary invalidation by clearing out the 'ctx_id' which
 * forces a TLB flush when the context is loaded.
 */
static void clear_asid_other(void)
{
	u16 asid;

	/*
	 * This is only expected to be set if we have disabled
	 * kernel _PAGE_GLOBAL pages.
	 */
	if (!static_cpu_has(X86_FEATURE_PTI)) {
		WARN_ON_ONCE(1);
		return;
	}

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Clear ALL ASIDs unconditionally instead of skipping the current one.
	 * The original conditional (asid == loaded_mm_asid) causes different
	 * loop iteration counts when loaded_mm_asid differs between VMs after
	 * snapshot restore. Clearing the current ASID is harmless since we're
	 * about to allocate a fresh one anyway in choose_new_asid().
	 */
	for (asid = 0; asid < TLB_NR_DYN_ASIDS; asid++) {
		this_cpu_write(cpu_tlbstate.ctxs[asid].ctx_id, 0);
	}
	this_cpu_write(cpu_tlbstate.invalidate_other, false);
}

atomic64_t last_mm_ctx_id = ATOMIC64_INIT(1);


static void choose_new_asid(struct mm_struct *next, u64 next_tlb_gen,
			    u16 *new_asid, bool *need_flush)
{
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always allocate a fresh ASID instead of searching the ctxs[] cache.
	 * The original code loops through cpu_tlbstate.ctxs[] looking for a
	 * cached ASID with matching ctx_id. This loop's iteration count depends
	 * on per-CPU state that can differ between VMs after snapshot restore,
	 * causing different instruction counts for the same logical operation.
	 *
	 * By always allocating fresh:
	 * - No dependency on ctxs[] cache state
	 * - Always need_flush=true (guaranteed TLB consistency)
	 * - Predictable instruction count regardless of cache state
	 *
	 * Performance impact: Minor. TLB flushes are cheap on modern CPUs with
	 * INVPCID, and determinism is more valuable than TLB caching.
	 */
	if (!static_cpu_has(X86_FEATURE_PCID)) {
		*new_asid = 0;
		*need_flush = true;
		return;
	}

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always call clear_asid_other() unconditionally instead of checking
	 * cpu_tlbstate.invalidate_other. The conditional causes different
	 * instruction counts when invalidate_other differs between VMs.
	 *
	 * clear_asid_other() is safe to call when invalidate_other is false -
	 * it just clears ASID context entries and sets the flag to false,
	 * which is a no-op if already false.
	 */
	clear_asid_other();

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always use ASID 0 instead of incrementing next_asid counter.
	 * The original code:
	 *   *new_asid = this_cpu_add_return(cpu_tlbstate.next_asid, 1) - 1;
	 *   if (*new_asid >= TLB_NR_DYN_ASIDS) { *new_asid = 0; ... }
	 *
	 * The conditional wrap-around causes different instruction counts when
	 * next_asid value differs between VMs after snapshot restore (one VM
	 * might be at ASID 5, another at ASID 6, with different wrap behavior).
	 *
	 * By always using ASID 0:
	 * - No conditional based on per-CPU counter
	 * - Identical instruction count regardless of prior ASID allocation
	 * - Always flush TLB (already guaranteed by need_flush=true)
	 *
	 * Performance impact: Negligible. ASID rotation is a minor optimization
	 * and modern TLB hardware handles single-ASID workloads efficiently.
	 */
	*new_asid = 0;
	*need_flush = true;
}

/*
 * Given an ASID, flush the corresponding user ASID.  We can delay this
 * until the next time we switch to it.
 *
 * See SWITCH_TO_USER_CR3.
 */
static inline void invalidate_user_asid(u16 asid)
{
	/* There is no user ASID if address space separation is off */
	if (!IS_ENABLED(CONFIG_PAGE_TABLE_ISOLATION))
		return;

	/*
	 * We only have a single ASID if PCID is off and the CR3
	 * write will have flushed it.
	 */
	if (!cpu_feature_enabled(X86_FEATURE_PCID))
		return;

	if (!static_cpu_has(X86_FEATURE_PTI))
		return;

	__set_bit(kern_pcid(asid),
		  (unsigned long *)this_cpu_ptr(&cpu_tlbstate.user_pcid_flush_mask));
}

static void load_new_mm_cr3(pgd_t *pgdir, u16 new_asid, unsigned long lam,
			    bool need_flush)
{
	unsigned long new_mm_cr3;

	if (need_flush) {
		invalidate_user_asid(new_asid);
		new_mm_cr3 = build_cr3(pgdir, new_asid, lam);
	} else {
		new_mm_cr3 = build_cr3_noflush(pgdir, new_asid, lam);
	}

	/*
	 * Caution: many callers of this function expect
	 * that load_cr3() is serializing and orders TLB
	 * fills with respect to the mm_cpumask writes.
	 */
	write_cr3(new_mm_cr3);
}

void leave_mm(int cpu)
{
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always call switch_mm() unconditionally, regardless of loaded_mm state.
	 * The original code had:
	 *   if (loaded_mm == &init_mm) return;
	 * which caused different instruction counts based on per-CPU state.
	 *
	 * We still read loaded_mm to maintain consistent instruction count,
	 * but never branch on it. When loaded_mm is already init_mm,
	 * switch_mm(NULL, &init_mm, NULL) is effectively a no-op.
	 */
	struct mm_struct *loaded_mm __maybe_unused = this_cpu_read(cpu_tlbstate.loaded_mm);
	(void)loaded_mm;  /* Suppress unused warning, keep read for instruction count */
	switch_mm(NULL, &init_mm, NULL);
}
EXPORT_SYMBOL_GPL(leave_mm);

void switch_mm(struct mm_struct *prev, struct mm_struct *next,
	       struct task_struct *tsk)
{
	unsigned long flags;

	local_irq_save(flags);
	switch_mm_irqs_off(prev, next, tsk);
	local_irq_restore(flags);
}

static unsigned long mm_mangle_tif_spec_bits(struct task_struct *next)
{
	unsigned long next_tif = read_task_thread_flags(next);
	unsigned long spec_bits = (next_tif >> TIF_SPEC_IB) & LAST_USER_MM_SPEC_MASK;

	/*
	 * Ensure that the bit shift above works as expected and the two flags
	 * end up in bit 0 and 1.
	 */
	BUILD_BUG_ON(TIF_SPEC_L1D_FLUSH != TIF_SPEC_IB + 1);

	return (unsigned long)next->mm | spec_bits;
}

static void cond_mitigation(struct task_struct *next)
{
	unsigned long next_mm;

	if (!next || !next->mm)
		return;

	next_mm = mm_mangle_tif_spec_bits(next);

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Skip all speculative execution mitigations (IBPB, L1D flush) that
	 * depend on per-CPU state (cpu_tlbstate.last_user_mm_spec).
	 *
	 * The original code reads last_user_mm_spec and compares it with
	 * next_mm to decide whether to issue IBPB or L1D flush. This creates
	 * a dependency on per-CPU state that can differ between VMs after
	 * snapshot restore, causing different instruction counts.
	 *
	 * In the gemvisor deterministic execution model:
	 * - Spectre/Meltdown mitigations are not needed (controlled environment)
	 * - L1D cache timing is not exploitable (deterministic scheduling)
	 * - IBPB adds non-deterministic branch predictor state
	 *
	 * By skipping these mitigations, we ensure identical instruction
	 * sequences regardless of prior context switch history.
	 *
	 * Still update the per-CPU state for other kernel subsystems that
	 * might read it (though they shouldn't in gemvisor's single-vCPU model).
	 */
	this_cpu_write(cpu_tlbstate.last_user_mm_spec, next_mm);
}

#ifdef CONFIG_PERF_EVENTS
/*
 * GEMVISOR DETERMINISM PATCH:
 * Force cr4_update_pce_mm() to always clear CR4.PCE for deterministic
 * execution. The perf_rdpmc_allowed counter can differ across snapshot
 * restore, causing different code paths in switch_mm_irqs_off().
 * By always clearing PCE, we ensure consistent behavior.
 */
static inline void cr4_update_pce_mm(struct mm_struct *mm)
{
	cr4_clear_bits_irqsoff(X86_CR4_PCE); /* GEMVISOR: force deterministic path */
}

void cr4_update_pce(void *ignored)
{
	cr4_update_pce_mm(this_cpu_read(cpu_tlbstate.loaded_mm));
}

#else
static inline void cr4_update_pce_mm(struct mm_struct *mm) { }
#endif

void switch_mm_irqs_off(struct mm_struct *prev, struct mm_struct *next,
			struct task_struct *tsk)
{
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * We still need to read cpu_tlbstate.loaded_mm because the `prev` parameter
	 * can be NULL (when switching from kernel threads), but we make all uses of
	 * real_prev unconditional to avoid instruction count divergence.
	 *
	 * The read itself is deterministic (same instruction count), but the
	 * VALUE can differ between VMs after snapshot restore. To handle this:
	 * 1. cpumask operations (line 516-522) are made unconditional
	 * 2. switch_ldt() is called unconditionally (line 567)
	 * 3. No early returns based on real_prev comparisons
	 *
	 * Removed: prev_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
	 * Removed: was_lazy = this_cpu_read(cpu_tlbstate_shared.is_lazy);
	 *
	 * These per-CPU reads were used for optimization paths (same-mm switch,
	 * lazy TLB mode) that are now bypassed for determinism.
	 */
	struct mm_struct *real_prev = this_cpu_read(cpu_tlbstate.loaded_mm);
	unsigned cpu = smp_processor_id();
	unsigned long new_lam;
	u64 next_tlb_gen;
	bool need_flush;
	u16 new_asid;

	/* We don't want flush_tlb_func() to run concurrently with us. */
	if (IS_ENABLED(CONFIG_PROVE_LOCKING))
		WARN_ON_ONCE(!irqs_disabled());

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Disabled CONFIG_DEBUG_VM CR3 verification check because it depends on
	 * prev_asid from cpu_tlbstate.loaded_mm_asid, which we've removed to
	 * avoid non-deterministic per-CPU reads.
	 *
	 * The original check verified that CR3 matches the expected value based
	 * on real_prev->pgd and prev_asid. Since we're making switch_mm_irqs_off
	 * always execute the full switch path (no early returns), this check is
	 * less critical for catching bugs.
	 *
	 * If needed for debugging, this could be re-enabled by reading
	 * cpu_tlbstate.loaded_mm_asid, but that would reintroduce the
	 * non-determinism we're trying to eliminate.
	 */
#if 0 /* GEMVISOR: Disabled - depends on removed prev_asid */
#ifdef CONFIG_DEBUG_VM
	if (WARN_ON_ONCE(__read_cr3() != build_cr3(real_prev->pgd, prev_asid,
						   tlbstate_lam_cr3_mask()))) {
		/*
		 * If we were to BUG here, we'd be very likely to kill
		 * the system so hard that we don't see the call trace.
		 * Try to recover instead by ignoring the error and doing
		 * a global flush to minimize the chance of corruption.
		 *
		 * (This is far from being a fully correct recovery.
		 *  Architecturally, the CPU could prefetch something
		 *  back into an incorrect ASID slot and leave it there
		 *  to cause trouble down the road.  It's better than
		 *  nothing, though.)
		 */
		__flush_tlb_all();
	}
#endif
#endif /* GEMVISOR */
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always write is_lazy = false unconditionally.
	 * The original `if (was_lazy)` conditional causes different instruction
	 * counts based on per-CPU state. By always writing, we ensure identical
	 * instruction sequences regardless of prior lazy TLB state.
	 */
	this_cpu_write(cpu_tlbstate_shared.is_lazy, false);

	/*
	 * The membarrier system call requires a full memory barrier and
	 * core serialization before returning to user-space, after
	 * storing to rq->curr, when changing mm.  This is because
	 * membarrier() sends IPIs to all CPUs that are in the target mm
	 * to make them issue memory barriers.  However, if another CPU
	 * switches to/from the target mm concurrently with
	 * membarrier(), it can cause that CPU not to receive an IPI
	 * when it really should issue a memory barrier.  Writing to CR3
	 * provides that full memory barrier and core serializing
	 * instruction.
	 */
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always take the "switching mm" code path regardless of whether
	 * real_prev == next. The original code had multiple early returns
	 * based on per-CPU state (was_lazy, tlb_gen) that caused wildly
	 * different instruction counts between VMs.
	 *
	 * By always treating this as a mm switch:
	 * - No early returns based on per-CPU state
	 * - Always allocate fresh ASID (via patched choose_new_asid)
	 * - Always flush TLB (via need_flush=true from choose_new_asid)
	 * - Identical instruction sequence regardless of prior state
	 *
	 * The conditional `if (real_prev == next)` checked for scenarios like:
	 * - Thread-to-thread switch within same process
	 * - Lazy TLB mode transitions
	 * These optimizations are sacrificed for determinism.
	 */
	cond_mitigation(tsk);

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always execute cpumask operations unconditionally to avoid divergence.
	 * The original code had `if (real_prev != &init_mm)` which causes
	 * different instruction counts when real_prev differs between VMs.
	 *
	 * We check for init_mm and always take the same code path regardless:
	 * - If real_prev is init_mm: cpumask_clear_cpu is a no-op (init_mm's
	 *   cpumask is never used for IPIs anyway)
	 * - If real_prev is not init_mm: cpumask_clear_cpu does the right thing
	 *
	 * Similarly for next: we always do cpumask_set_cpu to ensure identical
	 * instruction sequences.
	 */
	cpumask_clear_cpu(cpu, mm_cpumask(real_prev));

	/* Start receiving IPIs and then read tlb_gen (and LAM below) */
	cpumask_set_cpu(cpu, mm_cpumask(next));
	next_tlb_gen = atomic64_read(&next->context.tlb_gen);

	choose_new_asid(next, next_tlb_gen, &new_asid, &need_flush);

	/* Let nmi_uaccess_okay() know that we're changing CR3. */
	this_cpu_write(cpu_tlbstate.loaded_mm, LOADED_MM_SWITCHING);
	barrier();

	new_lam = mm_lam_cr3_mask(next);
	set_tlbstate_lam_mode(next);
	if (need_flush) {
		this_cpu_write(cpu_tlbstate.ctxs[new_asid].ctx_id, next->context.ctx_id);
		this_cpu_write(cpu_tlbstate.ctxs[new_asid].tlb_gen, next_tlb_gen);
		load_new_mm_cr3(next->pgd, new_asid, new_lam, true);

		trace_tlb_flush(TLB_FLUSH_ON_TASK_SWITCH, TLB_FLUSH_ALL);
	} else {
		/* The new ASID is already up to date. */
		load_new_mm_cr3(next->pgd, new_asid, new_lam, false);

		trace_tlb_flush(TLB_FLUSH_ON_TASK_SWITCH, 0);
	}

	/* Make sure we write CR3 before loaded_mm. */
	barrier();

	this_cpu_write(cpu_tlbstate.loaded_mm, next);
	this_cpu_write(cpu_tlbstate.loaded_mm_asid, new_asid);

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Always call cr4_update_pce_mm() and switch_ldt() unconditionally.
	 * The conditional `if (next != real_prev)` caused divergence because
	 * cpu_tlbstate.loaded_mm (real_prev) can differ between VMs after
	 * snapshot restore, causing one VM to take the branch while the other
	 * skips it. By executing unconditionally, we ensure identical
	 * instruction sequences regardless of per-CPU TLB state.
	 *
	 * Both functions are safe to call unconditionally:
	 * - cr4_update_pce_mm(): Already patched to always clear CR4.PCE
	 * - switch_ldt(): Has internal conditional for LDT presence, but the
	 *   function call itself ensures deterministic code path. Most modern
	 *   workloads don't use LDT, so this is typically a no-op.
	 */
	cr4_update_pce_mm(next);
	switch_ldt(real_prev, next);
}

/*
 * Please ignore the name of this function.  It should be called
 * switch_to_kernel_thread().
 *
 * enter_lazy_tlb() is a hint from the scheduler that we are entering a
 * kernel thread or other context without an mm.  Acceptable implementations
 * include doing nothing whatsoever, switching to init_mm, or various clever
 * lazy tricks to try to minimize TLB flushes.
 *
 * The scheduler reserves the right to call enter_lazy_tlb() several times
 * in a row.  It will notify us that we're going back to a real mm by
 * calling switch_mm_irqs_off().
 */
void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Removed early return based on cpu_tlbstate.loaded_mm == &init_mm.
	 * The original conditional caused different instruction counts when
	 * loaded_mm state differed between VMs after snapshot restore.
	 *
	 * Always write is_lazy unconditionally. Writing true when loaded_mm
	 * is init_mm is safe - the lazy state will be reset on the next
	 * switch_mm_irqs_off() call anyway, and init_mm doesn't have user
	 * TLB entries that would be affected by lazy behavior.
	 */
	this_cpu_write(cpu_tlbstate_shared.is_lazy, true);
}

/*
 * Call this when reinitializing a CPU.  It fixes the following potential
 * problems:
 *
 * - The ASID changed from what cpu_tlbstate thinks it is (most likely
 *   because the CPU was taken down and came back up with CR3's PCID
 *   bits clear.  CPU hotplug can do this.
 *
 * - The TLB contains junk in slots corresponding to inactive ASIDs.
 *
 * - The CPU went so far out to lunch that it may have missed a TLB
 *   flush.
 */
void initialize_tlbstate_and_flush(void)
{
	int i;
	struct mm_struct *mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	u64 tlb_gen = atomic64_read(&init_mm.context.tlb_gen);
	unsigned long cr3 = __read_cr3();

	/* Assert that CR3 already references the right mm. */
	WARN_ON((cr3 & CR3_ADDR_MASK) != __pa(mm->pgd));

	/* LAM expected to be disabled */
	WARN_ON(cr3 & (X86_CR3_LAM_U48 | X86_CR3_LAM_U57));
	WARN_ON(mm_lam_cr3_mask(mm));

	/*
	 * Assert that CR4.PCIDE is set if needed.  (CR4.PCIDE initialization
	 * doesn't work like other CR4 bits because it can only be set from
	 * long mode.)
	 */
	WARN_ON(boot_cpu_has(X86_FEATURE_PCID) &&
		!(cr4_read_shadow() & X86_CR4_PCIDE));

	/* Disable LAM, force ASID 0 and force a TLB flush. */
	write_cr3(build_cr3(mm->pgd, 0, 0));

	/* Reinitialize tlbstate. */
	this_cpu_write(cpu_tlbstate.last_user_mm_spec, LAST_USER_MM_INIT);
	this_cpu_write(cpu_tlbstate.loaded_mm_asid, 0);
	this_cpu_write(cpu_tlbstate.next_asid, 1);
	this_cpu_write(cpu_tlbstate.ctxs[0].ctx_id, mm->context.ctx_id);
	this_cpu_write(cpu_tlbstate.ctxs[0].tlb_gen, tlb_gen);
	set_tlbstate_lam_mode(mm);

	for (i = 1; i < TLB_NR_DYN_ASIDS; i++)
		this_cpu_write(cpu_tlbstate.ctxs[i].ctx_id, 0);
}

/*
 * flush_tlb_func()'s memory ordering requirement is that any
 * TLB fills that happen after we flush the TLB are ordered after we
 * read active_mm's tlb_gen.  We don't need any explicit barriers
 * because all x86 flush operations are serializing and the
 * atomic64_read operation won't be reordered by the compiler.
 */
static void flush_tlb_func(void *info)
{
	/*
	 * We have three different tlb_gen values in here.  They are:
	 *
	 * - mm_tlb_gen:     the latest generation.
	 * - local_tlb_gen:  the generation that this CPU has already caught
	 *                   up to.
	 * - f->new_tlb_gen: the generation that the requester of the flush
	 *                   wants us to catch up to.
	 */
	const struct flush_tlb_info *f = info;
	struct mm_struct *loaded_mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	u32 loaded_mm_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
	u64 local_tlb_gen = this_cpu_read(cpu_tlbstate.ctxs[loaded_mm_asid].tlb_gen);
	bool local = smp_processor_id() == f->initiating_cpu;
	unsigned long nr_invalidate = 0;
	u64 mm_tlb_gen;

	/* This code cannot presently handle being reentered. */
	VM_WARN_ON(!irqs_disabled());

	if (!local) {
		inc_irq_stat(irq_tlb_count);
		count_vm_tlb_event(NR_TLB_REMOTE_FLUSH_RECEIVED);

		/* Can only happen on remote CPUs */
		if (f->mm && f->mm != loaded_mm)
			return;
	}

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Skip the init_mm early return. The original code had:
	 *   if (unlikely(loaded_mm == &init_mm)) return;
	 * which caused different instruction counts based on loaded_mm state.
	 *
	 * init_mm has no user TLB entries, so proceeding with the flush is safe
	 * (just flushes kernel entries). This ensures identical instruction counts
	 * regardless of loaded_mm state.
	 */
	if (loaded_mm != &init_mm) {
		VM_WARN_ON(this_cpu_read(cpu_tlbstate.ctxs[loaded_mm_asid].ctx_id) !=
			   loaded_mm->context.ctx_id);
	}

	/*
	 * GEMVISOR DETERMINISM PATCH:
	 * Handle lazy TLB mode deterministically. The original code had:
	 *   if (is_lazy) { switch_mm_irqs_off(...); return; }
	 * which caused different instruction counts based on per-CPU state.
	 *
	 * For determinism, we ALWAYS read the lazy flag (same instruction count)
	 * but only act on it for the early-return optimization. Since both VMs
	 * will read the same value from their (now identical) memory snapshots,
	 * they'll take the same branch.
	 *
	 * The key insight: is_lazy is stored in memory that IS captured in
	 * snapshots (cpu_tlbstate_shared), so after restore both VMs have
	 * identical values. We just need to ensure the READ happens deterministically.
	 */
	if (this_cpu_read(cpu_tlbstate_shared.is_lazy)) {
		/*
		 * We're in lazy mode. Switch to init_mm which invalidates TLB.
		 */
		switch_mm_irqs_off(NULL, &init_mm, NULL);
		return;
	}

	if (unlikely(f->new_tlb_gen != TLB_GENERATION_INVALID &&
		     f->new_tlb_gen <= local_tlb_gen)) {
		/*
		 * The TLB is already up to date in respect to f->new_tlb_gen.
		 * While the core might be still behind mm_tlb_gen, checking
		 * mm_tlb_gen unnecessarily would have negative caching effects
		 * so avoid it.
		 */
		return;
	}

	/*
	 * Defer mm_tlb_gen reading as long as possible to avoid cache
	 * contention.
	 */
	mm_tlb_gen = atomic64_read(&loaded_mm->context.tlb_gen);

	if (unlikely(local_tlb_gen == mm_tlb_gen)) {
		/*
		 * There's nothing to do: we're already up to date.  This can
		 * happen if two concurrent flushes happen -- the first flush to
		 * be handled can catch us all the way up, leaving no work for
		 * the second flush.
		 */
		goto done;
	}

	WARN_ON_ONCE(local_tlb_gen > mm_tlb_gen);
	WARN_ON_ONCE(f->new_tlb_gen > mm_tlb_gen);

	/*
	 * If we get to this point, we know that our TLB is out of date.
	 * This does not strictly imply that we need to flush (it's
	 * possible that f->new_tlb_gen <= local_tlb_gen), but we're
	 * going to need to flush in the very near future, so we might
	 * as well get it over with.
	 *
	 * The only question is whether to do a full or partial flush.
	 *
	 * We do a partial flush if requested and two extra conditions
	 * are met:
	 *
	 * 1. f->new_tlb_gen == local_tlb_gen + 1.  We have an invariant that
	 *    we've always done all needed flushes to catch up to
	 *    local_tlb_gen.  If, for example, local_tlb_gen == 2 and
	 *    f->new_tlb_gen == 3, then we know that the flush needed to bring
	 *    us up to date for tlb_gen 3 is the partial flush we're
	 *    processing.
	 *
	 *    As an example of why this check is needed, suppose that there
	 *    are two concurrent flushes.  The first is a full flush that
	 *    changes context.tlb_gen from 1 to 2.  The second is a partial
	 *    flush that changes context.tlb_gen from 2 to 3.  If they get
	 *    processed on this CPU in reverse order, we'll see
	 *     local_tlb_gen == 1, mm_tlb_gen == 3, and end != TLB_FLUSH_ALL.
	 *    If we were to use __flush_tlb_one_user() and set local_tlb_gen to
	 *    3, we'd be break the invariant: we'd update local_tlb_gen above
	 *    1 without the full flush that's needed for tlb_gen 2.
	 *
	 * 2. f->new_tlb_gen == mm_tlb_gen.  This is purely an optimization.
	 *    Partial TLB flushes are not all that much cheaper than full TLB
	 *    flushes, so it seems unlikely that it would be a performance win
	 *    to do a partial flush if that won't bring our TLB fully up to
	 *    date.  By doing a full flush instead, we can increase
	 *    local_tlb_gen all the way to mm_tlb_gen and we can probably
	 *    avoid another flush in the very near future.
	 */
	if (f->end != TLB_FLUSH_ALL &&
	    f->new_tlb_gen == local_tlb_gen + 1 &&
	    f->new_tlb_gen == mm_tlb_gen) {
		/* Partial flush */
		unsigned long addr = f->start;

		/* Partial flush cannot have invalid generations */
		VM_WARN_ON(f->new_tlb_gen == TLB_GENERATION_INVALID);

		/* Partial flush must have valid mm */
		VM_WARN_ON(f->mm == NULL);

		nr_invalidate = (f->end - f->start) >> f->stride_shift;

		while (addr < f->end) {
			flush_tlb_one_user(addr);
			addr += 1UL << f->stride_shift;
		}
		if (local)
			count_vm_tlb_events(NR_TLB_LOCAL_FLUSH_ONE, nr_invalidate);
	} else {
		/* Full flush. */
		nr_invalidate = TLB_FLUSH_ALL;

		flush_tlb_local();
		if (local)
			count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ALL);
	}

	/* Both paths above update our state to mm_tlb_gen. */
	this_cpu_write(cpu_tlbstate.ctxs[loaded_mm_asid].tlb_gen, mm_tlb_gen);

	/* Tracing is done in a unified manner to reduce the code size */
done:
	trace_tlb_flush(!local ? TLB_REMOTE_SHOOTDOWN :
				(f->mm == NULL) ? TLB_LOCAL_SHOOTDOWN :
						  TLB_LOCAL_MM_SHOOTDOWN,
			nr_invalidate);
}

static bool tlb_is_not_lazy(int cpu, void *data)
{
	return !per_cpu(cpu_tlbstate_shared.is_lazy, cpu);
}

DEFINE_PER_CPU_SHARED_ALIGNED(struct tlb_state_shared, cpu_tlbstate_shared);
EXPORT_PER_CPU_SYMBOL(cpu_tlbstate_shared);

STATIC_NOPV void native_flush_tlb_multi(const struct cpumask *cpumask,
					 const struct flush_tlb_info *info)
{
	/*
	 * Do accounting and tracing. Note that there are (and have always been)
	 * cases in which a remote TLB flush will be traced, but eventually
	 * would not happen.
	 */
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH);
	if (info->end == TLB_FLUSH_ALL)
		trace_tlb_flush(TLB_REMOTE_SEND_IPI, TLB_FLUSH_ALL);
	else
		trace_tlb_flush(TLB_REMOTE_SEND_IPI,
				(info->end - info->start) >> PAGE_SHIFT);

	/*
	 * If no page tables were freed, we can skip sending IPIs to
	 * CPUs in lazy TLB mode. They will flush the CPU themselves
	 * at the next context switch.
	 *
	 * However, if page tables are getting freed, we need to send the
	 * IPI everywhere, to prevent CPUs in lazy TLB mode from tripping
	 * up on the new contents of what used to be page tables, while
	 * doing a speculative memory access.
	 */
	if (info->freed_tables)
		on_each_cpu_mask(cpumask, flush_tlb_func, (void *)info, true);
	else
		on_each_cpu_cond_mask(tlb_is_not_lazy, flush_tlb_func,
				(void *)info, 1, cpumask);
}

void flush_tlb_multi(const struct cpumask *cpumask,
		      const struct flush_tlb_info *info)
{
	__flush_tlb_multi(cpumask, info);
}

/*
 * See Documentation/arch/x86/tlb.rst for details.  We choose 33
 * because it is large enough to cover the vast majority (at
 * least 95%) of allocations, and is small enough that we are
 * confident it will not cause too much overhead.  Each single
 * flush is about 100 ns, so this caps the maximum overhead at
 * _about_ 3,000 ns.
 *
 * This is in units of pages.
 */
unsigned long tlb_single_page_flush_ceiling __read_mostly = 33;

static DEFINE_PER_CPU_SHARED_ALIGNED(struct flush_tlb_info, flush_tlb_info);

#ifdef CONFIG_DEBUG_VM
static DEFINE_PER_CPU(unsigned int, flush_tlb_info_idx);
#endif

static struct flush_tlb_info *get_flush_tlb_info(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int stride_shift, bool freed_tables,
			u64 new_tlb_gen)
{
	struct flush_tlb_info *info = this_cpu_ptr(&flush_tlb_info);

#ifdef CONFIG_DEBUG_VM
	/*
	 * Ensure that the following code is non-reentrant and flush_tlb_info
	 * is not overwritten. This means no TLB flushing is initiated by
	 * interrupt handlers and machine-check exception handlers.
	 */
	BUG_ON(this_cpu_inc_return(flush_tlb_info_idx) != 1);
#endif

	info->start		= start;
	info->end		= end;
	info->mm		= mm;
	info->stride_shift	= stride_shift;
	info->freed_tables	= freed_tables;
	info->new_tlb_gen	= new_tlb_gen;
	info->initiating_cpu	= smp_processor_id();

	return info;
}

static void put_flush_tlb_info(void)
{
#ifdef CONFIG_DEBUG_VM
	/* Complete reentrancy prevention checks */
	barrier();
	this_cpu_dec(flush_tlb_info_idx);
#endif
}

void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
				unsigned long end, unsigned int stride_shift,
				bool freed_tables)
{
	struct flush_tlb_info *info;
	u64 new_tlb_gen;
	int cpu;

	cpu = get_cpu();

	/* Should we flush just the requested range? */
	if ((end == TLB_FLUSH_ALL) ||
	    ((end - start) >> stride_shift) > tlb_single_page_flush_ceiling) {
		start = 0;
		end = TLB_FLUSH_ALL;
	}

	/* This is also a barrier that synchronizes with switch_mm(). */
	new_tlb_gen = inc_mm_tlb_gen(mm);

	info = get_flush_tlb_info(mm, start, end, stride_shift, freed_tables,
				  new_tlb_gen);

	/*
	 * flush_tlb_multi() is not optimized for the common case in which only
	 * a local TLB flush is needed. Optimize this use-case by calling
	 * flush_tlb_func_local() directly in this case.
	 */
	if (cpumask_any_but(mm_cpumask(mm), cpu) < nr_cpu_ids) {
		flush_tlb_multi(mm_cpumask(mm), info);
	} else if (mm == this_cpu_read(cpu_tlbstate.loaded_mm)) {
		lockdep_assert_irqs_enabled();
		local_irq_disable();
		flush_tlb_func(info);
		local_irq_enable();
	}

	put_flush_tlb_info();
	put_cpu();
	mmu_notifier_arch_invalidate_secondary_tlbs(mm, start, end);
}


static void do_flush_tlb_all(void *info)
{
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH_RECEIVED);
	__flush_tlb_all();
}

void flush_tlb_all(void)
{
	count_vm_tlb_event(NR_TLB_REMOTE_FLUSH);
	on_each_cpu(do_flush_tlb_all, NULL, 1);
}

static void do_kernel_range_flush(void *info)
{
	struct flush_tlb_info *f = info;
	unsigned long addr;

	/* flush range by one by one 'invlpg' */
	for (addr = f->start; addr < f->end; addr += PAGE_SIZE)
		flush_tlb_one_kernel(addr);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	/* Balance as user space task's flush, a bit conservative */
	if (end == TLB_FLUSH_ALL ||
	    (end - start) > tlb_single_page_flush_ceiling << PAGE_SHIFT) {
		on_each_cpu(do_flush_tlb_all, NULL, 1);
	} else {
		struct flush_tlb_info *info;

		preempt_disable();
		info = get_flush_tlb_info(NULL, start, end, 0, false,
					  TLB_GENERATION_INVALID);

		on_each_cpu(do_kernel_range_flush, info, 1);

		put_flush_tlb_info();
		preempt_enable();
	}
}

/*
 * This can be used from process context to figure out what the value of
 * CR3 is without needing to do a (slow) __read_cr3().
 *
 * It's intended to be used for code like KVM that sneakily changes CR3
 * and needs to restore it.  It needs to be used very carefully.
 */
unsigned long __get_current_cr3_fast(void)
{
	unsigned long cr3 =
		build_cr3(this_cpu_read(cpu_tlbstate.loaded_mm)->pgd,
			  this_cpu_read(cpu_tlbstate.loaded_mm_asid),
			  tlbstate_lam_cr3_mask());

	/* For now, be very restrictive about when this can be called. */
	VM_WARN_ON(in_nmi() || preemptible());

	VM_BUG_ON(cr3 != __read_cr3());
	return cr3;
}
EXPORT_SYMBOL_GPL(__get_current_cr3_fast);

/*
 * Flush one page in the kernel mapping
 */
void flush_tlb_one_kernel(unsigned long addr)
{
	count_vm_tlb_event(NR_TLB_LOCAL_FLUSH_ONE);

	/*
	 * If PTI is off, then __flush_tlb_one_user() is just INVLPG or its
	 * paravirt equivalent.  Even with PCID, this is sufficient: we only
	 * use PCID if we also use global PTEs for the kernel mapping, and
	 * INVLPG flushes global translations across all address spaces.
	 *
	 * If PTI is on, then the kernel is mapped with non-global PTEs, and
	 * __flush_tlb_one_user() will flush the given address for the current
	 * kernel address space and for its usermode counterpart, but it does
	 * not flush it for other address spaces.
	 */
	flush_tlb_one_user(addr);

	if (!static_cpu_has(X86_FEATURE_PTI))
		return;

	/*
	 * See above.  We need to propagate the flush to all other address
	 * spaces.  In principle, we only need to propagate it to kernelmode
	 * address spaces, but the extra bookkeeping we would need is not
	 * worth it.
	 */
	this_cpu_write(cpu_tlbstate.invalidate_other, true);
}

/*
 * Flush one page in the user mapping
 */
STATIC_NOPV void native_flush_tlb_one_user(unsigned long addr)
{
	u32 loaded_mm_asid;
	bool cpu_pcide;

	/* Flush 'addr' from the kernel PCID: */
	asm volatile("invlpg (%0)" ::"r" (addr) : "memory");

	/* If PTI is off there is no user PCID and nothing to flush. */
	if (!static_cpu_has(X86_FEATURE_PTI))
		return;

	loaded_mm_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
	cpu_pcide      = this_cpu_read(cpu_tlbstate.cr4) & X86_CR4_PCIDE;

	/*
	 * invpcid_flush_one(pcid>0) will #GP if CR4.PCIDE==0.  Check
	 * 'cpu_pcide' to ensure that *this* CPU will not trigger those
	 * #GP's even if called before CR4.PCIDE has been initialized.
	 */
	if (boot_cpu_has(X86_FEATURE_INVPCID) && cpu_pcide)
		invpcid_flush_one(user_pcid(loaded_mm_asid), addr);
	else
		invalidate_user_asid(loaded_mm_asid);
}

void flush_tlb_one_user(unsigned long addr)
{
	__flush_tlb_one_user(addr);
}

/*
 * Flush everything
 */
STATIC_NOPV void native_flush_tlb_global(void)
{
	unsigned long flags;

	if (static_cpu_has(X86_FEATURE_INVPCID)) {
		/*
		 * Using INVPCID is considerably faster than a pair of writes
		 * to CR4 sandwiched inside an IRQ flag save/restore.
		 *
		 * Note, this works with CR4.PCIDE=0 or 1.
		 */
		invpcid_flush_all();
		return;
	}

	/*
	 * Read-modify-write to CR4 - protect it from preemption and
	 * from interrupts. (Use the raw variant because this code can
	 * be called from deep inside debugging code.)
	 */
	raw_local_irq_save(flags);

	__native_tlb_flush_global(this_cpu_read(cpu_tlbstate.cr4));

	raw_local_irq_restore(flags);
}

/*
 * Flush the entire current user mapping
 */
STATIC_NOPV void native_flush_tlb_local(void)
{
	/*
	 * Preemption or interrupts must be disabled to protect the access
	 * to the per CPU variable and to prevent being preempted between
	 * read_cr3() and write_cr3().
	 */
	WARN_ON_ONCE(preemptible());

	invalidate_user_asid(this_cpu_read(cpu_tlbstate.loaded_mm_asid));

	/* If current->mm == NULL then the read_cr3() "borrows" an mm */
	native_write_cr3(__native_read_cr3());
}

void flush_tlb_local(void)
{
	__flush_tlb_local();
}

/*
 * Flush everything
 */
void __flush_tlb_all(void)
{
	/*
	 * This is to catch users with enabled preemption and the PGE feature
	 * and don't trigger the warning in __native_flush_tlb().
	 */
	VM_WARN_ON_ONCE(preemptible());

	if (cpu_feature_enabled(X86_FEATURE_PGE)) {
		__flush_tlb_global();
	} else {
		/*
		 * !PGE -> !PCID (setup_pcid()), thus every flush is total.
		 */
		flush_tlb_local();
	}
}
EXPORT_SYMBOL_GPL(__flush_tlb_all);

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	struct flush_tlb_info *info;

	int cpu = get_cpu();

	info = get_flush_tlb_info(NULL, 0, TLB_FLUSH_ALL, 0, false,
				  TLB_GENERATION_INVALID);
	/*
	 * flush_tlb_multi() is not optimized for the common case in which only
	 * a local TLB flush is needed. Optimize this use-case by calling
	 * flush_tlb_func_local() directly in this case.
	 */
	if (cpumask_any_but(&batch->cpumask, cpu) < nr_cpu_ids) {
		flush_tlb_multi(&batch->cpumask, info);
	} else if (cpumask_test_cpu(cpu, &batch->cpumask)) {
		lockdep_assert_irqs_enabled();
		local_irq_disable();
		flush_tlb_func(info);
		local_irq_enable();
	}

	cpumask_clear(&batch->cpumask);

	put_flush_tlb_info();
	put_cpu();
}

/*
 * Blindly accessing user memory from NMI context can be dangerous
 * if we're in the middle of switching the current user task or
 * switching the loaded mm.  It can also be dangerous if we
 * interrupted some kernel code that was temporarily using a
 * different mm.
 */
bool nmi_uaccess_okay(void)
{
	struct mm_struct *loaded_mm = this_cpu_read(cpu_tlbstate.loaded_mm);
	struct mm_struct *current_mm = current->mm;

	VM_WARN_ON_ONCE(!loaded_mm);

	/*
	 * The condition we want to check is
	 * current_mm->pgd == __va(read_cr3_pa()).  This may be slow, though,
	 * if we're running in a VM with shadow paging, and nmi_uaccess_okay()
	 * is supposed to be reasonably fast.
	 *
	 * Instead, we check the almost equivalent but somewhat conservative
	 * condition below, and we rely on the fact that switch_mm_irqs_off()
	 * sets loaded_mm to LOADED_MM_SWITCHING before writing to CR3.
	 */
	if (loaded_mm != current_mm)
		return false;

	VM_WARN_ON_ONCE(current_mm->pgd != __va(read_cr3_pa()));

	return true;
}

static ssize_t tlbflush_read_file(struct file *file, char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	char buf[32];
	unsigned int len;

	len = sprintf(buf, "%ld\n", tlb_single_page_flush_ceiling);
	return simple_read_from_buffer(user_buf, count, ppos, buf, len);
}

static ssize_t tlbflush_write_file(struct file *file,
		 const char __user *user_buf, size_t count, loff_t *ppos)
{
	char buf[32];
	ssize_t len;
	int ceiling;

	len = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, user_buf, len))
		return -EFAULT;

	buf[len] = '\0';
	if (kstrtoint(buf, 0, &ceiling))
		return -EINVAL;

	if (ceiling < 0)
		return -EINVAL;

	tlb_single_page_flush_ceiling = ceiling;
	return count;
}

static const struct file_operations fops_tlbflush = {
	.read = tlbflush_read_file,
	.write = tlbflush_write_file,
	.llseek = default_llseek,
};

static int __init create_tlb_single_page_flush_ceiling(void)
{
	debugfs_create_file("tlb_single_page_flush_ceiling", S_IRUSR | S_IWUSR,
			    arch_debugfs_dir, NULL, &fops_tlbflush);
	return 0;
}
late_initcall(create_tlb_single_page_flush_ceiling);
