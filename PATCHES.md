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
