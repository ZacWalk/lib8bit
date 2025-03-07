// lib8bit by Zac Walker
//
// A small two-pass 6502 assembler. Pass 1 assigns label addresses and instruction
// sizes; pass 2 resolves symbols and emits machine code. Instruction encoding is
// driven by reversing the opcode table in opcodes.cpp: (mnemonic, addressing
// mode) -> opcode byte.

#include "assembler.h"
#include "opcodes.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	enum
	{
		AM_IMP, AM_ACC, AM_IMM, AM_ZP, AM_ZPX, AM_ZPY, AM_ABS, AM_ABSX,
		AM_ABSY, AM_IND, AM_INDX, AM_INDY, AM_REL, AM_COUNT
	};

	int mode_index(const std::string_view s)
	{
		if (s == "implied") return AM_IMP;
		if (s == "accumulator") return AM_ACC;
		if (s == "#immediate") return AM_IMM;
		if (s == "$zero page") return AM_ZP;
		if (s == "$zero page,X") return AM_ZPX;
		if (s == "$zero page,Y") return AM_ZPY;
		if (s == "$absolute") return AM_ABS;
		if (s == "$absolute,X") return AM_ABSX;
		if (s == "$absolute,Y") return AM_ABSY;
		if (s == "($indirect)") return AM_IND;
		if (s == "($indirect,X)") return AM_INDX;
		if (s == "($indirect),Y") return AM_INDY;
		if (s == "relative") return AM_REL;
		return -1;
	}

	struct mnemonic_ops
	{
		int op[AM_COUNT];
		mnemonic_ops() { for (int& o : op) o = -1; }
	};

	// Reverse map built once from the opcode table: mnemonic -> opcode per mode.
	const std::unordered_map<std::string, mnemonic_ops>& mnemonic_table()
	{
		static const std::unordered_map<std::string, mnemonic_ops> table = []
		{
			std::unordered_map<std::string, mnemonic_ops> t;
			for (int b = 0; b < 256; ++b)
			{
				const std::string name = opcodes[b].name;
				if (name == "UNDEFINED") continue;
				const int m = mode_index(opcodes[b].addressing_mode);
				if (m < 0) continue;
				t[name].op[m] = b;
			}
			return t;
		}();
		return table;
	}

	std::string to_upper(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return s;
	}

	void trim(std::string& s)
	{
		const auto a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) { s.clear(); return; }
		const auto b = s.find_last_not_of(" \t\r\n");
		s = s.substr(a, b - a + 1);
	}

	std::string no_spaces(std::string s)
	{
		std::string r;
		for (const char c : s) if (c != ' ' && c != '\t') r.push_back(c);
		return r;
	}

	bool iends(const std::string& s, const char* suffix)
	{
		const std::string u = to_upper(s);
		const std::string v = to_upper(suffix);
		return u.size() >= v.size() && u.compare(u.size() - v.size(), v.size(), v) == 0;
	}

	std::string strip_comment(const std::string& s)
	{
		bool in_quote = false;
		for (size_t i = 0; i < s.size(); ++i)
		{
			if (s[i] == '"') in_quote = !in_quote;
			else if (s[i] == ';' && !in_quote) return s.substr(0, i);
		}
		return s;
	}

	struct asm_ctx
	{
		std::unordered_map<std::string, int> symbols;
		assembler_result r;
		uint16_t addr = 0xC000;
		uint16_t origin = 0xC000;
		bool started = false;
		bool final = false;
		int line = 0;

		bool fail(const std::string& msg)
		{
			r.ok = false;
			r.error = msg;
			r.error_line = line;
			return false;
		}

		void emit(uint8_t b)
		{
			if (final) r.bytes.push_back(b);
			if (!started) { origin = addr; started = true; }
			addr = static_cast<uint16_t>(addr + 1);
		}
	};

	int parse_term(asm_ctx& c, std::string t, bool& ok, bool& symbolic)
	{
		trim(t);
		if (t.empty()) { ok = false; return 0; }
		if (t == "*") { symbolic = true; return c.addr; }

		const char f = t[0];
		if (f == '$') return static_cast<int>(std::strtol(t.c_str() + 1, nullptr, 16));
		if (f == '%') return static_cast<int>(std::strtol(t.c_str() + 1, nullptr, 2));
		if (f == '\'') { if (t.size() >= 2) return static_cast<unsigned char>(t[1]); ok = false; return 0; }
		if (std::isdigit(static_cast<unsigned char>(f))) return static_cast<int>(std::strtol(t.c_str(), nullptr, 10));

		// Symbol / equate reference.
		symbolic = true;
		const auto it = c.symbols.find(to_upper(t));
		if (it != c.symbols.end()) return it->second;
		if (c.final) ok = false; // undefined symbol is only an error in the final pass
		return 0;
	}

	int parse_expr(asm_ctx& c, std::string e, bool& ok, bool& symbolic)
	{
		trim(e);
		ok = true;
		symbolic = false;
		if (e.empty()) { ok = false; return 0; }

		char hilo = 0;
		if (e[0] == '<' || e[0] == '>') { hilo = e[0]; e = e.substr(1); trim(e); }

		size_t op_pos = std::string::npos;
		for (size_t i = 1; i < e.size(); ++i)
			if (e[i] == '+' || e[i] == '-') { op_pos = i; break; }

		int value;
		if (op_pos == std::string::npos)
		{
			value = parse_term(c, e, ok, symbolic);
		}
		else
		{
			bool ok2 = true;
			const int base = parse_term(c, e.substr(0, op_pos), ok, symbolic);
			const int off = parse_term(c, e.substr(op_pos + 1), ok2, symbolic);
			if (!ok2) ok = false;
			value = e[op_pos] == '+' ? base + off : base - off;
		}

		if (hilo == '<') value &= 0xFF;
		else if (hilo == '>') value = (value >> 8) & 0xFF;
		return value & 0xFFFF;
	}

	enum class opclass { empty, acc, imm, indx, indy, ind, idxx, idxy, direct };

	struct operand { opclass cls; std::string expr; };

	operand classify(const std::string& o)
	{
		if (o.empty()) return {opclass::empty, ""};
		if (to_upper(o) == "A") return {opclass::acc, ""};
		if (o[0] == '#') return {opclass::imm, o.substr(1)};
		if (o[0] == '(')
		{
			if (iends(o, ",X)")) return {opclass::indx, o.substr(1, o.size() - 4)};
			if (iends(o, "),Y")) return {opclass::indy, o.substr(1, o.size() - 4)};
			if (o.back() == ')') return {opclass::ind, o.substr(1, o.size() - 2)};
		}
		if (iends(o, ",X")) return {opclass::idxx, o.substr(0, o.size() - 2)};
		if (iends(o, ",Y")) return {opclass::idxy, o.substr(0, o.size() - 2)};
		return {opclass::direct, o};
	}

	bool encode_instruction(asm_ctx& c, const std::string& mnemonic, const std::string& operand_text)
	{
		const std::string m = to_upper(mnemonic);
		const auto it = mnemonic_table().find(m);
		if (it == mnemonic_table().end()) return c.fail("unknown instruction '" + mnemonic + "'");
		const mnemonic_ops& ops = it->second;
		const auto has = [&](int mode) { return ops.op[mode] >= 0; };

		const operand od = classify(no_spaces(operand_text));
		bool ok = true, symbolic = false;
		int value = 0;
		int mode = -1;

		const auto expr = [&] { value = parse_expr(c, od.expr, ok, symbolic); };

		switch (od.cls)
		{
		case opclass::empty:
			if (has(AM_IMP)) mode = AM_IMP;
			else if (has(AM_ACC)) mode = AM_ACC;
			else return c.fail(m + " requires an operand");
			break;
		case opclass::acc:
			if (has(AM_ACC)) mode = AM_ACC; else return c.fail(m + " has no accumulator mode");
			break;
		case opclass::imm: expr(); mode = AM_IMM; break;
		case opclass::indx: expr(); mode = AM_INDX; break;
		case opclass::indy: expr(); mode = AM_INDY; break;
		case opclass::ind: expr(); mode = AM_IND; break;
		case opclass::idxx:
			expr();
			mode = (!symbolic && value <= 0xFF && has(AM_ZPX)) ? AM_ZPX : AM_ABSX;
			break;
		case opclass::idxy:
			expr();
			mode = (!symbolic && value <= 0xFF && has(AM_ZPY)) ? AM_ZPY : AM_ABSY;
			break;
		case opclass::direct:
			expr();
			if (has(AM_REL)) mode = AM_REL;
			else if (!symbolic && value <= 0xFF && has(AM_ZP)) mode = AM_ZP;
			else mode = AM_ABS;
			break;
		}

		if (!ok) return c.fail("bad expression in operand");
		if (mode < 0 || !has(mode)) return c.fail(m + " has no matching addressing mode");

		c.emit(static_cast<uint8_t>(ops.op[mode]));
		switch (mode)
		{
		case AM_IMP:
		case AM_ACC:
			break;
		case AM_IMM:
		case AM_ZP:
		case AM_ZPX:
		case AM_ZPY:
		case AM_INDX:
		case AM_INDY:
			c.emit(static_cast<uint8_t>(value & 0xFF));
			break;
		case AM_ABS:
		case AM_ABSX:
		case AM_ABSY:
		case AM_IND:
			c.emit(static_cast<uint8_t>(value & 0xFF));
			c.emit(static_cast<uint8_t>((value >> 8) & 0xFF));
			break;
		case AM_REL:
		{
			const int next = c.addr + 1; // address of the following instruction
			const int off = value - next;
			if (c.final && (off < -128 || off > 127)) return c.fail("branch target out of range");
			c.emit(static_cast<uint8_t>(off & 0xFF));
			break;
		}
		default:
			break;
		}
		return true;
	}

	bool set_origin(asm_ctx& c, int v)
	{
		const auto n = static_cast<uint16_t>(v & 0xFFFF);
		if (!c.started) { c.addr = n; return true; }
		if (n < c.addr) return c.fail(".org moves backwards");
		while (c.addr < n) c.emit(0); // zero-fill the gap
		return true;
	}

	bool emit_list(asm_ctx& c, const std::string& rest, bool word)
	{
		size_t start = 0;
		while (start <= rest.size())
		{
			const size_t comma = rest.find(',', start);
			std::string tok = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
			trim(tok);
			if (!tok.empty())
			{
				bool ok = true, symbolic = false;
				const int v = parse_expr(c, tok, ok, symbolic);
				if (!ok) return c.fail("bad value in data directive");
				c.emit(static_cast<uint8_t>(v & 0xFF));
				if (word) c.emit(static_cast<uint8_t>((v >> 8) & 0xFF));
			}
			if (comma == std::string::npos) break;
			start = comma + 1;
		}
		return true;
	}

	bool emit_text(asm_ctx& c, std::string rest)
	{
		trim(rest);
		if (rest.size() < 2 || rest.front() != '"' || rest.back() != '"')
			return c.fail(".text expects a quoted string");
		for (size_t i = 1; i + 1 < rest.size(); ++i) c.emit(static_cast<uint8_t>(rest[i]));
		return true;
	}

	bool encode_directive(asm_ctx& c, const std::string& line)
	{
		if (line[0] == '*')
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos) return c.fail("expected '=' after '*'");
			bool ok = true, symbolic = false;
			const int v = parse_expr(c, line.substr(eq + 1), ok, symbolic);
			if (!ok) return c.fail("bad origin expression");
			return set_origin(c, v);
		}

		const size_t sp = line.find_first_of(" \t");
		const std::string kw = to_upper(sp == std::string::npos ? line : line.substr(0, sp));
		std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		trim(rest);

		if (kw == ".ORG")
		{
			bool ok = true, symbolic = false;
			const int v = parse_expr(c, rest, ok, symbolic);
			if (!ok) return c.fail("bad .org expression");
			return set_origin(c, v);
		}
		if (kw == ".BYTE" || kw == ".DB") return emit_list(c, rest, false);
		if (kw == ".WORD" || kw == ".DW") return emit_list(c, rest, true);
		if (kw == ".TEXT" || kw == ".ASC") return emit_text(c, rest);
		return c.fail("unknown directive '" + kw + "'");
	}

	bool run_pass(asm_ctx& c, const std::vector<std::string>& lines, bool final)
	{
		c.addr = 0xC000;
		c.origin = 0xC000;
		c.started = false;
		c.final = final;

		for (size_t i = 0; i < lines.size(); ++i)
		{
			c.line = static_cast<int>(i) + 1;
			std::string s = strip_comment(lines[i]);
			trim(s);
			if (s.empty()) continue;

			// Leading label "name:".
			{
				size_t p = 0;
				while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
				if (p > 0 && p < s.size() && s[p] == ':')
				{
					if (!final) c.symbols[to_upper(s.substr(0, p))] = c.addr;
					s = s.substr(p + 1);
					trim(s);
					if (s.empty()) continue;
				}
			}

			// Equate "name = expr" (not '*=' and not '==').
			{
				size_t p = 0;
				while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
				size_t q = p;
				while (q < s.size() && (s[q] == ' ' || s[q] == '\t')) ++q;
				if (p > 0 && q < s.size() && s[q] == '=' && (q + 1 >= s.size() || s[q + 1] != '='))
				{
					bool ok = true, symbolic = false;
					const int v = parse_expr(c, s.substr(q + 1), ok, symbolic);
					if (!final)
					{
						if (!ok) return c.fail("bad equate value");
						c.symbols[to_upper(s.substr(0, p))] = v;
					}
					continue;
				}
			}

			if (s[0] == '.' || s[0] == '*')
			{
				if (!encode_directive(c, s)) return false;
				continue;
			}

			const size_t sp = s.find_first_of(" \t");
			const std::string mnemonic = sp == std::string::npos ? s : s.substr(0, sp);
			std::string operand = sp == std::string::npos ? "" : s.substr(sp + 1);
			trim(operand);
			if (!encode_instruction(c, mnemonic, operand)) return false;
		}

		c.r.origin = c.origin;
		return true;
	}
}

assembler_result assemble(const std::string_view source)
{
	std::vector<std::string> lines;
	std::string current;
	for (const char ch : source)
	{
		if (ch == '\n') { lines.push_back(current); current.clear(); }
		else if (ch != '\r') current.push_back(ch);
	}
	lines.push_back(current);

	asm_ctx c;
	if (!run_pass(c, lines, false)) return c.r; // pass 1: labels + sizes
	c.r.bytes.clear();
	if (!run_pass(c, lines, true)) return c.r;  // pass 2: emit code
	c.r.ok = true;
	return c.r;
}
