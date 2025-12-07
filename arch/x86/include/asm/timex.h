/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_TIMEX_H
#define _ASM_X86_TIMEX_H

#include <asm/processor.h>
#include <asm/tsc.h>

static inline unsigned long random_get_entropy(void)
{
	/*
	 * For deterministic hypervisor execution (gemvisor), return a constant
	 * value. This disables interrupt-timing-based entropy collection, which
	 * relies on TSC values at interrupt time that vary non-deterministically
	 * due to PMC skid. The kernel gets entropy from SETUP_RNG_SEED and
	 * virtio-rng instead.
	 */
	return 0;
}
#define random_get_entropy random_get_entropy

/* Assume we use the PIT time source for the clock tick */
#define CLOCK_TICK_RATE		PIT_TICK_RATE

#define ARCH_HAS_READ_CURRENT_TIMER

#endif /* _ASM_X86_TIMEX_H */
