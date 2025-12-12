// SPDX-License-Identifier: GPL-2.0
/*
 *	Precise Delay Loops for i386
 *
 *	Copyright (C) 1993 Linus Torvalds
 *	Copyright (C) 1997 Martin Mares <mj@atrey.karlin.mff.cuni.cz>
 *	Copyright (C) 2008 Jiri Hladky <hladky _dot_ jiri _at_ gmail _dot_ com>
 *
 *	The __delay function must _NOT_ be inlined as its execution time
 *	depends wildly on alignment on many x86 processors. The additional
 *	jump magic is needed to get the timing stable on all the CPU's
 *	we have to worry about.
 */

#include <linux/export.h>
#include <linux/sched.h>
#include <linux/timex.h>
#include <linux/preempt.h>
#include <linux/delay.h>

#include <asm/processor.h>
#include <asm/delay.h>
#include <asm/timer.h>
#include <asm/mwait.h>
#include <asm/io.h>

#ifdef CONFIG_SMP
# include <asm/smp.h>
#endif

/*
 * Gemvisor IO-port-based delay for deterministic execution.
 *
 * Instead of spinning in a loop (which has variable timing due to
 * PMC skid and instruction count variations), we write the delay
 * value to an IO port. The hypervisor intercepts this and advances
 * virtual time by the requested amount.
 */
#define GEMVISOR_DELAY_PORT	0x510
/* Must match GEMVISOR_MAX_DELAY_NS in the host to preserve delay semantics. */
#define GEMVISOR_MAX_DELAY_NS	1000000000ULL

static inline void gemvisor_delay_ns(unsigned long ns)
{
	/*
	 * Write delay in nanoseconds to the gemvisor delay port.
	 * The hypervisor will advance virtual time by this amount.
	 */
	outl((u32)ns, GEMVISOR_DELAY_PORT);
}

static void delay_loop(u64 __loops);

/*
 * Calibration and selection of the delay mechanism happens only once
 * during boot.
 */
static void (*delay_fn)(u64) __ro_after_init = delay_loop;
static void (*delay_halt_fn)(u64 start, u64 cycles) __ro_after_init;

/* simple loop based delay: */
static void delay_loop(u64 __loops)
{
	unsigned long loops = (unsigned long)__loops;

	asm volatile(
		"	test %0,%0	\n"
		"	jz 3f		\n"
		"	jmp 1f		\n"

		".align 16		\n"
		"1:	jmp 2f		\n"

		".align 16		\n"
		"2:	dec %0		\n"
		"	jnz 2b		\n"
		"3:	dec %0		\n"

		: "+a" (loops)
		:
	);
}

/* TSC based delay: */
static void delay_tsc(u64 cycles)
{
	u64 bclock, now;
	int cpu;

	preempt_disable();
	cpu = smp_processor_id();
	bclock = rdtsc_ordered();
	for (;;) {
		now = rdtsc_ordered();
		if ((now - bclock) >= cycles)
			break;

		/* Allow RT tasks to run */
		preempt_enable();
		rep_nop();
		preempt_disable();

		/*
		 * It is possible that we moved to another CPU, and
		 * since TSC's are per-cpu we need to calculate
		 * that. The delay must guarantee that we wait "at
		 * least" the amount of time. Being moved to another
		 * CPU could make the wait longer but we just need to
		 * make sure we waited long enough. Rebalance the
		 * counter for this CPU.
		 */
		if (unlikely(cpu != smp_processor_id())) {
			cycles -= (now - bclock);
			cpu = smp_processor_id();
			bclock = rdtsc_ordered();
		}
	}
	preempt_enable();
}

/*
 * On Intel the TPAUSE instruction waits until any of:
 * 1) the TSC counter exceeds the value provided in EDX:EAX
 * 2) global timeout in IA32_UMWAIT_CONTROL is exceeded
 * 3) an external interrupt occurs
 */
static void delay_halt_tpause(u64 start, u64 cycles)
{
	u64 until = start + cycles;
	u32 eax, edx;

	eax = lower_32_bits(until);
	edx = upper_32_bits(until);

	/*
	 * Hard code the deeper (C0.2) sleep state because exit latency is
	 * small compared to the "microseconds" that usleep() will delay.
	 */
	__tpause(TPAUSE_C02_STATE, edx, eax);
}

/*
 * On some AMD platforms, MWAITX has a configurable 32-bit timer, that
 * counts with TSC frequency. The input value is the number of TSC cycles
 * to wait. MWAITX will also exit when the timer expires.
 */
static void delay_halt_mwaitx(u64 unused, u64 cycles)
{
	u64 delay;

	delay = min_t(u64, MWAITX_MAX_WAIT_CYCLES, cycles);
	/*
	 * Use cpu_tss_rw as a cacheline-aligned, seldomly accessed per-cpu
	 * variable as the monitor target.
	 */
	 __monitorx(raw_cpu_ptr(&cpu_tss_rw), 0, 0);

	/*
	 * AMD, like Intel, supports the EAX hint and EAX=0xf means, do not
	 * enter any deep C-state and we use it here in delay() to minimize
	 * wakeup latency.
	 */
	__mwaitx(MWAITX_DISABLE_CSTATES, delay, MWAITX_ECX_TIMER_ENABLE);
}

/*
 * Call a vendor specific function to delay for a given amount of time. Because
 * these functions may return earlier than requested, check for actual elapsed
 * time and call again until done.
 */
static void delay_halt(u64 __cycles)
{
	u64 start, end, cycles = __cycles;

	/*
	 * Timer value of 0 causes MWAITX to wait indefinitely, unless there
	 * is a store on the memory monitored by MONITORX.
	 */
	if (!cycles)
		return;

	start = rdtsc_ordered();

	for (;;) {
		delay_halt_fn(start, cycles);
		end = rdtsc_ordered();

		if (cycles <= end - start)
			break;

		cycles -= end - start;
		start = end;
	}
}

void __init use_tsc_delay(void)
{
	if (delay_fn == delay_loop)
		delay_fn = delay_tsc;
}

void __init use_tpause_delay(void)
{
	delay_halt_fn = delay_halt_tpause;
	delay_fn = delay_halt;
}

void use_mwaitx_delay(void)
{
	delay_halt_fn = delay_halt_mwaitx;
	delay_fn = delay_halt;
}

int read_current_timer(unsigned long *timer_val)
{
	if (delay_fn == delay_tsc) {
		*timer_val = rdtsc();
		return 0;
	}
	return -1;
}

void __delay(unsigned long loops)
{
	/*
	 * For gemvisor deterministic execution: convert loops to nanoseconds
	 * and use IO port to advance virtual time.
	 *
	 * loops_per_jiffy corresponds to 1/HZ seconds worth of loops.
	 * ns = loops * (1e9 / HZ) / lpj
	 *
	 * Use 64-bit arithmetic to avoid overflow, guard lpj==0 during early boot,
	 * and emit long delays in u32-sized chunks because the delay port is 32-bit.
	 */
	unsigned long lpj = this_cpu_read(cpu_info.loops_per_jiffy) ? : loops_per_jiffy;
	u64 ns_per_jiffy = 1000000000ULL / HZ;
	u64 ns;

	if (!lpj) {
		/*
		 * loops_per_jiffy isn't calibrated yet. Fall back to the raw loop so
		 * callers still observe a delay.
		 */
		delay_loop(loops);
		return;
	}

	if (loops > U64_MAX / ns_per_jiffy)
		ns = U64_MAX;
	else
		ns = (u64)loops * ns_per_jiffy;
	ns = ns / (u64)lpj;

	while (ns) {
		u64 chunk64 = ns > GEMVISOR_MAX_DELAY_NS ? GEMVISOR_MAX_DELAY_NS : ns;
		gemvisor_delay_ns((unsigned long)chunk64);
		ns -= chunk64;
	}
}
EXPORT_SYMBOL(__delay);

noinline void __const_udelay(unsigned long xloops)
{
	unsigned long lpj = this_cpu_read(cpu_info.loops_per_jiffy) ? : loops_per_jiffy;
	int d0;

	xloops *= 4;
	asm("mull %%edx"
		:"=d" (xloops), "=&a" (d0)
		:"1" (xloops), "0" (lpj * (HZ / 4)));

	__delay(++xloops);
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	__const_udelay(usecs * 0x000010c7); /* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__const_udelay(nsecs * 0x00005); /* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);
