/*
 * 6502 assembler processor support for NES
 */
#include <string>
#include <vector>
#include <cstring>
#include <ctype.h>
#include <unistd.h> // TODO change this since we need it for sbrk
#include <signal.h>
#include "types.h"
#include "mapper_hdr.h"
#include "syms.h"

static char c;
static int sp {};
static const char *curfile[MAX_STACK] {};
static bool show_token_debugger {1};
static u8 prg_rom_size = 1, chr_rom_size = 1;
static bool mirroring {}, battery_backed {}, trainer {};
static std::string main_reloc { "_main" };
bool parse_line = false;
static std::vector<Sym> SymTable {};

class buffer_reader;

class buffer_reader {
private:
	char *buffer {};
	size_t size {};
	bool file_fail {};
	u32 idx {};
	u32 line {1};
public:
	bool valid_extension(const char *format,
					 	 const char *extension)
	{
		int i = strlen(format) - strlen(extension);
		if (i < 0) return false;
		format += i;
		return !strcmp(format, extension) ? true : false;
	}

	bool open_file(const char *file)
	{
		FILE *s = fopen(file, "r");
		file_fail = false;

		if (!s) {
			file_fail = true;
			return false;
		}

		fseek(s, 0, SEEK_END);
		size = ftell(s);
		rewind(s);
		buffer = (char *) new char[size + 0x20];
		memset((void *) buffer, 0, size + 0x20);
		fread(buffer, size, 1, s);
		fclose(s);
		return true;
	}

	inline void step_line() { ++line; }
	inline void rewind_line() { --line; }
	inline u32 cur_line() { return this->line; }
	inline u8 read_buffer() { return buffer[idx]; }
	inline u8 step_buffer() { return ++idx; }
	inline u8 rewind_buffer() { return --idx; }
	inline bool is_fail() { return file_fail; }
	inline void end_buffer() { free(buffer); buffer = NULL; }
};

static addr_t TEXT_PC = 0xC000;
static std::vector<u8> text_bin;
static inline void SET_TEXT_PC(u32 addr) { TEXT_PC = addr; }
static inline void ADD_TEXT_PC(u32 addr) { TEXT_PC += addr; }

static u32 DATA_PC = 0x0000;
static std::vector<u8> data_bin;
static inline void SET_DATA_PC(u32 addr) { DATA_PC = addr; }
static inline void ADD_DATA_PC(u32 addr) { DATA_PC += addr; }

static u32 RODATA_PC = 0x0000;
static std::vector<u8> rodata_bin;
inline void SET_RODATA_PC(u32 addr) { RODATA_PC = addr; }
static inline void ADD_RODATA_PC(u32 addr) { RODATA_PC += addr; }

static addr_t section = TEXT_SECTION;
static u16 mapper_type = NROM_MAPPER_TYPE;

static bool is_instruction_mask(char c) {
	static const char instruction_token[] = "@_0123456789abcdefghijklmnopqrstuvwxyz";

	int i;
	for (i = 0; i < 39; i++) {
		if (tolower(c) == instruction_token[i]) {
			return true;
		}
	}

	return false;
}

static bool is_hex_mask(char c) {
	int i;
	static const char hexadecimal[] = "0123456789abcdef";

	for (i = 0; i < 16; i++) {
		if (tolower(c) == hexadecimal[i]) {
			return true;
		}
	}

	return false;
}

static bool is_binary_mask(char c) {
	int i;
	static const char binary[] = "01";

	for (i = 0; i < 16; i++) {
		if (c == binary[i]) {
			return true;
		}
	}

	return false;
}

static Sym current_symbol;
inline Sym *read_sym() { return &current_symbol; }

bool skip_whitespace(buffer_reader *t);
static int errs;

void parse_escape_seq(buffer_reader *t)
{

}

int read_string(buffer_reader *t, char skip)
{
	do {
		t->step_buffer();
		c = t->read_buffer();
		if (c == '\\') {
			parse_escape_seq(t);
			c = t->read_buffer();
			if (c == '\0' || c == '\n') goto sterr;
		}

		if (c != skip) {
			if (c != '\n') {
				read_sym()->token += c;
			}
		}
	} while (c && c != skip && c != '\n');

	sterr:
	if (c != skip) {
		throwback("Expected %c", skip);
		read_sym()->id = NONE;
	} else {
		c = t->read_buffer();
		read_sym()->id = STRING;
		return 0;
	}

	return 1;
}

bool skip_comment(buffer_reader *t, char v) {
	if (t->read_buffer() == v) {
		do {
			t->step_buffer();
		} while ((c = t->read_buffer()) && (c != '\n'));
		read_sym()->id = NONE;
		t->rewind_buffer();
		return true;
	}

	return false;
}

static char tab[0x10];
int read_value(buffer_reader *t)
{
	int i;
	const char *skip0;

	if (!is_hex_mask(c = t->read_buffer())) {
		throwback("error: expected hexadecimal value");
		errs++;
		read_sym()->id = NONE;
		do {
			t->step_buffer();
		} while ((c = t->read_buffer()) != '\n');
		t->step_line();
	} else {
		i = 0;
		read_sym()->token += "$";
		do {
			i++;
			read_sym()->token += c;
			t->step_buffer();
		} while (is_hex_mask(c = t->read_buffer()));
		skip0 = read_sym()->token.c_str();

		if (*skip0 == '$') {
			skip0 += 1;
		} else if (*skip0 == '#') {
			skip0 += 2;
		}

		if (*skip0 == '0') {
			while (*skip0++ == '0') { i--; }
			if (*(skip0 - 2) == '0') {
				++i;
			}
		}

		t->rewind_buffer();
		return i;
	}

	return false;
}

int read_bin_value(buffer_reader *t)
{
	int i;

	if (!is_binary_mask(c = t->read_buffer())) {
		throwback("error: expected binary value");
		errs++;
		skip_comment(t, c);
		read_sym()->id = NONE;
		t->rewind_buffer();
	} else {
		memset(tab, 0, sizeof tab);
		i = 0;

		read_sym()->token = "$";
		do {
			if (t->read_buffer()=='1' || t->read_buffer()=='0') { /* ... */ } else{ throwback("error: binary value has only 1 or 0's"); errs++; }
			i++;
			read_sym()->token += c;
			t->step_buffer();
		} while (is_hex_mask(c = t->read_buffer()));

		sprintf(tab, "%lX", strtol(read_sym()->token.c_str() + 1, 0, 2) & 0xFFFF);
		i = strlen(tab);

		if (read_sym()->id == IMMEDIATE) {
			read_sym()->token = "#$";
		} else {
			read_sym()->token = "$";
		}

		read_sym()->token += tab;
		t->rewind_buffer();
		return i;
	}

	return false;
}

static int fast_skip = 0;

bool is_token(buffer_reader *t) {
	if (isalpha(c = t->read_buffer()) || c == '_' || c == '@') {
			do {
				read_sym()->token += t->read_buffer();
				t->step_buffer();
			} while (is_instruction_mask(t->read_buffer()));

			t->rewind_buffer();
			fast_skip = 1;
			return true;
	}

	return false;
}

Sym *next_sym(buffer_reader *t)
{
	int size;
	read_sym()->token = "";
	read_sym()->id = NONE;

	if (is_token(t)) {
		read_sym()->id = TOKEN;
	} else if (c == '$') {
		if (!fast_skip) goto error;
		t->step_buffer();
		c = t->read_buffer();
		if (!is_hex_mask(c)) {
			read_sym()->id = NONE;
			t->step_line();
			throwback("error: Expected hex value before '$'");
			goto err;
		}

		if ((size = read_value(t))) {
			if (size <= 2) {
				read_sym()->id = ZEROPAGE;
			} else {
				if (size > 4) {
					throwback("warning: absolute value overflow");
				}
				read_sym()->id = ABSOLUTE;
			}
		}
	} else if (c == '%') {
		if (!fast_skip) goto error;

		t->step_buffer();
		c = t->read_buffer();
		if (!is_binary_mask(c)) {
			read_sym()->id = NONE;
			t->step_line();
			throwback("error: Expected binary value before '%%'");
			goto err;
		}

		if ((size = read_bin_value(t))) {
			if (size <= 8) {
				read_sym()->id = ZEROPAGE;
			} else {
				if (size > 16) {
					throwback("warning: absolute binary value overflow");
				}
				read_sym()->id = ABSOLUTE;
			}
		}
	} else if (isdigit(c)) {
		if (!fast_skip) goto error;
		do {
			read_sym()->token += c;
			t->step_buffer();
		} while (isdigit(c = t->read_buffer()));
		t->rewind_buffer();
		read_sym()->id = DIGIT;
	} else if (c == '#') {
		if (!fast_skip) goto error;
		t->step_buffer();
		c = t->read_buffer();
		read_sym()->token += "#";
		if (c == '$') {
			t->step_buffer();

			read_sym()->id = IMMEDIATE;
			if ((size = read_value(t))) {
				if (size > 2) {
					throwback("warning: immediate value overflow");
				}

				read_sym()->id = IMMEDIATE;
			}
		} else if (c == '%') {
			t->step_buffer();
			read_sym()->id = IMMEDIATE;
			if ((size = read_bin_value(t))) {
				if (size > 8) {
					throwback("warning: binary value overflow");
				}
			}
		} else {
			read_sym()->id = NONE;
			throwback("Expected '$' or '%%' of value before '#'");
			goto err;
		}
	} else {
		if (fast_skip) {
			switch (c = t->read_buffer()) {
			case '(': read_sym()->token = c; read_sym()->id = INDIRECT_OPEN;  break;
			case ')': read_sym()->token = c; read_sym()->id = INDIRECT_CLOSE; break;
			case '+':
			case ',': read_sym()->token = c; read_sym()->id = EXTRA_OPERAND;  break;
			case ':': read_sym()->token = c; read_sym()->id = LABEL;  break;
			case '=': read_sym()->token = c; read_sym()->id = ASSIGNMENT;  break;
			case '<': if (read_string(t, '>')) goto err;  read_sym()->id = STRING; break;
			case '\'':if (read_string(t, '\'')) goto err; read_sym()->id = STRING; break;
			case '"': if (read_string(t, '"')) goto err;  read_sym()->id = STRING; break;
			default: error: throwback("error: junk '%c'", c); errs++; goto fail;
			}
		} else {
			goto error;
		fail:
			t->step_buffer();
		}
	}

	return read_sym();
err:
	if (c == '\n') {
		t->step_line();
	} else {
		do {
			t->step_buffer();
		} while ((c = t->read_buffer()) != '\n');
	}

	errs++;
	return read_sym();
}

bool skip_whitespace(buffer_reader *t)
{
	if (isspace(c = t->read_buffer())&&c!='\n') {
		while (isspace(t->read_buffer())) {
			t->step_buffer();
		}

		c = t->read_buffer();
		return true;
	}

	return false;
}

void read_buffer(buffer_reader *t);

static u32 oldpc = TEXT_PC;
bool preprocessor(buffer_reader *t)
{
	int id;
	FILE *chrfile;
	if ((c = t->read_buffer()) == '.') {
		t->step_buffer();
		/* Some assemblers doesn't support this i guess?
		   skip_whitespace ...*/
		skip_whitespace(t);
		next_sym(t);
		if (read_sym()->token == "include" || read_sym()->token == "import" || read_sym()->token == "inc") {
			t->step_buffer();
			skip_whitespace(t);
			next_sym(t);

			if (sp > MAX_STACK)
				goto fail;
			if (read_sym()->id != STRING) {
				throwback("error: expected string");
				errs++;
			} else {
				sp++;
				u32 len = read_sym()->token.length();

				if (!t[sp].open_file(read_sym()->token.c_str())) {
					sp--;
					throwback("error: no such file or directory %s", read_sym()->token.c_str());
					errs++;
				} else {
					curfile[sp] = (char *) sbrk(len);
					memset((void *) curfile[sp], 0, len);
					memcpy((void *) curfile[sp], read_sym()->token.c_str(), len);
					read_buffer(&t[sp]);
					t[sp].end_buffer();
					curfile[sp] = (char *) sbrk(0);
					--sp;
				}
			}
		} else if (read_sym()->token == "prgsize") {
			t->step_buffer();
			skip_whitespace(t);
			next_sym(t);
			id = read_sym()->id;

			if (read_sym()->token.c_str()[0]=='$') {
				prg_rom_size = strtol(read_sym()->token.c_str() + 1, 0, 16);
			} else if (id == DIGIT) {
				prg_rom_size = strtol(read_sym()->token.c_str(), 0, 10);
			} else {
				throwback("error: expected $oooo format or digit");
				errs++;
			}

			if (!prg_rom_size) {
				throwback("warning: prg size set to defaults to 1");
				prg_rom_size = 1;
			}
		} else if (read_sym()->token == "chrsize") {
			t->step_buffer();
			c = t->read_buffer();
			skip_whitespace(t);
			next_sym(t);
			id = read_sym()->id;

			if (read_sym()->token.c_str()[0]=='$') {
				chr_rom_size = strtol(read_sym()->token.c_str() + 1, 0, 16);
			} else if (id == DIGIT) {
				chr_rom_size = strtol(read_sym()->token.c_str(), 0, 10);
			} else {
				throwback("error: expected $oooo format or digit");
				errs++;
			}

			if (!chr_rom_size) {
				throwback("warning: using CHR-RAM");
			}
		} else if (read_sym()->token == "chrbin" || read_sym()->token == "incbin") {
			if (chr_rom_size) {
				t->step_buffer();
				skip_whitespace(t);
				next_sym(t);
				if (read_sym()->id != STRING) {
					throwback("error: Expected string");
					errs++;
				} else {
					const char *file = read_sym()->token.c_str();
					static bool taken;
					chrfile = fopen(file, "rb");
					if (!chrfile) {
						throwback("error: no such chr-rom binary %s", file);
						errs++;
					} else {
						if (!taken) {
							fseek(chrfile, 0, SEEK_END);
							size_t size = ftell(chrfile);
							size_t iter;
							rewind(chrfile);
							if (size != 0x2000 * chr_rom_size) {
								throwback("warning: Expected exact CHR-ROM size of $%04X and not $%04lX turn on fillbytes=0 to turn on filling bytes with $00's", 0x2000 * chr_rom_size, size);
								for (iter = 0; iter < size; ++iter) { data_bin.push_back((u8) fgetc(chrfile)); }
								//for (; iter < 0x2000; ++iter) { data_bin.push_back('\0'); }
							} else {
								for (iter = 0; iter < size; ++iter) { data_bin.push_back((u8) fgetc(chrfile)); }
								ADD_DATA_PC(0x2000 * chr_rom_size);
							}

							fclose(chrfile);
							taken = 1;
						} else {
							throwback("warning: already taken binary data");
						}
					}
				}
			} else {
				throwback("warning: couldn't include binary file since CHR-ROM size is 0");
				t->step_line(); // FIXME
				goto g;
			}
		} else if (read_sym()->token == "horizontal") {
			mirroring = 0;
		} else if (read_sym()->token == "vertical") {
			mirroring = 1;
		} else if (read_sym()->token == "battery") {
			battery_backed = true;
		} else if (read_sym()->token == "trainer") {
			trainer = 1;
		} else if (read_sym()->token == "reloc") {
			t->step_buffer();
			skip_whitespace(t);
			next_sym(t);
			if (read_sym()->id != STRING) {
				throwback("error: Expected string");
				errs++;
			} else {
				main_reloc = read_sym()->token;
			}
		} else if (read_sym()->token == "nrom16") {
			mapper_type = NROM_MAPPER_TYPE;
			SET_TEXT_PC(0xC000);
			SET_DATA_PC(0x2000);
		} else if (read_sym()->token == "nrom32") {
			mapper_type = NROM_MAPPER_TYPE;
			SET_TEXT_PC(0x8000);
			SET_DATA_PC(0x2000);
		} else if (read_sym()->token == "org") {
			t->step_buffer();
			skip_whitespace(t);
			next_sym(t);
			id = read_sym()->id;
			if (read_sym()->token.c_str()[0]=='$') {
				oldpc = TEXT_PC;
				SET_TEXT_PC(strtol(read_sym()->token.c_str() + 1, 0, 16));
			} else if (read_sym()->token == "old") {
				SET_TEXT_PC(oldpc);
			} else {
				throwback("error: expected $oooo format");
				errs++;
			}

		} else if (read_sym()->token == "mapper") {
			t->step_buffer();
			skip_whitespace(t);
			next_sym(t);
			id = read_sym()->id;
			if (read_sym()->token.c_str()[0]=='$') {
				mapper_type = strtol(read_sym()->token.c_str() + 1, 0, 16);
			} else if (id == DIGIT) {
				mapper_type = strtol(read_sym()->token.c_str(), 0, 10);
			} else {
				throwback("error: expected $oo format");
				errs++;
			}

			switch (mapper_type) {
			default:throwback("TODO: unsupported mapper %03d", mapper_type);
			case 0: break;
			}
		} else if (read_sym()->token == "nes") {
			throwback("warning: using processor of type '%s'", read_sym()->token.c_str());
		} else if (read_sym()->token == "rodata") {
			section = READ_ONLY_SECTION;
		} else if (read_sym()->token == "data") {
			section = DATA_SECTION;
		} else if (read_sym()->token == "text") {
			section = TEXT_SECTION;
		} else {
			throwback("error: invalid preprocessor directive %s", read_sym()->token.c_str());
			t->step_line();

		g:
			do {
				t->step_buffer();
			} while (t->read_buffer() && t->read_buffer() != '\n');

			c = t->read_buffer();
			errs++;
			return true;
		}

fail:
		c = t->read_buffer();
		return true;
	}

	return false;
}

bool save_sym(buffer_reader *t)
{
	next_sym(t);
	if (read_sym()->id != NONE) {
		SymTable.push_back(*read_sym());

		if (show_token_debugger) {
			//throwback("%s", read_sym()->token.c_str());
		}

		parse_line = true;
	}
	return true;
}

static std::vector<Instruction> instructions {};
static Label label {};
static std::vector<Label> labels {};
static Variable variable {};
static std::vector<Variable> variables {};
void save_instruction(u8 opcode, u8 bytes, u16 value, u8 required_jump = 0, Label *reqlabel = 0)
{
	Instruction g;
	g.opcode = opcode;
	g.bytes = bytes;
	g.value = value;
	if (bytes == 3)
		g.reverse();
	g.required_jump = required_jump;
	if (required_jump) {
		g.label = *reqlabel;
	}

	instructions.push_back(g);
	ADD_TEXT_PC(bytes);
}

bool save_label(Label label)
{
	for (auto& x : labels) {
		if (x.label == label.label) {
			return false;
		}
	}

	labels.push_back(label);
	return true;
}

size_t find_label(Label& label)
{
	size_t i = 0;
	for (auto& x : labels) {
		i++;
		if (x.label == label.label) {
			label = x;
			return i; /* Do an n - 1 calculation */
		}
	}
	return 0;
}

bool save_variable(Variable var)
{
	for (auto& x : variables) {
		if (x.name == var.name) {
			return false;
		}
	}

	variables.push_back(var);
	return true;
}

size_t find_variable(Variable& var)
{
	size_t i = 0;
	for (auto& x : variables) {
		i++;
		if (x.name == var.name) {
			var = x;
			return i;
		}
	}
	return 0;
}

template<typename T>
bool add_data_byte(Sym *temp, buffer_reader *t, size_t& i, void (*callback)(u32 pc), T& bin, u8 use_end = 0)
{
	bool ret {};
	size_t size { SymTable.size() };
	u16 val;
	bool completed {};
	if (temp->id == TOKEN) {
		if (temp->token == "byte" || temp->token == "db") {
		rep:
			if (i + 1 < size) {
				temp = &SymTable.at(i++);
				if (temp->id == EXTRA_OPERAND) {
					if (!completed) {
						throwback("error: expected expression in section .%s", section == DATA_SECTION ? "data" : "rodata");
						errs++;
						goto fail;
					} else {
						completed = false;
						if (i + 1 < size) goto rep;
						else { throwback("error: expected expression in section .%s", section == DATA_SECTION ? "data" : "rodata"); goto fail; }
					}
				} else if (temp->id == ZEROPAGE || temp->id == DIGIT || temp->id == ABSOLUTE) {
					if (temp->id == DIGIT) {
						val = strtol(temp->token.c_str(), 0, 10) & 0xFF;
					} else {
						val = strtol(temp->token.c_str() + 1, 0, 16) & 0xFF;
					}
					bin.push_back(val);
					callback(1);
					if (completed) { throwback("error: expected ','"); goto fail; }
					completed = true;
					if (i + 1 < size) goto rep;
				} else if (temp->id == STRING) {
					/* bad implementation */
					#if 0
					for (u8 *p = (u8 *) temp->token.c_str(); *p; ++p) {
						putchar(*p);
					}
					putchar('\n');
					#endif

					/* good implementation */
					for (auto& g : temp->token) {
						bin.push_back((u8) g);
						callback(1);
					}

					if (completed) { throwback("error: expected ','"); goto fail; }
					completed = true;
					if (i + 1 < size) goto rep;
				} else {
					throwback("error: bad expression in section .%s", section == DATA_SECTION ? "data" : "rodata");
					goto fail;
				}
			} else {
				if (completed) goto end;
				throwback("error: expected expression in section .%s", section == DATA_SECTION ? "data" : "rodata");
				goto fail;
			}
		} else {
			throwback("error: expected db or byte");
			goto fail;
		}

		ret = true;
	} else {
		throwback("error: expected token in section .%s", section == DATA_SECTION ? "data" : "rodata");
		goto fail;
	}

end:
	if (use_end) {
		bin.push_back('\0');
		callback(1);
	}
fail:
	return ret;
}

static inline int cmp(const char *str1, const char *str2)
{
	int i;
	for (i = 0; str1[i] && tolower(str1[i]) == tolower(str2[i]); ++i);
	return tolower(str2[i])-tolower(str1[i]);
}

// r0 r1 r2 A X Y
#define _if(g) if (!cmp(x->token.c_str(), g))
#define _elif(g) else if (!cmp(x->token.c_str(), g))

bool regex(buffer_reader *t)
{
	size_t i {};
	size_t size {};
	bool success {};
	bool finished_instruction = false;
	read_sym()->token = "";
	read_sym()->id = NONE;
	SymTable.push_back(*read_sym());
	i = 0;

	size = SymTable.size();
	while (1) {
		if (i>=size) break;
		Sym *x = &SymTable.at(i++);
		Sym *temp {};

		if (x->id == NONE)
			break;

		if (x->id == TOKEN) {
			if (finished_instruction) {
				throwback("error: more token parsing before instruction %s", x->token.c_str());
				goto fail;
			}

			if (i + 1 < size) {
				temp = &SymTable.at(i++);

				if (temp->id == LABEL) {
					label.label = x->token;
					if (section == TEXT_SECTION) { label.addr = TEXT_PC; }
					else if (section == DATA_SECTION) { label.addr = DATA_PC; }
					else if (section == READ_ONLY_SECTION) { label.addr = RODATA_PC; }
					label.section = section;
					if (!save_label(label)) {
						throwback("conflicting types for %s", x->token.c_str());
						goto fail;
					}
				} else {
					i--;
					goto instruction_parse;
				}
			} else {
			instruction_parse:
				if (section == TEXT_SECTION) {
					u8 opcode;
					u8 bytes;
					u8 reqjmp;
					u16 value;

					// Jumps/Branches
					_if("jmp") {
						finished_instruction = true;
						opcode = 0x4C;
						bytes = 3;
						reqjmp = 0;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;

							label.label = temp->token;
							if (temp->id == TOKEN) {
								if (find_label(label)) {
									value = label.addr;
								} else {
									reqjmp = 1;
								}
							} else if (temp->id == ABSOLUTE || temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16);
							} else if (temp->id == INDIRECT_OPEN) {
								opcode = 0x6C;
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == TOKEN) {
										if (find_label(label)) {
											value = label.addr;
										} else {
											reqjmp = 1;
										}
									} else if (temp->id == ABSOLUTE || temp->id == ZEROPAGE) {
										value = strtol(temp->token.c_str()+1, 0, 16);
									} else {
										throwback("error: expected valid value $nnnn or token");
										goto fail;
									}
								} else {
									throwback("error: expected valid value $nnnn or token");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id != INDIRECT_CLOSE) {
										throwback("error: expected ')' on indirect jmp");
										goto fail;
									}
								} else {
									throwback("error: expected ')' on indirect jmp");
									goto fail;
								}
							} else {
								throwback("error: expected valid value $nnnn or token");
								goto fail;
							}
						} else {
							throwback("error: expected address on jmp");
							goto fail;
						}

						save_instruction(opcode, bytes, value, reqjmp, &label);
					}

					_elif("jsr") {
						finished_instruction = true;
						opcode = 0x20;
						bytes = 3;
						reqjmp = 0;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;

							label.label = temp->token;
							if (temp->id == TOKEN) {
								if (find_label(label)) {
									value = label.addr;
								} else {
									reqjmp = 1;
								}
							} else if (temp->id == ABSOLUTE || temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16);
							} else {
								throwback("error: expected valid value $nnnn or token");
								goto fail;
							}
						} else {
							throwback("error: expected address on jsr");
							goto fail;
						}

						save_instruction(opcode, bytes, value, reqjmp, &label);
					}

					// Branches TODO

					// Load/Stores
					_elif("lda") {
						finished_instruction = true;
						opcode = 0xA9;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xBD;
										} else if (temp->token == "Y") {
											opcode = 0xB9;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0xAD;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xB5;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xA5;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0xB1;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0xA1;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on lda");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("sta") {
						finished_instruction = true;
						opcode = 0x85;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x9D;
										} else if (temp->token == "Y") {
											opcode = 0x99;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0x8D;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x95;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0x85;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0x91;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0x81;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on sta");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("ldx") {
						finished_instruction = true;
						opcode = 0xA2;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == IMMEDIATE) {
							value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
						} else if (temp->id == ZEROPAGE) {
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

							if (i+1<size) {
								temp = &SymTable.at(i++);

								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "Y") {
										opcode = 0xB6;
									} else {
										throwback("error: expected Y register");
										goto fail;
									}
								} else {
									throwback("error: expected Y register");
									goto fail;
								}
							} else {
								opcode = 0xA6;
							}
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							value = strtol(temp->token.c_str()+1, 0, 16);
							if (i+1<size) {
								temp = &SymTable.at(i++);
								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "Y") {
										opcode = 0xBE;
									} else {
										throwback("error: expected Y register");
										goto fail;
									}
								} else {
									throwback("error: expected Y register");
									goto fail;
								}
							} else {
								opcode = 0xAE;
							}
						} else {
							throwback("error: expected value on ldx");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("stx") {
						finished_instruction = true;
						opcode = 0x86;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == ZEROPAGE) {
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
							if (i+1<size) {
								temp = &SymTable.at(i++);

								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "Y") {
										opcode = 0x96;
									} else {
										throwback("error: expected Y register");
										goto fail;
									}
								} else {
									throwback("error: expected Y register");
									goto fail;
								}
							} else {
								opcode = 0x86;
							}
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							value = strtol(temp->token.c_str()+1, 0, 16);
							opcode = 0x8E;
						} else {
							throwback("error: expected value on stx");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("ldy") {
						finished_instruction = true;
						opcode = 0xA0;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == IMMEDIATE) {
							value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
						} else if (temp->id == ZEROPAGE) {
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

							if (i+1<size) {
								temp = &SymTable.at(i++);

								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "X") {
										opcode = 0xB4;
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									throwback("error: expected X register");
									goto fail;
								}
							} else {
								opcode = 0xA4;
							}
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							value = strtol(temp->token.c_str()+1, 0, 16);
							if (i+1<size) {
								temp = &SymTable.at(i++);
								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "X") {
										opcode = 0xBC;
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									throwback("error: expected X register");
									goto fail;
								}
							} else {
								opcode = 0xAC;
							}
						} else {
							throwback("error: expected value on ldy");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("sty") {
						finished_instruction = true;
						opcode = 0x84;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == ZEROPAGE) {
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
							if (i+1<size) {
								temp = &SymTable.at(i++);

								if (temp->id != EXTRA_OPERAND) {
									throwback("error: expected ','");
									goto fail;
								}

								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->token == "X") {
										opcode = 0x94;
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									throwback("error: expected X register");
									goto fail;
								}
							} else {
								opcode = 0x84;
							}
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							value = strtol(temp->token.c_str()+1, 0, 16);
							opcode = 0x8C;
						} else {
							throwback("error: expected value on sty");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					// Logical
					_elif("and") {
						finished_instruction = true;
						opcode = 0x29;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x3D;
										} else if (temp->token == "Y") {
											opcode = 0x39;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0x2D;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x35;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0x25;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0x31;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0x21;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on and");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("eor") {
						finished_instruction = true;
						opcode = 0x49;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x5D;
										} else if (temp->token == "Y") {
											opcode = 0x59;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0x4D;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x55;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0x45;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0x51;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0x41;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on eor");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("ora") {
						finished_instruction = true;
						opcode = 0x09;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x1D;
										} else if (temp->token == "Y") {
											opcode = 0x19;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0x0D;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x15;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0x05;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0x11;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0x01;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on ora");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("bit") {
						finished_instruction = true;
						opcode = 0x24;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == ZEROPAGE) {
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							opcode = 0x2C;
							value = strtol(temp->token.c_str()+1, 0, 16);
						} else {
							throwback("error: expected value on bit");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					// Arithmetic
					_elif("adc") {
						finished_instruction = true;
						opcode = 0x69;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x7D;
										} else if (temp->token == "Y") {
											opcode = 0x79;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0x6D;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0x75;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0x65;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0x71;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0x61;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on adc");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("sbc") {
						finished_instruction = true;
						opcode = 0xE9;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xFD;
										} else if (temp->token == "Y") {
											opcode = 0xF9;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0xED;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xF5;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xE5;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0xF1;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0xE1;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on sbc");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("cmp") {
						finished_instruction = true;
						opcode = 0xC9;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == IMMEDIATE) {
								value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
							} else if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xDD;
										} else if (temp->token == "Y") {
											opcode = 0xD9;
										} else {
											throwback("error: expected X or Y registers");
											goto fail;
										}
									} else {
										throwback("error: expected 8-bit register");
										goto fail;
									}
								} else {
									opcode = 0xCD;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xD5;
										} else {
											throwback("error: expected X register");
											goto fail;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xC5;
								}
							} else if (temp->id == INDIRECT_OPEN) {
								if (i+1<size) {
									temp = &SymTable.at(i++);
									if (temp->id == ZEROPAGE || temp->id == ABSOLUTE) {
										value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
										if (i+1<size) {
											temp = &SymTable.at(i++);

											if (temp->id == INDIRECT_CLOSE) {
												opcode = 0xD1;
												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->id != EXTRA_OPERAND) {
														throwback("error: expected ','");
														goto fail;
													}

													if (i+1<size) {
														temp = &SymTable.at(i++);
														if (temp->token != "Y") {
															throwback("error: expected Y register");
															goto fail;
														}
													} else {
														throwback("error: expected Y register");
														goto fail;
													}
												} else {
													throwback("error: expected ','");
													goto fail;
												}
											} else {
												opcode = 0xC1;
												if (temp->id != EXTRA_OPERAND) {
													throwback("error: expected ','");
													goto fail;
												}

												if (i+1<size) {
													temp = &SymTable.at(i++);
													if (temp->token == "X") {
														if (i+1<size) {
															temp = &SymTable.at(i++);
															if (temp->id != INDIRECT_CLOSE) {
																throwback("error: expected ')'");
																goto fail;
															}
														} else {
															throwback("error: expected ')'");
															goto fail;
														}
													} else {
														throwback("error: expected X register");
														goto fail;
													}
												} else {
													throwback("error: expected X register");
													goto fail;
												}
											}
										} else {
											throwback("error: expected value");
											goto fail;
										}
									} else {
										throwback("error: expected value");
										goto fail;
									}
								} else {
									throwback("error: expected value");
									goto fail;
								}
							} else {
								throwback("error: expected value");
								goto fail;
							}
						} else {
							throwback("error: expected value on cmp");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("cpx") {
						finished_instruction = true;
						opcode = 0xE0;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == IMMEDIATE) {
							value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
						} else if (temp->id == ZEROPAGE) {
							opcode = 0xE4;
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							opcode = 0xEC;
							value = strtol(temp->token.c_str()+1, 0, 16);
						} else {
							throwback("error: expected value on cpx");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					_elif("cpy") {
						finished_instruction = true;
						opcode = 0xC0;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i++);
						if (temp->id == IMMEDIATE) {
							value = strtol(temp->token.c_str()+2, 0, 16) & 0xFF;
						} else if (temp->id == ZEROPAGE) {
							opcode = 0xC4;
							value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;
						} else if (temp->id == ABSOLUTE) {
							bytes = 3;
							opcode = 0xCC;
							value = strtol(temp->token.c_str()+1, 0, 16);
						} else {
							throwback("error: expected value on cpy");
							goto fail;
						}

						save_instruction(opcode, bytes, value);
					}

					// inc/dec
					_elif("inc") {
						finished_instruction = true;
						opcode = 0xE6;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xFE;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xEE;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xF6;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xE6;
								}
							}
					}

						save_instruction(opcode, bytes, value);
					}

					_elif("dec") {
						finished_instruction = true;
						opcode = 0xC6;
						bytes = 2;
						value = 0;
						temp = &SymTable.at(i);

						if (i+1<size) {
							++i;
							label.label = temp->token;

							if (temp->id == ABSOLUTE) {
								bytes = 3;
								value = strtol(temp->token.c_str()+1, 0, 16);
								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xDE;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xCE;
								}
							} else if (temp->id == ZEROPAGE) {
								value = strtol(temp->token.c_str()+1, 0, 16) & 0xFF;

								if (i+1<size) {
									temp = &SymTable.at(i++);

									if (temp->id != EXTRA_OPERAND) {
										throwback("error: expected ','");
										goto fail;
									}

									if (i+1<size) {
										temp = &SymTable.at(i++);
										if (temp->token == "X") {
											opcode = 0xD6;
										}
									} else {
										throwback("error: expected X register");
										goto fail;
									}
								} else {
									opcode = 0xC6;
								}
							}
					}

						save_instruction(opcode, bytes, value);
					}
					// one byte ops
					#define T(g, x) _elif (g) { finished_instruction = true, save_instruction(x, 1, 0); }
					T("inx", 0xE8)T("iny", 0xC8)
					T("dex", 0xCA)T("dey", 0x88)

					T("tax", 0xAA)T("txa", 0x8A)T("tay", 0xA8)T("tya", 0x98)
					T("tsx", 0xBA)T("txs", 0x9A)
					T("pha", 0x48)T("php", 0x08)
					T("pla", 0x68)T("plp", 0x28)
					T("clc", 0x18)T("cld", 0xD8)T("cli", 0x58)T("clv", 0xB8)T("sec", 0x38)T("sed", 0xF8)T("sei", 0x78)
					T("rti", 0x40)T("rts", 0x60)
					T("nop", 0xEA)T("brk", 0x00)
					// spasm
					T("syscall", 0x00)T("break", 0x00)

					else {
						throwback("error: no such instruction '%s'", x->token.c_str());
						goto fail;
					}
				} else if (section == DATA_SECTION) {
					finished_instruction = true;
					if (x->id == TOKEN) {
						if (!add_data_byte(x, t, i, ADD_DATA_PC, data_bin, 0)) {
							printf("FA");
							goto fail;
						} else {
						}
					} else {
						throwback("error: expected value or label on .data");
						goto fail;
					}
				} else if (section == READ_ONLY_SECTION) {
					finished_instruction = true;
					if (x->id == TOKEN) {
						if (!add_data_byte(x, t, i, ADD_RODATA_PC, rodata_bin, 1))
							goto fail;
					} else {
						throwback("error: expected value or label on .rodata");
						goto fail;
					}
				} else {
					throwback("error: bad section");
					goto fail;
				}
			}
		} else {
			throwback("error: failed parsing '%s'", x->token.c_str());
			goto fail;
		}
	}

	success = true;
fail:
	SymTable.clear();
	return success;
}

#undef _if
#undef _elif

void read_buffer(buffer_reader *t) {
	int id;

	id = 0;
	while ((c = t->read_buffer()) != '\0') {

		/* End of terminal input emit all the opcodes and instruction */
		if (c == '\n') {
			if (parse_line) {
				if (!regex(t)) {
					errs++;
				}

				parse_line = false;
			}

			do {
				t->step_line();
				t->step_buffer();
			} while ((c = t->read_buffer()) == '\n');

			fast_skip = 0;
			id = 0;
		}

		skip_whitespace(t);
		if (c == '\0') { break; }

		if (skip_comment(t, ';')) {

		} else if (id == 1) {
			preprocessor(t);
		} else if (id == 2) {
			save_sym(t);
		} else if (id == 0) {
			if (preprocessor(t)) {
				id = 1;
			} else if (save_sym(t)) {
				id = 2;
			}
		}

		t->step_buffer();

#ifdef USE_ERRORS
#define MAX_ERRORS 3
		if (errs > MAX_ERRORS) {
			break;
		}
#endif
	}
}

#define temp 0x80
#define MAX_INSTRUCTIONS temp

bool check_errors(const char *argv[])
{
	if (errs) {
		return true;
	}

	return false;
}

void reset_compiler()
{
	SET_TEXT_PC(0);
}

int compile_assembler(const char *argv[], const char *file)
{
	int rv;

	buffer_reader *g = new buffer_reader[sizeof *g * MAX_STACK];
	g->open_file(file);
	rv = 1;
	if (g->is_fail()) {
		printf("%s: No such file or directory %s\n", argv[0], file);
		errs++;
		goto err;
	}

	read_buffer(g);
	rv = 0;

err:
	g->end_buffer();
	delete[] g;
	g = NULL;

	if (check_errors(argv)) {
		printf("%s: %s has occured\n", argv[0], errs<=1?"An error":"Multiple errors");
		return 0xFF;
	}

	return rv;
}

void err(int)
{
	printf("error: Internal compiler segmentation fault on noob65\n");
	exit(0);
}

int main(int argc, const char *argv[])
{
	signal(SIGSEGV, err);
	const char *f {};
	const char *object_reloc {};
	FILE *object {};
	bool found_start {};
	std::string rodata_mask {};
	bool rv {};
	u8 *mem {}, *dmem;
	u16 tPC {};
	u32 prg_capacity {};
	u32 chr_capacity {};
	u16 prg_pc {};
	struct iNes hdr;
	curfile[sp] = f;

	#define log(g) printf(#g "\n");
	if (argc < 2) {
		printf("%s: no input files\n", argv[0]);
		return 0xFF;
	} else {
		#define t(c) (!strcmp(argv[i], c))
		for (int i = 1; i < argc; ++i) {
			if (!strcmp(argv[i], "--help")) {
				log(Usage: nesasm [options] file... (wip))
				log((-o|-object) file\tCompiles the object file)
				log((-f|-file) file\t\tGets the file to be compiled)
				log((-h|-v) file\t\tChanges the mirroring type)
				log((-b|-bat) file\t\tAdds battery-backed support)
				log((-t|-tnr) file\t\tAdds trainer support)
				log((-m ...) file\t\tGets mapper value)
				log((-prom ...) file\tChanges the PRG-ROM Size)
				log((-pram ...) file\tChanges the PRG-RAM Size)
				log((-crom ...) file\tChanges the CHR-ROM Size)
				log((-incbin ...) file\tIncludes the CHR-ROM binary)
				log(--version\t\tGets the version of the assembler)
				log((C) level1337noob -- nesasm 0.1\nLicensed under GNU GPLv2 License)
				return 0xFF;
			} else if (t("-f") || t("-file")) {
				i++;
				if (i+1>argc) {
					printf("%s: expected argument\n", argv[0]);
					return 0xFF;
				}

				f = argv[i];
			} else if (t("-o") || t("-object")) {
				i++;
				if (i+1>argc) {
					printf("%s: expected argument\n", argv[0]);
					return 0xFF;
				}

				object_reloc = argv[i];
			} else if (t("-prom")) {
				i++;
				if (i+1>argc) {
					printf("%s: expected argument\n", argv[0]);
					return 0xFF;
				}
				
				if (argv[i][0] == '$') {
					argv[i] += 1;
					for (int j = 0; argv[i][j]; ++j) {
						if (!is_hex_mask(argv[i][j])) {
							printf("expected valid base 16 digit");
							return 0xFF;
						}
					}

					prg_rom_size = strtol(argv[i], 0, 16);
				} else {
					for (int j = 0; argv[i][j]; ++j) {
						if (!isdigit(argv[i][j])) {
							printf("expected valid base 10 digit");
							return 0xFF;
						}
					}

					prg_rom_size = strtol(argv[i], 0, 10);
				}
			} else if (t("-pram")) {

			} else if (t("-crom")) {
				i++;
				if (i+1>argc) {
					printf("%s: expected argument\n", argv[0]);
					return 0xFF;
				}
				
				if (argv[i][0] == '$') {
					argv[i] += 1;
					for (int j = 0; argv[i][j]; ++j) {
						if (!is_hex_mask(argv[i][j])) {
							printf("expected valid base 16 digit");
							return 0xFF;
						}
					}

					chr_rom_size = strtol(argv[i], 0, 16);
				} else {
					for (int j = 0; argv[i][j]; ++j) {
						if (!isdigit(argv[i][j])) {
							printf("expected valid base 10 digit");
							return 0xFF;
						}
					}

					chr_rom_size = strtol(argv[i], 0, 10);
				}
			} else if (t("-incbin")) {

			} else if (t("--version")) {
				log((C) level1337noob -- nesasm 0.1\nLicensed under GNU GPLv2 License)
				log(updates: added compiler to github)
				log(updates: only support for mapper 0 now)
				return 0xFF;
			} else {
				printf("%s: usage --help for options and not %s\n", argv[0], argv[i]);
				return 0xFF;
			}
		}
	}

	if (!f) return !printf("%s: error: expected file to compile to\n", argv[0]);
	if (!object_reloc) object_reloc = "a.out";

	if (compile_assembler(argv, f)) {
		goto fail;
	}


	for (auto& x : rodata_bin) rodata_mask += x;

	for (auto& g : labels) {
		if (g.section == READ_ONLY_SECTION) {
			//printf("<%s:$%04X> \"%s\"; RODATA\n", g.label.c_str(), g.addr, rodata_mask.c_str()+g.addr);
		} else if (g.section == TEXT_SECTION) {
			if (g.label == main_reloc) found_start = true;
		}
	}

	if (!found_start) {
		printf("<nooblinker:$%04X> undefined reference to '%s'\n", TEXT_PC, main_reloc.c_str());
		goto fail;
	}

	prg_capacity = 0x4000 * prg_rom_size;
	chr_capacity = 0x2000 * chr_rom_size;
	mem = (u8 *) malloc(prg_capacity);
	if (chr_capacity)
		dmem = (u8 *) malloc(chr_capacity);

	for (auto& x : instructions) {
		if (x.required_jump) {
			label = x.label;
			if (find_label(label)) {
				x.value = label.addr;
				x.reverse();
			} else {
				printf("<nooblinker:$%04X> undefined reference label %s\n", prg_pc, label.label.c_str());
				rv = 1;
			}
		}

		if (x.bytes == 3) {
			mem[tPC++%(prg_capacity)] = x.opcode;
			mem[tPC++%(prg_capacity)] = x.value >> 8;
			mem[tPC++%(prg_capacity)] = x.value & 0xFF;
		} else if (x.bytes == 2) {
			mem[tPC++%(prg_capacity)] = x.opcode;
			mem[tPC++%(prg_capacity)] = x.value & 0xFF;
		} else {
			mem[tPC++%(prg_capacity)] = x.opcode;
		}

		prg_pc += x.bytes;
	}

	if (rv) goto fail;

	for (; tPC < prg_capacity; ++tPC)mem[tPC] = 0x00;

	if (chr_capacity) {
		if (DATA_PC != chr_capacity) {
			printf("%s: warning: filling $00's in data pc from $%04X 0's based on CHR-ROM size\n", argv[0], DATA_PC);
			for (; DATA_PC < chr_capacity; ++DATA_PC) { data_bin.push_back(0); }
		}

		delete dmem;
	}

	memset((void *) &hdr, 0, sizeof hdr);
	memcpy(hdr.magic, "\x4e\x45\x53\x1a", 4);
	hdr.prg_rom_size = prg_rom_size;
	hdr.chr_rom_size = chr_rom_size;
	hdr.mapper_nr |= mirroring & 1;
	hdr.mapper_nr |= (mapper_type & 0xF) << 4;
	hdr.mapper_2_0_nr |= (mapper_type & 0xF0) >> 4;


	if (0) {
	fail:
		rv = 1;
	} else {
		object = fopen(object_reloc, "wb+");
		for (u8 g = 0; g < 0x10; ++g) {
			u8 *p = (u8 *) (hdr.magic+g);
			fwrite(p, 1, 1, object);
		}

		for (u32 p=0;p<tPC;++p) { u8 *g = &mem[p]; fwrite(g,1,1,object); }
		for (u32 p=0;p<DATA_PC;++p) { u8 *g = &data_bin.at(p); fwrite(g,1,1,object); }
		// putchar('\n');
	}

	delete mem;
	return rv;
}
