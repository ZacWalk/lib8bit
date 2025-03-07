// lib8bit by Zac Walker
//
// lib8bit test app: a self-contained Win32 front-end for the C64 emulator.
//
// Threading model (audio-clocked emulation):
//   * An emulation thread runs the C64 one PAL frame at a time, renders the
//     VIC-II picture into a shared "present" buffer, and pushes the frame's SID
//     samples onto a bounded, synchronised queue.
//   * An audio thread drains that queue into waveOut buffers. The sound card
//     consumes at a fixed 44100 Hz, so the queue's back-pressure paces the
//     emulation thread at exactly real time — gapless audio with no timer.
//   * The UI (main) thread owns the window: it blits the latest present buffer
//     on WM_PAINT and feeds keyboard / file / menu input to the machine.
// All machine access is serialised by g_machine_mutex; the present buffer by
// g_present_mutex.

#include "machine.h"
#include "disk.h"
#include "Resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")

namespace
{
	constexpr int kFramebufferW = 384;
	constexpr int kFramebufferH = 272;
	constexpr int kSampleRate = 44100;

	// One PAL frame of CPU time per emulation step. With audio back-pressure the
	// step rate self-adjusts to real time; this just sets the video snapshot rate
	// (~50 Hz) and the SID master clock reference.
	constexpr int kClockPal = 985248;
	constexpr int kCyclesPerFrame = kClockPal / 50;

	// Audio buffering. The queue is the elastic band between the two threads; the
	// waveOut blocks are what the sound card actually plays.
	constexpr size_t kQueueCapacity = kSampleRate / 10; // ~100 ms of slack

	constexpr int kCmdOpen = 1001;
	constexpr int kCmdReset = 1002;
	constexpr int kCmdExit = 1003;
	constexpr int kCmdAbout = 1004;
	constexpr int kCmdArrowsCursor = 1010;
	constexpr int kCmdArrowsJoy1 = 1011;
	constexpr int kCmdArrowsJoy2 = 1012;

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

	machine g_machine;
	std::mutex g_machine_mutex;                     // guards every g_machine access
	std::atomic<arrow_mode> g_arrow_mode{arrow_mode::cursor};

	std::mutex g_present_mutex;                      // guards g_present
	std::vector<uint32_t> g_present(static_cast<size_t>(kFramebufferW) * kFramebufferH);
	std::vector<uint32_t> g_paint(static_cast<size_t>(kFramebufferW) * kFramebufferH); // UI-thread only

	std::atomic<bool> g_running{false};
	std::thread g_emu_thread;
	HWND g_hwnd = nullptr;

	// ---- Synchronised sample queue (bounded ring buffer) -------------------
	// push() blocks while full; pop() blocks while empty. Either returns early
	// once stop() is called so the threads can unwind.
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
				_not_empty.notify_one();
			}
		}

		size_t pop(int16_t* out, size_t n)
		{
			std::unique_lock<std::mutex> lk(_m);
			_not_empty.wait(lk, [&] { return _count > 0 || _stopping; });
			if (_count == 0) return 0; // stopping and drained
			size_t got = 0;
			while (got < n && _count > 0)
			{
				out[got++] = _buf[_head];
				_head = (_head + 1) % _cap;
				--_count;
			}
			_not_full.notify_one();
			return got;
		}

		void stop()
		{
			{
				std::lock_guard<std::mutex> lk(_m);
				_stopping = true;
			}
			_not_full.notify_all();
			_not_empty.notify_all();
		}

	private:
		std::vector<int16_t> _buf;
		size_t _cap;
		size_t _head = 0, _tail = 0, _count = 0;
		bool _stopping = false;
		std::mutex _m;
		std::condition_variable _not_full, _not_empty;
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
		void fill(int i)
		{
			const size_t n = _queue->pop(_blocks[i].data(), kChunk);
			if (n == 0) return; // stopping
			if (n < kChunk) std::fill(_blocks[i].begin() + n, _blocks[i].end(), int16_t{0});

			WAVEHDR& hdr = _headers[i];
			hdr = {};
			hdr.lpData = reinterpret_cast<LPSTR>(_blocks[i].data());
			hdr.dwBufferLength = kChunk * sizeof(int16_t);
			waveOutPrepareHeader(_hwo, &hdr, sizeof(hdr));
			waveOutWrite(_hwo, &hdr, sizeof(hdr));
		}

		void run()
		{
			for (int i = 0; i < kBuffers && _running; ++i) fill(i); // prime

			while (_running)
			{
				WaitForSingleObject(_event, 50);
				for (int i = 0; i < kBuffers; ++i)
				{
					if (_headers[i].dwFlags & WHDR_DONE)
					{
						waveOutUnprepareHeader(_hwo, &_headers[i], sizeof(_headers[i]));
						_headers[i].dwFlags = 0;
						if (_running) fill(i);
					}
				}
			}
		}

		static constexpr int kChunk = 512;   // samples per waveOut buffer (~11.6 ms)
		static constexpr int kBuffers = 4;   // ~46 ms in the sound card

		HWAVEOUT _hwo = nullptr;
		HANDLE _event = nullptr;
		WAVEHDR _headers[kBuffers] = {};
		std::array<std::vector<int16_t>, kBuffers> _blocks;
		sample_queue* _queue = nullptr;
		std::thread _thread;
		std::atomic<bool> _running{false};
	};

	audio_engine g_audio;
	bool g_audio_ok = false;

	// ---- File loading (UI thread; holds the machine lock) -------------------
	std::vector<uint8_t> read_file(const wchar_t* path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream) return {};
		const auto size = stream.tellg();
		if (size <= 0) return {};
		std::vector<uint8_t> bytes(static_cast<size_t>(size));
		stream.seekg(0);
		stream.read(reinterpret_cast<char*>(bytes.data()), size);
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

	void load_file(const wchar_t* path, HWND hwnd)
	{
		const auto bytes = read_file(path);
		if (bytes.empty())
		{
			MessageBoxW(hwnd, L"Failed to read file.", L"lib8bit Test App", MB_OK | MB_ICONWARNING);
			return;
		}

		std::lock_guard<std::mutex> lk(g_machine_mutex);
		if (is_sid(bytes))
		{
			if (!g_machine.load_sid(bytes.data(), bytes.size()))
				MessageBoxW(hwnd, L"Not a valid SID file.", L"lib8bit Test App", MB_OK | MB_ICONWARNING);
		}
		else if (is_crt(bytes))
		{
			if (!g_machine.load_crt(bytes.data(), bytes.size()))
				MessageBoxW(hwnd, L"Not a valid CRT file.", L"lib8bit Test App", MB_OK | MB_ICONWARNING);
		}
		else if (disk_directory dir; read_disk_directory(bytes.data(), bytes.size(), dir))
		{
			// A disk image (.d64/.d71/.d81): load and run the first program,
			// like the C64's LOAD"*",8 followed by RUN.
			std::vector<uint8_t> prg;
			if (!read_disk_file(bytes.data(), bytes.size(), "*", prg))
			{
				MessageBoxW(hwnd, L"No program found on disk.", L"lib8bit Test App", MB_OK | MB_ICONWARNING);
				return;
			}
			g_machine._state->reset();
			g_machine.exec(3'000'000);
			g_machine.load_prg(prg.data(), prg.size());
		}
		else
		{
			// Treat as a .PRG: reset, run the kernal to READY, then inject + RUN.
			g_machine._state->reset();
			g_machine.exec(3'000'000);
			if (!g_machine.load_prg(bytes.data(), bytes.size()))
			{
				MessageBoxW(hwnd, L"Not a valid PRG file.", L"lib8bit Test App", MB_OK | MB_ICONWARNING);
				return;
			}
		}

		// reset() zeroes the SID audio clock; re-sync it to "now" so the next
		// frame renders one frame of samples rather than a huge backlog.
		g_machine.set_audio_enabled(true);
	}

	void set_arrow_mode(HWND hwnd, arrow_mode mode)
	{
		g_arrow_mode.store(mode);
		const int checked = mode == arrow_mode::cursor ? kCmdArrowsCursor
			: mode == arrow_mode::joystick1 ? kCmdArrowsJoy1 : kCmdArrowsJoy2;
		CheckMenuRadioItem(GetMenu(hwnd), kCmdArrowsCursor, kCmdArrowsJoy2, checked, MF_BYCOMMAND);
	}

	// Poll the physical arrow keys + fire and drive the selected joystick. Called
	// on the emulation thread while it holds the machine lock. Space/Ctrl = fire.
	void poll_joystick()
	{
		if (g_arrow_mode.load() == arrow_mode::cursor)
		{
			g_machine.set_joystick(1, false, false, false, false, false);
			g_machine.set_joystick(2, false, false, false, false, false);
			return;
		}

		const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
		const int port = g_arrow_mode.load() == arrow_mode::joystick1 ? 1 : 2;
		const int other = port == 1 ? 2 : 1;
		const bool fire = down(VK_SPACE) || down(VK_CONTROL);
		g_machine.set_joystick(port, down(VK_UP), down(VK_DOWN), down(VK_LEFT), down(VK_RIGHT), fire);
		g_machine.set_joystick(other, false, false, false, false, false);
	}

	// ---- Emulation thread ---------------------------------------------------
	void emu_thread_main()
	{
		std::vector<int16_t> samples(4096);
		auto next_frame = std::chrono::steady_clock::now();

		while (g_running.load())
		{
			int count = 0;
			{
				std::lock_guard<std::mutex> lk(g_machine_mutex);
				poll_joystick();
				g_machine.exec(kCyclesPerFrame);
				count = g_machine.generate_audio(samples.data(), static_cast<int>(samples.size()));

				std::lock_guard<std::mutex> pl(g_present_mutex);
				std::memcpy(g_present.data(), g_machine.framebuffer(),
					g_present.size() * sizeof(uint32_t));
			}

			if (g_audio_ok)
			{
				// Back-pressure from the audio queue paces us to real time.
				g_queue.push(samples.data(), static_cast<size_t>(count));
			}
			else
			{
				// No audio device: pace the frame ourselves.
				next_frame += std::chrono::microseconds(1'000'000 / 50);
				std::this_thread::sleep_until(next_frame);
			}

			if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
		}
	}

	// ---- Painting (UI thread) ----------------------------------------------
	void paint(HWND hwnd)
	{
		PAINTSTRUCT ps;
		const HDC hdc = BeginPaint(hwnd, &ps);

		{
			std::lock_guard<std::mutex> lk(g_present_mutex);
			std::memcpy(g_paint.data(), g_present.data(), g_paint.size() * sizeof(uint32_t));
		}

		RECT client;
		GetClientRect(hwnd, &client);
		const int cw = client.right;
		const int ch = client.bottom;

		const HDC mem = CreateCompatibleDC(hdc);
		const HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
		const auto old_bmp = SelectObject(mem, bmp);

		FillRect(mem, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

		const int scale = std::max(1, std::min(cw / kFramebufferW, ch / kFramebufferH));
		const int render_w = kFramebufferW * scale;
		const int render_h = kFramebufferH * scale;
		const int x = (cw - render_w) / 2;
		const int y = (ch - render_h) / 2;

		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = kFramebufferW;
		bmi.bmiHeader.biHeight = -kFramebufferH; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		SetStretchBltMode(mem, COLORONCOLOR);
		StretchDIBits(mem, x, y, render_w, render_h, 0, 0, kFramebufferW, kFramebufferH,
			g_paint.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

		BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

		SelectObject(mem, old_bmp);
		DeleteObject(bmp);
		DeleteDC(mem);
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
		ofn.lpstrTitle = L"Open PRG, CRT or SID";
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
		case VK_SHIFT: case VK_LSHIFT: return one(1, 7);
		case VK_RSHIFT: return one(6, 4);
		case VK_CONTROL: case VK_LCONTROL: return one(7, 2);
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

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		switch (msg)
		{
		case WM_CREATE:
			DragAcceptFiles(hwnd, TRUE);
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

			// In joystick mode the arrows + Space/Ctrl drive the joystick (polled
			// on the emulation thread), so keep them off the keyboard matrix.
			if (g_arrow_mode.load() != arrow_mode::cursor)
			{
				switch (wparam)
				{
				case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
				case VK_SPACE: case VK_CONTROL: case VK_LCONTROL:
					return 0;
				default: break;
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
		{
			// Release every key so nothing sticks while we're in the background.
			std::lock_guard<std::mutex> lk(g_machine_mutex);
			g_machine.clear_keys();
			return 0;
		}

		case WM_DROPFILES:
		{
			const auto drop = reinterpret_cast<HDROP>(wparam);
			wchar_t path[MAX_PATH];
			if (DragQueryFileW(drop, 0, path, MAX_PATH))
				load_file(path, hwnd);
			DragFinish(drop);
			return 0;
		}

		case WM_COMMAND:
			switch (LOWORD(wparam))
			{
			case kCmdOpen: open_dialog(hwnd); break;
			case kCmdReset:
			{
				std::lock_guard<std::mutex> lk(g_machine_mutex);
				g_machine._state->reset();
				g_machine.set_audio_enabled(true); // resync audio clock after reset
				break;
			}
			case kCmdExit: DestroyWindow(hwnd); break;
			case kCmdArrowsCursor: set_arrow_mode(hwnd, arrow_mode::cursor); break;
			case kCmdArrowsJoy1: set_arrow_mode(hwnd, arrow_mode::joystick1); break;
			case kCmdArrowsJoy2: set_arrow_mode(hwnd, arrow_mode::joystick2); break;
			case kCmdAbout:
				MessageBoxW(hwnd,
					L"lib8bit Test App\n\nCommodore 64 emulator. Open or drop a .PRG, .CRT or "
					L".SID file.\nUse the Input menu to point the arrow keys at the cursor or a "
					L"joystick (Space or Ctrl = fire).",
					L"About lib8bit", MB_OK | MB_ICONINFORMATION);
				break;
			default: break;
			}
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;

		default:
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}
	}

	HMENU build_menu()
	{
		const HMENU file = CreatePopupMenu();
		AppendMenuW(file, MF_STRING, kCmdOpen, L"&Open...");
		AppendMenuW(file, MF_STRING, kCmdReset, L"&Reset");
		AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(file, MF_STRING, kCmdExit, L"E&xit");

		const HMENU input = CreatePopupMenu();
		AppendMenuW(input, MF_STRING, kCmdArrowsCursor, L"&Cursor Keys");
		AppendMenuW(input, MF_STRING, kCmdArrowsJoy1, L"Joystick &1");
		AppendMenuW(input, MF_STRING, kCmdArrowsJoy2, L"Joystick &2");

		const HMENU help = CreatePopupMenu();
		AppendMenuW(help, MF_STRING, kCmdAbout, L"&About...");

		const HMENU bar = CreateMenu();
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(input), L"&Input");
		AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
		return bar;
	}
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show)
{
	// Boot the emulator to READY before starting the threads / showing the UI.
	g_machine.set_render_enabled(true);
	g_machine.exec(3'000'000);
	g_machine.set_audio_enabled(true); // sync the SID audio clock to "now"
	timeBeginPeriod(1);                // finer sleep granularity for the no-audio fallback

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = instance;
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_C64));
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	wc.lpszClassName = L"lib8bit_app";
	RegisterClassExW(&wc);

	// Size the window so the client fits the picture at 2x, including the menu.
	RECT rc{0, 0, kFramebufferW * 2, kFramebufferH * 2};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, TRUE);

	g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"lib8bit Test App",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		rc.right - rc.left, rc.bottom - rc.top, nullptr, build_menu(), instance, nullptr);
	if (!g_hwnd) return 0;

	set_arrow_mode(g_hwnd, arrow_mode::cursor);

	// Start the worker threads: audio first so its device is ready, then the
	// emulation thread that feeds it.
	g_running.store(true);
	g_audio_ok = g_audio.start(&g_queue);
	g_emu_thread = std::thread(emu_thread_main);

	ShowWindow(g_hwnd, show);
	UpdateWindow(g_hwnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	// Shut down: stop the emulation thread first (unblock its queue push), then
	// the audio thread (its queue pop unblocks too).
	g_running.store(false);
	g_queue.stop();
	if (g_emu_thread.joinable()) g_emu_thread.join();
	g_audio.stop();

	timeEndPeriod(1);
	return static_cast<int>(msg.wParam);
}
