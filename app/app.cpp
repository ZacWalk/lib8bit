// lib8bit by Zac Walker
//
// lib8bit: a self-contained Win32 front-end for the C64 emulator.
//
// Threading model (audio-clocked emulation):
//   * An emulation thread runs the C64 one PAL frame at a time, renders the
//     VIC-II picture plus a debug snapshot into a triple-buffered frame, and
//     pushes the frame's SID samples onto a bounded, synchronised queue.
//   * An audio thread drains that queue into waveOut buffers. The sound card
//     consumes at a fixed 44100 Hz, so the queue's back-pressure paces the
//     emulation thread at exactly real time - gapless audio with no timer.
//     The audio thread never blocks on the queue: an emulation clocked by a
//     device that has stopped consuming can only run slow, and it can never
//     make the lost time back.
//   * The UI (main) thread owns the window. It takes the newest published frame
//     under a lock held just long enough to swap two indices, so a slow repaint
//     can never stall emulation.
//
// Rendering: the whole client area is composed by the CPU into one 32-bit DIB
// and presented with a single BitBlt - nearest-neighbour integer scale for the
// picture, and a debug panel drawn with the C64's own 8x8 character ROM. No
// GDI fonts, brushes or stretch blits are involved.

#include "machine.h"
#include "debug.h"
#include "disk.h"
#include "Resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

namespace
{
	constexpr wchar_t kProductName[] = L"lib8bit";

	constexpr int kFramebufferW = 384;
	constexpr int kFramebufferH = 272;
	constexpr int kSampleRate = 44100;

	// One PAL frame of CPU time per emulation step: 63 cycles x 312 raster lines,
	// the same frame the VIC-II renderer uses. Any other value would move the
	// video snapshot point a little further into the frame every step.
	constexpr int kCyclesPerFrame = 63 * 312;

	// Audio buffering. The queue is the elastic band between the two threads; the
	// waveOut blocks are what the sound card actually plays.
	constexpr size_t kQueueCapacity = kSampleRate / 25; // ~40 ms of slack

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

	// Posted by the emulation thread when the SID player's X key has stopped
	// playback, so the title bar stops naming a tune that is no longer playing.
	constexpr UINT WM_SID_STOPPED = WM_APP + 1;

	// GetOpenFileName filter: NUL-separated "label\0pattern\0" pairs ending in a
	// double NUL. Declared as an array so the embedded NULs are preserved (the
	// compiler appends the final terminating NUL).
	const wchar_t kFileFilter[] =
		L"C64 Files (*.prg;*.crt;*.sid;*.d64;*.d71;*.d81)\0*.prg;*.crt;*.sid;*.d64;*.d71;*.d81\0"
		L"Programs (*.prg)\0*.prg\0"
		L"Cartridges (*.crt)\0*.crt\0"
		L"SID Tunes (*.sid;*.psid)\0*.sid;*.psid\0"
		L"Disk Images (*.d64;*.d71;*.d81)\0*.d64;*.d71;*.d81\0"
		L"All Files (*.*)\0*.*\0";

	// What the arrow keys drive (read on the emulation thread, set on the UI
	// thread — hence atomic).
	enum class arrow_mode { cursor, joystick1, joystick2 };

	// Joystick state, built on the UI thread from WM_KEYDOWN / WM_KEYUP so that
	// keys pressed while the app is in the background are never seen.
	constexpr uint8_t kJoyUp = 0x01;
	constexpr uint8_t kJoyDown = 0x02;
	constexpr uint8_t kJoyLeft = 0x04;
	constexpr uint8_t kJoyRight = 0x08;
	constexpr uint8_t kJoyFire = 0x10;

	machine g_machine;
	std::mutex g_machine_mutex;                     // guards every g_machine access
	std::atomic<arrow_mode> g_arrow_mode{arrow_mode::cursor};
	std::atomic<uint8_t> g_joystick{0};
	std::atomic<bool> g_paused{false};
	std::atomic<bool> g_show_debug{false};
	std::atomic<bool> g_sid_playing{false};
	// Bumped whenever media changes, so a stop posted for the previous medium is
	// ignored once something else has been loaded.
	std::atomic<uint32_t> g_media_generation{0};
	std::atomic<int64_t> g_emu_frames{0}; // emulated frames completed, for the speed readout

	// ---- Emulation -> UI frame handoff --------------------------------------
	// Triple buffered so the two threads never wait on each other: the emulation
	// thread fills `write` and publishes it as `ready`; the UI thread claims
	// `ready` as `read` when it paints. The lock is held only across an index
	// swap, which matters because the emulation clock comes from the sound card
	// and any stall is time it can never make back.
	struct frame
	{
		// Parentheses, not braces: braces would pick the initializer_list
		// constructor and build a one-element vector.
		std::vector<uint32_t> pixels = std::vector<uint32_t>(
			static_cast<size_t>(kFramebufferW) * kFramebufferH);
		debug_state debug;
	};

	std::mutex g_frame_mutex;
	frame g_frames[3];
	int g_frame_write = 0, g_frame_ready = 1, g_frame_read = 2;
	bool g_frame_fresh = false;

	// Client-sized back buffer, recreated only on WM_SIZE (UI thread only). The
	// whole window is composed into its pixels by hand.
	HDC g_back_dc = nullptr;
	HBITMAP g_back_bmp = nullptr;
	HGDIOBJ g_back_old = nullptr;
	uint32_t* g_back_bits = nullptr;
	int g_back_w = 0;
	int g_back_h = 0;

	// ---- Debug panel layout (UI thread only) --------------------------------
	int g_dpi = 96;
	int g_panel_w = 0;    // debug panel width in physical pixels
	bool g_dragging = false;

	// ---- Persisted UI state -------------------------------------------------
	// Read from the ini before the window exists and written back on close.
	struct ui_settings
	{
		RECT window{};             // restored (non-maximized) frame rectangle
		bool has_window = false;
		bool maximized = false;
		int panel_w96 = 0;         // panel width in 96-dpi units, 0 = use the default
		bool show_debug = false;
	};

	ui_settings g_settings;

	// The window's restored rectangle in physical screen pixels, tracked as it
	// moves. WINDOWPLACEMENT would be the obvious source, but its rectangle does
	// not round-trip through Set/GetWindowPlacement under per-monitor DPI.
	RECT g_normal_rect{};

	void track_normal_rect(HWND hwnd)
	{
		if (IsZoomed(hwnd) || IsIconic(hwnd)) return;
		GetWindowRect(hwnd, &g_normal_rect);
	}

	// %APPDATA%\lib8bit\lib8bit.ini - the install directory may not be writable.
	// Empty if the folder could not be resolved, in which case settings are skipped
	// rather than falling back to win.ini.
	const wchar_t* settings_path()
	{
		static const std::wstring path = []
		{
			std::wstring result;
			PWSTR appdata = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)))
			{
				result = appdata;
				CoTaskMemFree(appdata);
				result += L"\\lib8bit";
				CreateDirectoryW(result.c_str(), nullptr);
				result += L"\\lib8bit.ini";
			}
			return result;
		}();
		return path.empty() ? nullptr : path.c_str();
	}

	// Read as text, not GetPrivateProfileInt: window coordinates can be negative
	// on a monitor left of or above the primary one.
	int profile_int(const wchar_t* section, const wchar_t* key, int fallback)
	{
		const wchar_t* const path = settings_path();
		wchar_t text[32]{};
		if (!path || !GetPrivateProfileStringW(section, key, L"", text, ARRAYSIZE(text), path) || !text[0])
			return fallback;
		return static_cast<int>(wcstol(text, nullptr, 10));
	}

	void profile_write_int(const wchar_t* section, const wchar_t* key, int value)
	{
		const wchar_t* const path = settings_path();
		if (!path) return;
		wchar_t text[32];
		swprintf_s(text, L"%d", value);
		WritePrivateProfileStringW(section, key, text, path);
	}

	void load_settings()
	{
		const int x = profile_int(L"window", L"x", INT_MIN);
		const int y = profile_int(L"window", L"y", INT_MIN);
		const int w = profile_int(L"window", L"width", 0);
		const int h = profile_int(L"window", L"height", 0);
		if (x != INT_MIN && y != INT_MIN && w > 0 && h > 0)
		{
			const RECT r{x, y, x + w, y + h};
			// Drop the position if that monitor is gone, rather than restoring offscreen.
			if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL))
			{
				g_settings.window = r;
				g_settings.has_window = true;
			}
		}

		g_settings.maximized = profile_int(L"window", L"maximized", 0) != 0;
		g_settings.panel_w96 = profile_int(L"panel", L"width", 0);
		g_settings.show_debug = profile_int(L"panel", L"visible", 0) != 0;
	}

	void save_settings(HWND hwnd)
	{
		const RECT& r = g_normal_rect;
		if (r.right > r.left && r.bottom > r.top)
		{
			profile_write_int(L"window", L"x", r.left);
			profile_write_int(L"window", L"y", r.top);
			profile_write_int(L"window", L"width", r.right - r.left);
			profile_write_int(L"window", L"height", r.bottom - r.top);
		}

		profile_write_int(L"window", L"maximized", IsZoomed(hwnd) ? 1 : 0);
		profile_write_int(L"panel", L"width", MulDiv(g_panel_w, 96, g_dpi));
		profile_write_int(L"panel", L"visible", g_show_debug.load() ? 1 : 0);
	}

	std::atomic<bool> g_running{false};
	std::thread g_emu_thread;
	std::atomic<HWND> g_hwnd{nullptr};

	// ---- Synchronised sample queue (bounded ring buffer) -------------------
	// push() blocks while full, and that back-pressure is what paces emulation;
	// pop() never blocks (see audio_engine::fill). Both return early once stop()
	// has been called so the threads can unwind.
	class sample_queue
	{
	public:
		explicit sample_queue(size_t capacity) : _buf(capacity), _cap(capacity) {}

		void push(const int16_t* data, size_t n)
		{
			std::unique_lock<std::mutex> lk(_m);
			size_t off = 0;
			while (off < n)
			{
				_not_full.wait(lk, [&] { return _count < _cap || _stopping; });
				if (_stopping) return;
				while (off < n && _count < _cap)
				{
					_buf[_tail] = data[off++];
					_tail = (_tail + 1) % _cap;
					++_count;
				}
			}
		}

		// Takes whatever is queued, up to n, without waiting. The audio thread must
		// never block here: while it waits, buffers that have finished playing are
		// not resubmitted, the device runs dry, and every microsecond it spends idle
		// is emulated time the audio-clocked emulation thread can never make back.
		size_t pop(int16_t* out, size_t n)
		{
			std::lock_guard<std::mutex> lk(_m);
			size_t got = 0;
			while (got < n && _count > 0)
			{
				out[got++] = _buf[_head];
				_head = (_head + 1) % _cap;
				--_count;
			}
			if (got) _not_full.notify_one();
			return got;
		}

		void stop()
		{
			{
				std::lock_guard<std::mutex> lk(_m);
				_stopping = true;
			}
			_not_full.notify_all();
		}

	private:
		std::vector<int16_t> _buf;
		size_t _cap;
		size_t _head = 0, _tail = 0, _count = 0;
		bool _stopping = false;
		std::mutex _m;
		std::condition_variable _not_full;
	};

	sample_queue g_queue(kQueueCapacity);

	// ---- Audio engine: pulls from the queue, feeds waveOut ------------------
	class audio_engine
	{
	public:
		bool start(sample_queue* queue)
		{
			_queue = queue;
			_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!_event) return false;

			WAVEFORMATEX wfx{};
			wfx.wFormatTag = WAVE_FORMAT_PCM;
			wfx.nChannels = 1;
			wfx.nSamplesPerSec = kSampleRate;
			wfx.wBitsPerSample = 16;
			wfx.nBlockAlign = 2;
			wfx.nAvgBytesPerSec = kSampleRate * 2;
			if (waveOutOpen(&_hwo, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(_event), 0,
				CALLBACK_EVENT) != MMSYSERR_NOERROR)
			{
				CloseHandle(_event);
				_event = nullptr;
				return false;
			}

			for (auto& b : _blocks) b.resize(kChunk);
			// Headers are prepared once and re-submitted; preparing and unpreparing
			// per block would page-lock the same memory ~170 times a second.
			for (int i = 0; i < kBuffers; ++i)
			{
				_headers[i] = {};
				_headers[i].lpData = reinterpret_cast<LPSTR>(_blocks[i].data());
				_headers[i].dwBufferLength = kChunk * sizeof(int16_t);
				if (waveOutPrepareHeader(_hwo, &_headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
				{
					waveOutClose(_hwo);
					_hwo = nullptr;
					CloseHandle(_event);
					_event = nullptr;
					return false;
				}
			}

			_running = true;
			_thread = std::thread([this] { run(); });
			return true;
		}

		void stop()
		{
			_running = false;
			if (_event) SetEvent(_event);
			if (_thread.joinable()) _thread.join();
			if (_hwo)
			{
				waveOutReset(_hwo);
				for (auto& hdr : _headers)
					if (hdr.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(_hwo, &hdr, sizeof(hdr));
				waveOutClose(_hwo);
				_hwo = nullptr;
			}
			if (_event)
			{
				CloseHandle(_event);
				_event = nullptr;
			}
		}

	private:
		// Submits block i. Whatever the queue has is used; the rest of the block
		// holds the last sample, because a step to silence and back is an audible
		// click and stopping to wait would be worse still.
		void fill(int i)
		{
			const size_t n = _queue->pop(_blocks[i].data(), kChunk);
			if (n) _last = _blocks[i][n - 1];
			std::fill(_blocks[i].begin() + n, _blocks[i].end(), _last);

			WAVEHDR& hdr = _headers[i];
			hdr.dwFlags &= ~WHDR_DONE;
			if (waveOutWrite(_hwo, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) _queued[i] = true;
		}

		void run()
		{
			// The device is the emulator's clock; keep this thread off the runnable
			// queue behind ordinary work.
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

			for (int i = 0; i < kBuffers && _running; ++i) fill(i); // prime

			while (_running)
			{
				// One block plays in ~5.8 ms, so a missed event costs at most a poll.
				WaitForSingleObject(_event, 5);
				for (int i = 0; i < kBuffers; ++i)
				{
					if (!_queued[i])
					{
						if (_running) fill(i);
						continue;
					}
					// The driver updates dwFlags from outside the C++ memory model.
					const volatile DWORD* const flags = &_headers[i].dwFlags;
					if (*flags & WHDR_DONE)
					{
						_queued[i] = false;
						if (_running) fill(i);
					}
				}
			}
		}

		static constexpr int kChunk = 256;   // samples per waveOut buffer (~5.8 ms)
		static constexpr int kBuffers = 6;   // ~35 ms in the sound card

		HWAVEOUT _hwo = nullptr;
		HANDLE _event = nullptr;
		WAVEHDR _headers[kBuffers] = {};
		bool _queued[kBuffers] = {};
		int16_t _last = 0;
		std::array<std::vector<int16_t>, kBuffers> _blocks;
		sample_queue* _queue = nullptr;
		std::thread _thread;
		std::atomic<bool> _running{false};
	};

	audio_engine g_audio;
	bool g_audio_ok = false;

	// ---- Machine state shown in the UI (UI thread only) ---------------------
	std::wstring g_media;         // what is loaded, for the title bar
	bool g_cart_inserted = false; // whether a cartridge is plugged in
	bool g_disk_inserted = false; // whether a disk image is in the drive

	void refresh_menu(HWND hwnd)
	{
		const HMENU menu = GetMenu(hwnd);
		if (!menu) return;

		const arrow_mode mode = g_arrow_mode.load();
		const int checked = mode == arrow_mode::cursor ? kCmdArrowsCursor
			: mode == arrow_mode::joystick1 ? kCmdArrowsJoy1 : kCmdArrowsJoy2;
		CheckMenuRadioItem(menu, kCmdArrowsCursor, kCmdArrowsJoy2, checked, MF_BYCOMMAND);
		CheckMenuItem(menu, kCmdPause, MF_BYCOMMAND | (g_paused.load() ? MF_CHECKED : MF_UNCHECKED));
		CheckMenuItem(menu, kCmdDebug, MF_BYCOMMAND | (g_show_debug.load() ? MF_CHECKED : MF_UNCHECKED));
		EnableMenuItem(menu, kCmdEject, MF_BYCOMMAND | (g_cart_inserted || g_disk_inserted ? MF_ENABLED : MF_GRAYED));
	}

	// The title bar is the app's status display: media, input mode, pause state
	// and whether the sound device came up at all.
	void update_title()
	{
		const HWND hwnd = g_hwnd.load();
		if (!hwnd) return;

		std::wstring title = kProductName;
		title += L" - ";
		title += g_media.empty() ? L"no media" : g_media;

		switch (g_arrow_mode.load())
		{
		case arrow_mode::joystick1: title += L" [Joystick 1]"; break;
		case arrow_mode::joystick2: title += L" [Joystick 2]"; break;
		default: title += L" [Keyboard]"; break;
		}

		if (g_paused.load()) title += L" - Paused";
		if (!g_audio_ok) title += L" (no audio)";
		SetWindowTextW(hwnd, title.c_str());
	}

	// ---- File loading (UI thread) -------------------------------------------
	// Comfortably larger than any C64 medium (a .d81 is 800 KB), and small enough
	// that pointing the app at a huge file fails cleanly instead of allocating it.
	constexpr std::streamoff kMaxFileSize = 16 * 1024 * 1024;

	std::vector<uint8_t> read_file(const wchar_t* path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream) return {};
		const auto size = stream.tellg();
		if (size <= 0 || size > kMaxFileSize) return {};
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), size);
		if (stream.gcount() != size) return {};
		return bytes;
	}

	bool has_signature(const std::vector<uint8_t>& bytes, const char* sig)
	{
		const size_t len = std::strlen(sig);
		return bytes.size() >= len && std::memcmp(bytes.data(), sig, len) == 0;
	}

	bool is_crt(const std::vector<uint8_t>& bytes) { return has_signature(bytes, "C64 CARTRIDGE   "); }
	bool is_sid(const std::vector<uint8_t>& bytes)
	{
		return has_signature(bytes, "PSID") || has_signature(bytes, "RSID");
	}

	std::wstring file_name(const wchar_t* path)
	{
		const wchar_t* name = path;
		for (const wchar_t* p = path; *p; ++p)
			if (*p == L'\\' || *p == L'/' || *p == L':') name = p + 1;
		return name;
	}

	void load_file(const wchar_t* path, HWND hwnd)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			MessageBoxW(hwnd, L"Failed to read file.", kProductName, MB_OK | MB_ICONWARNING);
			return;
		}

		// Decide the outcome under the lock and report it after releasing it:
		// MessageBoxW takes focus, which sends WM_KILLFOCUS to this same thread,
		// and that handler takes the same non-recursive mutex.
		const wchar_t* error = nullptr;
		bool loaded = false;
		bool cart = g_cart_inserted;
		bool disk = g_disk_inserted;
		{
			std::lock_guard<std::mutex> lk(g_machine_mutex);
			g_media_generation.fetch_add(1);
			g_sid_playing.store(false);
			if (is_sid(bytes))
			{
				loaded = g_machine.load_sid(bytes.data(), bytes.size());
				g_sid_playing.store(loaded);
				if (!loaded) error = L"Not a valid SID file.";
			}
			else if (is_crt(bytes))
			{
				loaded = g_machine.load_crt(bytes.data(), bytes.size());
				cart = loaded;
				if (!loaded) error = L"Not a valid CRT file.";
			}
			else if (g_machine.insert_disk(bytes.data(), bytes.size()))
			{
				// The image stays in the drive, so the kernal LOAD routine is served
				// from it and a program can keep loading files while it runs. The
				// first program is started here, as LOAD"*",8 followed by RUN would.
				disk = true;
				loaded = true; // the disk is in the drive even if nothing autostarts
				g_machine._state->reset();
				g_machine.exec(3'000'000);

				std::vector<uint8_t> prg;
				if (!read_disk_file(bytes.data(), bytes.size(), "*", prg))
					error = L"No program found on this disk. It is still in the drive.";
				else if (!g_machine.load_prg(prg.data(), prg.size()))
					error = L"The program on this disk could not be started.";
			}
			else
			{
				// Treat as a .PRG: reset, run the kernal to READY, then inject + RUN.
				g_machine._state->reset();
				g_machine.exec(3'000'000);
				loaded = g_machine.load_prg(bytes.data(), bytes.size());
				if (!loaded) error = L"Not a valid PRG file.";
			}

			// reset() zeroes the SID audio clock; re-sync it to "now" so the next
			// frame renders one frame of samples rather than a huge backlog.
			g_machine.set_audio_enabled(true);
		}

		if (loaded)
		{
			g_media = file_name(path);
			g_cart_inserted = cart;
			g_disk_inserted = disk;
			refresh_menu(hwnd);
			update_title();
		}

		if (error) MessageBoxW(hwnd, error, kProductName, MB_OK | MB_ICONWARNING);
	}

	void set_arrow_mode(HWND hwnd, arrow_mode mode)
	{
		g_arrow_mode.store(mode);
		if (mode == arrow_mode::cursor) g_joystick.store(0);
		refresh_menu(hwnd);
		update_title();
	}

	// Drive the selected joystick from the bitmask the UI thread maintains. Called
	// on the emulation thread while it holds the machine lock.
	void apply_joystick()
	{
		const arrow_mode mode = g_arrow_mode.load();
		if (mode == arrow_mode::cursor)
		{
			g_machine.set_joystick(1, false, false, false, false, false);
			g_machine.set_joystick(2, false, false, false, false, false);
			return;
		}

		const uint8_t bits = g_joystick.load();
		const int port = mode == arrow_mode::joystick1 ? 1 : 2;
		const int other = port == 1 ? 2 : 1;
		g_machine.set_joystick(port, (bits & kJoyUp) != 0, (bits & kJoyDown) != 0,
			(bits & kJoyLeft) != 0, (bits & kJoyRight) != 0, (bits & kJoyFire) != 0);
		g_machine.set_joystick(other, false, false, false, false, false);
	}

	// ---- Emulation thread ---------------------------------------------------
	void emu_thread_main()
	{
		std::vector<int16_t> samples(4096);
		auto next_frame = std::chrono::steady_clock::now();

		while (g_running.load())
		{
			if (g_paused.load())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				next_frame = std::chrono::steady_clock::now();
				continue;
			}

			int count = 0;
			frame& target = g_frames[g_frame_write];
			{
				std::lock_guard<std::mutex> lk(g_machine_mutex);
				apply_joystick();
				g_machine.exec(kCyclesPerFrame);
				count = g_machine.generate_audio(samples.data(), static_cast<int>(samples.size()));
				std::memcpy(target.pixels.data(), g_machine.framebuffer(),
					target.pixels.size() * sizeof(uint32_t));
				if (g_show_debug.load()) g_machine.capture_debug(target.debug);
				if (!g_machine.sid_player_active() && g_sid_playing.exchange(false))
				{
					const HWND hwnd = g_hwnd.load();
					if (hwnd) PostMessageW(hwnd, WM_SID_STOPPED, g_media_generation.load(), 0);
				}
			}

			{
				std::lock_guard<std::mutex> lk(g_frame_mutex);
				std::swap(g_frame_write, g_frame_ready);
				g_frame_fresh = true;
			}
			g_emu_frames.fetch_add(1, std::memory_order_relaxed);

			if (g_audio_ok)
			{
				// Back-pressure from the audio queue paces us to real time.
				g_queue.push(samples.data(), static_cast<size_t>(count));
			}
			else
			{
				// No audio device: pace the frame ourselves. After a long stall the
				// deadline is far in the past, so drop the backlog instead of running
				// flat out to catch up.
				const auto now = std::chrono::steady_clock::now();
				if (now > next_frame + std::chrono::milliseconds(250)) next_frame = now;
				next_frame += std::chrono::microseconds(1'000'000 / 50);
				std::this_thread::sleep_until(next_frame);
			}

			const HWND hwnd = g_hwnd.load();
			if (hwnd && !IsIconic(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
		}
	}

	// ---- Composition (UI thread) --------------------------------------------
	HDC create_dib(HDC ref, int w, int h, HBITMAP& bmp, HGDIOBJ& old, uint32_t** bits)
	{
		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = w;
		bmi.bmiHeader.biHeight = -h; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		void* pixels = nullptr;
		bmp = static_cast<HBITMAP>(CreateDIBSection(ref, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0));
		if (!bmp) return nullptr;

		const HDC dc = CreateCompatibleDC(ref);
		if (!dc)
		{
			DeleteObject(bmp);
			bmp = nullptr;
			return nullptr;
		}

		old = SelectObject(dc, bmp);
		if (bits) *bits = static_cast<uint32_t*>(pixels);
		return dc;
	}

	void destroy_dib(HDC& dc, HBITMAP& bmp, HGDIOBJ& old)
	{
		if (dc)
		{
			SelectObject(dc, old);
			DeleteDC(dc);
			dc = nullptr;
		}
		if (bmp)
		{
			DeleteObject(bmp);
			bmp = nullptr;
		}
		old = nullptr;
	}

	void resize_back_buffer(HWND hwnd, int w, int h)
	{
		if (g_back_dc && w == g_back_w && h == g_back_h) return;

		destroy_dib(g_back_dc, g_back_bmp, g_back_old);
		g_back_bits = nullptr;
		g_back_w = 0;
		g_back_h = 0;
		if (w <= 0 || h <= 0) return;

		const HDC hdc = GetDC(hwnd);
		g_back_dc = create_dib(hdc, w, h, g_back_bmp, g_back_old, &g_back_bits);
		ReleaseDC(hwnd, hdc);
		if (g_back_dc)
		{
			g_back_w = w;
			g_back_h = h;
		}
	}

	void fill_rect(int x, int y, int w, int h, uint32_t colour)
	{
		if (w <= 0 || h <= 0) return;
		for (int row = 0; row < h; row++)
		{
			uint32_t* const p = g_back_bits + static_cast<size_t>(y + row) * g_back_w + x;
			std::fill(p, p + w, colour);
		}
	}

	// Nearest-neighbour integer upscale of the C64 picture. One source row is
	// expanded once and the duplicate rows are memcpy'd, which is what makes a
	// full-window CPU composite cheaper than a GDI stretch blit.
	void blit_picture(const uint32_t* src, int dx, int dy, int scale)
	{
		const size_t row_bytes = static_cast<size_t>(kFramebufferW) * scale * sizeof(uint32_t);
		for (int y = 0; y < kFramebufferH; y++)
		{
			uint32_t* const row = g_back_bits + static_cast<size_t>(dy + y * scale) * g_back_w + dx;
			for (int x = 0; x < kFramebufferW; x++)
				std::fill_n(row + static_cast<size_t>(x) * scale, scale, src[x]);
			for (int r = 1; r < scale; r++)
				std::memcpy(row + static_cast<size_t>(r) * g_back_w, row, row_bytes);
			src += kFramebufferW;
		}
	}

	// ---- Debug panel text ----------------------------------------------------
	// Drawn with the machine's own character ROM (charset 0, upper case and
	// graphics) so the panel needs no GDI font, no device context state and no
	// per-frame allocation - just the same 8x8 bitmaps the emulated screen uses.
	uint8_t screen_code(char c)
	{
		const auto u = static_cast<uint8_t>(c);
		if (u >= 0x60 && u < 0x80) return static_cast<uint8_t>(u - 0x60); // lower case
		if (u >= 0x40 && u < 0x60) return static_cast<uint8_t>(u - 0x40);
		if (u >= 0x20 && u < 0x40) return u;
		return 0x20;
	}

	class panel_writer
	{
	public:
		// In measuring mode nothing is drawn and nothing clips, so the caller can
		// find the block's size before deciding where to put it.
		panel_writer(int left, int top, int right, int bottom, int scale, bool measuring = false)
			: _left(left), _top(top), _right(right), _bottom(bottom), _scale(scale),
			  _y(top), _measuring(measuring) {}

		int cell() const { return 8 * _scale; }
		int height() const { return _y - _top; }
		int width() const { return _width; }

		void blank() { _y += cell(); }

		void line(uint32_t colour, const char* format, ...)
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
		void glyph(int x, int y, uint8_t code, uint32_t colour) const
		{
			const uint8_t* const rows = rom_chars + static_cast<size_t>(code) * 8;
			for (int gy = 0; gy < 8; gy++)
			{
				const uint8_t bits = rows[gy];
				if (!bits) continue;
				for (int gx = 0; gx < 8; gx++)
				{
					if (!(bits & (0x80 >> gx))) continue;
					uint32_t* p = g_back_bits + static_cast<size_t>(y + gy * _scale) * g_back_w
						+ x + gx * _scale;
					for (int sy = 0; sy < _scale; sy++, p += g_back_w)
						std::fill_n(p, _scale, colour);
				}
			}
		}

		int _left, _top, _right, _bottom, _scale, _y;
		bool _measuring;
		int _width = 0;
	};

	void emit_debug_lines(panel_writer& w, const debug_state& d, int emulated_fps)
	{
		const uint32_t head = vic_palette[1];  // white
		const uint32_t text = vic_palette[14]; // light blue
		const uint32_t mark = vic_palette[7];  // yellow

		char flags[9];
		static constexpr char kFlagNames[] = "NV-BDIZC";
		for (int i = 0; i < 8; i++) flags[i] = (d.status & (0x80 >> i)) ? kFlagNames[i] : '.';
		flags[8] = 0;

		w.line(head, "CPU");
		w.line(text, "PC %04X A %02X X %02X Y %02X SP %02X", d.pc, d.a, d.x, d.y, d.sp);
		w.line(text, "P %02X %s  IRQ %c NMI %c", d.status, flags,
			d.irq_pending ? '*' : '.', d.nmi_pending ? '*' : '.');
		w.line(text, "$01 %02X %s %s %s %s%s", d.port,
			d.basic_on ? "BAS" : "---", d.kernal_on ? "KER" : "---",
			d.io_on ? "I/O" : "---", d.char_on ? "CHR" : "---",
			d.cart_inserted ? " CRT" : "");

		w.blank();
		w.line(head, "DISASSEMBLY");
		for (int i = 0; i < d.disasm_count; i++)
		{
			const disasm_line& l = d.disasm[i];
			char raw[10];
			switch (l.length)
			{
			case 1: std::snprintf(raw, sizeof(raw), "%02X      ", l.bytes[0]); break;
			case 2: std::snprintf(raw, sizeof(raw), "%02X %02X   ", l.bytes[0], l.bytes[1]); break;
			default: std::snprintf(raw, sizeof(raw), "%02X %02X %02X",
				l.bytes[0], l.bytes[1], l.bytes[2]); break;
			}
			w.line(i == d.current ? mark : text, "%c%04X %s %s",
				i == d.current ? '>' : ' ', l.address, raw, l.text);
		}

		w.blank();
		w.line(head, "VIC-II");
		w.line(text, "MODE %-9s RASTER %03u/%03u", d.mode, d.raster, d.raster_compare);
		w.line(text, "D011 %02X D016 %02X D018 %02X", d.ctrl1, d.ctrl2, d.mem_ptr);
		w.line(text, "SCR %04X CHR %04X BMP %04X", d.screen_base, d.char_base, d.bitmap_base);
		w.line(text, "BANK %04X BORDER %X BG %X", d.vic_bank, d.border, d.background);
		w.line(text, "SPR %02X HIT %02X/%02X IRQ %02X/%02X", d.sprite_enable,
			d.sprite_sprite_hit, d.sprite_bg_hit, d.vic_irq_status, d.vic_irq_enable);

		w.blank();
		w.line(head, "CIA");
		for (int c = 0; c < 2; c++)
			w.line(text, "%d A %04X%c B %04X%c ICR %02X/%02X", c + 1,
				d.timer[c * 2], d.timer_running[c * 2] ? '>' : '.',
				d.timer[c * 2 + 1], d.timer_running[c * 2 + 1] ? '>' : '.',
				d.cia_icr[c], d.cia_mask[c]);

		w.blank();
		w.line(head, "SID");
		for (int v = 0; v < 3; v++)
		{
			const uint8_t* const r = d.sid + v * 7;
			w.line(text, "%d F%04X W%02X AD%02X SR%02X E%02X", v + 1,
				static_cast<unsigned>(r[0] | (r[1] << 8)), r[4], r[5], r[6], d.sid_env[v]);
		}
		w.line(text, "VOL %X FILT %02X CUTOFF %04X", d.sid[0x18] & 0x0F, d.sid[0x17],
			static_cast<unsigned>((d.sid[0x15] & 0x07) | (d.sid[0x16] << 3)));

		w.blank();
		w.line(head, "HOST");
		w.line(emulated_fps < 48 ? mark : text, "EMULATION %d FPS (%d%%)",
			emulated_fps, emulated_fps * 2);
	}

	void draw_debug_panel(int left, int top, int right, int bottom, int scale,
		const debug_state& d, int emulated_fps)
	{
		panel_writer measure(left, top, right, bottom, scale, true);
		emit_debug_lines(measure, d, emulated_fps);

		// Centre the block, falling back to a small inset when it does not fit.
		const int x = left + std::max(scale, (right - left - measure.width()) / 2);
		const int y = top + std::max(scale, (bottom - top - measure.height()) / 2);

		panel_writer draw(x, y, right, bottom, scale);
		emit_debug_lines(draw, d, emulated_fps);
	}

	// Splitter geometry. The debug panel is not a child window: the client area
	// is one surface, split by a draggable bar.
	int splitter_width() { return std::max(4, MulDiv(5, g_dpi, 96)); }
	int text_scale() { return std::max(2, g_dpi * 2 / 96); }
	int default_panel_width() { return 31 * 8 * text_scale() + 4 * text_scale(); }

	// x of the splitter's left edge, or -1 when the panel is hidden or would not fit.
	int splitter_x(int client_w)
	{
		if (!g_show_debug.load()) return -1;
		const int x = client_w - g_panel_w - splitter_width();
		return x >= 0 ? x : -1;
	}

	void clamp_panel(int client_w)
	{
		// Always leave room for the picture at 1x, so dragging can never hide it.
		const int max_w = std::max(0, client_w - splitter_width() - kFramebufferW);
		const int min_w = std::min(default_panel_width(), max_w); // the width the text needs
		g_panel_w = std::clamp(g_panel_w, min_w, max_w);
	}

	// Emulated frames per second, sampled over the last half second. A tune that
	// drifts below 50 is the emulator being paced slower than real time.
	int emulated_fps()
	{
		static auto last_time = std::chrono::steady_clock::now();
		static int64_t last_frames = 0;
		static int value = 50;

		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration<double>(now - last_time).count();
		if (elapsed >= 0.5)
		{
			const int64_t frames = g_emu_frames.load(std::memory_order_relaxed);
			value = static_cast<int>((frames - last_frames) / elapsed + 0.5);
			last_frames = frames;
			last_time = now;
		}
		return value;
	}

	void paint(HWND hwnd)
	{
		PAINTSTRUCT ps;
		const HDC hdc = BeginPaint(hwnd, &ps);

		RECT client;
		GetClientRect(hwnd, &client);
		const int cw = client.right;
		const int ch = client.bottom;

		resize_back_buffer(hwnd, cw, ch);
		if (cw <= 0 || ch <= 0 || !g_back_bits)
		{
			EndPaint(hwnd, &ps);
			return;
		}

		// Claim the newest published frame; the emulation thread keeps writing
		// into a different buffer while this paint runs.
		{
			std::lock_guard<std::mutex> lk(g_frame_mutex);
			if (g_frame_fresh)
			{
				std::swap(g_frame_ready, g_frame_read);
				g_frame_fresh = false;
			}
		}
		const frame& current = g_frames[g_frame_read];

		const bool want_debug = g_show_debug.load();
		if (want_debug) clamp_panel(cw);
		const int split = splitter_x(cw);
		const bool debug = split >= 0;
		const int picture_w = debug ? split : cw;

		const int scale = std::max(1, std::min(picture_w / kFramebufferW, ch / kFramebufferH));
		const int render_w = kFramebufferW * scale;
		const int render_h = kFramebufferH * scale;
		const int x = (picture_w - render_w) / 2;
		const int y = (ch - render_h) / 2;

		// Only the letterbox needs clearing; the picture covers the rest.
		fill_rect(0, 0, picture_w, y, 0);
		fill_rect(0, y + render_h, picture_w, ch - y - render_h, 0);
		fill_rect(0, y, x, render_h, 0);
		fill_rect(x + render_w, y, picture_w - x - render_w, render_h, 0);
		if (render_w <= picture_w && render_h <= ch)
			blit_picture(current.pixels.data(), x, y, scale);

		if (debug)
		{
			fill_rect(split, 0, splitter_width(), ch, vic_palette[11]);       // dark grey bar
			const int panel_x = split + splitter_width();
			fill_rect(panel_x, 0, cw - panel_x, ch, vic_palette[6]);          // C64 blue
			draw_debug_panel(panel_x, 0, cw, ch, text_scale(), current.debug, emulated_fps());
		}

		BitBlt(hdc, 0, 0, cw, ch, g_back_dc, 0, 0, SRCCOPY);
		EndPaint(hwnd, &ps);
	}

	void open_dialog(HWND hwnd)
	{
		wchar_t file[MAX_PATH] = L"";
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = hwnd;
		ofn.lpstrFilter = kFileFilter;
		ofn.lpstrFile = file;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = L"Open Program, Cartridge, SID Tune or Disk Image";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
		if (GetOpenFileNameW(&ofn))
			load_file(file, hwnd);
	}

	// Map a Windows virtual key to up to two C64 keyboard-matrix positions
	// (positional layout). Returns the count; 0 = unmapped. Keys that are a
	// shifted combination on the C64 (cursor up/left, F2/F4/F6/F8) also press
	// LEFT SHIFT.
	struct keypos { int row; int col; };

	int map_key(WPARAM vk, keypos pos[2])
	{
		auto one = [&](int r, int c) { pos[0] = {r, c}; return 1; };
		auto shifted = [&](int r, int c) { pos[0] = {1, 7}; pos[1] = {r, c}; return 2; };

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

		case VK_OEM_COMMA: return one(5, 7);  // ,
		case VK_OEM_PERIOD: return one(5, 4);  // .
		case VK_OEM_2: return one(6, 7);       // /
		case VK_OEM_1: return one(6, 2);       // ;
		case VK_OEM_MINUS: return one(5, 3);   // -
		case VK_OEM_PLUS: return one(5, 0);    // + / =
		case VK_OEM_3: return one(7, 1);       // ` -> left-arrow

		case VK_RETURN: return one(0, 1);
		case VK_SPACE: return one(7, 4);
		case VK_BACK: return one(0, 0);        // INST/DEL
		case VK_DELETE: return one(0, 0);
		case VK_SHIFT: return one(1, 7);
		case VK_CONTROL: return one(7, 2);
		case VK_TAB: return one(7, 5);         // Commodore key
		case VK_ESCAPE: return one(7, 7);      // RUN/STOP
		case VK_HOME: return one(6, 3);        // CLR/HOME

		case VK_DOWN: return one(0, 7);
		case VK_RIGHT: return one(0, 2);
		case VK_UP: return shifted(0, 7);
		case VK_LEFT: return shifted(0, 2);

		case VK_F1: return one(0, 4); case VK_F3: return one(0, 5);
		case VK_F5: return one(0, 6); case VK_F7: return one(0, 3);
		case VK_F2: return shifted(0, 4); case VK_F4: return shifted(0, 5);
		case VK_F6: return shifted(0, 6); case VK_F8: return shifted(0, 3);

		default: return 0;
		}
	}

	// Stops the worker threads. Called from WM_CLOSE so neither thread can touch
	// the window after it is destroyed, and again from WinMain as a backstop.
	void shutdown_workers()
	{
		if (!g_running.exchange(false)) return;
		g_queue.stop();
		if (g_emu_thread.joinable()) g_emu_thread.join();
		g_audio.stop();
	}

	void release_all_keys()
	{
		g_joystick.store(0);
		std::lock_guard<std::mutex> lk(g_machine_mutex);
		g_machine.clear_keys();
	}

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		switch (msg)
		{
		case WM_CREATE:
			g_dpi = static_cast<int>(GetDpiForWindow(hwnd));
			g_panel_w = g_settings.panel_w96 > 0
				? MulDiv(g_settings.panel_w96, g_dpi, 96)
				: default_panel_width();
			g_show_debug.store(g_settings.show_debug);
			return 0;

		case WM_SIZE:
			resize_back_buffer(hwnd, LOWORD(lparam), HIWORD(lparam));
			track_normal_rect(hwnd);
			return 0;

		case WM_MOVE:
			track_normal_rect(hwnd);
			return 0;

		case WM_DPICHANGED:
		{
			const int old_dpi = g_dpi;
			g_dpi = static_cast<int>(HIWORD(wparam));
			g_panel_w = MulDiv(g_panel_w, g_dpi, old_dpi);

			const RECT* const suggested = reinterpret_cast<const RECT*>(lparam);
			SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
				suggested->right - suggested->left, suggested->bottom - suggested->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}

		case WM_SETCURSOR:
			if (LOWORD(lparam) == HTCLIENT)
			{
				POINT pt;
				RECT client;
				GetCursorPos(&pt);
				ScreenToClient(hwnd, &pt);
				GetClientRect(hwnd, &client);
				const int split = splitter_x(client.right);
				if (g_dragging || (split >= 0 && pt.x >= split && pt.x < split + splitter_width()))
				{
					SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
					return TRUE;
				}
			}
			return DefWindowProcW(hwnd, msg, wparam, lparam);

		case WM_LBUTTONDOWN:
		{
			RECT client;
			GetClientRect(hwnd, &client);
			const int split = splitter_x(client.right);
			const int x = GET_X_LPARAM(lparam);
			if (split >= 0 && x >= split && x < split + splitter_width())
			{
				g_dragging = true;
				SetCapture(hwnd);
			}
			return 0;
		}

		case WM_MOUSEMOVE:
			if (g_dragging)
			{
				RECT client;
				GetClientRect(hwnd, &client);
				g_panel_w = client.right - GET_X_LPARAM(lparam) - splitter_width();
				clamp_panel(client.right);
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return 0;

		case WM_LBUTTONUP:
			if (g_dragging) ReleaseCapture();
			return 0;

		case WM_CAPTURECHANGED:
			g_dragging = false;
			return 0;

		case WM_ERASEBKGND:
			return 1; // painted fully (double-buffered) in WM_PAINT

		case WM_PAINT:
			paint(hwnd);
			return 0;

		case WM_KEYDOWN:
		case WM_KEYUP:
		{
			const bool pressed = msg == WM_KEYDOWN;

			// In joystick mode the arrows drive the joystick instead of the C64
			// cursor keys. Space is shared: it fires *and* still reaches the matrix,
			// so the mode never silently swallows the space bar.
			if (g_arrow_mode.load() != arrow_mode::cursor)
			{
				uint8_t bit = 0;
				switch (wparam)
				{
				case VK_UP: bit = kJoyUp; break;
				case VK_DOWN: bit = kJoyDown; break;
				case VK_LEFT: bit = kJoyLeft; break;
				case VK_RIGHT: bit = kJoyRight; break;
				case VK_SPACE: bit = kJoyFire; break;
				default: break;
				}

				if (bit)
				{
					if (pressed) g_joystick.fetch_or(bit);
					else g_joystick.fetch_and(static_cast<uint8_t>(~bit));
					if (wparam != VK_SPACE) return 0;
				}
			}

			keypos pos[2];
			const int count = map_key(wparam, pos);
			if (count > 0)
			{
				std::lock_guard<std::mutex> lk(g_machine_mutex);
				for (int i = 0; i < count; ++i)
					g_machine.set_key(pos[i].row, pos[i].col, pressed);
			}
			return 0;
		}

		case WM_KILLFOCUS:
			// Release everything so nothing sticks while we're in the background.
			release_all_keys();
			return 0;

		case WM_SID_STOPPED:
			// The tune stopped itself (the player's X key), so it is no longer the
			// loaded medium - unless something else has been loaded since.
			if (static_cast<uint32_t>(wparam) == g_media_generation.load())
			{
				g_media.clear();
				update_title();
			}
			return 0;

		case WM_DROPFILES:
		{
			const auto drop = reinterpret_cast<HDROP>(wparam);
			const UINT chars = DragQueryFileW(drop, 0, nullptr, 0); // excludes the NUL
			if (chars)
			{
				std::wstring path(chars, L'\0');
				if (DragQueryFileW(drop, 0, path.data(), chars + 1))
					load_file(path.c_str(), hwnd); // only the first file is loaded
			}
			DragFinish(drop);
			return 0;
		}

		case WM_COMMAND:
		{
			const int id = LOWORD(wparam);
			if (HIWORD(wparam) == 1)
			{
				// From an accelerator: the key itself never reached the matrix, but
				// the Ctrl modifier did, so let go of everything.
				release_all_keys();
			}

			switch (id)
			{
			case kCmdOpen: open_dialog(hwnd); break;
			case kCmdReset:
			{
				std::lock_guard<std::mutex> lk(g_machine_mutex);
				g_machine._state->reset();
				g_machine.set_audio_enabled(true); // resync audio clock after reset
				break;
			}
			case kCmdEject:
			{
				{
					std::lock_guard<std::mutex> lk(g_machine_mutex);
					g_machine.eject_disk();
					g_machine.eject_crt(); // also resets, so the machine comes up bare
					g_machine.set_audio_enabled(true);
				}
				g_cart_inserted = false;
				g_disk_inserted = false;
				g_media.clear();
				refresh_menu(hwnd);
				update_title();
				break;
			}
			case kCmdPause:
				g_paused.store(!g_paused.load());
				refresh_menu(hwnd);
				update_title();
				break;
			case kCmdDebug:
				g_show_debug.store(!g_show_debug.load());
				refresh_menu(hwnd);
				InvalidateRect(hwnd, nullptr, FALSE);
				break;
			case kCmdExit: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
			case kCmdArrowsCursor: set_arrow_mode(hwnd, arrow_mode::cursor); break;
			case kCmdArrowsJoy1: set_arrow_mode(hwnd, arrow_mode::joystick1); break;
			case kCmdArrowsJoy2: set_arrow_mode(hwnd, arrow_mode::joystick2); break;
			case kCmdKeyboard:
				MessageBoxW(hwnd,
					L"Emulated C64 keys\n\n"
					L"Esc\t\tRUN/STOP\n"
					L"Tab\t\tCommodore key\n"
					L"` (backtick)\tLeft arrow\n"
					L"Home\t\tCLR/HOME\n"
					L"Backspace, Delete\tINST/DEL\n"
					L"F1 - F8\t\tC64 function keys\n"
					L"Arrow keys\tC64 cursor keys, or the joystick when the Input "
					L"menu selects one\n"
					L"Space\t\tSpace bar, and joystick fire in joystick mode\n\n"
					L"Host shortcuts\n\n"
					L"Ctrl+O\t\tOpen a file\n"
					L"Ctrl+R\t\tReset (the cartridge stays inserted)\n"
					L"Ctrl+E\t\tEject cartridge\n"
					L"F9\t\tPause / resume\n"
					L"F11\t\tShow / hide the debug panel\n"
					L"F12\t\tAbout",
					kProductName, MB_OK | MB_ICONINFORMATION);
				break;
			case kCmdAbout:
				MessageBoxW(hwnd,
					L"lib8bit\n\nA Commodore 64 emulator.\n\n"
					L"Open or drop a .PRG, .CRT, .SID, .D64, .D71 or .D81 file, or pass "
					L"one on the command line.\n"
					L"A .SID tune opens in a player screen: 1-9 select a sub-tune and X "
					L"stops playback.\n"
					L"Use the Input menu to point the arrow keys at the C64 cursor keys "
					L"or at joystick port 1 or 2; Space is fire and still types a space.\n\n"
					L"Help > Keyboard lists the emulated key layout.",
					kProductName, MB_OK | MB_ICONINFORMATION);
				break;
			default: break;
			}
			return 0;
		}

		case WM_ENDSESSION:
			// Log-off or shutdown never delivers WM_CLOSE, so save geometry here too.
			if (wparam) save_settings(hwnd);
			return 0;

		case WM_CLOSE:
			// Stop the workers while the window is still alive, then stop publishing
			// the handle before it becomes a recycled HWND.
			save_settings(hwnd);
			shutdown_workers();
			g_hwnd.store(nullptr);
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			destroy_dib(g_back_dc, g_back_bmp, g_back_old);
			g_back_bits = nullptr;
			g_back_w = 0;
			g_back_h = 0;
			PostQuitMessage(0);
			return 0;

		default:
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}
	}

	HMENU build_menu()
	{
		const HMENU file = CreatePopupMenu();
		AppendMenuW(file, MF_STRING, kCmdOpen, L"&Open...\tCtrl+O");
		AppendMenuW(file, MF_STRING, kCmdPause, L"&Pause\tF9");
		AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(file, MF_STRING, kCmdReset, L"&Reset (Keeps Cartridge)\tCtrl+R");
		AppendMenuW(file, MF_STRING, kCmdEject, L"&Eject Media\tCtrl+E");
		AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(file, MF_STRING, kCmdExit, L"E&xit");

		const HMENU input = CreatePopupMenu();
		AppendMenuW(input, MF_STRING, kCmdArrowsCursor, L"&Cursor Keys");
		AppendMenuW(input, MF_STRING, kCmdArrowsJoy1, L"Joystick &1");
		AppendMenuW(input, MF_STRING, kCmdArrowsJoy2, L"Joystick &2");

		const HMENU view = CreatePopupMenu();
		AppendMenuW(view, MF_STRING, kCmdDebug, L"&Debug Panel\tF11");

		const HMENU help = CreatePopupMenu();
		AppendMenuW(help, MF_STRING, kCmdKeyboard, L"&Keyboard...");
		AppendMenuW(help, MF_STRING, kCmdAbout, L"&About...\tF12");

		const HMENU bar = CreateMenu();
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(input), L"&Input");
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
		return bar;
	}

	// Host shortcuts. Ctrl-modified keys and F9/F12 are chosen because
	// TranslateAccelerator swallows them before they can reach the emulated
	// keyboard, which claims almost every unmodified key including F1-F8.
	HACCEL create_accelerators()
	{
		ACCEL accels[] = {
			{FVIRTKEY | FCONTROL, 'O', kCmdOpen},
			{FVIRTKEY | FCONTROL, 'R', kCmdReset},
			{FVIRTKEY | FCONTROL, 'E', kCmdEject},
			{FVIRTKEY, VK_F9, kCmdPause},
			{FVIRTKEY, VK_F11, kCmdDebug},
			{FVIRTKEY, VK_F12, kCmdAbout},
		};
		return CreateAcceleratorTableW(accels, static_cast<int>(ARRAYSIZE(accels)));
	}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show)
{
	// Boot the emulator to READY before starting the threads / showing the UI.
	g_machine.set_render_enabled(true);
	g_machine.exec(3'000'000);
	g_machine.set_audio_enabled(true); // sync the SID audio clock to "now"

	load_settings(); // WM_CREATE reads the panel state out of this

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = instance;
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_C64));
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"lib8bit";
	RegisterClassExW(&wc);

	// Size the window so the client fits the picture at 2x, including the menu.
	RECT rc{0, 0, kFramebufferW * 2, kFramebufferH * 2};
	AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0, GetDpiForSystem());

	const HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, kProductName,
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top, nullptr, build_menu(), instance, nullptr);
	if (!hwnd) return 0;
	g_hwnd.store(hwnd);

	// Per-monitor DPI: the window may have landed on a monitor with a different
	// scale than the one the size above was computed for, so redo it now that
	// there is a window to ask.
	if (g_settings.has_window)
	{
		const RECT& r = g_settings.window;
		SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
			SWP_NOZORDER | SWP_NOACTIVATE);
	}
	else
	{
		RECT sized{0, 0, kFramebufferW * 2, kFramebufferH * 2};
		AdjustWindowRectExForDpi(&sized, WS_OVERLAPPEDWINDOW, TRUE, 0, GetDpiForWindow(hwnd));
		SetWindowPos(hwnd, nullptr, 0, 0, sized.right - sized.left, sized.bottom - sized.top,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	set_arrow_mode(hwnd, arrow_mode::cursor);

	// Start the worker threads: audio first so its device is ready, then the
	// emulation thread that feeds it.
	g_running.store(true);
	g_audio_ok = g_audio.start(&g_queue);
	bool timer_period = false;
	if (!g_audio_ok)
	{
		timeBeginPeriod(1); // finer sleep granularity for the no-audio fallback
		timer_period = true;
	}
	g_emu_thread = std::thread(emu_thread_main);

	update_title();
	ShowWindow(hwnd, g_settings.maximized && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show);
	UpdateWindow(hwnd);

	// "Open with" / shell association: load the first path on the command line.
	int argc = 0;
	if (wchar_t** const argv = CommandLineToArgvW(GetCommandLineW(), &argc))
	{
		if (argc > 1) load_file(argv[1], hwnd);
		LocalFree(argv);
	}

	const HACCEL accel = create_accelerators();

	MSG msg;
	while (const BOOL result = GetMessageW(&msg, nullptr, 0, 0))
	{
		if (result == -1) break; // GetMessage failed; msg holds nothing usable
		const HWND target = g_hwnd.load();
		if (accel && target && TranslateAcceleratorW(target, accel, &msg)) continue;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (accel) DestroyAcceleratorTable(accel);
	shutdown_workers(); // no-op if WM_CLOSE already did it
	if (timer_period) timeEndPeriod(1);
	return static_cast<int>(msg.wParam);
}
