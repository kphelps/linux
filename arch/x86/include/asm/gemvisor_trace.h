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
#define GEMVISOR_TRACE_PAGE_SIZE   (16 * 1024)
#define GEMVISOR_TRACE_MAGIC       0x47454D54  /* "GEMT" */
#define GEMVISOR_TRACE_HEADER_SIZE 64

/* Payload format */
#define GEM_TRACE_PAYLOAD_VERSION  3
/*
 * Stack depth and payload sizing are bounded by the u8 event size field.
 * With a 32-byte header and richer register payloads we cap the depth at 20
 * to stay < 255 bytes while still capturing meaningful context.
 */
#define GEM_TRACE_MAX_STACK_DEPTH  20

/* Hypercall numbers */
#define GEMVISOR_HC_TRACE_INIT     0x47454D03
#define GEMVISOR_HC_TRACE_FLUSH    0x47454D02

/* Event types - Timing (0x00xx) */
#define GEM_EVT_TIMER_ARM          0x0001
#define GEM_EVT_TIMER_FIRE         0x0002
#define GEM_EVT_DELAY_REQ          0x0003
#define GEM_EVT_HRTIMER_START      0x0004
#define GEM_EVT_HRTIMER_EXPIRE     0x0005
#define GEM_EVT_HRTIMER_CANCEL     0x0006

/* Event types - Memory (0x10xx) */
#define GEM_EVT_PAGE_FAULT         0x1001
#define GEM_EVT_TLB_FLUSH          0x1002
#define GEM_EVT_CR3_SWITCH         0x1003
#define GEM_EVT_SPURIOUS_FAULT     0x1004
#define GEM_EVT_PTE_MODIFY         0x1005  /* PTE permission/presence change */
#define GEM_EVT_COW_FAULT          0x1006  /* Copy-on-write page copy */
#define GEM_EVT_DO_WP_PAGE         0x1007  /* Write-protect fault handling entry */

/* Event types - Interrupt (0x20xx) */
#define GEM_EVT_IRQ_ENTRY          0x2001
#define GEM_EVT_IRQ_EXIT           0x2002
#define GEM_EVT_SOFTIRQ_ENTRY      0x2003
#define GEM_EVT_SOFTIRQ_EXIT       0x2004
#define GEM_EVT_IRQ_EOI            0x2005

/* Event types - Scheduler (0x30xx) */
#define GEM_EVT_SCHED_SWITCH       0x3001
#define GEM_EVT_SCHED_WAKEUP       0x3002

/* Event types - Syscall (0x50xx) */
#define GEM_EVT_SYSCALL_ENTER      0x5001
#define GEM_EVT_SYSCALL_EXIT       0x5002

/* Event types - Control (0xF0xx) */
#define GEM_EVT_MILESTONE          0xF001
#define GEM_EVT_BUFFER_WRAP        0xF002
#define GEM_EVT_FLUSH_REQ          0xF003

/* Syscall ABI identifiers */
#define GEM_SYSCALL_ABI_X64        1
#define GEM_SYSCALL_ABI_X32        2
#define GEM_SYSCALL_ABI_IA32       3

/* Trace header structure (64 bytes) */
struct gem_trace_header {
	__u32 magic;
	__u16 version;
	__u16 flags;
	__u32 write_ptr;
	__u32 read_ptr;
	__u32 sequence;
	__u32 wrap_count;
	__u32 ring_size;
	__u8 reserved[36];
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

/* Payload header for structured guest events */
struct gem_trace_payload_hdr {
	__u8 version;      /* GEM_TRACE_PAYLOAD_VERSION */
	__u8 body_len;     /* Length of event-specific payload (bytes) */
	__u8 stack_depth;  /* Number of captured stack frames */
	__u8 reserved;     /* For v2: size in bytes of appended register snapshot */
} __packed;

/* Initialize the gemvisor trace subsystem (call early in boot) */
void __init gemvisor_trace_init(void);

/* Emit a trace event (call from anywhere) */
void gemvisor_trace_emit(u16 event_type, u32 flags, const void *payload, u8 payload_len);
/* Emit a trace event with explicit pt_regs (preferred when available) */
void gemvisor_trace_emit_regs(u16 event_type, u32 flags, const void *payload, u8 payload_len,
				 struct pt_regs *regs);

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

/*
 * Extended page fault payload with VMA and PTE context for debugging.
 * Total: 20 bytes (original) + 36 bytes (new) = 56 bytes
 */
struct gem_pf_payload {
	/* Original fields */
	__u64 gva;		/* Faulting guest virtual address */
	__u32 error_code;	/* x86 page fault error code */
	__u64 rip;		/* Instruction pointer that caused fault */
	/* Extended fields for root cause analysis */
	__u64 vm_start;		/* VMA start (0 if no VMA found) */
	__u64 vm_end;		/* VMA end (0 if no VMA found) */
	__u64 vm_flags;		/* VMA flags (VM_READ|VM_WRITE|VM_EXEC|...) */
	__u64 pte_val;		/* PTE value at fault address (0 if walk failed) */
	__u32 pid;		/* current->pid */
} __packed;

/* Helper to collect extended page fault info - implemented in gemvisor_trace.c */
void gem_trace_collect_pf_context(struct gem_pf_payload *pl, unsigned long address,
				  struct pt_regs *regs);

#define gem_trace_page_fault(_gva, _ec) do { \
	struct gem_pf_payload _pl = { \
		.gva = (_gva), \
		.error_code = (_ec), \
		.rip = (u64)__builtin_return_address(0), \
	}; \
	gem_trace_collect_pf_context(&_pl, (_gva), NULL); \
	gemvisor_trace_emit(GEM_EVT_PAGE_FAULT, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_page_fault_regs(_regs, _gva, _ec) do { \
	struct gem_pf_payload _pl = { \
		.gva = (_gva), \
		.error_code = (_ec), \
		.rip = (_regs) ? (u64)(_regs)->ip : 0, \
	}; \
	gem_trace_collect_pf_context(&_pl, (_gva), (_regs)); \
	gemvisor_trace_emit_regs(GEM_EVT_PAGE_FAULT, 0, &_pl, sizeof(_pl), (_regs)); \
} while (0)

#define gem_trace_irq_entry(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_IRQ_ENTRY, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_irq_exit(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_IRQ_EXIT, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_irq_eoi(vector) do { \
	u8 _v = (vector); \
	gemvisor_trace_emit(GEM_EVT_IRQ_EOI, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_softirq_entry(vec) do { \
	u32 _v = (vec); \
	gemvisor_trace_emit(GEM_EVT_SOFTIRQ_ENTRY, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_softirq_exit(vec) do { \
	u32 _v = (vec); \
	gemvisor_trace_emit(GEM_EVT_SOFTIRQ_EXIT, 0, &_v, sizeof(_v)); \
} while (0)

#define gem_trace_tlb_flush(reason, addr) do { \
	struct { u8 reason_field; u64 addr_field; } __packed _pl = { \
		.reason_field = (u8)(reason), .addr_field = (u64)(addr) }; \
	gemvisor_trace_emit(GEM_EVT_TLB_FLUSH, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_cr3_switch(old_cr3, new_cr3) do { \
	struct { u64 old_field; u64 new_field; } __packed _pl = { \
		.old_field = (u64)(old_cr3), .new_field = (u64)(new_cr3) }; \
	gemvisor_trace_emit(GEM_EVT_CR3_SWITCH, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_sched_switch(prev_pid, next_pid, prev_state) do { \
	struct { u32 prev_field; u32 next_field; u8 state_field; } __packed _pl = { \
		.prev_field = (u32)(prev_pid), .next_field = (u32)(next_pid), .state_field = (u8)(prev_state) }; \
	gemvisor_trace_emit(GEM_EVT_SCHED_SWITCH, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_sched_wakeup(pid, cpu) do { \
	struct { u32 pid_field; u8 cpu_field; } __packed _pl = { \
		.pid_field = (u32)(pid), .cpu_field = (u8)(cpu) }; \
	gemvisor_trace_emit(GEM_EVT_SCHED_WAKEUP, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_hrtimer_start(expires_ns) do { \
	u64 _e = (expires_ns); \
	gemvisor_trace_emit(GEM_EVT_HRTIMER_START, 0, &_e, sizeof(_e)); \
} while (0)

#define gem_trace_hrtimer_expire(timer_addr) do { \
	u64 _a = (u64)(timer_addr); \
	gemvisor_trace_emit(GEM_EVT_HRTIMER_EXPIRE, 0, &_a, sizeof(_a)); \
} while (0)

#define gem_trace_hrtimer_cancel(timer_addr) do { \
	u64 _a = (u64)(timer_addr); \
	gemvisor_trace_emit(GEM_EVT_HRTIMER_CANCEL, 0, &_a, sizeof(_a)); \
} while (0)

/* Syscall trace helpers */
#define gem_trace_syscall_enter(nr, abi_tag) do { \
	struct { __u32 nr_field; __u8 abi_field; __u8 _pad[3]; } __packed _pl = { \
		.nr_field = (__u32)(nr), .abi_field = (__u8)(abi_tag), ._pad = { 0, 0, 0 } }; \
	gemvisor_trace_emit(GEM_EVT_SYSCALL_ENTER, 0, &_pl, sizeof(_pl)); \
} while (0)

#define gem_trace_syscall_exit(nr, abi_tag, ret) do { \
	struct { __u32 nr_field; __u8 abi_field; __u8 _pad[3]; __s64 ret_field; } __packed _pl = { \
		.nr_field = (__u32)(nr), .abi_field = (__u8)(abi_tag), ._pad = { 0, 0, 0 }, .ret_field = (__s64)(ret) }; \
	gemvisor_trace_emit(GEM_EVT_SYSCALL_EXIT, 0, &_pl, sizeof(_pl)); \
} while (0)

/* PTE modification tracing for determinism debugging */
#define gem_trace_pte_modify(gva, old_pte, new_pte) do { \
	struct { u64 gva_field; u64 old_pte_field; u64 new_pte_field; } __packed _pl = { \
		.gva_field = (u64)(gva), .old_pte_field = (u64)(old_pte), \
		.new_pte_field = (u64)(new_pte) }; \
	gemvisor_trace_emit(GEM_EVT_PTE_MODIFY, 0, &_pl, sizeof(_pl)); \
} while (0)

/* Spurious kernel fault tracing (stale TLB entry) */
#define gem_trace_spurious_fault(addr, error_code) do { \
	struct { u64 addr_field; u32 error_code_field; } __packed _pl = { \
		.addr_field = (u64)(addr), .error_code_field = (u32)(error_code) }; \
	gemvisor_trace_emit(GEM_EVT_SPURIOUS_FAULT, 0, &_pl, sizeof(_pl)); \
} while (0)

/* Copy-on-write fault tracing */
#define gem_trace_cow_fault(addr, old_pfn, new_pfn) do { \
	struct { u64 addr_field; u64 old_pfn_field; u64 new_pfn_field; } __packed _pl = { \
		.addr_field = (u64)(addr), .old_pfn_field = (u64)(old_pfn), \
		.new_pfn_field = (u64)(new_pfn) }; \
	gemvisor_trace_emit(GEM_EVT_COW_FAULT, 0, &_pl, sizeof(_pl)); \
} while (0)

/* Write-protect page fault entry tracing (with registers) */
#define gem_trace_do_wp_page(regs, vmf_addr, vmf_flags) do { \
	struct { u64 addr_field; u32 flags_field; } __packed _pl = { \
		.addr_field = (u64)(vmf_addr), .flags_field = (u32)(vmf_flags) }; \
	gemvisor_trace_emit_regs(GEM_EVT_DO_WP_PAGE, 0, &_pl, sizeof(_pl), (regs)); \
} while (0)

#endif /* _ASM_X86_GEMVISOR_TRACE_H */
