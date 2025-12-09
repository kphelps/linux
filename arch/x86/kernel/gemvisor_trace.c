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
#include <asm/early_ioremap.h>
#include <asm/gemvisor_trace.h>
#include <asm/processor.h>

#define MIN_EVENT_SIZE 32

static struct gem_trace_header __iomem *trace_header;
static void __iomem *trace_buffer;
static bool trace_enabled;
static void __iomem *early_trace_mapping;
static u32 ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;

/* I/O port for trace hypercalls (KVM reliably forwards I/O to userspace) */
#define GEMVISOR_TRACE_PORT	0x512

/* Trace hypercall commands */
#define TRACE_CMD_INIT		1
#define TRACE_CMD_FLUSH		2

void __init gemvisor_trace_init(void)
{
	void __iomem *ptr;
	u32 magic;

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

	trace_header = (struct gem_trace_header __iomem *)ptr;
	trace_buffer = (void __iomem *)trace_header + GEMVISOR_TRACE_HEADER_SIZE;
	early_trace_mapping = ptr;
	ring_buffer_size = readl(&trace_header->ring_size);
	if (!ring_buffer_size)
		ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;

	/* Initialize hypervisor trace subsystem via I/O port */
	outl(TRACE_CMD_INIT, GEMVISOR_TRACE_PORT);

	trace_enabled = true;
	pr_info("gemvisor-trace: initialized at GPA %#lx\n",
		(unsigned long)GEMVISOR_TRACE_PAGE_GPA);
}

static int __init gemvisor_trace_remap(void)
{
	void __iomem *ptr;
	unsigned long flags;

	if (!trace_enabled || !trace_header || !early_trace_mapping)
		return 0;

	ptr = ioremap(GEMVISOR_TRACE_PAGE_GPA, GEMVISOR_TRACE_PAGE_SIZE);
	if (!ptr) {
		pr_warn("gemvisor-trace: ioremap remap failed; continuing with early mapping\n");
		return 0;
	}

	local_irq_save(flags);
	trace_header = (struct gem_trace_header __iomem *)ptr;
	trace_buffer = (void __iomem *)trace_header + GEMVISOR_TRACE_HEADER_SIZE;
	ring_buffer_size = readl(&trace_header->ring_size);
	if (!ring_buffer_size)
		ring_buffer_size = GEMVISOR_TRACE_PAGE_SIZE - GEMVISOR_TRACE_HEADER_SIZE;
	local_irq_restore(flags);

	early_iounmap(early_trace_mapping, GEMVISOR_TRACE_PAGE_SIZE);
	early_trace_mapping = NULL;

	pr_info("gemvisor-trace: remapped trace page with permanent mapping\n");
	return 0;
}
core_initcall(gemvisor_trace_remap);

void gemvisor_trace_emit(u16 event_type, u32 flags, const void *payload, u8 payload_len)
{
	struct gem_trace_event evt;
	u32 write_ptr, event_size, avail;
	unsigned int flush_tries = 0;
	unsigned long irq_flags;

	if (!trace_enabled || !trace_header)
		return;

	/* Prevent IRQ handlers from interleaving writes into the trace ring */
	local_irq_save(irq_flags);

	/* Calculate event size (32-byte header + payload, aligned to 8 bytes) */
	event_size = MIN_EVENT_SIZE + payload_len;
	event_size = (event_size + 7) & ~7;

	if (event_size > ring_buffer_size) {
		local_irq_restore(irq_flags);
		panic("gemvisor-trace: event larger than trace ring");
	}

	if (event_size > 128) {
		/* Limit event size to prevent buffer overflow */
		local_irq_restore(irq_flags);
		return;
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
	/* Ensure header/payload stores are visible before updating pointers */
	wmb();
	writel(write_ptr + event_size, &trace_header->write_ptr);
	writel(readl(&trace_header->sequence) + 1, &trace_header->sequence);

	local_irq_restore(irq_flags);
}
EXPORT_SYMBOL_GPL(gemvisor_trace_emit);
