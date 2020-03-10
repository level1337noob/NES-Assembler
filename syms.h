#ifndef SYMS_H
#define SYMS_H

struct Sym {
	std::string token {};
	int id {};
};

struct Variable {
	std::string name {};
	u16 value;
	u8 type; // zpg abs imm?
};

struct Label {
	std::string label {};
	location_t addr;
	u8 section; // data or text?
};

struct Instruction {
public:
	u8 opcode {};
	u8 bytes {};
	u16 value {};
	Label label;
	u8 required_jump {}; // 0x1 = JUMP 0x2 = RELATIVE
	inline void reverse() { value = (value >> 8) | (value & 0xFF) << 8; }
};

#endif
