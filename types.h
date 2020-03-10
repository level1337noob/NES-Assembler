#ifndef TYPES_H
#define TYPES_H

#define	_NONE	    0x700
#define IMMEDIATE   0x701
#define ABSOLUTE    0x702
#define ABSOLUTE_X  0x703
#define ABSOLUTE_Y  0x704
#define ZEROPAGE    0x705
#define ZEROPAGE_X  0x706
#define ZEROPAGE_Y  0x707
#define INDIRECT    0x708
#define INDIRECT_X  0x709
#define INDIRECT_Y  0x70A
#define RELATIVE    0x70B
#define MAX_STACK 0x40

#define TEXT_SECTION 0x0
#define DATA_SECTION 0x1
#define READ_ONLY_SECTION 0x2
#define BATTERY_BACKED_SECTION 0x3
#define TRAINER_SECTION 0x4

#define throwback(...)								\
	do {											\
		printf("%s:%d: ", curfile[sp], t->cur_line());\
		printf(__VA_ARGS__);						\
		putchar('\n');								\
	} while (0);

enum {
	NONE = 0x00,
	COMMENT = 0x01,
	ASSIGNMENT = 0x02,
	TOKEN = 0x300,
	DIGIT = 0x301,
	STRING = 0x400,
	LABEL = 0x500,
	EXTRA_OPERAND = 0x800,
	INDIRECT_OPEN = 0x801,
	INDIRECT_CLOSE = 0x802,
};

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short int u16;
typedef signed short int s16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long int u64;
typedef signed long long int s64;
typedef u16 addr_t;
typedef addr_t location_t;

#endif
