// lib8bit by Zac Walker
//
// The C64 host is an application of libdesktop's platform layer. The emulator
// remains a byte-in/byte-out library: this file owns media files, the C64
// keyboard layout, presentation, menus, settings and the audio clock.

#include "debug.h"
#include "disk.h"
#include "machine.h"
#include "platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	constexpr std::string_view kProductName = "lib8bit";
	constexpr int kFramebufferW = machine::framebuffer_width();
	constexpr int kFramebufferH = machine::framebuffer_height();
	constexpr int kSampleRate = 44100;
	constexpr int kCyclesPerFrame = pal_cycles_per_frame;
	constexpr size_t kMaxFileSize = 16 * 1024 * 1024;
	constexpr int kAudioBlocks = 4;

	constexpr int kCmdOpen = 1001;
	constexpr int kCmdReset = 1002;
	constexpr int kCmdExit = 1003;
	constexpr int kCmdAbout = 1004;
	constexpr int kCmdPause = 1005;
	constexpr int kCmdEject = 1006;
	constexpr int kCmdKeyboard = 1007;
	constexpr int kCmdDebug = 1008;
	constexpr int kCmdArrowsCursor = 1010;
	constexpr int kCmdArrowsJoy1 = 1011;
	constexpr int kCmdArrowsJoy2 = 1012;

	constexpr uint8_t kJoyUp = 0x01;
	constexpr uint8_t kJoyDown = 0x02;
	constexpr uint8_t kJoyLeft = 0x04;
	constexpr uint8_t kJoyRight = 0x08;
	constexpr uint8_t kJoyFire = 0x10;

	// The filter is NUL-separated because pf::open_file_path forwards the same
	// Win32-compatible filter convention without putting an OS type in this app.
	constexpr char kFileFilter[] =
		"C64 Files (*.prg;*.crt;*.sid;*.d64;*.d71;*.d81)\0"
		"*.prg;*.crt;*.sid;*.d64;*.d71;*.d81\0"
		"Programs (*.prg)\0*.prg\0"
		"Cartridges (*.crt)\0*.crt\0"
		"SID Tunes (*.sid;*.psid)\0*.sid;*.psid\0"
		"Disk Images (*.d64;*.d71;*.d81)\0*.d64;*.d71;*.d81\0"
		"All Files (*.*)\0*.*\0\0";

	enum class arrow_mode
	{
		cursor,
		joystick1,
		joystick2,
	};

	struct keypos
	{
		int row = 0;
		int col = 0;
	};

	machine g_machine;
	pf::window_frame_ptr g_frame;
	std::shared_ptr<pf::frame_reactor> g_reactor;
	std::mutex g_machine_mutex;
	pf::bitmap g_surface;

	struct frame_snapshot
	{
		std::vector<uint32_t> pixels = std::vector<uint32_t>(
			static_cast<size_t>(kFramebufferW) * kFramebufferH);
		debug_state debug;
	};

	std::mutex g_frame_mutex;
	frame_snapshot g_frames[3];
	int g_frame_write = 0;
	int g_frame_ready = 1;
	int g_frame_read = 2;
	bool g_frame_fresh = false;

	std::atomic<bool> g_running{true};
	std::atomic<bool> g_worker_stop{false};
	std::atomic<bool> g_worker_active{false};
	std::atomic<bool> g_shutting_down{false};
	std::atomic<bool> g_ui_tick_pending{false};
	std::atomic<bool> g_sid_stopped_pending{false};
	std::atomic<uint32_t> g_media_generation{0};
	std::atomic<uint32_t> g_sid_stopped_generation{0};
	std::atomic<bool> g_audio_failure_pending{false};
	bool g_headless = false;
	std::atomic<bool> g_paused{false};
	std::atomic<bool> g_show_debug{false};
	std::atomic<bool> g_audio_ok{false};
	bool g_sound_started = false;
	std::atomic<bool> g_sid_playing{false};
	std::atomic<arrow_mode> g_arrow_mode{arrow_mode::cursor};
	std::atomic<uint8_t> g_joystick{0};
	std::atomic<uint8_t> g_joystick_taps{0};
	std::atomic<int64_t> g_emu_frames{0};

	std::string g_media;
	bool g_cart_inserted = false;
	bool g_disk_inserted = false;
	pf::file_path g_start_path;
	pf::serial_executor_ptr g_emu_executor;

	struct ui_settings
	{
		pf::window_frame::placement placement;
		bool has_placement = false;
		int panel_w96 = 0;
		bool show_debug = false;
	};

	ui_settings g_settings;
	bool g_settings_available = false;
	int g_dpi = 96;
	int g_panel_w = 0;
	bool g_dragging = false;
	pf::isize g_minimum_client_size;
	bool g_end_session_saved = false;

	struct picture_layout
	{
		int x = 0;
		int y = 0;
		int scale = 1;
		int width = kFramebufferW;
		int height = kFramebufferH;
		int picture_width = 0;
		int splitter = -1;
		bool debug = false;

		bool operator==(const picture_layout&) const = default;
	};

	pf::isize g_client;
	picture_layout g_layout;
	std::vector<int16_t> g_audio_scratch(4096);

	int config_int(const std::string_view section, const std::string_view key, const int fallback)
	{
		const auto text = pf::config_read(section, key);
		int value = fallback;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		return parsed.ec == std::errc{} ? value : fallback;
	}

	void configure_settings()
	{
		const auto roaming = pf::roaming_app_data_path();
		if (roaming.empty()) return;

		const auto folder = roaming.combine("lib8bit");
		if (!pf::is_directory(folder) && !pf::platform_create_directory(folder))
			return;

		pf::config_set_path(folder.combine("lib8bit.ini"));
		g_settings_available = true;
	}

	void load_settings()
	{
		if (!g_settings_available) return;
		const int x = config_int("window", "x", std::numeric_limits<int>::min());
		const int y = config_int("window", "y", std::numeric_limits<int>::min());
		const int width = config_int("window", "width", 0);
		const int height = config_int("window", "height", 0);
		if (x != std::numeric_limits<int>::min() && y != std::numeric_limits<int>::min() &&
			width > 0 && height > 0)
		{
			g_settings.placement.normal_bounds = {x, y, x + width, y + height};
			g_settings.has_placement = true;
		}

		g_settings.placement.maximized = config_int("window", "maximized", 0) != 0;
		g_settings.panel_w96 = config_int("panel", "width", 0);
		g_settings.show_debug = config_int("panel", "visible", 0) != 0;
	}

	void save_settings()
	{
		if (!g_frame) return;
		g_end_session_saved = true;
		if (g_headless || !g_settings_available) return;

		const auto placement = g_frame->get_placement();
		const auto& bounds = placement.normal_bounds;
		if (bounds.width() > 0 && bounds.height() > 0)
		{
			pf::config_write("window", "x", std::to_string(bounds.left));
			pf::config_write("window", "y", std::to_string(bounds.top));
			pf::config_write("window", "width", std::to_string(bounds.width()));
			pf::config_write("window", "height", std::to_string(bounds.height()));
		}

		pf::config_write("window", "maximized", placement.maximized ? "1" : "0");
		pf::config_write("panel", "width",
			std::to_string(pf::mul_div(g_panel_w, 96, std::max(1, g_dpi))));
		pf::config_write("panel", "visible", g_show_debug.load() ? "1" : "0");
		pf::config_flush();
	}

	void report(const std::string_view message)
	{
		if (g_frame)
		{
			g_frame->message_box(message, kProductName,
				pf::msg_box_style::ok | pf::msg_box_style::icon_warning);
		}
		else
		{
			pf::platform_show_error(message, kProductName);
		}
	}

	std::vector<uint8_t> read_file(const pf::file_path& path)
	{
		const auto file = pf::open_for_read(path);
		if (!file) return {};

		const uint32_t size = file->size();
		if (size == 0 || size > kMaxFileSize) return {};

		std::vector<uint8_t> bytes(size);
		size_t offset = 0;
		while (offset < bytes.size())
		{
			uint32_t read = 0;
			const uint32_t remaining = static_cast<uint32_t>(bytes.size() - offset);
			if (!file->read(bytes.data() + offset, remaining, &read) || read == 0)
				return {};
			offset += read;
		}
		return bytes;
	}

	bool has_signature(const std::vector<uint8_t>& bytes, const std::string_view signature)
	{
		return bytes.size() >= signature.size() &&
			std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
	}

	bool is_crt(const std::vector<uint8_t>& bytes)
	{
		return has_signature(bytes, "C64 CARTRIDGE   ");
	}

	bool is_sid(const std::vector<uint8_t>& bytes)
	{
		return has_signature(bytes, "PSID") || has_signature(bytes, "RSID");
	}

	bool named_like_prg(const pf::file_path& path)
	{
		const auto extension = path.extension();
		return extension.empty() || pf::icmp(extension, ".prg") == 0;
	}

	void stop_audio();
	void stop_emulation_worker();
	void reset_audio_clock();
	void update_title();

	void load_file(const pf::file_path& path)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			report("Failed to read file.");
			return;
		}

		std::string error;
		bool loaded = false;
		bool cart = g_cart_inserted;
		bool disk = g_disk_inserted;

		{
			std::lock_guard lock(g_machine_mutex);
			if (is_sid(bytes))
			{
				loaded = g_machine.load_sid(bytes.data(), bytes.size());
				if (!loaded) error = "Not a valid SID file.";
			}
			else if (is_crt(bytes))
			{
				loaded = g_machine.load_crt(bytes.data(), bytes.size());
				cart = loaded;
				if (!loaded) error = "Not a valid CRT file.";
			}
			else if (g_machine.insert_disk(bytes.data(), bytes.size()))
			{
				// Keep the image mounted after the first program starts. The KERNAL
				// LOAD trap can then service later loads from the same bytes.
				disk = true;
				loaded = true;
				g_machine.reset();
				g_machine.exec(3'000'000);

				std::vector<uint8_t> prg;
				if (!read_disk_file(bytes.data(), bytes.size(), "*", prg))
					error = "No program found on this disk. It is still in the drive.";
				else if (!g_machine.load_prg(prg.data(), prg.size()))
					error = "The program on this disk could not be started.";
			}
			else if (!named_like_prg(path))
			{
				error = "Not a C64 program, cartridge, SID tune or disk image.";
			}
			else if (!machine::is_prg(bytes.data(), bytes.size()))
			{
				error = "Not a valid PRG file.";
			}
			else
			{
				// Boot first so a BASIC program is injected exactly as the old host
				// did, rather than relying on an implementation detail of KERNAL RAM.
				g_machine.reset();
				g_machine.exec(3'000'000);
				loaded = g_machine.load_prg(bytes.data(), bytes.size());
				if (!loaded) error = "Not a valid PRG file.";
			}

			if (loaded)
			{
				reset_audio_clock();
				g_machine.set_audio_enabled(true);
				g_sid_playing.store(g_machine.sid_player_active());
			}
		}

		if (loaded)
		{
			g_media_generation.fetch_add(1);
			g_cart_inserted = cart;
			g_disk_inserted = disk;
			g_media = path.name();
			update_title();
		}

		if (!error.empty()) report(error);
	}

	void open_dialog()
	{
		constexpr std::string_view filters(kFileFilter, sizeof(kFileFilter) - 1);
		const auto path = pf::open_file_path(
			"Open Program, Cartridge, SID Tune or Disk Image", filters);
		if (!path.empty()) load_file(path);
	}

	void release_all_keys()
	{
		std::lock_guard lock(g_machine_mutex);
		g_joystick.store(0);
		g_joystick_taps.store(0);
		g_machine.clear_keys();
	}

	void set_arrow_mode(const arrow_mode mode)
	{
		g_arrow_mode.store(mode);
		if (mode == arrow_mode::cursor)
		{
			g_joystick.store(0);
			g_joystick_taps.store(0);
		}
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void apply_joystick()
	{
		const arrow_mode mode = g_arrow_mode.load();
		if (mode == arrow_mode::cursor)
		{
			g_machine.set_joystick(1, false, false, false, false, false);
			g_machine.set_joystick(2, false, false, false, false, false);
			return;
		}

		const uint8_t bits = static_cast<uint8_t>(g_joystick.load() | g_joystick_taps.exchange(0));
		const int port = mode == arrow_mode::joystick1 ? 1 : 2;
		const int other = port == 1 ? 2 : 1;
		g_machine.set_joystick(port, (bits & kJoyUp) != 0, (bits & kJoyDown) != 0,
			(bits & kJoyLeft) != 0, (bits & kJoyRight) != 0, (bits & kJoyFire) != 0);
		g_machine.set_joystick(other, false, false, false, false, false);
	}

	void send_keys(const keypos* positions, const int count, const bool pressed)
	{
		if (count <= 0) return;
		std::lock_guard lock(g_machine_mutex);

		if (g_paused.load())
		{
			g_machine.flush_keys();
			for (int i = 0; i < count; ++i)
				g_machine.set_key(positions[i].row, positions[i].col, pressed);
		}
		else
		{
			for (int i = 0; i < count; ++i)
				g_machine.queue_key(positions[i].row, positions[i].col, pressed);
		}
	}

	// The map deliberately follows VICE's positional layout. Shifted cursor
	// keys and function keys press the C64's left shift as a second matrix key.
	int map_key(const unsigned int vk, keypos positions[2])
	{
		const auto one = [&](const int row, const int col)
		{
			positions[0] = {row, col};
			return 1;
		};
		const auto shifted = [&](const int row, const int col)
		{
			positions[0] = {1, 7};
			positions[1] = {row, col};
			return 2;
		};

		switch (vk)
		{
		case 'A': return one(1, 2); case 'B': return one(3, 4); case 'C': return one(2, 4);
		case 'D': return one(2, 2); case 'E': return one(1, 6); case 'F': return one(2, 5);
		case 'G': return one(3, 2); case 'H': return one(3, 5); case 'I': return one(4, 1);
		case 'J': return one(4, 2); case 'K': return one(4, 5); case 'L': return one(5, 2);
		case 'M': return one(4, 4); case 'N': return one(4, 7); case 'O': return one(4, 6);
		case 'P': return one(5, 1); case 'Q': return one(7, 6); case 'R': return one(2, 1);
		case 'S': return one(1, 5); case 'T': return one(2, 6); case 'U': return one(3, 6);
		case 'V': return one(3, 7); case 'W': return one(1, 1); case 'X': return one(2, 7);
		case 'Y': return one(3, 1); case 'Z': return one(1, 4);

		case '1': return one(7, 0); case '2': return one(7, 3); case '3': return one(1, 0);
		case '4': return one(1, 3); case '5': return one(2, 0); case '6': return one(2, 3);
		case '7': return one(3, 0); case '8': return one(3, 3); case '9': return one(4, 0);
		case '0': return one(4, 3);

		case pf::platform_key::Comma: return one(5, 7);
		case pf::platform_key::Period: return one(5, 4);
		case pf::platform_key::Slash: return one(6, 7);
		case pf::platform_key::Semicolon: return one(6, 2);
		case pf::platform_key::Minus: return one(5, 3);
		case pf::platform_key::Equals: return one(5, 0);
		case pf::platform_key::Backtick: return one(7, 1);
		case pf::platform_key::Quote: return one(5, 5);
		case pf::platform_key::LeftBracket: return one(5, 6);
		case pf::platform_key::RightBracket: return one(6, 1);
		case pf::platform_key::Backslash: return one(6, 6);
		case pf::platform_key::Next: return one(6, 5);
		case pf::platform_key::End: return one(6, 0);

		case pf::platform_key::Return: return one(0, 1);
		case pf::platform_key::Space: return one(7, 4);
		case pf::platform_key::Back: return one(0, 0);
		case pf::platform_key::Delete: return one(0, 0);
		case pf::platform_key::Shift: return one(1, 7);
		case pf::platform_key::Control: return one(7, 2);
		case pf::platform_key::Tab: return one(7, 5);
		case pf::platform_key::Escape: return one(7, 7);
		case pf::platform_key::Home: return one(6, 3);

		case pf::platform_key::Down: return one(0, 7);
		case pf::platform_key::Right: return one(0, 2);
		case pf::platform_key::Up: return shifted(0, 7);
		case pf::platform_key::Left: return shifted(0, 2);

		case pf::platform_key::F1: return one(0, 4);
		case pf::platform_key::F3: return one(0, 5);
		case pf::platform_key::F5: return one(0, 6);
		case pf::platform_key::F7: return one(0, 3);
		case pf::platform_key::F2: return shifted(0, 4);
		case pf::platform_key::F4: return shifted(0, 5);
		case pf::platform_key::F6: return shifted(0, 6);
		case pf::platform_key::F8: return shifted(0, 3);
		default: return 0;
		}
	}

	class audio_pacer
	{
	public:
		enum class submit_result
		{
			no_stream,
			not_ready,
			submitted,
			rejected,
		};

		void set_stream(pf::audio_stream_ptr stream)
		{
			_stream = std::move(stream);
		}

		[[nodiscard]] bool can_step() const
		{
			return !_stream || _stream->can_write();
		}

		// Checking before generate_audio is important: a full ring must not
		// advance SID's audio clock and then silently lose that frame.
		submit_result submit(machine& c64)
		{
			if (!_stream) return submit_result::no_stream;
			if (!_stream->can_write()) return submit_result::not_ready;

			const int count = c64.generate_audio(_samples.data(),
				static_cast<int>(_samples.size()));
			if (count <= 0) return submit_result::submitted;
			return _stream->write(std::span<const int16_t>(_samples.data(),
				static_cast<size_t>(count)))
				? submit_result::submitted
				: submit_result::rejected;
		}

		void stop()
		{
			if (_stream) _stream->stop();
		}

	private:
		pf::audio_stream_ptr _stream;
		std::vector<int16_t> _samples = std::vector<int16_t>(4096);
	};

	audio_pacer g_audio_pacer;
	pf::audio_stream_ptr g_audio;

	void start_audio()
	{
		if (g_audio || g_headless) return;
		if (!pf::sound_init()) return;

		g_audio = pf::create_audio_stream(kSampleRate, 1, kAudioBlocks);
		if (!g_audio)
		{
			pf::sound_shutdown();
			return;
		}

		g_audio_pacer.set_stream(g_audio);
		g_sound_started = true;
		g_audio_ok.store(true);
		update_title();
	}

	void stop_audio()
	{
		if (g_audio)
		{
			g_audio_pacer.stop();
			g_audio.reset();
			g_audio_pacer.set_stream(nullptr);
		}
		if (g_sound_started)
		{
			pf::sound_shutdown();
			g_sound_started = false;
		}
		g_audio_ok.store(false);
	}

	void reset_audio_clock()
	{
		if (g_audio) g_audio->stop();
		g_machine.set_audio_enabled(true);
	}

	void reset_machine()
	{
		bool was_sid = false;
		{
			std::lock_guard lock(g_machine_mutex);
			was_sid = g_sid_playing.load();
			g_machine.reset();
			reset_audio_clock();
			g_sid_playing.store(false);
			g_media_generation.fetch_add(1);
		}
		if (was_sid) g_media.clear();
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void eject_media()
	{
		{
			std::lock_guard lock(g_machine_mutex);
			g_machine.eject_disk();
			g_machine.eject_crt();
			reset_audio_clock();
			g_sid_playing.store(false);
			g_media_generation.fetch_add(1);
		}
		g_cart_inserted = false;
		g_disk_inserted = false;
		g_media.clear();
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void toggle_pause()
	{
		g_paused.store(!g_paused.load());
		update_title();
		if (g_frame) g_frame->invalidate();
	}

	void close_app()
	{
		if (!g_frame) return;
		g_shutting_down.store(true);
		stop_emulation_worker();
		if (!g_headless) save_settings();
		g_frame->close();
	}

	std::string input_mode_name()
	{
		switch (g_arrow_mode.load())
		{
		case arrow_mode::joystick1: return " [Joystick 1]";
		case arrow_mode::joystick2: return " [Joystick 2]";
		default: return " [Keyboard]";
		}
	}

	void update_title()
	{
		if (!g_frame) return;

		std::string title(kProductName);
		title += " - ";
		title += g_media.empty() ? "no media" : g_media;
		title += input_mode_name();
		if (g_paused.load()) title += " - Paused";
		if (!g_audio_ok.load()) title += " (no audio)";
		g_frame->set_text(title);
	}

	int splitter_width()
	{
		return std::max(4, pf::mul_div(5, g_dpi, 96));
	}

	int text_scale()
	{
		return std::max(2, pf::mul_div(2, g_dpi, 96));
	}

	int default_panel_width()
	{
		return 31 * 8 * text_scale() + 4 * text_scale();
	}

	int panel_width(const int client_width)
	{
		const int max_width = std::max(0, client_width - splitter_width() - kFramebufferW);
		const int min_width = std::min(default_panel_width(), max_width);
		return std::clamp(g_panel_w, min_width, max_width);
	}

	int splitter_x(const int client_width)
	{
		if (!g_show_debug.load()) return -1;
		const int x = client_width - panel_width(client_width) - splitter_width();
		return x >= 0 ? x : -1;
	}

	picture_layout compute_layout()
	{
		picture_layout layout;
		layout.picture_width = g_client.cx;
		layout.splitter = splitter_x(g_client.cx);
		layout.debug = layout.splitter >= 0;
		if (layout.debug) layout.picture_width = layout.splitter;

		if (g_client.cx <= 0 || g_client.cy <= 0 || layout.picture_width <= 0)
			return layout;

		layout.scale = std::max(1, std::min(
			layout.picture_width / kFramebufferW, g_client.cy / kFramebufferH));
		const int render_width = kFramebufferW * layout.scale;
		const int render_height = kFramebufferH * layout.scale;
		layout.x = (layout.picture_width - render_width) / 2;
		layout.y = (g_client.cy - render_height) / 2;
		return layout;
	}

	void fill_rect(const int x, const int y, const int width, const int height,
		const uint32_t colour)
	{
		if (g_surface.pixels.empty() || width <= 0 || height <= 0) return;

		const int left = std::max(0, x);
		const int top = std::max(0, y);
		const int right = std::min(g_surface.width, x + width);
		const int bottom = std::min(g_surface.height, y + height);
		if (right <= left || bottom <= top) return;

		for (int row = top; row < bottom; ++row)
		{
			auto* const begin = g_surface.pixels.data() +
				static_cast<size_t>(row) * g_surface.width + left;
			std::fill(begin, begin + right - left, colour);
		}
	}

	void blit_picture(const uint32_t* source, const int dx, const int dy, const int scale)
	{
		if (!source || g_surface.pixels.empty()) return;

		const int width = kFramebufferW * scale;
		const int height = kFramebufferH * scale;
		const bool fits = dx >= 0 && dy >= 0 &&
			dx + width <= g_surface.width && dy + height <= g_surface.height;
		if (fits)
		{
			const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint32_t);
			for (int y = 0; y < kFramebufferH; ++y)
			{
				auto* const row = g_surface.pixels.data() +
					static_cast<size_t>(dy + y * scale) * g_surface.width + dx;
				for (int x = 0; x < kFramebufferW; ++x)
					std::fill_n(row + static_cast<size_t>(x) * scale, scale, source[x]);
				for (int r = 1; r < scale; ++r)
					std::memcpy(row + static_cast<size_t>(r) * g_surface.width, row, row_bytes);
				source += kFramebufferW;
			}
			return;
		}

		// The platform lets the user resize below the preferred minimum. Keep the
		// compositor safe and retain the same nearest-neighbour mapping there.
		for (int sy = 0; sy < kFramebufferH; ++sy)
		{
			for (int sx = 0; sx < kFramebufferW; ++sx)
			{
				const uint32_t colour = source[sy * kFramebufferW + sx];
				for (int yy = 0; yy < scale; ++yy)
				{
					const int y = dy + sy * scale + yy;
					if (y < 0 || y >= g_surface.height) continue;
					for (int xx = 0; xx < scale; ++xx)
					{
						const int x = dx + sx * scale + xx;
						if (x >= 0 && x < g_surface.width)
							g_surface.pixels[static_cast<size_t>(y) * g_surface.width + x] = colour;
					}
				}
			}
		}
	}

	uint8_t screen_code(const char c)
	{
		const auto value = static_cast<uint8_t>(c);
		if (value >= 0x60 && value < 0x80) return static_cast<uint8_t>(value - 0x60);
		if (value >= 0x40 && value < 0x60) return static_cast<uint8_t>(value - 0x40);
		if (value >= 0x20 && value < 0x40) return value;
		return 0x20;
	}

	class panel_writer
	{
	public:
		panel_writer(const int left, const int top, const int right, const int bottom,
			const int scale, const bool measuring = false)
			: _left(left), _top(top), _right(right), _bottom(bottom), _scale(scale),
			  _y(top), _measuring(measuring)
		{
		}

		int cell() const { return 8 * _scale; }
		int height() const { return _y - _top; }
		int width() const { return _width; }

		void blank() { _y += cell(); }

		void line(const uint32_t colour, const char* format, ...)
		{
			char text[80];
			va_list args;
			va_start(args, format);
			const int written = std::vsnprintf(text, sizeof(text), format, args);
			va_end(args);

			const int chars = std::clamp(written, 0, static_cast<int>(sizeof(text)) - 1);
			_width = std::max(_width, chars * cell());
			if (_measuring)
			{
				_y += cell();
				return;
			}

			if (_y + cell() > _bottom) return;
			int x = _left;
			for (const char* c = text; *c && x + cell() <= _right; ++c, x += cell())
				glyph(x, _y, screen_code(*c), colour);
			_y += cell();
		}

	private:
		void glyph(const int x, const int y, const uint8_t code, const uint32_t colour) const
		{
			const uint8_t* const rows = rom_chars + static_cast<size_t>(code) * 8;
			for (int gy = 0; gy < 8; ++gy)
			{
				const uint8_t bits = rows[gy];
				for (int gx = 0; gx < 8; ++gx)
				{
					if (!(bits & (0x80 >> gx))) continue;
					for (int sy = 0; sy < _scale; ++sy)
					{
						const int py = y + gy * _scale + sy;
						if (py < 0 || py >= g_surface.height) continue;
						for (int sx = 0; sx < _scale; ++sx)
						{
							const int px = x + gx * _scale + sx;
							if (px >= 0 && px < g_surface.width)
								g_surface.pixels[static_cast<size_t>(py) * g_surface.width + px] = colour;
						}
					}
				}
			}
		}

		int _left;
		int _top;
		int _right;
		int _bottom;
		int _scale;
		int _y;
		bool _measuring;
		int _width = 0;
	};

	void emit_debug_lines(panel_writer& writer, const debug_state& state, const int fps)
	{
		const uint32_t head = vic_palette[1];
		const uint32_t text = vic_palette[14];
		const uint32_t mark = vic_palette[7];

		char flags[9];
		static constexpr char flag_names[] = "NV-BDIZC";
		for (int i = 0; i < 8; ++i)
			flags[i] = (state.status & (0x80 >> i)) ? flag_names[i] : '.';
		flags[8] = 0;

		writer.line(head, "CPU");
		writer.line(text, "PC %04X A %02X X %02X Y %02X SP %02X",
			state.pc, state.a, state.x, state.y, state.sp);
		writer.line(text, "P %02X %s  IRQ %c NMI %c", state.status, flags,
			state.irq_pending ? '*' : '.', state.nmi_pending ? '*' : '.');
		writer.line(text, "$01 %02X %s %s %s %s%s", state.port,
			state.basic_on ? "BAS" : "---", state.kernal_on ? "KER" : "---",
			state.io_on ? "I/O" : "---", state.char_on ? "CHR" : "---",
			state.cart_inserted ? " CRT" : "");

		writer.blank();
		writer.line(head, "DISASSEMBLY");
		for (int i = 0; i < state.disasm_count; ++i)
		{
			const disasm_line& line = state.disasm[i];
			char raw[10];
			switch (line.length)
			{
			case 1: std::snprintf(raw, sizeof(raw), "%02X      ", line.bytes[0]); break;
			case 2: std::snprintf(raw, sizeof(raw), "%02X %02X   ",
				line.bytes[0], line.bytes[1]); break;
			default: std::snprintf(raw, sizeof(raw), "%02X %02X %02X",
				line.bytes[0], line.bytes[1], line.bytes[2]); break;
			}
			writer.line(i == state.current ? mark : text, "%c%04X %s %s",
				i == state.current ? '>' : ' ', line.address, raw, line.text);
		}

		writer.blank();
		writer.line(head, "VIC-II");
		writer.line(text, "MODE %-9s RASTER %03u/%03u", state.mode,
			state.raster, state.raster_compare);
		writer.line(text, "D011 %02X D016 %02X D018 %02X",
			state.ctrl1, state.ctrl2, state.mem_ptr);
		writer.line(text, "SCR %04X CHR %04X BMP %04X",
			state.screen_base, state.char_base, state.bitmap_base);
		writer.line(text, "BANK %04X BORDER %X BG %X",
			state.vic_bank, state.border, state.background);
		writer.line(text, "SPR %02X HIT %02X/%02X IRQ %02X/%02X",
			state.sprite_enable, state.sprite_sprite_hit, state.sprite_bg_hit,
			state.vic_irq_status, state.vic_irq_enable);

		writer.blank();
		writer.line(head, "CIA");
		for (int c = 0; c < 2; ++c)
			writer.line(text, "%d A %04X%c B %04X%c ICR %02X/%02X", c + 1,
				state.timer[c * 2], state.timer_running[c * 2] ? '>' : '.',
				state.timer[c * 2 + 1], state.timer_running[c * 2 + 1] ? '>' : '.',
				state.cia_icr[c], state.cia_mask[c]);

		writer.blank();
		writer.line(head, "SID");
		for (int v = 0; v < 3; ++v)
		{
			const uint8_t* const regs = state.sid + v * 7;
			writer.line(text, "%d F%04X W%02X AD%02X SR%02X E%02X", v + 1,
				static_cast<unsigned>(regs[0] | (regs[1] << 8)), regs[4],
				regs[5], regs[6], state.sid_env[v]);
		}
		writer.line(text, "VOL %X FILT %02X CUTOFF %04X", state.sid[0x18] & 0x0F,
			state.sid[0x17], static_cast<unsigned>((state.sid[0x15] & 0x07) |
			(state.sid[0x16] << 3)));

		writer.blank();
		writer.line(head, "HOST");
		if (fps < 0)
			writer.line(text, "EMULATION PAUSED");
		else
			writer.line(fps < 48 ? mark : text, "EMULATION %d FPS (%d%%)", fps, fps * 2);
	}

	int emulated_fps()
	{
		static auto last_time = std::chrono::steady_clock::now();
		static int64_t last_frames = 0;
		static int value = 50;
		static bool was_paused = false;
		const auto now = std::chrono::steady_clock::now();

		if (g_paused.load())
		{
			was_paused = true;
			return -1;
		}
		if (was_paused)
		{
			was_paused = false;
			last_frames = g_emu_frames.load();
			last_time = now;
		}

		const auto elapsed = std::chrono::duration<double>(now - last_time).count();
		if (elapsed >= 0.5)
		{
			const int64_t frames = g_emu_frames.load();
			value = static_cast<int>((frames - last_frames) / elapsed + 0.5);
			last_frames = frames;
			last_time = now;
		}
		return value;
	}

	void draw_debug_panel(const int left, const int top, const int right, const int bottom,
		const debug_state& state)
	{
		const int scale = text_scale();
		panel_writer measure(left, top, right, bottom, scale, true);
		emit_debug_lines(measure, state, emulated_fps());

		const int x = left + std::max(scale, (right - left - measure.width()) / 2);
		const int y = top + std::max(scale, (bottom - top - measure.height()) / 2);
		panel_writer draw(x, y, right, bottom, scale);
		emit_debug_lines(draw, state, emulated_fps());
	}

	bool compose()
	{
		if (g_client.cx <= 0 || g_client.cy <= 0) return false;

		{
			std::lock_guard lock(g_frame_mutex);
			if (g_frame_fresh)
			{
				std::swap(g_frame_ready, g_frame_read);
				g_frame_fresh = false;
			}
		}
		const frame_snapshot& current = g_frames[g_frame_read];
		const picture_layout layout = compute_layout();
		if (layout != g_layout ||
			g_surface.pixels.size() != static_cast<size_t>(g_client.cx) * g_client.cy)
		{
			g_layout = layout;
			g_surface.width = g_client.cx;
			g_surface.height = g_client.cy;
			g_surface.pixels.assign(static_cast<size_t>(g_client.cx) * g_client.cy, 0);
		}
		else
		{
			std::fill(g_surface.pixels.begin(), g_surface.pixels.end(), 0);
		}

		fill_rect(0, 0, g_layout.picture_width, g_layout.y, 0);
		fill_rect(0, g_layout.y + kFramebufferH * g_layout.scale,
			g_layout.picture_width, g_client.cy, 0);
		fill_rect(0, g_layout.y, g_layout.x, kFramebufferH * g_layout.scale, 0);
		fill_rect(g_layout.x + kFramebufferW * g_layout.scale, g_layout.y,
			g_layout.picture_width, kFramebufferH * g_layout.scale, 0);
		blit_picture(current.pixels.data(), g_layout.x, g_layout.y, g_layout.scale);

		if (g_layout.debug)
		{
			fill_rect(g_layout.splitter, 0, splitter_width(), g_client.cy, vic_palette[11]);
			const int panel_x = g_layout.splitter + splitter_width();
			fill_rect(panel_x, 0, g_client.cx - panel_x, g_client.cy, vic_palette[6]);
			draw_debug_panel(panel_x, 0, g_client.cx, g_client.cy, current.debug);
		}
		return true;
	}

	void present()
	{
		if (g_frame && compose())
			g_frame->present_pixels(g_surface.pixels.data(), g_surface.width, g_surface.height);
	}

	void request_ui_tick()
	{
		if (g_ui_tick_pending.exchange(true)) return;
		pf::run_ui([]
		{
			g_ui_tick_pending.store(false);
			if (g_shutting_down.load()) return;

			if (g_sid_stopped_pending.exchange(false) &&
				g_sid_stopped_generation.load() == g_media_generation.load())
			{
				g_media.clear();
				update_title();
			}
			if (g_audio_failure_pending.exchange(false))
				update_title();
			if (g_frame) g_frame->invalidate();
		});
	}

	void audio_fallback_after_rejection()
	{
		if (!g_audio_ok.exchange(false)) return;
		g_audio_pacer.stop();
		g_audio_failure_pending.store(true);
		pf::debug_trace("lib8bit: audio stream rejected a PCM block; falling back to wall-clock pacing\n");
	}

	void emulation_worker()
	{
		auto next_frame = pf::platform_get_time();

		while (!g_worker_stop.load())
		{
			if (g_paused.load())
			{
				pf::platform_sleep(10);
				next_frame = pf::platform_get_time();
				continue;
			}

			const bool audio = g_audio_ok.load();
			if (audio)
			{
				if (!g_audio_pacer.can_step())
				{
					pf::platform_sleep(1);
					continue;
				}
			}
			else if (!g_headless)
			{
				constexpr double period = 1.0 / 50.0;
				const double now = pf::platform_get_time();
				if (now - next_frame < period)
				{
					pf::platform_sleep(1);
					continue;
				}
				next_frame += period;
				if (now - next_frame > period * 4) next_frame = now;
			}

			frame_snapshot& target = g_frames[g_frame_write];
			audio_pacer::submit_result audio_result = audio_pacer::submit_result::no_stream;
			bool sid_stopped = false;
			{
				std::lock_guard lock(g_machine_mutex);
				if (g_worker_stop.load()) break;

				apply_joystick();
				g_machine.exec(kCyclesPerFrame);
				if (audio)
				{
					audio_result = g_audio_pacer.submit(g_machine);
					if (audio_result == audio_pacer::submit_result::not_ready ||
						audio_result == audio_pacer::submit_result::no_stream)
					{
						// The pre-flight check and write are intentionally separate
						// public operations. If capacity changed between them,
						// consume this frame's SID span before switching clocks.
						g_machine.generate_audio(g_audio_scratch.data(),
							static_cast<int>(g_audio_scratch.size()));
					}
				}
				else
				{
					g_machine.generate_audio(g_audio_scratch.data(),
						static_cast<int>(g_audio_scratch.size()));
					audio_result = audio_pacer::submit_result::submitted;
				}

				if (audio && audio_result != audio_pacer::submit_result::submitted)
					audio_fallback_after_rejection();

				std::memcpy(target.pixels.data(), g_machine.framebuffer(),
					target.pixels.size() * sizeof(uint32_t));
				if (g_show_debug.load()) g_machine.capture_debug(target.debug);
				if (g_sid_playing.load() && !g_machine.sid_player_active())
				{
					g_sid_playing.store(false);
					g_sid_stopped_generation.store(g_media_generation.load());
					sid_stopped = true;
				}
			}

			{
				std::lock_guard lock(g_frame_mutex);
				std::swap(g_frame_write, g_frame_ready);
				g_frame_fresh = true;
			}
			g_emu_frames.fetch_add(1);
			if (sid_stopped) g_sid_stopped_pending.store(true);
			request_ui_tick();
			if (!audio && g_headless) pf::platform_sleep(1);
		}

		g_worker_active.store(false);
	}

	bool start_emulation_worker()
	{
		if (g_emu_executor) return true;

		g_worker_stop.store(false);
		g_worker_active.store(true);
		g_emu_executor = pf::create_serial_executor();
		if (!g_emu_executor || !g_emu_executor->post(emulation_worker))
		{
			pf::debug_trace("lib8bit: could not start the emulation worker\n");
			if (g_emu_executor) g_emu_executor->stop();
			g_emu_executor.reset();
			g_worker_active.store(false);
			return false;
		}
		return true;
	}

	void stop_emulation_worker()
	{
		g_worker_stop.store(true);
		if (g_emu_executor)
		{
			g_emu_executor->stop();
			g_emu_executor.reset();
		}
		g_worker_active.store(false);
	}

	class app_reactor final : public pf::frame_reactor
	{
	public:
		uint32_t handle_message(pf::window_frame_ptr window,
			const pf::message_type message, const pf::message_params& params) override
		{
			switch (message)
			{
			case pf::message_type::create:
			{
				const double scale = window->get_dpi_scale();
				g_dpi = std::max(96, static_cast<int>(scale * 96.0 + 0.5));
				g_panel_w = g_settings.panel_w96 > 0
					? pf::mul_div(g_settings.panel_w96, g_dpi, 96)
					: default_panel_width();
				g_show_debug.store(g_headless || g_settings.show_debug);
				window->accept_drop_files(true);
				g_minimum_client_size = {kFramebufferW + splitter_width(), kFramebufferH};
				window->set_minimum_client_size(g_minimum_client_size);

				if (g_settings.has_placement)
				{
					window->set_placement(g_settings.placement);
				}
				else
				{
					const int panel = g_show_debug.load()
						? default_panel_width() + splitter_width() : 0;
					const int width = static_cast<int>(kFramebufferW * 2 * scale) + panel +
						static_cast<int>(96 * scale);
					const int height = static_cast<int>(kFramebufferH * 2 * scale) +
						static_cast<int>(80 * scale);
					const auto screen = pf::platform_screen_size();
					const int left = std::max(0, (screen.cx - width) / 2);
					const int top = std::max(0, (screen.cy - height) / 2);
					window->move_window({left, top, left + width, top + height});
				}

				if (!g_start_path.empty())
				{
					const auto path = g_start_path;
					g_start_path = {};
					load_file(path);
				}
				update_title();
				return 0;
			}

			case pf::message_type::dpi_changed:
			{
				const int old_dpi = g_dpi;
				g_dpi = std::max(96, static_cast<int>(params.dpi_scale * 96.0 + 0.5));
				g_panel_w = pf::mul_div(g_panel_w, g_dpi, std::max(1, old_dpi));
				window->invalidate();
				return 0;
			}

			case pf::message_type::erase_background:
				return 1;

			case pf::message_type::kill_focus:
				release_all_keys();
				g_dragging = false;
				window->release_capture();
				return 0;

			case pf::message_type::capture_changed:
				g_dragging = false;
				return 0;

			case pf::message_type::end_session:
				if (params.session_ending) save_settings();
				return 0;

			case pf::message_type::drop_files:
				if (!params.dropped_paths.empty()) load_file(params.dropped_paths.front());
				return 0;

			case pf::message_type::close:
				g_shutting_down.store(true);
				stop_emulation_worker();
				if (!g_headless) save_settings();
				window->close();
				return 0;

			case pf::message_type::destroy:
				g_shutting_down.store(true);
				stop_emulation_worker();
				g_running.store(false);
				return 0;

			default:
				return 0;
			}
		}

		uint32_t handle_keyboard(pf::window_frame_ptr,
			const pf::keyboard_message_type message, const pf::keyboard_params& params) override
		{
			const bool pressed = message == pf::keyboard_message_type::key_down;
			if (message != pf::keyboard_message_type::key_down &&
				message != pf::keyboard_message_type::key_up)
				return 0;

			if (params.vk == pf::platform_key::Prior)
			{
				if (pressed && !params.repeat)
				{
					std::lock_guard lock(g_machine_mutex);
					g_machine.press_restore();
				}
				return 0;
			}

			if (g_arrow_mode.load() != arrow_mode::cursor)
			{
				uint8_t bit = 0;
				switch (params.vk)
				{
				case pf::platform_key::Up: bit = kJoyUp; break;
				case pf::platform_key::Down: bit = kJoyDown; break;
				case pf::platform_key::Left: bit = kJoyLeft; break;
				case pf::platform_key::Right: bit = kJoyRight; break;
				case pf::platform_key::Space: bit = kJoyFire; break;
				default: break;
				}

				if (bit)
				{
					if (pressed)
					{
						g_joystick.fetch_or(bit);
						if (!g_paused.load()) g_joystick_taps.fetch_or(bit);
					}
					else
					{
						g_joystick.fetch_and(static_cast<uint8_t>(~bit));
					}
					if (params.vk != pf::platform_key::Space) return 0;
				}
			}

			keypos positions[2];
			send_keys(positions, map_key(params.vk, positions), pressed);
			return 0;
		}

		uint32_t handle_mouse(pf::window_frame_ptr window,
			const pf::mouse_message_type message, const pf::mouse_params& params) override
		{
			switch (message)
			{
			case pf::mouse_message_type::set_cursor:
				if (g_dragging || (g_layout.splitter >= 0 &&
					params.point.x >= g_layout.splitter &&
					params.point.x < g_layout.splitter + splitter_width()))
				{
					window->set_cursor_shape(pf::cursor_shape::size_we);
					return 1;
				}
				window->set_cursor_shape(pf::cursor_shape::arrow);
				return 0;

			case pf::mouse_message_type::left_button_down:
				if (g_layout.splitter >= 0 && params.point.x >= g_layout.splitter &&
					params.point.x < g_layout.splitter + splitter_width())
				{
					g_dragging = true;
					window->set_capture();
				}
				return 0;

			case pf::mouse_message_type::mouse_move:
				if (g_dragging)
				{
					g_panel_w = g_client.cx - params.point.x - splitter_width();
					g_panel_w = panel_width(g_client.cx);
					window->invalidate();
				}
				return 0;

			case pf::mouse_message_type::left_button_up:
				if (g_dragging)
				{
					g_dragging = false;
					window->release_capture();
				}
				return 0;

			default:
				return 0;
			}
		}

		void handle_paint(pf::window_frame_ptr&, pf::draw_context& draw) override
		{
			if (compose())
				draw.draw_bitmap(0, 0, g_surface);
			else
				draw.fill_solid_rect(draw.clip_rect(), {});
		}

		void handle_size(pf::window_frame_ptr&, const pf::isize extent,
			pf::measure_context&) override
		{
			g_client = extent;
			if (g_frame) g_frame->invalidate();
		}
	};

	std::vector<pf::menu_command> build_menu()
	{
		using pf::key_binding;
		using pf::menu_command;

		menu_command file;
		file.text = "&File";
		file.children.emplace_back("&Open...", kCmdOpen, open_dialog, nullptr, nullptr,
			key_binding{'O', pf::key_mod::ctrl});
		file.children.emplace_back("&Pause", kCmdPause, toggle_pause, nullptr,
			[] { return g_paused.load(); }, key_binding{pf::platform_key::F9});
		file.children.emplace_back();
		file.children.emplace_back("&Reset (Keeps Cartridge)", kCmdReset, reset_machine,
			nullptr, nullptr, key_binding{'R', pf::key_mod::ctrl});
		file.children.emplace_back("&Eject Media", kCmdEject, eject_media,
			[] { return g_cart_inserted || g_disk_inserted; }, nullptr,
			key_binding{'E', pf::key_mod::ctrl});
		file.children.emplace_back();
		file.children.emplace_back("E&xit", kCmdExit, []
		{
			close_app();
		});

		menu_command input;
		input.text = "&Input";
		input.children.emplace_back("&Cursor Keys", kCmdArrowsCursor,
			[] { set_arrow_mode(arrow_mode::cursor); }, nullptr,
			[] { return g_arrow_mode.load() == arrow_mode::cursor; });
		input.children.emplace_back("Joystick &1", kCmdArrowsJoy1,
			[] { set_arrow_mode(arrow_mode::joystick1); }, nullptr,
			[] { return g_arrow_mode.load() == arrow_mode::joystick1; });
		input.children.emplace_back("Joystick &2", kCmdArrowsJoy2,
			[] { set_arrow_mode(arrow_mode::joystick2); }, nullptr,
			[] { return g_arrow_mode.load() == arrow_mode::joystick2; });

		menu_command view;
		view.text = "&View";
		view.children.emplace_back("&Debug Panel", kCmdDebug, []
		{
			g_show_debug.store(!g_show_debug.load());
			if (g_frame) g_frame->invalidate();
		}, nullptr, [] { return g_show_debug.load(); }, key_binding{pf::platform_key::F11});

		menu_command help;
		help.text = "&Help";
		help.children.emplace_back("&Keyboard...", kCmdKeyboard, []
		{
			if (g_frame)
				g_frame->message_box(
					"Emulated C64 keys\n\n"
					"Keys are mapped by position, so Shift types the C64's own shifted symbols.\n\n"
					"Esc\t\tRUN/STOP\n"
					"Page Up\t\tRESTORE (with Esc held: RUN/STOP+RESTORE)\n"
					"Tab\t\tCommodore key\n"
					"` (backtick)\tLeft arrow\n"
					"Home\t\tCLR/HOME\n"
					"Backspace, Delete\tINST/DEL\n"
					"' (quote)\t\t:\n"
					"[  ]\t\t@  *\n"
					"\\ (backslash)\tUp arrow\n"
					"Page Down\t=\n"
					"End\t\tpound\n"
					"F1 - F8\t\tC64 function keys\n"
					"Arrow keys\tCursor keys, or the Input menu's joystick\n"
					"Space\t\tSpace bar, and joystick fire in joystick mode\n\n"
					"Host shortcuts\n\n"
					"Ctrl+O\t\tOpen a file\n"
					"Ctrl+R\t\tReset (the cartridge stays inserted)\n"
					"Ctrl+E\t\tEject media\n"
					"F9\t\tPause / resume\n"
					"F11\t\tShow / hide the debug panel\n"
					"F12\t\tAbout",
					kProductName, pf::msg_box_style::ok);
		});
		help.children.emplace_back("&About...", kCmdAbout, []
		{
			if (g_frame)
				g_frame->message_box(
					"lib8bit\n\nA Commodore 64 emulator.\n\n"
					"Open or drop a .PRG, .CRT, .SID, .D64, .D71 or .D81 file, "
					"or pass one on the command line.\n"
					"A .SID tune opens in a player screen: 1-9 select a sub-tune "
					"and X stops playback.\n"
					"Use the Input menu to point the arrow keys at the C64 cursor "
					"keys or at joystick port 1 or 2.",
					kProductName, pf::msg_box_style::ok);
		}, nullptr, nullptr, key_binding{pf::platform_key::F12});

		return {std::move(file), std::move(input), std::move(view), std::move(help)};
	}

	int run_frame_loop()
	{
		start_audio();
		if (!start_emulation_worker())
		{
			report("Failed to start the emulation worker.");
			g_shutting_down.store(true);
			if (g_frame) g_frame->close();
			stop_audio();
			return 1;
		}
		update_title();

		while (g_running.load())
		{
			if (!pf::platform_events())
			{
				g_shutting_down.store(true);
				break;
			}
			pf::pump_ui_tasks(0);
			pf::platform_sleep(1);
		}
		stop_emulation_worker();
		stop_audio();
		return 0;
	}

	class recording_draw_context final : public pf::draw_context
	{
	public:
		pf::irect clip;
		const pf::bitmap* drawn = nullptr;

		pf::irect clip_rect() const override { return clip; }
		void fill_solid_rect(const pf::irect&, pf::color_t) override {}
		void fill_solid_rect(int, int, int, int, pf::color_t) override {}
		void draw_text(int, int, const pf::irect&, std::string_view, const pf::font&,
			pf::color_t, pf::color_t) override {}
		pf::isize measure_text(std::string_view, const pf::font&) const override { return {}; }
		void draw_text_h(int, int, std::string_view, pf::font_handle, pf::color_t) override {}
		pf::isize measure_text_h(std::string_view, pf::font_handle) const override { return {}; }
		void draw_lines(std::span<const pf::ipoint>, pf::color_t) override {}
		void draw_solid_line(pf::ipoint, pf::ipoint, pf::color_t, int) override {}
		void draw_ellipse(int, int, int, int, pf::color_t, int) override {}
		void fill_ellipse(int, int, int, int, pf::color_t) override {}
		void set_clip_rect(const pf::irect&) override {}
		void clear_clip_rect() override {}
		void draw_bitmap(int, int, const pf::bitmap& bitmap) override { drawn = &bitmap; }
		void draw_bitmap(const pf::irect&, const pf::bitmap& bitmap) override { drawn = &bitmap; }
	};

	class fake_audio_stream final : public pf::audio_stream
	{
	public:
		explicit fake_audio_stream(const int capacity) : _capacity(capacity) {}

		int queued_blocks() const override { return _queued; }
		bool can_write() const override { return _queued < _capacity; }

		bool write(const std::span<const int16_t> samples) override
		{
			if (!can_write() || samples.empty()) return false;
			if (_reject_next)
			{
				_reject_next = false;
				return false;
			}
			++_queued;
			_last_block_size = samples.size();
			return true;
		}

		void set_volume(float) override {}

		void stop() override
		{
			_queued = 0;
			_last_block_size = 0;
		}

		void drain_one()
		{
			if (_queued > 0) --_queued;
		}

		void reject_next_write() { _reject_next = true; }
		size_t last_block_size() const { return _last_block_size; }

	private:
		int _capacity;
		int _queued = 0;
		bool _reject_next = false;
		size_t _last_block_size = 0;
	};

	bool check_screen_key(const int row, const int col, const bool pressed)
	{
		std::lock_guard lock(g_machine_mutex);
		const uint8_t bit = static_cast<uint8_t>(1u << col);
		return pressed ? (g_machine._state->key_matrix[row] & bit) == 0
			: (g_machine._state->key_matrix[row] & bit) != 0;
	}

	bool wait_for_frame(const int64_t target)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (g_running.load() && g_emu_frames.load() < target &&
			std::chrono::steady_clock::now() < deadline)
		{
			if (!pf::platform_events()) return false;
			pf::pump_ui_tasks(0);
			present();
			pf::platform_sleep(1);
		}
		return g_emu_frames.load() >= target;
	}

	int run_headless()
	{
		pf::attach_console();
		int failures = 0;
		const auto check = [&](const bool ok, const std::string_view what)
		{
			pf::write_stdout(std::string(ok ? "PASS " : "FAIL ") + std::string(what) + "\n");
			if (!ok) ++failures;
		};

		if (!start_emulation_worker())
			check(false, "offscreen emulation worker starts");
		const int64_t initial_frames = g_emu_frames.load();
		check(wait_for_frame(initial_frames + 120),
			"background worker publishes PAL frames");
		for (int i = 0; i < 4 && g_running.load(); ++i)
		{
			if (!pf::platform_events()) break;
			pf::pump_ui_tasks(0);
			present();
			pf::platform_sleep(1);
		}

		check(g_client.cx > 0 && g_client.cy > 0, "offscreen window reports a client area");
		check(!g_settings_available, "headless test leaves user settings untouched");
		check(g_minimum_client_size.cx == kFramebufferW + splitter_width() &&
			g_minimum_client_size.cy == kFramebufferH,
			"window minimum preserves framebuffer and splitter");
		check(g_layout.scale >= 1 && g_layout.scale == static_cast<int>(g_layout.scale),
			"picture layout uses an integer scale");
		check(g_layout.x >= 0 && g_layout.y >= 0, "picture is letterboxed inside the client");
		check(g_surface.width == g_client.cx && g_surface.height == g_client.cy &&
			g_surface.pixels.size() == static_cast<size_t>(g_client.cx) * g_client.cy,
			"surface covers the client area");
		check(std::any_of(g_surface.pixels.begin(), g_surface.pixels.end(),
			[](const uint32_t pixel) { return pixel != 0; }),
			"framebuffer and debug panel reach the surface");

		recording_draw_context draw;
		draw.clip = {0, 0, g_client.cx, g_client.cy};
		pf::window_frame_ptr frame = g_frame;
		g_reactor->handle_paint(frame, draw);
		check(draw.drawn && draw.drawn->width == g_client.cx &&
			draw.drawn->height == g_client.cy, "repaint uses draw_context");

		g_end_session_saved = false;
		pf::message_params ending;
		ending.session_ending = true;
		g_reactor->handle_message(frame, pf::message_type::end_session, ending);
		check(g_end_session_saved, "end-session callback saves window settings");

		set_arrow_mode(arrow_mode::cursor);
		pf::keyboard_params key_a;
		key_a.vk = 'A';
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_down, key_a);
		const int64_t key_frame = g_emu_frames.load();
		wait_for_frame(key_frame + 1);
		check(check_screen_key(1, 2, true), "keyboard reactor queues a C64 matrix press");
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_up, key_a);
		wait_for_frame(g_emu_frames.load() + 1);
		check(check_screen_key(1, 2, false), "keyboard reactor queues a C64 matrix release");

		pf::keyboard_params restore;
		restore.vk = pf::platform_key::Prior;
		g_paused.store(true);
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_down, restore);
		{
			std::lock_guard lock(g_machine_mutex);
			g_machine.flush_keys();
			check(g_machine._state->nmi_pending, "Page Up pulses RESTORE through the reactor");
		}
		g_paused.store(false);

		set_arrow_mode(arrow_mode::joystick1);
		pf::keyboard_params up;
		up.vk = pf::platform_key::Up;
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_down, up);
		wait_for_frame(g_emu_frames.load() + 1);
		{
			std::lock_guard lock(g_machine_mutex);
			check((g_machine._state->joystick1 & kJoyUp) == 0,
				"joystick mode preserves a short directional tap");
		}
		g_reactor->handle_keyboard(frame, pf::keyboard_message_type::key_up, up);
		wait_for_frame(g_emu_frames.load() + 1);
		{
			std::lock_guard lock(g_machine_mutex);
			check((g_machine._state->joystick1 & kJoyUp) != 0,
				"joystick release reaches the emulated port");
		}
		set_arrow_mode(arrow_mode::cursor);

		auto fake = std::make_shared<fake_audio_stream>(1);
		audio_pacer adapter;
		adapter.set_stream(fake);
		machine audio_machine;
		audio_machine.set_audio_enabled(true);
		audio_machine.exec(kCyclesPerFrame);
		check(adapter.can_step(), "audio adapter reports room in the stream");
		check(adapter.submit(audio_machine) == audio_pacer::submit_result::submitted &&
			fake->last_block_size() > 0,
			"audio adapter writes one generated PCM block");
		const int64_t audio_cycle = audio_machine._state->sid.audio_cycle;
		check(!adapter.can_step() &&
			adapter.submit(audio_machine) == audio_pacer::submit_result::not_ready &&
			audio_machine._state->sid.audio_cycle == audio_cycle,
			"full audio ring does not advance the SID clock");
		fake->drain_one();
		check(adapter.can_step(), "audio adapter resumes after a block drains");
		audio_machine.exec(kCyclesPerFrame);
		const int64_t rejected_cycle = audio_machine._state->sid.audio_cycle;
		fake->reject_next_write();
		check(adapter.submit(audio_machine) == audio_pacer::submit_result::rejected &&
			audio_machine._state->sid.audio_cycle > rejected_cycle,
			"audio adapter reports a write refused after can_write");

		if (g_layout.splitter >= 0)
		{
			const int old_panel = g_panel_w;
			pf::mouse_params down;
			down.point = {g_layout.splitter + 1, 4};
			g_reactor->handle_mouse(frame, pf::mouse_message_type::left_button_down, down);
			pf::mouse_params move;
			move.point = {std::max(0, g_layout.splitter - 16), 4};
			g_reactor->handle_mouse(frame, pf::mouse_message_type::mouse_move, move);
			g_reactor->handle_mouse(frame, pf::mouse_message_type::left_button_up, move);
			check(g_panel_w != old_panel, "debug splitter resizes through platform mouse input");

			const int captured_panel = g_panel_w;
			g_reactor->handle_mouse(frame, pf::mouse_message_type::left_button_down, down);
			g_reactor->handle_message(frame, pf::message_type::capture_changed, {});
			move.point.x = std::max(0, g_layout.splitter - 32);
			g_reactor->handle_mouse(frame, pf::mouse_message_type::mouse_move, move);
			check(g_panel_w == captured_panel, "capture loss clears splitter dragging");
		}

		g_reactor->handle_message(frame, pf::message_type::close, {});
		check(!g_running.load(), "close message ends the offscreen reactor loop");
		if (g_running.load()) g_frame->close();

		pf::write_stdout("lib8bit app headless: " +
			std::string(failures == 0 ? "PASS" : "FAIL") + "\n");
		return failures == 0 ? 0 : 1;
	}
}

app_init_result app_init(const pf::window_frame_ptr& main_frame,
	std::span<const std::string_view> params)
{
	pf::config_set_app_name("lib8bit");

	for (const auto param : params)
	{
		if (pf::icmp(param, "-test") == 0 || pf::icmp(param, "/test") == 0)
			g_headless = true;
		else if (g_start_path.empty())
			g_start_path = pf::file_path{pf::unquote(param)};
	}
	if (!g_headless)
	{
		configure_settings();
		load_settings();
	}

	g_frame = main_frame;
	g_reactor = std::make_shared<app_reactor>();
	main_frame->set_reactor(g_reactor);
	main_frame->set_menu(build_menu());
	main_frame->set_text(kProductName);

	g_machine.set_render_enabled(true);
	g_machine.exec(3'000'000);
	g_machine.set_audio_enabled(true);
	g_running.store(true);
	g_worker_stop.store(false);
	g_shutting_down.store(false);

	app_init_result result;
	result.offscreen_gui = g_headless;
	result.main_loop = g_headless ? run_headless : run_frame_loop;
	return result;
}

void app_idle()
{
}

void app_destroy()
{
	g_shutting_down.store(true);
	stop_emulation_worker();
	stop_audio();
	{
		std::lock_guard lock(g_machine_mutex);
		g_machine.set_audio_enabled(false);
	}
	if (!g_headless && g_settings_available) pf::config_flush();
	g_reactor.reset();
	g_frame.reset();
}
