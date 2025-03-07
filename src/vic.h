#pragma once

#include <cstdint>

struct machine_state;

// lib8bit by Zac Walker
//
// VIC-II raster renderer interface: frame buffer dimensions and register
// addresses used by the per-scanline renderer in vic.cpp.
//
// Frame buffer dimensions (visible picture including border): a full C64 display
// fits as 320x200 screen plus border.
constexpr int VIC_FB_WIDTH = 384;
constexpr int VIC_FB_HEIGHT = 272;

// VIC-II raster line shown at the top of the visible frame buffer. Screen
// content starts at raster 51; we show 36 border lines above it.
constexpr int VIC_FIRST_VISIBLE_RASTER = 51 - 36; // 15

// VIC-II register addresses.
constexpr uint16_t VIC_CTRL1 = 0xD011; // ECM/BMM/DEN/RSEL/YSCROLL + raster bit 8
constexpr uint16_t VIC_CTRL2 = 0xD016; // MCM/CSEL/XSCROLL
constexpr uint16_t VIC_MEMORY = 0xD018; // video matrix + char/bitmap base

// C64 colour palette (VICE default), packed 0x00RRGGBB.
extern const uint32_t vic_palette[16];

// Render one VIC-II raster line (0-311) into s->framebuffer, including the
// active graphics mode and any sprites intersecting the line.
void vic_render_scanline(machine_state* s, int raster_line);
