// lib8bit by Zac Walker
//
// VIC-II raster renderer. Renders one raster line at a time into a 384x272
// packed-RGB frame buffer, supporting every C64 graphics mode plus the eight
// hardware sprites. Reads the live VIC state straight out of C64 RAM/ROM exactly
// as the VIC-II chip would, so raster-split effects work when the CPU changes
// registers mid-frame.

#include "machine.h"
#include "vic.h"

#include <cstring>

const uint32_t vic_palette[16] = {
	0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
	0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
	0x6F4F25, 0x433900, 0x9A6759, 0x444444,
	0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};

namespace
{
	constexpr int SCREEN_W = 320;
	constexpr int SCREEN_H = 200;
	constexpr int BORDER_X = (VIC_FB_WIDTH - SCREEN_W) / 2;  // 32
	constexpr int BORDER_Y = (VIC_FB_HEIGHT - SCREEN_H) / 2; // 36

	constexpr int COLOR_RAM = 0xD800;

	enum vic_mode
	{
		MODE_STANDARD_CHARACTER = 0,
		MODE_MULTICOLOR_CHARACTER,
		MODE_STANDARD_BITMAP,
		MODE_MULTICOLOR_BITMAP,
		MODE_EXTENDED_BACKGROUND,
		MODE_INVALID,
	};

	struct mem_addrs
	{
		int vic_bank;
		int screen_addr;
		int char_addr;
		int bitmap_addr;
		int char_rom_offset;
		bool use_char_rom;
	};

	int graphics_mode(const uint8_t* ram)
	{
		const uint8_t ctrl1 = ram[VIC_CTRL1];
		const uint8_t ctrl2 = ram[VIC_CTRL2];
		const bool ecm = (ctrl1 & 0x40) != 0;
		const bool bmm = (ctrl1 & 0x20) != 0;
		const bool mcm = (ctrl2 & 0x10) != 0;

		if (ecm && (bmm || mcm)) return MODE_INVALID;
		if (bmm && mcm) return MODE_MULTICOLOR_BITMAP;
		if (bmm) return MODE_STANDARD_BITMAP;
		if (ecm) return MODE_EXTENDED_BACKGROUND;
		if (mcm) return MODE_MULTICOLOR_CHARACTER;
		return MODE_STANDARD_CHARACTER;
	}

	mem_addrs get_mem_addrs(const uint8_t* ram)
	{
		// VIC bank from CIA2 $DD00 (bits 0-1, inverted). Port A lines set to input
		// float high, which is why a machine that has not programmed the DDR yet
		// (a bare reset) sees bank 0.
		const int cia2_port_a = (ram[0xDD00] & ram[0xDD02]) | (0xFF & ~ram[0xDD02]);
		const int vic_bank_num = (~cia2_port_a) & 0x03;
		const int vic_bank = vic_bank_num * 0x4000;

		const int mem_ctrl = ram[VIC_MEMORY];
		const int screen_addr = vic_bank + ((mem_ctrl >> 4) & 0x0F) * 0x0400;
		const int char_offset = ((mem_ctrl >> 1) & 0x07) * 0x0800;
		const int char_addr = vic_bank + char_offset;
		const int bitmap_addr = vic_bank + ((mem_ctrl & 0x08) ? 0x2000 : 0x0000);

		// Character ROM appears to the VIC at $1000-$1FFF within banks 0 and 2; the
		// lower-case set is the second half of the 4 KB ROM.
		const bool use_char_rom = (vic_bank_num == 0 || vic_bank_num == 2) &&
			(char_offset == 0x1000 || char_offset == 0x1800);

		return {vic_bank, screen_addr, char_addr, bitmap_addr, char_offset & 0x0800, use_char_rom};
	}

	void render_standard_character(uint32_t* fb, uint8_t* fgmask, const uint8_t* ram, int canvas_y,
	                               int char_row, int char_pixel_y, const mem_addrs& ma, int scroll_x)
	{
		const int row_offset = canvas_y * VIC_FB_WIDTH;
		for (int c = 0; c < 40; c++)
		{
			const int char_code = ram[ma.screen_addr + char_row * 40 + c];
			const uint32_t color = vic_palette[ram[COLOR_RAM + char_row * 40 + c] & 0x0F];
			const int glyph = char_code * 8;
			const uint8_t line = ma.use_char_rom
				? rom_chars[ma.char_rom_offset + glyph + char_pixel_y]
				: ram[ma.char_addr + glyph + char_pixel_y];

			for (int cx = 0; cx < 8; cx++)
			{
				if (line & (0x80 >> cx))
				{
					const int px = BORDER_X + c * 8 + cx + scroll_x;
					if (px >= 0 && px < VIC_FB_WIDTH)
					{
						fb[row_offset + px] = color;
						fgmask[px] = 1;
					}
				}
			}
		}
	}

	void render_multicolor_character(uint32_t* fb, uint8_t* fgmask, const uint8_t* ram, int canvas_y,
	                                 int char_row, int char_pixel_y, const mem_addrs& ma, int scroll_x)
	{
		const int row_offset = canvas_y * VIC_FB_WIDTH;
		const uint32_t bg0 = vic_palette[ram[0xD021] & 0x0F];
		const uint32_t bg1 = vic_palette[ram[0xD022] & 0x0F];
		const uint32_t bg2 = vic_palette[ram[0xD023] & 0x0F];

		for (int c = 0; c < 40; c++)
		{
			const int char_code = ram[ma.screen_addr + char_row * 40 + c];
			const uint8_t color_ram = ram[COLOR_RAM + char_row * 40 + c];
			const bool is_mc = (color_ram & 0x08) != 0;
			const int fg = color_ram & 0x07;
			const int glyph = char_code * 8;
			const uint8_t line = ma.use_char_rom
				? rom_chars[ma.char_rom_offset + glyph + char_pixel_y]
				: ram[ma.char_addr + glyph + char_pixel_y];

			if (is_mc)
			{
				for (int cx = 0; cx < 4; cx++)
				{
					const int bit_pair = (line >> (6 - cx * 2)) & 0x03;
					uint32_t color = bg0;
					switch (bit_pair)
					{
					case 0: color = bg0; break;
					case 1: color = bg1; break;
					case 2: color = bg2; break;
					case 3: color = vic_palette[fg]; break;
					}
					const int px = BORDER_X + c * 8 + cx * 2 + scroll_x;
					if (px >= 0 && px + 1 < VIC_FB_WIDTH)
					{
						fb[row_offset + px] = color;
						fb[row_offset + px + 1] = color;
						if (bit_pair & 0x02) { fgmask[px] = 1; fgmask[px + 1] = 1; }
					}
				}
			}
			else
			{
				const uint32_t color = vic_palette[color_ram & 0x0F];
				for (int cx = 0; cx < 8; cx++)
				{
					if (line & (0x80 >> cx))
					{
						const int px = BORDER_X + c * 8 + cx + scroll_x;
						if (px >= 0 && px < VIC_FB_WIDTH)
						{
							fb[row_offset + px] = color;
							fgmask[px] = 1;
						}
					}
				}
			}
		}
	}

	void render_standard_bitmap(uint32_t* fb, uint8_t* fgmask, const uint8_t* ram, int canvas_y,
	                            int char_row, int char_pixel_y, const mem_addrs& ma, int scroll_x)
	{
		const int row_offset = canvas_y * VIC_FB_WIDTH;
		for (int col = 0; col < 40; col++)
		{
			const uint8_t color_byte = ram[ma.screen_addr + char_row * 40 + col];
			const uint32_t fg = vic_palette[(color_byte >> 4) & 0x0F];
			const uint32_t bg = vic_palette[color_byte & 0x0F];
			const int cell = ma.bitmap_addr + (char_row * 40 + col) * 8;
			const uint8_t line = ram[cell + char_pixel_y];

			for (int cx = 0; cx < 8; cx++)
			{
				const int bit = (line >> (7 - cx)) & 1;
				const int px = BORDER_X + col * 8 + cx + scroll_x;
				if (px >= 0 && px < VIC_FB_WIDTH)
				{
					fb[row_offset + px] = bit ? fg : bg;
					if (bit) fgmask[px] = 1;
				}
			}
		}
	}

	void render_multicolor_bitmap(uint32_t* fb, uint8_t* fgmask, const uint8_t* ram, int canvas_y,
	                              int char_row, int char_pixel_y, const mem_addrs& ma, int scroll_x)
	{
		const int row_offset = canvas_y * VIC_FB_WIDTH;
		const uint32_t bg0 = vic_palette[ram[0xD021] & 0x0F];

		for (int col = 0; col < 40; col++)
		{
			const uint8_t screen_byte = ram[ma.screen_addr + char_row * 40 + col];
			const uint8_t color_ram_byte = ram[COLOR_RAM + char_row * 40 + col];
			const uint32_t color1 = vic_palette[(screen_byte >> 4) & 0x0F];
			const uint32_t color2 = vic_palette[screen_byte & 0x0F];
			const uint32_t color3 = vic_palette[color_ram_byte & 0x0F];
			const int cell = ma.bitmap_addr + (char_row * 40 + col) * 8;
			const uint8_t line = ram[cell + char_pixel_y];

			for (int cx = 0; cx < 4; cx++)
			{
				const int bit_pair = (line >> (6 - cx * 2)) & 0x03;
				uint32_t color = bg0;
				switch (bit_pair)
				{
				case 0: color = bg0; break;
				case 1: color = color1; break;
				case 2: color = color2; break;
				case 3: color = color3; break;
				}
				const int px = BORDER_X + col * 8 + cx * 2 + scroll_x;
				if (px >= 0 && px + 1 < VIC_FB_WIDTH)
				{
					fb[row_offset + px] = color;
					fb[row_offset + px + 1] = color;
					if (bit_pair & 0x02) { fgmask[px] = 1; fgmask[px + 1] = 1; }
				}
			}
		}
	}

	void render_extended_background(uint32_t* fb, uint8_t* fgmask, const uint8_t* ram, int canvas_y,
	                                int char_row, int char_pixel_y, const mem_addrs& ma, int scroll_x)
	{
		const int row_offset = canvas_y * VIC_FB_WIDTH;
		const uint32_t bg_colors[4] = {
			vic_palette[ram[0xD021] & 0x0F],
			vic_palette[ram[0xD022] & 0x0F],
			vic_palette[ram[0xD023] & 0x0F],
			vic_palette[ram[0xD024] & 0x0F],
		};

		for (int c = 0; c < 40; c++)
		{
			const int char_code = ram[ma.screen_addr + char_row * 40 + c];
			const int actual = char_code & 0x3F;
			const uint32_t bg = bg_colors[(char_code >> 6) & 0x03];
			const uint32_t fg = vic_palette[ram[COLOR_RAM + char_row * 40 + c] & 0x0F];
			const int glyph = actual * 8;
			const uint8_t line = ma.use_char_rom
				? rom_chars[ma.char_rom_offset + glyph + char_pixel_y]
				: ram[ma.char_addr + glyph + char_pixel_y];

			for (int cx = 0; cx < 8; cx++)
			{
				const int px = BORDER_X + c * 8 + cx + scroll_x;
				if (px >= 0 && px < VIC_FB_WIDTH)
				{
					const bool set = (line & (0x80 >> cx)) != 0;
					fb[row_offset + px] = set ? fg : bg;
					if (set) fgmask[px] = 1;
				}
			}
		}
	}

	void render_sprites(machine_state* s, uint32_t* fb, uint8_t* ram, int canvas_y,
	                    const mem_addrs& ma)
	{
		const uint8_t sprite_enable = ram[0xD015];
		if (sprite_enable == 0) return;

		int8_t* collision = s->sprite_collision;
		std::memset(collision, 0xFF, VIC_FB_WIDTH);

		const uint8_t x_expand = ram[0xD01D];
		const uint8_t y_expand = ram[0xD017];
		const uint8_t multicolor = ram[0xD01C];
		const uint8_t priority = ram[0xD01B];
		const int mc0 = ram[0xD025] & 0x0F;
		const int mc1 = ram[0xD026] & 0x0F;
		const int sprite_ptr_base = ma.screen_addr + 0x03F8;
		const uint8_t* fgmask = s->fg_mask;
		const int row_offset = canvas_y * VIC_FB_WIDTH;

		// A collision latches even when the sprite is drawn behind the graphics, so
		// compare the registers either side of the whole scanline and raise the
		// interrupt once rather than per pixel.
		const uint8_t sprite_sprite_before = ram[0xD01E];
		const uint8_t sprite_bg_before = ram[0xD01F];

		// Render 7..0 so lower-numbered sprites end up on top.
		for (int sprite = 7; sprite >= 0; sprite--)
		{
			if (!(sprite_enable & (1 << sprite))) continue;

			const int vic_x = ram[0xD000 + sprite * 2] | ((ram[0xD010] & (1 << sprite)) ? 0x100 : 0);
			const int vic_y = ram[0xD001 + sprite * 2];
			const int sprite_x = vic_x - 24 + BORDER_X;
			const int sprite_y = vic_y - 50 + BORDER_Y;

			const bool y_exp = (y_expand & (1 << sprite)) != 0;
			const int sprite_height = y_exp ? 42 : 21;
			if (canvas_y < sprite_y || canvas_y >= sprite_y + sprite_height) continue;

			const int sprite_row = y_exp ? (canvas_y - sprite_y) / 2 : (canvas_y - sprite_y);
			const int sprite_ptr = ram[sprite_ptr_base + sprite];
			const int sprite_data_addr = ma.vic_bank + sprite_ptr * 64;
			const int sprite_color = ram[0xD027 + sprite] & 0x0F;

			const bool is_mc = (multicolor & (1 << sprite)) != 0;
			const bool x_exp = (x_expand & (1 << sprite)) != 0;
			const bool behind = (priority & (1 << sprite)) != 0;
			const int x_step = x_exp ? 2 : 1;

			for (int byte_col = 0; byte_col < 3; byte_col++)
			{
				const uint8_t data = ram[sprite_data_addr + sprite_row * 3 + byte_col];

				if (is_mc)
				{
					for (int pair = 0; pair < 4; pair++)
					{
						const int bits = (data >> (6 - pair * 2)) & 0x03;
						if (bits == 0) continue;
						int pixel_color = sprite_color;
						switch (bits)
						{
						case 1: pixel_color = mc0; break;
						case 2: pixel_color = sprite_color; break;
						case 3: pixel_color = mc1; break;
						}
						const int base_x = sprite_x + (byte_col * 8 + pair * 2) * x_step;
						const int width = 2 * x_step;
						for (int dx = 0; dx < width; dx++)
						{
							const int px = base_x + dx;
							if (px < 0 || px >= VIC_FB_WIDTH) continue;
							const int other = collision[px];
							if (other != -1) ram[0xD01E] |= (1 << sprite) | (1 << other);
							collision[px] = static_cast<int8_t>(sprite);
							if (fgmask[px]) ram[0xD01F] |= (1 << sprite);
							if (behind && fgmask[px]) continue;
							fb[row_offset + px] = vic_palette[pixel_color];
						}
					}
				}
				else
				{
					for (int bit = 0; bit < 8; bit++)
					{
						if (!(data & (0x80 >> bit))) continue;
						const int base_x = sprite_x + (byte_col * 8 + bit) * x_step;
						for (int dx = 0; dx < x_step; dx++)
						{
							const int px = base_x + dx;
							if (px < 0 || px >= VIC_FB_WIDTH) continue;
							const int other = collision[px];
							if (other != -1) ram[0xD01E] |= (1 << sprite) | (1 << other);
							collision[px] = static_cast<int8_t>(sprite);
							if (fgmask[px]) ram[0xD01F] |= (1 << sprite);
							if (behind && fgmask[px]) continue;
							fb[row_offset + px] = vic_palette[sprite_color];
						}
					}
				}
			}
		}

		uint8_t latched = 0;
		if (ram[0xD01E] != sprite_sprite_before) latched |= 0x02; // $D019 sprite-sprite
		if (ram[0xD01F] != sprite_bg_before) latched |= 0x04;     // $D019 sprite-background
		if (latched)
		{
			s->vic.irq_status |= latched;
			s->update_vic_irq();
		}
	}
}

void vic_render_scanline(machine_state* s, const int raster_line)
{
	const int canvas_y = raster_line - VIC_FIRST_VISIBLE_RASTER;
	if (canvas_y < 0 || canvas_y >= VIC_FB_HEIGHT) return;

	uint8_t* ram = s->RAM;
	uint32_t* fb = s->framebuffer;

	const uint32_t border_color = vic_palette[ram[0xD020] & 0x0F];
	const uint32_t bg_color = vic_palette[ram[0xD021] & 0x0F];
	// DEN ($D011 bit 4) off blanks the display window down to border colour.
	const bool display_enabled = (ram[VIC_CTRL1] & 0x10) != 0;
	const bool in_screen_y = display_enabled &&
		canvas_y >= BORDER_Y && canvas_y < (BORDER_Y + SCREEN_H);

	const int row_offset = canvas_y * VIC_FB_WIDTH;
	for (int x = 0; x < VIC_FB_WIDTH; x++)
	{
		const bool in_screen_x = x >= BORDER_X && x < (BORDER_X + SCREEN_W);
		fb[row_offset + x] = (in_screen_x && in_screen_y) ? bg_color : border_color;
	}

	if (!in_screen_y) return;

	const int mode = graphics_mode(ram);
	if (mode == MODE_INVALID) return;

	const mem_addrs ma = get_mem_addrs(ram);
	const int xscroll = ram[VIC_CTRL2] & 0x07;
	const int yscroll = ram[VIC_CTRL1] & 0x07;
	const bool rsel = (ram[VIC_CTRL1] & 0x08) != 0;

	const int screen_y = canvas_y - BORDER_Y; // 0-199
	if (!rsel && (screen_y < 4 || screen_y >= 196)) return; // 24-row border mask

	// YSCROLL 3 is the neutral position: the first badline is raster 0x30+YSCROLL,
	// so the KERNAL default of 3 starts screen row 0 at the top of the window.
	const int adjusted_y = screen_y - (yscroll - 3);
	if (adjusted_y < 0 || adjusted_y >= 200) return;

	const int char_row = adjusted_y >> 3;
	const int char_pixel_y = adjusted_y & 7;

	// Foreground mask for this scanline: 1 where the graphics layer is foreground
	// (used for accurate sprite-background collision). Border/background = 0.
	std::memset(s->fg_mask, 0, VIC_FB_WIDTH);

	switch (mode)
	{
	case MODE_STANDARD_CHARACTER:
		render_standard_character(fb, s->fg_mask, ram, canvas_y, char_row, char_pixel_y, ma, xscroll);
		break;
	case MODE_MULTICOLOR_CHARACTER:
		render_multicolor_character(fb, s->fg_mask, ram, canvas_y, char_row, char_pixel_y, ma, xscroll);
		break;
	case MODE_STANDARD_BITMAP:
		render_standard_bitmap(fb, s->fg_mask, ram, canvas_y, char_row, char_pixel_y, ma, xscroll);
		break;
	case MODE_MULTICOLOR_BITMAP:
		render_multicolor_bitmap(fb, s->fg_mask, ram, canvas_y, char_row, char_pixel_y, ma, xscroll);
		break;
	case MODE_EXTENDED_BACKGROUND:
		render_extended_background(fb, s->fg_mask, ram, canvas_y, char_row, char_pixel_y, ma, xscroll);
		break;
	default:
		break;
	}

	render_sprites(s, fb, ram, canvas_y, ma);
}
