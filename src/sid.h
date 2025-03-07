#pragma once

#include <cstdint>
#include <vector>

// lib8bit by Zac Walker
//
// MOS6581 / MOS8580 SID sound-chip emulation. Three voices (waveform + ADSR
// envelope) feed a two-integrator
// analog filter model and an external RC/DC-block stage, resampled with a FIR
// low-pass into 16-bit PCM.
//
// Note: the kinked-DAC output tables are unused in the audio pipeline (the
// filter mixes the raw 12-bit waveform * 8-bit envelope), so they are
// intentionally omitted here.

enum class sid_model { mos6581, mos8580 };

// Builds the shared 4096-entry waveform tables and per-model combined-waveform
// pulldown tables (AVERAGE config). Idempotent; called by SID construction.
void sid_init_tables();

class waveform_generator
{
public:
	waveform_generator() { reset(); }

	void set_model(bool is_6581) { is6581 = is_6581; }
	void set_waveform_models(const int16_t (*models)[4096]);
	void set_pulldown_models(const int16_t (*models)[4096]);
	void set_other(waveform_generator* prev, waveform_generator* next) { prev_v = prev; next_v = next; }

	void reset();
	void clock();
	uint16_t output();
	void synchronize();

	void write_freq_lo(uint8_t v) { freq = static_cast<uint16_t>((freq & 0xFF00) | v); }
	void write_freq_hi(uint8_t v) { freq = static_cast<uint16_t>(((v << 8) & 0xFF00) | (freq & 0xFF)); }
	void write_pw_lo(uint8_t v) { pw = static_cast<uint16_t>((pw & 0xF00) | v); }
	void write_pw_hi(uint8_t v) { pw = static_cast<uint16_t>(((v << 8) & 0xF00) | (pw & 0x0FF)); }
	void write_control(uint8_t control);

	uint8_t read_osc() const { return static_cast<uint8_t>((osc3 >> 4) & 0xFF); }
	uint32_t read_accumulator() const { return accumulator; }
	uint16_t read_freq() const { return freq; }
	bool read_test() const { return test; }
	bool read_following_voice_sync() const { return next_v ? next_v->sync : false; }

	uint32_t accumulator = 0x555555;
	bool sync = false;

private:
	void shift_phase2(int waveform_new);
	void set_noise_output();
	void set_no_noise_or_noise_output() { no_noise_or_noise_output = static_cast<uint16_t>(no_noise | noise_output); }
	void write_shift_register();
	void wave_bitfade() { waveform_output &= static_cast<uint16_t>(waveform_output >> 1); }
	void shiftreg_bitfade() { shift_register = (shift_register | 0x400000) & 0x7FFFFF; }

	const int16_t (*model_wave)[4096] = nullptr;
	const int16_t (*model_pulldown)[4096] = nullptr;
	const int16_t* wave = nullptr;
	const int16_t* pulldown = nullptr;

	uint16_t pw = 0;
	uint32_t shift_register = 0x7FFFFF;
	uint32_t shift_latch = 0;
	int shift_pipeline = 0;
	uint32_t ring_msb_mask = 0;
	uint16_t no_noise = 0xFFF;
	uint16_t noise_output = 0;
	uint16_t no_noise_or_noise_output = 0xFFF;
	uint16_t no_pulse = 0xFFF;
	uint16_t pulse_output = 0xFFF;
	int waveform = 0;
	uint16_t waveform_output = 0;
	uint16_t freq = 0;
	uint16_t tri_saw_pipeline = 0x555;
	uint16_t osc3 = 0;
	int shift_register_reset = 0;
	int floating_output_ttl = 0;
	bool test = false;
	bool test_or_reset = false;
	bool msb_rising = false;
	bool is6581 = true;
	waveform_generator* prev_v = nullptr;
	waveform_generator* next_v = nullptr;
};

class envelope_generator
{
public:
	envelope_generator() { reset(); }

	void reset();
	void clock();
	uint8_t output() const { return envelope_counter; }
	uint8_t read_env() const { return env3; }

	void write_control(uint8_t control);
	void write_attack_decay(uint8_t v);
	void write_sustain_release(uint8_t v);

private:
	void state_change();
	void set_exponential_counter();

	enum { ATTACK = 0, DECAY_SUSTAIN = 1, RELEASE = 2 };

	uint16_t lfsr = 0x7FFF;
	uint16_t rate = 0;
	int exponential_counter = 0;
	int exponential_counter_period = 1;
	int new_exponential_counter_period = 0;
	int state_pipeline = 0;
	int envelope_pipeline = 0;
	int exponential_pipeline = 0;
	int state = RELEASE;
	int next_state = RELEASE;
	bool counter_enabled = true;
	bool gate = false;
	bool reset_lfsr = false;
	uint8_t envelope_counter = 0xAA;
	uint8_t attack = 0;
	uint8_t decay = 0;
	uint8_t sustain = 0;
	uint8_t release = 0;
	uint8_t env3 = 0;
};

class voice
{
public:
	waveform_generator wave;
	envelope_generator envelope;

	void reset() { wave.reset(); envelope.reset(); }
	void write_control(uint8_t v) { wave.write_control(v); envelope.write_control(v); }
};

// Two-integrator-loop biquad filter shared shape; 6581/8580 differ only in the
// cutoff-frequency table (built in the constructor).
class sid_filter
{
public:
	explicit sid_filter(bool is_6581);

	void reset();
	void input(int v) { ve = v; }
	int clock(voice& v1, voice& v2, voice& v3);

	void write_fc_lo(uint8_t v) { fc = (fc & 0x7F8) | (v & 0x007); update_w0(); }
	void write_fc_hi(uint8_t v) { fc = ((v << 3) & 0x7F8) | (fc & 0x007); update_w0(); }
	void write_res_filt(uint8_t v);
	void write_mode_vol(uint8_t v);
	void enable(bool e);

private:
	int normalized_voice(voice* v) const;
	void route_voices(voice& v1, voice& v2, voice& v3, int& vi, int& vo);
	void apply_filter(int vi);
	int mix_output(int vo);
	void update_resonance(int res) { resonance_coeff = 1.0 / (0.707 + res * 0.22); }
	void update_w0() { w0 = freq_table[fc < 2048 ? fc : 2047]; }

	bool is6581;
	int vhp = 0, vbp = 0, vlp = 0, ve = 0;
	int fc = 0;
	double w0 = 0;
	double resonance_coeff = 1.0;
	bool filt1 = false, filt2 = false, filt3 = false, filtE = false;
	bool voice3off = false, hp = false, bp = false, lp = false;
	int vol = 0;
	bool enabled = true;
	uint8_t filt = 0;
	float freq_table[2048] = {};
};

// C64 audio output stage: 16 kHz low-pass + ~1.6 Hz DC blocker.
class external_filter
{
public:
	void set_clock_frequency(double frequency);
	void reset() { vlp = 0; vhp = 0; }
	int clock(int input);

private:
	int64_t vlp = 0;
	int64_t vhp = 0;
	int64_t w0lp_1_s7 = 0;
	int64_t w0hp_1_s17 = 0;
};

class sid_chip
{
public:
	sid_chip();

	void set_chip_model(sid_model model);
	void set_sampling_parameters(double clock_frequency, double sampling_frequency);
	void reset();

	uint8_t read(uint16_t offset);
	// Queue a register write timestamped at an absolute CPU cycle.
	void write(uint16_t offset, uint8_t value, int64_t cycle);
	// Apply a register write immediately (used when audio output is disabled).
	void write_now(uint16_t offset, uint8_t value) { apply_register_write(offset & 0x1F, value); }

	void begin_frame();
	// Clock the chip for `cycles`, replaying queued writes at their timestamps,
	// appending up to max_samples 16-bit samples to buf. Returns samples written.
	int clock(int cycles, int16_t* buf, int64_t start_cycle, int max_samples);

	int64_t audio_cycle = 0; // absolute CPU cycle the chip has produced audio up to

private:
	struct queued_write { uint8_t offset; uint8_t value; int64_t cycle; };

	void apply_register_write(uint8_t offset, uint8_t value);
	void age_bus_value(int n);
	void voice_sync(bool sync);
	void init_fir_filter(double clock_frequency, double sampling_frequency);
	bool resampler_input(double sample);
	int resampler_output();

	voice voices[3];
	sid_filter filter6581;
	sid_filter filter8580;
	sid_filter* filter;
	external_filter ext_filter;

	sid_model model = sid_model::mos8580;
	double scale_factor = 0.4;
	int model_ttl = 0xA2000;

	uint8_t bus_value = 0;
	int bus_value_ttl = 0;
	int64_t next_voice_sync = 0x7FFFFFFF;

	double clock_frequency = 985248;
	double sampling_frequency = 44100;
	int cycles_per_sample = 0;
	int sample_offset = 0;
	double output_value = 0;
	double dc_offset = 0;

	static constexpr int FIR_LEN = 32;
	float fir_coeffs[FIR_LEN] = {};
	float fir_buffer[FIR_LEN] = {};
	int fir_index = 0;

	std::vector<queued_write> write_queue;
	size_t write_queue_index = 0;
};
