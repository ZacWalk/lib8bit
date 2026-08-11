// lib8bit by Zac Walker
//
// 6502 CPU core: instruction execution, addressing modes, arithmetic and
// flag handling, the stack, and IRQ/NMI interrupt servicing.

#include <stdint.h>

#include "cpu.h"
#include "machine.h"
#include "opcodes.h"


#define BASE_STACK     0x100


// Everything below is internal to the CPU core; only cpu_exec, cpu_irq and
// nmi6502 have external linkage so lib8bit can be linked into a host app.
namespace
{
// Entry point of the KERNAL's own LOAD routine, i.e. the default contents of
// the LOAD vector at $0330 (see machine_state::kernal_load).
constexpr uint16_t KERNAL_LOAD_TRAP = 0xF4A5;
//general functions used by various other functions
void push16(machine_state* s, const uint16_t pushval)
{
	s->ram_write(BASE_STACK + s->cpu.sp, (pushval >> 8) & 0xFF);
	s->ram_write(BASE_STACK + ((s->cpu.sp - 1) & 0xFF), pushval & 0xFF);
	s->cpu.sp -= 2;
}

void push8(machine_state* s, const uint8_t pushval)
{
	s->ram_write(BASE_STACK + s->cpu.sp--, pushval);
}

uint16_t pull16(machine_state* s)
{
	const auto temp16 = s->ram_read(BASE_STACK + ((s->cpu.sp + 1) & 0xFF)) | (static_cast<uint16_t>(s->ram_read(
		BASE_STACK + ((s->cpu.sp + 2) & 0xFF))) << 8);
	s->cpu.sp += 2;
	return temp16;
}

uint8_t pull8(machine_state* s)
{
	return (s->ram_read(BASE_STACK + ++s->cpu.sp));
}


//addressing mode functions, calculates effective addresses
void imp(machine_state* s)
{
	//implied
}

uint16_t imm(machine_state* s)
{
	//immediate
	return s->cpu.pc++;
}

uint16_t zp(machine_state* s)
{
	//zero-page
	return s->ram_read(s->cpu.pc++);
}

uint16_t zpx(machine_state* s)
{
	//zero-page,s->cpu.x
	//zero-page wraparound
	return (static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s->cpu.x)) & 0xFF;
}

uint16_t zpy(machine_state* s)
{
	//zero-page,s->cpu.y
	//zero-page wraparound
	return (static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s->cpu.y)) & 0xFF;
}

uint16_t rel(machine_state* s)
{
	//relative for branch ops (8-bit immediate value, sign-extended)
	auto reladdr = static_cast<uint16_t>(s->ram_read(s->cpu.pc++));
	if (reladdr & 0x80) reladdr |= 0xFF00;
	return reladdr;
}

uint16_t abso(machine_state* s)
{
	//absolute
	const auto ea = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) | (static_cast<uint16_t>(s->ram_read(s->cpu.pc + 1))
		<< 8);
	s->cpu.pc += 2;
	return ea;
}

uint16_t absx(machine_state* s)
{
	//absolute,s->cpu.x
	const auto base = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) |
		(static_cast<uint16_t>(s->ram_read(s->cpu.pc + 1)) << 8);
	const auto ea = static_cast<uint16_t>(base + s->cpu.x);
	if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
	s->cpu.pc += 2;
	return ea;
}

uint16_t absy(machine_state* s)
{
	//absolute,s->cpu.y
	const auto base = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) |
		(static_cast<uint16_t>(s->ram_read(s->cpu.pc + 1)) << 8);
	const auto ea = static_cast<uint16_t>(base + s->cpu.y);
	if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
	s->cpu.pc += 2;
	return ea;
}

uint16_t ind(machine_state* s)
{
	//indirect
	const auto eahelp = static_cast<uint16_t>(s->ram_read(s->cpu.pc)) | static_cast<uint16_t>(static_cast<uint16_t>(s->
		ram_read(s->cpu.pc + 1)) << 8);
	const auto eahelp2 = (eahelp & 0xFF00) | ((eahelp + 1) & 0x00FF); //replicate 6502 page-boundary wraparound bug
	const auto ea = static_cast<uint16_t>(s->ram_read(eahelp)) | (static_cast<uint16_t>(s->ram_read(eahelp2)) << 8);
	s->cpu.pc += 2;
	return ea;
}

uint16_t indx(machine_state* s)
{
	// (indirect,s->cpu.x)	
	const auto eahelp = static_cast<uint16_t>((static_cast<uint16_t>(s->ram_read(s->cpu.pc++)) + static_cast<uint16_t>(s
		->cpu.x)) & 0xFF);
	//zero-page wraparound for table pointer
	const auto ea = static_cast<uint16_t>(s->ram_read(eahelp & 0x00FF)) | (static_cast<uint16_t>(s->ram_read(
		(eahelp + 1) & 0x00FF)) << 8);
	return ea;
}

uint16_t indy(machine_state* s)
{
	// (indirect),s->cpu.y

	const auto ea1 = static_cast<uint16_t>(s->ram_read(s->cpu.pc++));
	const auto ea2 = (ea1 & 0xFF00) | ((ea1 + 1) & 0x00FF); //zero-page wraparound
	const auto base = static_cast<uint16_t>(s->ram_read(ea1)) | (static_cast<uint16_t>(s->ram_read(ea2)) << 8);
	const auto ea = static_cast<uint16_t>(base + s->cpu.y);
	if ((ea & 0xFF00) != (base & 0xFF00)) s->penalty_addr = 1; // page crossed
	return ea;
}

static uint16_t get_ea(machine_state* s, const uint16_t ea)
{
	return s->ram_read(ea);
}

static uint16_t get_a(const machine_state* s)
{
	return s->cpu.a;
}

void put_ea(machine_state* s, const uint16_t saveval, const uint16_t ea)
{
	s->ram_write(ea, (saveval & 0x00FF));
}

void put_a(machine_state* s, const uint16_t saveval)
{
	s->cpu.a = static_cast<uint8_t>(saveval & 0x00FF);
}


//instruction handler functions
uint16_t adc(machine_state* s, const uint16_t value)
{
	const auto operand = value & 0x00FF;
	const auto carry_in = static_cast<uint16_t>(s->cpu.status & FLAG_CARRY);
	const auto result = static_cast<uint16_t>(s->cpu.a) + operand + carry_in;

	// N, Z, V, C come from the binary sum on the NMOS 6502/6510.
	s->cpu.calc_carry(result);
	s->cpu.calc_zero(result);
	s->cpu.calc_overflow(result, s->cpu.a, operand);
	s->cpu.calc_sign(result);

	if (s->cpu.status & FLAG_DECIMAL)
	{
		// Proper BCD addition with carry propagation between nybbles. On the NMOS
		// 6502 Z comes from the binary sum above, but N and V are taken from the
		// intermediate result after the low-nybble fixup and before the high one.
		unsigned al = (s->cpu.a & 0x0F) + (operand & 0x0F) + carry_in;
		if (al > 0x09) al = ((al + 0x06) & 0x0F) + 0x10;
		unsigned bcd = (s->cpu.a & 0xF0) + (operand & 0xF0) + al;

		s->cpu.calc_sign(static_cast<uint16_t>(bcd));
		if ((~(s->cpu.a ^ operand) & (s->cpu.a ^ bcd) & 0x80) != 0)
			s->cpu.set_overflow();
		else
			s->cpu.clear_overflow();

		if (bcd >= 0xA0) bcd += 0x60;
		if (bcd > 0xFF)
			s->cpu.set_carry();
		else
			s->cpu.clear_carry();

		return static_cast<uint16_t>(bcd & 0xFF);
	}

	return result;
}

uint16_t op_and(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.a) & value;

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

uint16_t asl(machine_state* s, const uint16_t value)
{
	const auto result = value << 1;

	s->cpu.calc_carry(result);
	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void bcc(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_CARRY) == 0)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void bcs(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_CARRY) == FLAG_CARRY)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void beq(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_ZERO) == FLAG_ZERO)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void op_bit(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.a) & value;

	s->cpu.calc_zero(result);
	s->cpu.status = (s->cpu.status & 0x3F) | static_cast<uint8_t>(value & 0xC0);
}

void bmi(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_SIGN) == FLAG_SIGN)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void bne(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_ZERO) == 0)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void bpl(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_SIGN) == 0)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void brk(machine_state* s)
{
	s->cpu.pc++;
	push16(s, s->cpu.pc); //push next instruction address onto stack
	push8(s, s->cpu.status | FLAG_BREAK); //push CPU s->cpu.status to stack
	s->cpu.set_interrupt(); //set interrupt flag
	s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFE)) | (static_cast<uint16_t>(s->ram_read(0xFFFF)) << 8);
}

void bvc(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_OVERFLOW) == 0)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void bvs(machine_state* s, const uint16_t reladdr)
{
	if ((s->cpu.status & FLAG_OVERFLOW) == FLAG_OVERFLOW)
	{
		const auto old_pc = s->cpu.pc;
		s->cpu.pc += reladdr;
		if ((old_pc & 0xFF00) != (s->cpu.pc & 0xFF00)) s->clock_ticks += 2;
			//check if jump crossed s->cpu.a page boundary
		else s->clock_ticks++;
	}
}

void clc(machine_state* s)
{
	s->cpu.clear_carry();
}

void cld(machine_state* s)
{
	s->cpu.clear_decimal();
}

void cli(machine_state* s)
{
	s->cpu.clear_interrupt();
}

void clv(machine_state* s)
{
	s->cpu.clear_overflow();
}

void cmp(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.a) - value;

	if (s->cpu.a >= static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_carry();
	else
		s->cpu.clear_carry();

	if (s->cpu.a == static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_zero();
	else
		s->cpu.clear_zero();

	s->cpu.calc_sign(result);
}

void cpx(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.x) - value;

	if (s->cpu.x >= static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_carry();
	else
		s->cpu.clear_carry();

	if (s->cpu.x == static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_zero();
	else
		s->cpu.clear_zero();

	s->cpu.calc_sign(result);
}

void cpy(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.y) - value;

	if (s->cpu.y >= static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_carry();
	else
		s->cpu.clear_carry();
	if (s->cpu.y == static_cast<uint8_t>(value & 0x00FF))
		s->cpu.set_zero();
	else
		s->cpu.clear_zero();
	s->cpu.calc_sign(result);
}

uint16_t dec(machine_state* s, const uint16_t value)
{
	const auto result = value - 1;

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void dex(machine_state* s)
{
	s->cpu.x--;

	s->cpu.calc_zero(s->cpu.x);
	s->cpu.calc_sign(s->cpu.x);
}

void dey(machine_state* s)
{
	s->cpu.y--;

	s->cpu.calc_zero(s->cpu.y);
	s->cpu.calc_sign(s->cpu.y);
}

uint16_t eor(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.a) ^ value;

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

uint16_t inc(machine_state* s, const uint16_t value)
{
	const auto result = value + 1;

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void inx(machine_state* s)
{
	s->cpu.x++;

	s->cpu.calc_zero(s->cpu.x);
	s->cpu.calc_sign(s->cpu.x);
}

void iny(machine_state* s)
{
	s->cpu.y++;

	s->cpu.calc_zero(s->cpu.y);
	s->cpu.calc_sign(s->cpu.y);
}

void jmp(machine_state* s, const uint16_t ea)
{
	s->cpu.pc = ea;
}

void jsr(machine_state* s, const uint16_t ea)
{
	push16(s, s->cpu.pc - 1);
	s->cpu.pc = ea;
}

void lda(machine_state* s, const uint16_t value)
{
	s->cpu.a = static_cast<uint8_t>(value & 0x00FF);

	s->cpu.calc_zero(s->cpu.a);
	s->cpu.calc_sign(s->cpu.a);
}

void ldx(machine_state* s, const uint16_t value)
{
	s->cpu.x = static_cast<uint8_t>(value & 0x00FF);

	s->cpu.calc_zero(s->cpu.x);
	s->cpu.calc_sign(s->cpu.x);
}

void ldy(machine_state* s, const uint16_t value)
{
	s->cpu.y = static_cast<uint8_t>(value & 0x00FF);

	s->cpu.calc_zero(s->cpu.y);
	s->cpu.calc_sign(s->cpu.y);
}

uint16_t lsr(machine_state* s, const uint16_t value)
{
	const auto result = value >> 1;

	if (value & 1)
		s->cpu.set_carry();
	else
		s->cpu.clear_carry();

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void nop(machine_state* s)
{
}

uint16_t ora(machine_state* s, const uint16_t value)
{
	const auto result = static_cast<uint16_t>(s->cpu.a) | value;

	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void pha(machine_state* s)
{
	push8(s, s->cpu.a);
}

void php(machine_state* s)
{
	push8(s, s->cpu.status | FLAG_BREAK);
}

void pla(machine_state* s)
{
	s->cpu.a = pull8(s);

	s->cpu.calc_zero(s->cpu.a);
	s->cpu.calc_sign(s->cpu.a);
}

void plp(machine_state* s)
{
	s->cpu.status = pull8(s) | FLAG_CONSTANT;
}

uint16_t rol(machine_state* s, const uint16_t value)
{
	const auto result = (value << 1) | (s->cpu.status & FLAG_CARRY);

	s->cpu.calc_carry(result);
	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

uint16_t ror(machine_state* s, const uint16_t value)
{
	const auto result = (value >> 1) | ((s->cpu.status & FLAG_CARRY) << 7);

	if (value & 1)
		s->cpu.set_carry();
	else
		s->cpu.clear_carry();
	s->cpu.calc_zero(result);
	s->cpu.calc_sign(result);

	return result;
}

void rti(machine_state* s)
{
	// On RTI the BREAK flag must be cleared and the unused CONSTANT bit forced on.
	s->cpu.status = (pull8(s) & ~FLAG_BREAK) | FLAG_CONSTANT;
	s->cpu.pc = pull16(s);
}

void rts(machine_state* s)
{
	const auto value = pull16(s);
	s->cpu.pc = value + 1;
}

uint16_t sbc(machine_state* s, const uint16_t value)
{
	const auto operand = value & 0x00FF;
	const auto inv = operand ^ 0x00FF;
	const auto carry_in = static_cast<uint16_t>(s->cpu.status & FLAG_CARRY);
	const auto result = static_cast<uint16_t>(s->cpu.a) + inv + carry_in;

	// N, Z, V, C are computed from the binary result on both NMOS 6502/6510.
	s->cpu.calc_carry(result);
	s->cpu.calc_zero(result);
	s->cpu.calc_overflow(result, s->cpu.a, inv);
	s->cpu.calc_sign(result);

	if (s->cpu.status & FLAG_DECIMAL)
	{
		// Standard NMOS BCD subtract with borrow.
		int al = (s->cpu.a & 0x0F) - (operand & 0x0F) - (carry_in ? 0 : 1);
		int ah = (s->cpu.a >> 4) - (operand >> 4);
		if (al < 0)
		{
			al -= 0x06;
			ah--;
		}
		if (ah < 0) { ah -= 0x06; }
		return static_cast<uint16_t>(((ah << 4) | (al & 0x0F)) & 0xFF);
	}

	return result;
}

void sec(machine_state* s)
{
	s->cpu.set_carry();
}

void sed(machine_state* s)
{
	s->cpu.set_decimal();
}

void sei(machine_state* s)
{
	s->cpu.set_interrupt();
}

void tax(machine_state* s)
{
	s->cpu.x = s->cpu.a;

	s->cpu.calc_zero(s->cpu.x);
	s->cpu.calc_sign(s->cpu.x);
}

void tay(machine_state* s)
{
	s->cpu.y = s->cpu.a;

	s->cpu.calc_zero(s->cpu.y);
	s->cpu.calc_sign(s->cpu.y);
}

void tsx(machine_state* s)
{
	s->cpu.x = s->cpu.sp;

	s->cpu.calc_zero(s->cpu.x);
	s->cpu.calc_sign(s->cpu.x);
}

void txa(machine_state* s)
{
	s->cpu.a = s->cpu.x;

	s->cpu.calc_zero(s->cpu.a);
	s->cpu.calc_sign(s->cpu.a);
}

void txs(machine_state* s)
{
	s->cpu.sp = s->cpu.x;
}

void tya(machine_state* s)
{
	s->cpu.a = s->cpu.y;

	s->cpu.calc_zero(s->cpu.a);
	s->cpu.calc_sign(s->cpu.a);
}
} // namespace


void nmi6502(machine_state* s)
{
	push16(s, s->cpu.pc);
	// Hardware interrupts push status with BREAK cleared and CONSTANT set.
	push8(s, (s->cpu.status | FLAG_CONSTANT) & ~FLAG_BREAK);
	s->cpu.set_interrupt();
	s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFA)) | (static_cast<uint16_t>(s->ram_read(0xFFFB)) << 8);
}

void cpu_irq(machine_state* s)
{
	push16(s, s->cpu.pc);
	// A hardware IRQ pushes only PC and status with BREAK cleared and CONSTANT set.
	push8(s, (s->cpu.status | FLAG_CONSTANT) & ~FLAG_BREAK);
	s->cpu.set_interrupt();
	// Vector through the real 6502 IRQ vector at $FFFE, which on the C64 points at
	// the kernal entry ($FF48). That entry saves A/X/Y on the stack before doing
	// JMP ($0314) to the user handler at $EA31; the handler restores A/X/Y and RTIs.
	// Jumping straight to $0314 here would skip the register save, so the handler's
	// closing PLA/PLA/PLA would unbalance the stack and RTI to a garbage address.
	s->cpu.pc = static_cast<uint16_t>(s->ram_read(0xFFFE)) | (static_cast<uint16_t>(s->ram_read(0xFFFF)) << 8);
}

namespace
{
// Read instructions using abs,X / abs,Y / (ind),Y that pay the page-cross
// penalty (one extra cycle when the effective address crosses a page).
constexpr bool penalty_op(const uint8_t opcode)
{
	switch (opcode)
	{
	case 0x1D: case 0x19: case 0x11: // ORA
	case 0x3D: case 0x39: case 0x31: // AND
	case 0x5D: case 0x59: case 0x51: // EOR
	case 0x7D: case 0x79: case 0x71: // ADC
	case 0xBD: case 0xB9: case 0xB1: // LDA
	case 0xBE:                       // LDX abs,Y
	case 0xBC:                       // LDY abs,X
	case 0xDD: case 0xD9: case 0xD1: // CMP
	case 0xFD: case 0xF9: case 0xF1: // SBC
		return true;
	default:
		return false;
	}
}

constexpr uint8_t ticktable[256] = {
	7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
	6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
	6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
	6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
	2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
	2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
	2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
	2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
	2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
	2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
	2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7
};

// A 6502 read-modify-write instruction writes the unmodified byte back before
// the modified one. C64 code depends on that dummy write reaching I/O, which is
// what makes INC $D019 / LSR $D019 / DEC $DC0D acknowledge an interrupt.
void rmw(machine_state* s, const uint16_t ea, uint16_t (*op)(machine_state*, uint16_t))
{
	const uint16_t value = get_ea(s, ea);
	put_ea(s, value, ea);
	put_ea(s, op(s, value), ea);
}


// Service a pending interrupt at an instruction boundary, mirroring the 6510's
// behaviour: NMI is edge-triggered and always taken; IRQ is level-triggered and
// taken only while the interrupt-disable flag is clear. Either takes 7 cycles.
static bool cpu_service_interrupts(machine_state* s)
{
	if (s->nmi_pending)
	{
		s->nmi_pending = false;
		nmi6502(s);
		s->clock_ticks += 7;
		return true;
	}

	if (s->irq_pending && (s->cpu.status & FLAG_INTERRUPT) == 0)
	{
		s->ext_irq = false; // a host-raised IRQ is a pulse, not a held line
		s->update_irq_line();
		cpu_irq(s);
		s->clock_ticks += 7;
		return true;
	}

	return false;
}
} // namespace


void cpu_exec(machine_state* s, const int32_t tick_count)
{
	uint16_t ea;

	int64_t clock_limit = s->clock_ticks + tick_count;

	while (clock_limit > s->clock_ticks)
	{
		const int64_t cycles_before = s->clock_ticks;

		if (cpu_service_interrupts(s))
		{
			s->step_hardware(static_cast<int32_t>(s->clock_ticks - cycles_before));
			continue;
		}

		// $F4A5 is where the LOAD vector at $0330 points by default. Servicing it
		// from the mounted disk image is what lets a program load further files;
		// a program that revectors $0330 to its own loader is not intercepted.
		if (s->cpu.pc == KERNAL_LOAD_TRAP && !s->disk.empty() && !s->raw_ram
			&& s->read_map[0xF4] == 2 && s->kernal_load())
			continue;

		const auto opcode = s->ram_read(s->cpu.pc++);
		s->cpu.status |= FLAG_CONSTANT;
		s->penalty_addr = 0;

		switch (opcode)
		{
		case 0x0:
			imp(s);
			brk(s);
			break;
		case 0x1:
			ea = indx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x5:
			ea = zp(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x6:
			ea = zp(s);
			rmw(s, ea, asl);
			break;
		case 0x8:
			imp(s);
			php(s);
			break;
		case 0x9:
			ea = imm(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0xA:
			put_a(s, asl(s, get_a(s)));
			break;
		case 0xD:
			ea = abso(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0xE:
			ea = abso(s);
			rmw(s, ea, asl);
			break;
		case 0x10:
			bpl(s, rel(s));
			break;
		case 0x11:
			ea = indy(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x15:
			ea = zpx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x16:
			ea = zpx(s);
			rmw(s, ea, asl);
			break;
		case 0x18:
			imp(s);
			clc(s);
			break;
		case 0x19:
			ea = absy(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x1D:
			ea = absx(s);
			put_a(s, ora(s, get_ea(s, ea)));
			break;
		case 0x1E:
			ea = absx(s);
			rmw(s, ea, asl);
			break;
		case 0x20:
			ea = abso(s);
			jsr(s, ea);
			break;
		case 0x21:
			ea = indx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x24:
			ea = zp(s);
			op_bit(s, get_ea(s, ea));
			break;
		case 0x25:
			ea = zp(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x26:
			ea = zp(s);
			rmw(s, ea, rol);
			break;
		case 0x28:
			imp(s);
			plp(s);
			break;
		case 0x29:
			ea = imm(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x2A:
			put_a(s, rol(s, get_a(s)));
			break;
		case 0x2C:
			ea = abso(s);
			op_bit(s, get_ea(s, ea));
			break;
		case 0x2D:
			ea = abso(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x2E:
			ea = abso(s);
			rmw(s, ea, rol);
			break;
		case 0x30:
			bmi(s, rel(s));
			break;
		case 0x31:
			ea = indy(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x35:
			ea = zpx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x36:
			ea = zpx(s);
			rmw(s, ea, rol);
			break;
		case 0x38:
			imp(s);
			sec(s);
			break;
		case 0x39:
			ea = absy(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x3D:
			ea = absx(s);
			put_a(s, op_and(s, get_ea(s, ea)));
			break;
		case 0x3E:
			ea = absx(s);
			rmw(s, ea, rol);
			break;
		case 0x40:
			imp(s);
			rti(s);
			break;
		case 0x41:
			ea = indx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x45:
			ea = zp(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x46:
			ea = zp(s);
			rmw(s, ea, lsr);
			break;
		case 0x48:
			imp(s);
			pha(s);
			break;
		case 0x49:
			ea = imm(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x4A:
			put_a(s, lsr(s, get_a(s)));
			break;
		case 0x4C:
			ea = abso(s);
			jmp(s, ea);
			break;
		case 0x4D:
			ea = abso(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x4E:
			ea = abso(s);
			rmw(s, ea, lsr);
			break;
		case 0x50:
			bvc(s, rel(s));
			break;
		case 0x51:
			ea = indy(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x55:
			ea = zpx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x56:
			ea = zpx(s);
			rmw(s, ea, lsr);
			break;
		case 0x58:
			imp(s);
			cli(s);
			break;
		case 0x59:
			ea = absy(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x5D:
			ea = absx(s);
			put_a(s, eor(s, get_ea(s, ea)));
			break;
		case 0x5E:
			ea = absx(s);
			rmw(s, ea, lsr);
			break;
		case 0x60:
			imp(s);
			rts(s);
			break;
		case 0x61:
			ea = indx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x65:
			ea = zp(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x66:
			ea = zp(s);
			rmw(s, ea, ror);
			break;
		case 0x68:
			imp(s);
			pla(s);
			break;
		case 0x69:
			ea = imm(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x6A:
			put_a(s, ror(s, get_a(s)));
			break;
		case 0x6C:
			ea = ind(s);
			jmp(s, ea);
			break;
		case 0x6D:
			ea = abso(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x6E:
			ea = abso(s);
			rmw(s, ea, ror);
			break;
		case 0x70:
			bvs(s, rel(s));
			break;
		case 0x71:
			ea = indy(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x75:
			ea = zpx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x76:
			ea = zpx(s);
			rmw(s, ea, ror);
			break;
		case 0x78:
			imp(s);
			sei(s);
			break;
		case 0x79:
			ea = absy(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x7D:
			ea = absx(s);
			put_a(s, adc(s, get_ea(s, ea)));
			break;
		case 0x7E:
			ea = absx(s);
			rmw(s, ea, ror);
			break;
		case 0x81:
			ea = indx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x84:
			ea = zp(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x85:
			ea = zp(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x86:
			ea = zp(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x88:
			imp(s);
			dey(s);
			break;
		case 0x8A:
			imp(s);
			txa(s);
			break;
		case 0x8C:
			ea = abso(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x8D:
			ea = abso(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x8E:
			ea = abso(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x90:
			bcc(s, rel(s));
			break;
		case 0x91:
			ea = indy(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x94:
			ea = zpx(s);
			put_ea(s, s->cpu.y, ea); // sty
			break;
		case 0x95:
			ea = zpx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x96:
			ea = zpy(s);
			put_ea(s, s->cpu.x, ea); // stx
			break;
		case 0x98:
			imp(s);
			tya(s);
			break;
		case 0x99:
			ea = absy(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0x9A:
			imp(s);
			txs(s);
			break;
		case 0x9D:
			ea = absx(s);
			put_ea(s, s->cpu.a, ea); // sta
			break;
		case 0xA0:
			ea = imm(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xA1:
			ea = indx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xA2:
			ea = imm(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xA4:
			ea = zp(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xA5:
			ea = zp(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xA6:
			ea = zp(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xA8:
			imp(s);
			tay(s);
			break;
		case 0xA9:
			ea = imm(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xAA:
			imp(s);
			tax(s);
			break;
		case 0xAC:
			ea = abso(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xAD:
			ea = abso(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xAE:
			ea = abso(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xB0:
			bcs(s, rel(s));
			break;
		case 0xB1:
			ea = indy(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xB4:
			ea = zpx(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xB5:
			ea = zpx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xB6:
			ea = zpy(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xB8:
			imp(s);
			clv(s);
			break;
		case 0xB9:
			ea = absy(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xBA:
			imp(s);
			tsx(s);
			break;
		case 0xBC:
			ea = absx(s);
			ldy(s, get_ea(s, ea));
			break;
		case 0xBD:
			ea = absx(s);
			lda(s, get_ea(s, ea));
			break;
		case 0xBE:
			ea = absy(s);
			ldx(s, get_ea(s, ea));
			break;
		case 0xC0:
			ea = imm(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xC1:
			ea = indx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xC4:
			ea = zp(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xC5:
			ea = zp(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xC6:
			ea = zp(s);
			rmw(s, ea, dec);
			break;
		case 0xC8:
			imp(s);
			iny(s);
			break;
		case 0xC9:
			ea = imm(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xCA:
			imp(s);
			dex(s);
			break;
		case 0xCC:
			ea = abso(s);
			cpy(s, get_ea(s, ea));
			break;
		case 0xCD:
			ea = abso(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xCE:
			ea = abso(s);
			rmw(s, ea, dec);
			break;
		case 0xD0:
			bne(s, rel(s));
			break;
		case 0xD1:
			ea = indy(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xD5:
			ea = zpx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xD6:
			ea = zpx(s);
			rmw(s, ea, dec);
			break;
		case 0xD8:
			imp(s);
			cld(s);
			break;
		case 0xD9:
			ea = absy(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xDD:
			ea = absx(s);
			cmp(s, get_ea(s, ea));
			break;
		case 0xDE:
			ea = absx(s);
			rmw(s, ea, dec);
			break;
		case 0xE0:
			ea = imm(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xE1:
			ea = indx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xE4:
			ea = zp(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xE5:
			ea = zp(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xE6:
			ea = zp(s);
			rmw(s, ea, inc);
			break;
		case 0xE8:
			imp(s);
			inx(s);
			break;
		case 0xE9:
			ea = imm(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEB:
			ea = imm(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEC:
			ea = abso(s);
			cpx(s, get_ea(s, ea));
			break;
		case 0xED:
			ea = abso(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xEE:
			ea = abso(s);
			rmw(s, ea, inc);
			break;
		case 0xF0:
			beq(s, rel(s));
			break;
		case 0xF1:
			ea = indy(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xF5:
			ea = zpx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xF6:
			ea = zpx(s);
			rmw(s, ea, inc);
			break;
		case 0xF8:
			imp(s);
			sed(s);
			break;
		case 0xF9:
			ea = absy(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xFD:
			ea = absx(s);
			put_a(s, sbc(s, get_ea(s, ea)));
			break;
		case 0xFE:
			ea = absx(s);
			rmw(s, ea, inc);
			break;
		default:
		{
			// Illegal/undocumented opcodes are not emulated, but their operand bytes
			// must still be consumed or the instruction stream derails. JAM/KIL has
			// no length: leave PC on the opcode so the CPU spins there.
			const int len = opcodes[opcode].length;
			s->cpu.pc = static_cast<uint16_t>(s->cpu.pc + (len == 0 ? -1 : len - 1));
			break;
		}
		}

		s->clock_ticks += static_cast<int32_t>(ticktable[opcode]);
		// 6502 read instructions with abs,X / abs,Y / (ind),Y addressing take one
		// extra cycle when the effective address crosses a page boundary.
		if (s->penalty_addr && penalty_op(opcode)) s->clock_ticks += 1;
		s->instruction_count++;
		s->step_hardware(static_cast<int32_t>(s->clock_ticks - cycles_before));
	}
}
