#pragma once

// lib8bit by Zac Walker
//
// 6502 CPU core interface: instruction execution and IRQ/NMI entry points.

struct machine_state;

void cpu_exec(machine_state* s, int32_t tick_count);
void cpu_irq(machine_state* s);
void nmi6502(machine_state* s);
