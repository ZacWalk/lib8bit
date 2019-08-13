#pragma once

struct machine_state;

uint16_t getpc();
uint8_t getop();
void cpu_exec(machine_state* s, int32_t tick_count);
void cpu_irq(machine_state* s);
void nmi6502(machine_state* s);
