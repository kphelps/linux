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
#include <asm/early_ioremap.h>
#include <asm/gemvisor_trace.h>

#define RING_BUFFER_SIZE (GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE)
#define MIN_EVENT_SIZE 32

static struct gem_trace_header __iomem *trace_header;
static void __iomem *trace_buffer;
static bool trace_enabled;

/* I/O port for trace hypercalls (KVM reliably forwards I/O to userspace) */
#define GEMVISOR_TRACE_PORT	0x512

/* Trace hypercall commands */
#define TRACE_CMD_INIT		1
#define TRACE_CMD_FLUSH		2

void __init gemvisor_trace_init(void)
{
	void __iomem *ptr;
	u32 magic;

	/* Use early_ioremap during setup_arch() before slab is ready */
	ptr = early_ioremap(GEMVISOR_TRACE_PAGE_GPA, GEMVISOR_TRACE_PAGE_SIZE);
	if (!ptr) {
		pr_info("gemvisor-trace: early_ioremap failed\n");
		return;
	}

	/* Check for gemvisor magic */
	magic = readl(ptr);
	if (magic != GEMVISOR_TRACE_MAGIC) {
		early_iounmap(ptr, GEMVISOR_TRACE_PAGE_SIZE);
		pr_info("gemvisor-trace: not running under gemvisor (magic=%#x)\n", magic);
		return;
	}

	/* Keep the early mapping as our permanent mapping during boot.
	 * Note: We could switch to regular ioremap later, but the early
	 * mapping is sufficient for our purposes.
	 */
	trace_header = (struct gem_trace_header __iomem *)ptr;
	trace_buffer = (void __iomem *)trace_header + GEMVISOR_TRACE_HEADER_SIZE;

	/* Initialize hypervisor trace subsystem via I/O port */
	outl(TRACE_CMD_INIT, GEMVISOR_TRACE_PORT);

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
