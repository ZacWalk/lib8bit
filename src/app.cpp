// app.cpp : C64 emulator GUI ported to the pf:: platform abstraction.

#include "platform.h"
#include "machine.h"

#include <algorithm>
#include <span>
#include <string_view>

static machine mm;

namespace
{
	constexpr uint32_t kTimerId = 1;
	constexpr int kFramebufferW = 320;
	constexpr int kFramebufferH = 200;

	constexpr int kCmdOpenPrg = 1001;
	constexpr int kCmdReset = 1002;
	constexpr int kCmdExit = 1003;
	constexpr int kCmdAbout = 1004;

	constexpr std::string_view kPrgFilters =
		"Turbo 8bit Program Files (*.prg)\0*.prg\0All Files (*.*)\0*.*\0\0";

	void load_prg_from_path(const pf::file_path& path, pf::window_frame_ptr& window)
	{
		if (path.empty()) return;
		const auto bytes = pf::read_file_bytes(path);
		if (bytes.empty())
		{
			window->message_box("Failed to read file.", "Turbo 8bit", pf::msg_box_style::ok);
			return;
		}
		// Reset the machine before loading so a fresh BASIC environment hosts the program.
		mm._state->reset();
		// Run the kernal init for a moment so the BASIC pointers are set up,
		// then patch in our PRG and inject RUN.
		mm.exec(2'000'000); // ~2 seconds of CPU time to finish ROM init
		if (!mm.load_prg(bytes.data(), bytes.size()))
		{
			window->message_box("Not a valid PRG file.", "Turbo 8bit", pf::msg_box_style::ok);
			return;
		}
		window->invalidate();
	}

	void render_framebuffer(uint32_t* dst)
	{
		const auto bg = mm._state->RAM[mem_center_color] & 0x0F;
		auto p_text_color = mem_text_color;

		for (auto row = 0; row < 25; row++)
		{
			const auto text_line = mm._state->RAM + (40 * row) + text_video_mem_offset;
			const auto yy = row * 8;

			for (auto col = 0; col < 40; col++)
			{
				const auto fg_pixel = c64_pallette[mm._state->RAM[p_text_color++] & 0x0F];
				const auto bg_pixel = c64_pallette[bg];
				const auto ch = text_line[col];
				const auto xx = col * 8;
				const auto bits = rom_chars + (ch * 8);

				for (auto i = 0; i < 8; i++)
				{
					const auto b = bits[i];
					auto* row_pixels = dst + ((yy + i) * kFramebufferW) + xx;
					row_pixels[0] = (b & 0x80) ? fg_pixel : bg_pixel;
					row_pixels[1] = (b & 0x40) ? fg_pixel : bg_pixel;
					row_pixels[2] = (b & 0x20) ? fg_pixel : bg_pixel;
					row_pixels[3] = (b & 0x10) ? fg_pixel : bg_pixel;
					row_pixels[4] = (b & 0x08) ? fg_pixel : bg_pixel;
					row_pixels[5] = (b & 0x04) ? fg_pixel : bg_pixel;
					row_pixels[6] = (b & 0x02) ? fg_pixel : bg_pixel;
					row_pixels[7] = (b & 0x01) ? fg_pixel : bg_pixel;
				}
			}
		}
	}

	struct c64_reactor : pf::frame_reactor
	{
		pf::bitmap _frame{kFramebufferW, kFramebufferH, std::vector<uint32_t>(kFramebufferW * kFramebufferH)};

		uint32_t handle_message(pf::window_frame_ptr window, pf::message_type message,
		                        uintptr_t wParam, intptr_t /*lParam*/) override
		{
			switch (message)
			{
			case pf::message_type::create:
				window->set_text("Turbo 8bit");
				window->set_timer(kTimerId, 1000 / 50);
				window->accept_drop_files(true);
				return 0;

			case pf::message_type::timer:
				mm.irq();
				mm.exec();
				if (mm.video_is_invalid())
					window->invalidate();
				return 0;

			case pf::message_type::drop_files:
			{
				const auto files = pf::dropped_files(wParam);
				if (!files.empty())
					load_prg_from_path(files.front(), window);
				return 0;
			}

			case pf::message_type::erase_background:
				return 1;

			case pf::message_type::close:
				window->close();
				return 0;

			case pf::message_type::destroy:
				window->kill_timer(kTimerId);
				return 0;

			default:
				return 0;
			}
		}

		uint32_t handle_keyboard(pf::window_frame_ptr /*window*/, pf::keyboard_message_type message,
		                         const pf::keyboard_params& params) override
		{
			if (message == pf::keyboard_message_type::key_down)
				mm.add_char(params.vk);
			return 0;
		}

		void handle_paint(pf::window_frame_ptr& window, pf::draw_context& draw) override
		{
			const auto border = c64_pallette[mm._state->RAM[mem_border_color] & 0x0F];
			const pf::color_t border_color{
				static_cast<uint8_t>((border >> 16) & 0xff),
				static_cast<uint8_t>((border >> 8) & 0xff),
				static_cast<uint8_t>(border & 0xff)
			};

			const auto client = window->get_client_rect();
			draw.fill_solid_rect(client, border_color);

			render_framebuffer(_frame.pixels.data());

			const int scale_x = client.width() / kFramebufferW;
			const int scale_y = client.height() / kFramebufferH;
			const int scale = std::max(1, std::min(scale_x, scale_y));
			const int render_w = kFramebufferW * scale;
			const int render_h = kFramebufferH * scale;
			const auto cx = (client.width() - render_w) / 2;
			const auto cy = (client.height() - render_h) / 2;

			const pf::irect dest{cx, cy, cx + render_w, cy + render_h};
			draw.draw_bitmap(dest, _frame);
		}

		void handle_size(pf::window_frame_ptr& window, pf::isize /*extent*/,
						 pf::measure_context& /*measure*/) override
		{
			window->invalidate();
		}
	};

	std::shared_ptr<c64_reactor> g_reactor;
}

app_init_result app_init(const pf::window_frame_ptr& main_frame,
                         std::span<const std::string_view> /*params*/)
{
	mm.exec(1000000);

	g_reactor = std::make_shared<c64_reactor>();
	main_frame->set_reactor(g_reactor);
	main_frame->set_text("Turbo 8bit");

	// Build the menu (File / Help) using the platform menu API.
	auto frame = main_frame; // shared_ptr captured by value into menu actions
	std::vector<pf::menu_command> file_items;
	file_items.emplace_back("&Open PRG...", kCmdOpenPrg,
		[frame]() mutable
		{
			auto path = pf::open_file_path("Open PRG", kPrgFilters);
			if (!path.empty())
				load_prg_from_path(path, frame);
		});
	file_items.emplace_back("&Reset", kCmdReset,
		[]
		{
			mm._state->reset();
		});
	file_items.emplace_back("", 0, nullptr); // separator
	file_items.emplace_back("E&xit", kCmdExit,
		[frame] { frame->close(); });

	std::vector<pf::menu_command> help_items;
	help_items.emplace_back("&About...", kCmdAbout,
		[frame]
		{
			frame->message_box("Turbo 8bit\n\nCommodore 64 emulator. Drop a .PRG file on the window or use File > Open PRG.",
			                   "About Turbo 8bit", pf::msg_box_style::ok);
		});

	std::vector<pf::menu_command> menu;
	menu.emplace_back("&File", 0, nullptr, nullptr, nullptr, std::move(file_items));
	menu.emplace_back("&Help", 0, nullptr, nullptr, nullptr, std::move(help_items));
	main_frame->set_menu(std::move(menu));

	main_frame->accept_drop_files(true);
	main_frame->show(true);

	return {};
}

void app_idle()
{
}

void app_destroy()
{
	g_reactor.reset();
}
