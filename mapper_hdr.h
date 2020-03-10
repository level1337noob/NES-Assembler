#ifndef MAPPER_HDR_H
#define MAPPER_HDR_H

#define MAPPER(g) \
	g##_MAPPER_TYPE
enum {
	MAPPER(NROM) = 0x0,
};

struct iNes {
	u8 magic[0x4];
	u8 prg_rom_size {};
	u8 chr_rom_size {}; // 0 = chr ram
	u8 mapper_nr {};
	u8 mapper_2_0_nr {};
	u8 prg_ram_size {};
	u8 tv_system {};
	u8 tv_system_prg_ram_prescense {};
	u8 p1 {}, p2 {}, p3 {}, p4 {}, p5 {}; // unused padding
};


#endif
