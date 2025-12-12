# Linux Kernel Patches for Gemvisor

This document tracks deviations from upstream Linux kernel sources required for deterministic execution.

## 1. Remove jitterentropy dependency from CRYPTO_DRBG

**Kernel Version:** 6.6.61

**File Modified:** `crypto/Kconfig`

**Change:**
Removed `select CRYPTO_JITTERENTROPY` from the `CRYPTO_DRBG` config block.

```diff
 config CRYPTO_DRBG
 	tristate
 	default CRYPTO_DRBG_MENU
 	select CRYPTO_RNG
-	select CRYPTO_JITTERENTROPY
```

**Reason:**
Jitterentropy RNG relies on CPU timing jitter measured via RDTSC. Gemvisor's deterministic TSC virtualization returns consistent time values within each execution quantum, which causes jitterentropy's health check to fail with error code 9 (JENT_EHEALTH - "host not compliant with requirements").

Without this patch, `CONFIG_CRYPTO_DRBG=y` would force `CONFIG_CRYPTO_JITTERENTROPY=y` via the Kconfig `select` mechanism, making it impossible to disable jitterentropy.

**Impact:**
- DRBG continues to function normally, seeding from other entropy sources (virtio-rng)
- No security impact: virtio-rng is the designated controlled entropy source for deterministic execution
- Kernel boot no longer shows jitterentropy initialization failure

**Related Config:**
- `CONFIG_CRYPTO_JITTERENTROPY=n` in `.config`
- `CONFIG_HW_RANDOM_VIRTIO=y` for virtio-rng entropy source

## 2. Disable Machine Check Exception (MCE)

**Kernel Version:** 6.6.61

**Config Change:** `# CONFIG_X86_MCE is not set`

**Reason:**
Machine Check Exception (MCE) reports hardware errors (memory bit flips, CPU overheating, cache errors, etc.). These are inherently non-deterministic events that cannot be replayed. MCE is incompatible with gemvisor's deterministic execution model.

Previously, the hypervisor passed `nomce` boot parameter to disable MCE at runtime, but this still caused an error message: "mce: Unable to init MCE device (rc: -5)". Disabling at compile time eliminates both the MCE subsystem and the error message.

**Impact:**
- No MCE device initialization or error messages during boot
- Slightly smaller kernel (no MCE code compiled in)
- No functional impact: MCE is only useful for detecting physical hardware errors, which are not possible in a virtual environment with controlled execution

## 3. Disable Wireless Subsystem (cfg80211/mac80211/rfkill)

**Kernel Version:** 6.6.61

**Config Changes:**
- `# CONFIG_CFG80211 is not set`
- `# CONFIG_RFKILL is not set`

**Reason:**
The wireless subsystem (cfg80211, mac80211, rfkill) is not needed in a deterministic hypervisor environment. The guest uses virtio-net for networking, not WiFi. Disabling these eliminates:
- "platform regulatory.0: Direct firmware load for regulatory.db failed with error -2"
- "cfg80211: failed to load regulatory.db"

**Impact:**
- No wireless/WiFi support (not needed - virtio-net is used)
- Smaller kernel (no wireless stack compiled in)
- Cleaner boot log without firmware loading errors

## 4. Deterministic random_get_entropy()

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/include/asm/timex.h`

**Change:**
```diff
 static inline unsigned long random_get_entropy(void)
 {
-	if (!IS_ENABLED(CONFIG_X86_TSC) &&
-	    !cpu_feature_enabled(X86_FEATURE_TSC))
-		return random_get_entropy_fallback();
-	return rdtsc();
+	/*
+	 * For deterministic hypervisor execution (gemvisor), return a constant
+	 * value. This disables interrupt-timing-based entropy collection, which
+	 * relies on TSC values at interrupt time that vary non-deterministically
+	 * due to PMC skid. The kernel gets entropy from SETUP_RNG_SEED and
+	 * virtio-rng instead.
+	 */
+	return 0;
 }
```

**Reason:**
The kernel's `add_interrupt_randomness()` function collects entropy by sampling the TSC (via `random_get_entropy()`) at each interrupt. Due to PMC skid in the hypervisor, interrupts arrive at slightly different instruction counts on each run, causing TSC values to differ. This introduces non-determinism into the kernel's CRNG state.

By returning a constant value, we disable this entropy source entirely. The kernel still receives deterministic entropy from:
- `SETUP_RNG_SEED` (bootloader-provided seed via setup_data)
- `virtio-rng` device (seeded by hypervisor config)

**Impact:**
- Kernel prints a warning at boot: "Missing cycle counter and fallback timer; RNG entropy collection will consequently suffer."
- This is expected and harmless for deterministic execution
- All entropy comes from controlled, deterministic sources
- Boot is fully deterministic with identical serial logs across runs

**Related Hypervisor Changes:**
- MSR filter denies access to LBR MSRs (0x1c8-0x1cf, 0x680-0x6ff, 0xdc0-0xddf) for deterministic kernel PMU initialization
- Exit handler returns 0 for LBR MSR reads

## 5. IO-Port-based Delay

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/lib/delay.c`

**Change:**
Replace the standard delay loop with an IO port write that advances virtual time deterministically.

```c
#define GEMVISOR_DELAY_PORT	0x510

static inline void gemvisor_delay_ns(unsigned long ns)
{
	/* Write delay in nanoseconds to the gemvisor delay port. */
	outl((u32)ns, GEMVISOR_DELAY_PORT);
}
```

The `__delay()` function is modified to:
1. Convert loops to nanoseconds using `loops_per_jiffy` calibration
2. Write the nanoseconds value to IO port 0x510
3. The hypervisor intercepts this IO write and advances virtual time

**Reason:**
Standard delay loops are non-deterministic because:
- The number of loop iterations completed varies due to PMC skid
- The actual delay depends on instruction execution timing
- Different runs may complete different numbers of loops before being interrupted

With the IO port approach:
- The guest requests a specific delay amount via IO port write
- The hypervisor advances virtual time exactly by that amount
- No loop iteration count variance affects the outcome
- Uses KVM_EXIT_IO which is the recommended guest-to-host interface

**Impact:**
- `udelay()`, `ndelay()`, and `mdelay()` become deterministic
- Hardware initialization delays are exact
- No timing-based non-determinism from delay loops
- Requires `lpj=` boot parameter to be set (e.g., `lpj=5000000` for 1GHz TSC)

**Related Hypervisor Changes:**
- Added `GEMVISOR_DELAY_PORT` (0x510) handling in `handle_io_out()`
- IO writes to port 0x510 advance vtime by the written nanoseconds value
- Supports both 4-byte (u32) and 8-byte (u64) writes

## 6. Disable Hardware Breakpoint Activity Checks

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/include/asm/debugreg.h`

**Change:**
Force `hw_breakpoint_active()` to always return false when `CONFIG_GEMVISOR_DETERMINISM=y`:

```diff
 static __always_inline bool hw_breakpoint_active(void)
 {
-	return this_cpu_read(cpu_dr7) & DR7_ACTIVE_MASK;
+	/* GEMVISOR: force deterministic path */
+	return false;
 }
```

**Reason:**
During text-patching and other sensitive paths (e.g., `text_poke()` via `use_temporary_mm()`), the kernel conditionally disables hardware breakpoints based on the per‑CPU `cpu_dr7` shadow state. After snapshot/restore, that shadow can legitimately differ between identical VMs, causing:

- Different branch decisions (disable vs. skip disable)
- Different instruction counts and debug‑register side effects

By forcing the “no breakpoints active” path, the guest always takes the same instruction sequence.

**Impact:**
- Hardware watchpoints / breakpoints are effectively disabled in determinism builds.
- The following guest features will not work with `CONFIG_GEMVISOR_DETERMINISM=y`:
  - GDB hardware watchpoints / breakpoints (DR0–DR7)
  - perf hardware breakpoint events
  - KGDB/KDB hardware breakpoints
  - ptrace DR register access (PEEKUSR/POKEUSR)
- Use software watchpoints instead, or rebuild with `CONFIG_GEMVISOR_DETERMINISM=n` if you need DR‑based debugging (at the cost of determinism guarantees).
- Deterministic text‑patching paths no longer depend on per‑CPU debug state.

## 7. Deterministic CR4 Shadow Updates

**Kernel Version:** 6.6.61

**Files Modified:**
- `arch/x86/kernel/cpu/common.c`
- `arch/x86/kernel/process.c`
- `arch/x86/mm/tlb.c`

**Change:**
Make CR4 update helpers derive from the real CR4 register and always write unconditionally:

- `cr4_update_irqsoff(set, clear)` now reads CR4 via `__read_cr4()` instead of `cpu_tlbstate.cr4`, computes `newval`, then writes both the shadow and hardware CR4 every call.
- `cr4_toggle_bits_irqsoff(mask)` similarly reads CR4 from hardware and writes both shadow + hardware unconditionally.
- `cr4_update_pce_mm()` always clears `CR4.PCE` rather than branching on per‑CPU perf state.

**Reason:**
Upstream uses per‑CPU CR4 shadow state (`cpu_tlbstate.cr4`) and perf bookkeeping to avoid redundant CR4 writes. After snapshot/restore, those per‑CPU shadows may differ between otherwise identical VMs. If we base CR4 decisions on those shadows, the guest can:

- Take different branches
- Execute different numbers of CR4 writes over time
- Accumulate instruction‑count drift

Reading the authoritative hardware CR4 value eliminates the per‑CPU dependency, and unconditional writes keep instruction counts identical regardless of prior state.

**Impact:**
- Eliminates CR4‑shadow divergence across restores.
- Minor performance cost from extra CR4 reads/writes.
- User‑level RDPMC (CR4.PCE) is always disabled, matching gemvisor’s deterministic PMU model.

## 8. Eager TLB Flushes on Permission Upgrades

**Kernel Version:** 6.6.61

**Files Modified:**
- `arch/x86/mm/pat/set_memory.c`
- `mm/memory.c`
- `arch/x86/kernel/ldt.c`

**Change:**
Flush stale TLB entries immediately when increasing permissions:

- In `set_memory_*()` (CPA / PAT path), after updating a PTE/PMD to **clear NX** or **set RW**, flush the relevant TLB entries eagerly (`flush_tlb_one_kernel()` for 4K mappings; `flush_tlb_all()` for large mappings).
- In the COW fast‑path (`wp_page_reuse()`), flush `flush_tlb_page()` before installing a writable PTE.
- In the LDT IPI handler (`flush_ldt()`), remove the early return that depended on `cpu_tlbstate.loaded_mm` and always reload LDT + refresh segment caches.

**Reason:**
Linux normally defers TLB invalidation on permission increases for performance. That can leave stale RO/NX entries resident, producing **spurious faults** whose timing varies between runs. In gemvisor’s determinism model, that window is unacceptable because fault arrival can differ by a few instructions after restore.

Eager flushing removes the stale‑TLB window and also avoids branching on per‑CPU mm state in the LDT flush path.

**Impact:**
- Spurious faults from lazy permission upgrades should not occur; any remaining cases are surfaced by patch 9 instrumentation.
- More TLB flushes (minor perf hit) but deterministic fault behavior.

## 9. Spurious Kernel Fault Instrumentation

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/fault.c`

**Change:**
Add instrumentation to detect when `spurious_kernel_fault()` successfully handles a fault:

```c
#define GEMVISOR_SPURIOUS_FAULT_PORT	0x511

/* In spurious_kernel_fault(), after successful check: */
if (ret) {
	/* Notify hypervisor of spurious fault for determinism debugging */
	outl((u32)address, GEMVISOR_SPURIOUS_FAULT_PORT);
	pr_warn_ratelimited("spurious_kernel_fault: addr=%lx error_code=%lx\n",
			    address, error_code);
}
```

**Reason:**
Spurious kernel faults occur when the TLB contains stale entries with fewer permissions than the page table entry. This happens after permission increases (RO→RW or NX→X) when the kernel lazily defers TLB invalidation for performance.

With eager TLB flushes (patch 004), spurious faults should NOT occur. If this instrumentation fires, it indicates:
1. The eager TLB flush is not covering all permission increase paths
2. There's a window where stale TLB entries can persist
3. Non-deterministic behavior may result (fault timing varies between runs)

**Impact:**
- Provides visibility into TLB flush effectiveness
- IO port 0x511 signals hypervisor when spurious fault is handled
- `pr_warn_ratelimited` logs the event to dmesg (rate-limited to avoid flood)
- Zero runtime cost when no spurious faults occur

**Related Hypervisor Changes:**
- Added `GEMVISOR_SPURIOUS_FAULT_PORT` (0x511) handling in `handle_io_out()`
- Logs warning: "SPURIOUS_KERNEL_FAULT: Guest handled spurious fault at {addr}"
- Any occurrence of this warning indicates a determinism bug to investigate

## 10. Unconditional switch_mm_irqs_off Operations

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
Remove the conditional around `cr4_update_pce_mm()` and `switch_ldt()` calls at the end of `switch_mm_irqs_off()`:

```diff
 	this_cpu_write(cpu_tlbstate.loaded_mm, next);
 	this_cpu_write(cpu_tlbstate.loaded_mm_asid, new_asid);

-	if (next != real_prev) {
-		cr4_update_pce_mm(next);
-		switch_ldt(real_prev, next);
-	}
+	/*
+	 * GEMVISOR DETERMINISM PATCH:
+	 * Always call cr4_update_pce_mm() and switch_ldt() unconditionally.
+	 */
+	cr4_update_pce_mm(next);
+	switch_ldt(real_prev, next);
 }
```

**Reason:**
The conditional `if (next != real_prev)` compares the `next` mm_struct with `real_prev`, which is read from the per-CPU `cpu_tlbstate.loaded_mm` at function entry. After snapshot restore, this per-CPU state can differ between VMs even when executing from the same logical point:

1. `real_prev = this_cpu_read(cpu_tlbstate.loaded_mm)` - reads per-CPU TLB state
2. At line 655: `if (next != real_prev)` - conditional on per-CPU state
3. VM A may have `next != real_prev` (true) and take the branch
4. VM B may have `next == real_prev` (false) and skip the branch
5. This causes RIP divergence with identical vtime/retired counts

The divergence manifests as:
- VM A at `switch_ldt` (already past cr4_update_pce_mm)
- VM B at `cr4_update_irqsoff` (inside cr4_update_pce_mm)

By removing the conditional, both VMs always execute the same code path regardless of per-CPU TLB state.

**Impact:**
- Eliminates per-CPU state divergence in switch_mm_irqs_off
- Both functions are safe to call unconditionally:
  - `cr4_update_pce_mm()`: Always clears CR4.PCE (via patch 7)
  - `switch_ldt()`: Internal conditional checks for LDT presence; no-op for most workloads
- Minimal performance impact: extra function calls that typically do nothing
- Required for instruction-level determinism during mm context switches

**Related Patches:**
- Patch 7: Deterministic CR4 updates (cr4_update_pce_mm/irqsoff/toggle helpers)

## 11. Deterministic ASID Allocation in choose_new_asid

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
Skip the ASID cache lookup loop and always use ASID 0 (fresh) with an unconditional flush:

```diff
 static void choose_new_asid(struct mm_struct *next, u64 next_tlb_gen,
 			    u16 *new_asid, bool *need_flush)
 {
-	u16 asid;
-
 	if (!static_cpu_has(X86_FEATURE_PCID)) {
 		*new_asid = 0;
 		*need_flush = true;
 		return;
 	}

-	if (this_cpu_read(cpu_tlbstate.invalidate_other))
-		clear_asid_other();
-
-	for (asid = 0; asid < TLB_NR_DYN_ASIDS; asid++) {
-		if (this_cpu_read(cpu_tlbstate.ctxs[asid].ctx_id) !=
-		    next->context.ctx_id)
-			continue;
-
-		*new_asid = asid;
-		*need_flush = (this_cpu_read(cpu_tlbstate.ctxs[asid].tlb_gen) <
-			       next_tlb_gen);
-		return;
-	}
-
-	/* GEMVISOR: Always allocate fresh ASID (skip cache lookup loop) */
-	*new_asid = this_cpu_add_return(cpu_tlbstate.next_asid, 1) - 1;
-	if (*new_asid >= TLB_NR_DYN_ASIDS) {
-		*new_asid = 0;
-		this_cpu_write(cpu_tlbstate.next_asid, 1);
-	}
-	*need_flush = true;
+	/* GEMVISOR: clear other-ASID slots only when PTI is enabled. */
+	if (static_cpu_has(X86_FEATURE_PTI))
+		clear_asid_other();
+
+	/* GEMVISOR: Always use ASID 0 and always flush. */
+	*new_asid = 0;
+	*need_flush = true;
 }
```

**Reason:**
The upstream implementation has two nondeterministic sources after restore:

1. **Cache lookup loop:** It scans `cpu_tlbstate.ctxs[]` to find a matching cached ASID. The iteration count and early‑exit point depend on per‑CPU cache contents, which may diverge across restores → different instruction counts.
2. **next_asid wrap‑around:** If no cached ASID is found, it increments `cpu_tlbstate.next_asid` and conditionally wraps at `TLB_NR_DYN_ASIDS`. Divergent per‑CPU counters lead to different wrap branches.

Gemvisor removes both by skipping the lookup loop entirely and pinning all user ASID allocations to **ASID 0** with `need_flush=true`. The PTI guard remains a `static_cpu_has()` check (boot‑time constant), so both VMs take the same path.

**Impact:**
- Eliminates ASID‑related per‑CPU state divergence at context switches.
- Always flushes user TLBs on switch (small perf hit; acceptable for determinism).
- Disables ASID rotation/caching optimizations, but single‑ASID workloads remain efficient on modern CPUs.

## 12. Skip Speculative Execution Mitigations in cond_mitigation

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
Skip IBPB and L1D flush decisions that depend on per-CPU state:

```diff
 static void cond_mitigation(struct task_struct *next)
 {
-	unsigned long prev_mm, next_mm;
+	unsigned long next_mm;

 	if (!next || !next->mm)
 		return;

 	next_mm = mm_mangle_tif_spec_bits(next);
-	prev_mm = this_cpu_read(cpu_tlbstate.last_user_mm_spec);
-
-	/* [IBPB and L1D flush logic removed for determinism] */
-	if (static_branch_likely(&switch_mm_cond_ibpb)) {
-		if (next_mm != prev_mm &&
-		    (next_mm | prev_mm) & LAST_USER_MM_IBPB)
-			indirect_branch_prediction_barrier();
-	}
-	/* ... additional mitigation checks removed ... */

+	/* GEMVISOR: Skip mitigations, just update per-CPU state */
 	this_cpu_write(cpu_tlbstate.last_user_mm_spec, next_mm);
 }
```

**Reason:**
The original code reads `cpu_tlbstate.last_user_mm_spec` and compares it with `next_mm` to decide whether to issue IBPB or L1D flush. This creates per-CPU state dependencies:
- Different `last_user_mm_spec` values → different branch decisions
- IBPB instruction itself adds non-deterministic branch predictor state

In gemvisor's deterministic execution model:
- Spectre/Meltdown mitigations are unnecessary (controlled single-process environment)
- L1D cache timing is not exploitable (deterministic scheduling)
- IBPB adds non-determinism to branch predictor state

**Impact:**
- Eliminates mitigation-related instruction count divergence
- Slightly reduced security (acceptable for deterministic execution)
- Still updates per-CPU state for kernel consistency

## 13. Remove Per-CPU Variable Reads at switch_mm_irqs_off Entry

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
Remove reads of `cpu_tlbstate.loaded_mm_asid` and `cpu_tlbstate_shared.is_lazy` at function entry:

```diff
 void switch_mm_irqs_off(...)
 {
     struct mm_struct *real_prev = this_cpu_read(cpu_tlbstate.loaded_mm);
-    u16 prev_asid = this_cpu_read(cpu_tlbstate.loaded_mm_asid);
-    bool was_lazy = this_cpu_read(cpu_tlbstate_shared.is_lazy);
+    /* GEMVISOR: Removed prev_asid and was_lazy reads */
```

**Reason:**
These per-CPU reads were only used for optimization paths that are now bypassed. Removing them:
- Eliminates variable instruction counts from cache/memory access patterns
- Reduces per-CPU state dependency at function entry

## 14. Always Write is_lazy = false Unconditionally

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
```diff
-    if (was_lazy)
-        this_cpu_write(cpu_tlbstate_shared.is_lazy, false);
+    /* GEMVISOR: Always write unconditionally for identical instruction count */
+    this_cpu_write(cpu_tlbstate_shared.is_lazy, false);
```

**Reason:**
The conditional write causes different instruction counts based on lazy TLB state.

## 15. Always Take "Switching MM" Path in switch_mm_irqs_off

**Kernel Version:** 6.6.61

**File Modified:** `arch/x86/mm/tlb.c`

**Change:**
Remove the `if (real_prev == next)` conditional block entirely and always execute the "switching mm" path.

**Original Code Had:**
1. `if (real_prev == next)` - "not switching" path with early returns
2. `else` - "switching" path with full mm switch logic

**Patched Code:**
Always executes the "switching" path regardless of whether real_prev == next.

**Reason:**
The original code had multiple early returns based on per-CPU state:
- `if (!was_lazy) return;` - early return based on lazy TLB mode
- `if (tlb_gen matches) return;` - early return if TLB is up to date

These early returns caused wildly different instruction counts (hundreds of instructions difference) between VMs that happened to have different per-CPU TLB state.

**Impact:**
- No early returns based on per-CPU state
- Always allocates fresh ASID (via patched choose_new_asid)
- Always flushes TLB (via need_flush=true from choose_new_asid)
- Identical instruction sequence regardless of prior TLB state
- Thread-to-thread switches within same process now do full mm switch (minor overhead)

**Test Improvement:**
- Before these patches: Failed at iteration 1, quantum 920 (~221K total quanta)
- After patches 11-12: Failed at iteration 2, quantum 9389 (~449K total quanta)
- After patches 13-15: Failed at iteration 3, quantum 1267 (~661K total quanta)
- Total improvement: ~3x more deterministic execution before divergence

## 16. Stabilize Page-Table Accessed/Dirty Bits (A/D)

**Kernel Version:** 6.6.61

**Files Modified:**
- `arch/x86/mm/pgtable.c`
- `arch/x86/include/asm/pgtable.h`

**Change:**
When `CONFIG_GEMVISOR_DETERMINISM=y`, prevent the guest from clearing page‑table Accessed bits and pre‑set A/D bits on new mappings:

- The aging helpers no longer clear the Accessed (A) bit:
  - `ptep_test_and_clear_young()`
  - `pmdp_test_and_clear_young()`
  - `pudp_test_and_clear_young()`
  - They now return whether the entry is young but leave `_PAGE_ACCESSED` set.
- The “mkold” helpers become no‑ops:
  - `pte_mkold()`, `pmd_mkold()`, `pud_mkold()` return the entry unchanged.
- On installation of present mappings:
  - `pfn_pte()`, `pfn_pmd()`, `pfn_pud()` always set `_PAGE_ACCESSED`.
  - If the entry is writable, they also set `_PAGE_DIRTY`.
- Writable mapping constructors start dirty:
  - `pte_mkwrite_novma()`, `pmd_mkwrite_novma()`, and `pud_mkwrite()` set Dirty alongside RW.

**Reason:**
x86 hardware sets A/D bits lazily during page walks. The exact instruction at which the CPU decides to set these bits can vary between runs due to microarchitectural timing and KVM skid. Two deterministic runs restored from the same snapshot can therefore diverge in paging‑structure RAM if:

1. The guest clears A bits for aging, and hardware re‑sets them later at different times.
2. The CPU tries to set A/D on a page‑table page that is currently mapped RX/shared after a checkpoint, causing first‑touch faults at different RIPs.

By never clearing A and pre‑setting A/D in software for present mappings, the guest avoids later hardware A/D updates entirely, keeping page‑table contents stable across checkpoints/restores.

**Impact:**
- Page aging may treat entries as “young” longer, which can mildly reduce reclamation efficiency.
- Writable mappings start Dirty more often, increasing dirty‑tracking pressure slightly.
- Both effects are accepted trade‑offs for instruction‑level determinism.
