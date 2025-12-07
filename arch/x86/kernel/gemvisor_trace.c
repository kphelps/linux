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
#include <linux/io.h>
#include <linux/string.h>
#include <asm/gemvisor_trace.h>

#define RING_BUFFER_SIZE (GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE)
#define MIN_EVENT_SIZE 32

static struct gem_trace_header __iomem *trace_header;
static void __iomem *trace_buffer;
static bool trace_enabled;

/* Issue a VMCALL hypercall */
static inline u64 gemvisor_hypercall(u64 nr, u64 a0)
{
	u64 ret;
	asm volatile("vmcall"
		: "=a" (ret)
		: "a" (nr), "b" (a0)
		: "memory");
	return ret;
}

/* Check if running under gemvisor */
static bool __init is_gemvisor_guest(void)
{
	/* Check for the magic in the trace header */
	void __iomem *ptr = ioremap(GEMVISOR_TRACE_PAGE_GPA, GEMVISOR_TRACE_PAGE_SIZE);
	if (!ptr)
		return false;

	u32 magic = readl(ptr);
	iounmap(ptr);

	return magic == GEMVISOR_TRACE_MAGIC;
}

void __init gemvisor_trace_init(void)
{
	u64 ret;

	if (!is_gemvisor_guest()) {
		pr_info("gemvisor-trace: not running under gemvisor\n");
		return;
	}

	trace_header = ioremap(GEMVISOR_TRACE_PAGE_GPA, GEMVISOR_TRACE_PAGE_SIZE);
	if (!trace_header) {
		pr_err("gemvisor-trace: failed to map trace page\n");
		return;
	}

	trace_buffer = (void __iomem *)trace_header + GEMVISOR_TRACE_HEADER_SIZE;

	/* Verify magic */
	if (readl(&trace_header->magic) != GEMVISOR_TRACE_MAGIC) {
		pr_warn("gemvisor-trace: invalid magic in trace header\n");
		iounmap(trace_header);
		trace_header = NULL;
		return;
	}

	/* Initialize hypervisor trace subsystem */
	ret = gemvisor_hypercall(GEMVISOR_HC_TRACE_INIT, 0);
	if (ret != 0) {
		pr_warn("gemvisor-trace: TRACE_INIT hypercall failed: %lld\n", ret);
	}

	trace_enabled = true;
	pr_info("gemvisor-trace: initialized at GPA %#lx\n",
		(unsigned long)GEMVISOR_TRACE_PAGE_GPA);
}

void gemvisor_trace_emit(u16 event_type, u32 flags, const void *payload, u8 payload_len)
{
	struct gem_trace_event evt;
	u32 write_ptr, event_size, avail;

	if (!trace_enabled || !trace_header)
		return;

	/* Calculate event size (32-byte header + payload, aligned to 8 bytes) */
	event_size = MIN_EVENT_SIZE + payload_len;
	event_size = (event_size + 7) & ~7;

	if (event_size > 128) {
		/* Limit event size to prevent buffer overflow */
		return;
	}

	/* Read current write pointer */
	write_ptr = readl(&trace_header->write_ptr);

	/* Check if we need to wrap */
	avail = RING_BUFFER_SIZE - write_ptr;
	if (avail < event_size) {
		/* Wrap to start */
		writel(0, &trace_header->write_ptr);
		write_ptr = 0;
		writel(readl(&trace_header->wrap_count) + 1, &trace_header->wrap_count);
	}

	/* Build event header */
	evt.size = event_size;
	evt.type_hi = (event_type >> 8) & 0xFF;
	evt.type_lo = event_type & 0xFFFF;
	evt.flags = flags;
	evt.vtime_ns = 0;  /* Hypervisor fills this from pvclock */
	evt.retired = 0;   /* Hypervisor fills this from PMC */
	evt.rip = (u64)__builtin_return_address(0);

	/* Write event header */
	memcpy_toio(trace_buffer + write_ptr, &evt, MIN_EVENT_SIZE);

	/* Write payload if present */
	if (payload_len > 0 && payload != NULL) {
		memcpy_toio(trace_buffer + write_ptr + MIN_EVENT_SIZE, payload, payload_len);
	}

	/* Advance write pointer and sequence */
	writel(write_ptr + event_size, &trace_header->write_ptr);
	writel(readl(&trace_header->sequence) + 1, &trace_header->sequence);
}
EXPORT_SYMBOL_GPL(gemvisor_trace_emit);
