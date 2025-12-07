/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Gemvisor guest-to-hypervisor trace subsystem
 *
 * This provides a low-overhead mechanism for the guest kernel to send
 * structured binary events to the hypervisor for determinism debugging.
 */
#ifndef _ASM_X86_GEMVISOR_TRACE_H
#define _ASM_X86_GEMVISOR_TRACE_H

#include <linux/types.h>

/* Guest physical address for the trace shared page */
#define GEMVISOR_TRACE_PAGE_GPA    0xFEF10000
#define GEMVISOR_TRACE_PAGE_SIZE   4096
#define GEMVISOR_TRACE_MAGIC       0x47454D54  /* "GEMT" */
#define GEMVISOR_TRACE_HEADER_SIZE 64

/* Hypercall numbers */
#define GEMVISOR_HC_TRACE_INIT     0x47454D03
#define GEMVISOR_HC_TRACE_FLUSH    0x47454D02

/* Event types */
#define GEM_EVT_TIMER_ARM          0x0001
#define GEM_EVT_TIMER_FIRE         0x0002
#define GEM_EVT_DELAY_REQ          0x0003
#define GEM_EVT_PAGE_FAULT         0x1001
#define GEM_EVT_TLB_FLUSH          0x1002
#define GEM_EVT_CR3_SWITCH         0x1003
#define GEM_EVT_SPURIOUS_FAULT     0x1004
#define GEM_EVT_IRQ_ENTRY          0x2001
#define GEM_EVT_IRQ_EXIT           0x2002
#define GEM_EVT_SOFTIRQ_ENTRY      0x2003
#define GEM_EVT_SOFTIRQ_EXIT       0x2004
#define GEM_EVT_MILESTONE          0xF001
#define GEM_EVT_BUFFER_WRAP        0xF002
#define GEM_EVT_FLUSH_REQ          0xF003

/* Trace header structure (64 bytes) */
struct gem_trace_header {
	__u32 magic;
	__u16 version;
	__u16 flags;
	__u32 write_ptr;
	__u32 read_ptr;
	__u32 sequence;
	__u32 wrap_count;
	__u8 reserved[40];
} __packed;

/* Event header structure (32 bytes minimum) */
struct gem_trace_event {
	__u8 size;
	__u8 type_hi;
	__u16 type_lo;
	__u32 flags;
	__u64 vtime_ns;
	__u64 retired;
	__u64 rip;
	__u8 payload[];
} __packed;

/* Initialize the gemvisor trace subsystem (call early in boot) */
void __init gemvisor_trace_init(void);

/* Emit a trace event (call from anywhere) */
void gemvisor_trace_emit(u16 event_type, u32 flags, const void *payload, u8 payload_len);

/* Convenience macros for common events */
#define gem_trace_milestone(id) do { \
	u32 _id = (id); \
	gemvisor_trace_emit(GEM_EVT_MILESTONE, 0, &_id, sizeof(_id)); \
} while (0)

#define gem_trace_timer_arm(deadline_ns) do { \
	u64 _d = (deadline_ns); \
	gemvisor_trace_emit(GEM_EVT_TIMER_ARM, 0, &_d, sizeof(_d)); \
} while (0)

#define gem_trace_timer_fire(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_TIMER_FIRE, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_page_fault(gva, error_code) do { \
	struct { u64 gva; u32 ec; } __packed _pl = { (gva), (error_code) }; \
	gemvisor_trace_emit(GEM_EVT_PAGE_FAULT, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_irq_entry(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_IRQ_ENTRY, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_irq_exit(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_IRQ_EXIT, 0, &_v, sizeof(_v)); \
} while (0)

#endif /* _ASM_X86_GEMVISOR_TRACE_H */
