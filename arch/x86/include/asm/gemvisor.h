/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Gemvisor guest↔hypervisor interface constants.
 *
 * These values are part of the determinism contract between the guest kernel
 * and gemvisor’s host VMM. Keep them synchronized with the host-side constants
 * under src/hypervisor/.
 */
#ifndef _ASM_X86_GEMVISOR_H
#define _ASM_X86_GEMVISOR_H

/*
 * I/O port allocation
 *
 * Gemvisor reserves a small contiguous range of I/O ports for deterministic
 * guest↔host communication. The base port is chosen to be stable across runs
 * and easy to recognize in traces.
 *
 * NOTE: 0x510 is used by QEMU fw_cfg in some configurations. Gemvisor guests
 * do not rely on fw_cfg, but any future integration should avoid overlapping
 * this range.
 */
#define GEMVISOR_PORT_BASE		0x510
#define GEMVISOR_DELAY_PORT		(GEMVISOR_PORT_BASE + 0)
#define GEMVISOR_SPURIOUS_FAULT_PORT	(GEMVISOR_PORT_BASE + 1)
#define GEMVISOR_TRACE_PORT		(GEMVISOR_PORT_BASE + 2)

/*
 * Shared trace buffer GPA
 *
 * The trace ring is mapped at a fixed guest physical address outside normal
 * RAM. The host memory layout reserves the high MMIO hole (0xFEC00000+) for
 * APIC/IOAPIC and gemvisor shared pages; this GPA must not overlap guest RAM.
 */
#define GEMVISOR_TRACE_PAGE_GPA		0xFEF10000

#endif /* _ASM_X86_GEMVISOR_H */
