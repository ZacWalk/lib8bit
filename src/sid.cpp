// lib8bit by Zac Walker
//
// MOS6581 / MOS8580 SID sound-chip emulation.

#include "sid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ===========================================================================
// Shared waveform + combined-waveform pulldown tables
// ===========================================================================

namespace
{
	int16_t g_wftable[4][4096];
	int16_t g_pulldown[2][5][4096];
	bool g_tables_built = false;

	struct wave_config
	{
		int dist_func; // 0 exponential, 1 linear, 2 quadratic
		double threshold;
		double topbit;
		double pulsestrength;
		double distance1;
		double distance2;
	};

	// Monte-Carlo derived combined-waveform parameters (AVERAGE), [model][waveform].
	const wave_config CONFIG_AVERAGE[2][5] = {
		{
			{0, 0.877322257, 1.11349654, 0, 2.14537621, 9.08618164},
			{1, 0.941692829, 1, 1.80072665, 0.033124879, 0.232303441},
			{1, 1.66494179, 1.03760982, 5.62705326, 0.291590303, 0.283631504},
			{1, 1.09762526, 0.975265801, 1.52196741, 0.151528224, 0.841949463},
			{0, 0.96, 1, 2.5, 1.1, 1.2},
		},
		{
			{0, 0.853578329, 1.09615636, 0, 1.8819375, 6.80794907},
			{0, 0.929835618, 1, 1.12836814, 1.10453653, 1.48065746},
			{2, 0.911938608, 0.996440411, 1.2278074, 0.000117214302, 0.18948476},
			{0, 0.938004673, 1.04827631, 1.21178246, 0.915959001, 1.42698038},
			{0, 0.95, 1, 1.15, 1, 1.45},
		},
	};

	double dist_func(int func, double distance, int i)
	{
		switch (func)
		{
		case 0: return std::pow(distance, -i);          // exponential
		case 1: return 1.0 / (1.0 + i * distance);       // linear
		default: return 1.0 / (1.0 + (i * i) * distance); // quadratic
		}
	}

	int tri_xor(int val)
	{
		return (((val & 0x800) == 0) ? val : (val ^ 0xFFF)) << 1;
	}

	int calculate_pulldown(const double* distance_table, double topbit, double pulsestrength,
	                       double threshold, int accumulator)
	{
		double bit[12];
		for (int i = 0; i < 12; i++) bit[i] = (accumulator & (1 << i)) ? 1.0 : 0.0;
		bit[11] *= topbit;

		double pulldown[12];
		for (int sb = 0; sb < 12; sb++)
		{
			double avg = 0, n = 0;
			for (int cb = 0; cb < 12; cb++)
			{
				if (cb == sb) continue;
				const double weight = distance_table[sb - cb + 12];
				avg += (1 - bit[cb]) * weight;
				n += weight;
			}
			avg -= pulsestrength;
			pulldown[sb] = avg / n;
		}

		int value = 0;
		for (int i = 0; i < 12; i++)
		{
			const double bit_value = bit[i] > 0 ? 1 - pulldown[i] : 0;
			if (bit_value > threshold) value |= 1 << i;
		}
		return value;
	}
}

void sid_init_tables()
{
	if (g_tables_built) return;
	g_tables_built = true;

	for (int idx = 0; idx < 4096; idx++)
	{
		const int saw = idx;
		g_wftable[0][idx] = static_cast<int16_t>(0xFFF);
		g_wftable[1][idx] = static_cast<int16_t>(tri_xor(idx));
		g_wftable[2][idx] = static_cast<int16_t>(saw);
		g_wftable[3][idx] = static_cast<int16_t>(saw & (saw << 1));
	}

	for (int model = 0; model < 2; model++)
	{
		for (int wf = 0; wf < 5; wf++)
		{
			const wave_config& cfg = CONFIG_AVERAGE[model][wf];
			double distance_table[25];
			for (int i = 0; i < 25; i++)
			{
				const double dist = i < 12 ? cfg.distance1 : cfg.distance2;
				distance_table[i] = dist_func(cfg.dist_func, dist, std::abs(i - 12));
			}
			for (int idx = 0; idx < 4096; idx++)
			{
				g_pulldown[model][wf][idx] = static_cast<int16_t>(
					calculate_pulldown(distance_table, cfg.topbit, cfg.pulsestrength, cfg.threshold, idx));
			}
		}
	}
}

// ===========================================================================
// Waveform generator
// ===========================================================================

void waveform_generator::set_waveform_models(const int16_t (*models)[4096])
{
	model_wave = models;
	if (waveform < 4) wave = models[waveform];
}

void waveform_generator::set_pulldown_models(const int16_t (*models)[4096])
{
	model_pulldown = models;
	if (models && waveform >= 3 && waveform < 8) pulldown = models[waveform - 3];
	else pulldown = nullptr;
}

void waveform_generator::reset()
{
	accumulator = 0x555555;
	freq = 0;
	pw = 0;
	waveform = 0;
	test = false;
	sync = false;
	msb_rising = false;
	shift_register = 0x7FFFFF;
	shift_latch = 0;
	shift_pipeline = 0;
	pulse_output = 0xFFF;
	waveform_output = 0;
	osc3 = 0;
	floating_output_ttl = 0;
	shift_register_reset = 0;
	no_noise = 0xFFF;
	no_pulse = 0xFFF;
	ring_msb_mask = 0;
	test_or_reset = false;
	tri_saw_pipeline = 0x555;
	noise_output = 0;
	no_noise_or_noise_output = no_noise;
	if (model_wave) wave = model_wave[0];
	pulldown = nullptr;
}

void waveform_generator::clock()
{
	if (test)
	{
		if (shift_register_reset != 0 && --shift_register_reset == 0)
		{
			shiftreg_bitfade();
			shift_latch = shift_register;
			set_noise_output();
		}
		test_or_reset = true;
		pulse_output = 0xFFF;
	}
	else
	{
		const uint32_t acc_old = accumulator;
		accumulator = (accumulator + freq) & 0xFFFFFF;
		const uint32_t acc_bits_set = ~acc_old & accumulator;
		msb_rising = (acc_bits_set & 0x800000) != 0;

		if (acc_bits_set & 0x080000)
		{
			shift_pipeline = 2;
		}
		else if (shift_pipeline != 0)
		{
			if (--shift_pipeline == 0)
			{
				shift_phase2(waveform);
			}
			else
			{
				test_or_reset = false;
				shift_latch = shift_register;
			}
		}
	}
}

uint16_t waveform_generator::output()
{
	if (waveform != 0)
	{
		const uint32_t prev_acc = prev_v ? prev_v->accumulator : 0;
		const uint32_t ix = (accumulator ^ (~prev_acc & ring_msb_mask)) >> 12;

		waveform_output = static_cast<uint16_t>((wave ? wave[ix] : 0) &
			(no_pulse | pulse_output) & no_noise_or_noise_output);

		if (pulldown)
		{
			const uint16_t pd = static_cast<uint16_t>(pulldown[waveform_output]);
			waveform_output = pd ? pd : waveform_output;
		}

		if ((waveform & 3) && !is6581)
		{
			osc3 = static_cast<uint16_t>(tri_saw_pipeline & (no_pulse | pulse_output) & no_noise_or_noise_output);
			if (pulldown)
			{
				const uint16_t pd = static_cast<uint16_t>(pulldown[osc3]);
				osc3 = pd ? pd : osc3;
			}
			tri_saw_pipeline = static_cast<uint16_t>(wave ? wave[ix] : 0);
		}
		else
		{
			osc3 = waveform_output;
		}

		if (is6581 && (waveform & 0x2) && (waveform_output & 0x800) == 0)
		{
			msb_rising = false;
			accumulator &= 0x7FFFFF;
		}
		write_shift_register();
	}
	else
	{
		if (floating_output_ttl != 0 && --floating_output_ttl == 0) wave_bitfade();
	}

	pulse_output = ((accumulator >> 12) >= pw) ? 0xFFF : 0x000;
	return waveform_output;
}

void waveform_generator::synchronize()
{
	if (msb_rising && next_v && next_v->sync) next_v->accumulator = 0;
}

void waveform_generator::shift_phase2(int waveform_new)
{
	const uint32_t bit0 = ((shift_latch >> 22) ^ (shift_latch >> 17)) & 1;
	shift_register = ((shift_latch << 1) | (test_or_reset ? 0 : bit0)) & 0x7FFFFF;
	if (waveform_new >= 8) set_noise_output();
}

void waveform_generator::set_noise_output()
{
	noise_output = static_cast<uint16_t>(
		((shift_register & (1u << 2)) << 9) |
		((shift_register & (1u << 4)) << 6) |
		((shift_register & (1u << 8)) << 1) |
		((shift_register & (1u << 11)) >> 3) |
		((shift_register & (1u << 13)) >> 6) |
		((shift_register & (1u << 17)) >> 12) |
		((shift_register & (1u << 20)) >> 16) |
		((shift_register & (1u << 22)) >> 22));
	set_no_noise_or_noise_output();
}

void waveform_generator::write_shift_register()
{
	if ((waveform & 8) != 0 && (waveform_output & 0x800) == 0)
	{
		if (!(waveform_output & (1 << 11))) shift_register &= ~(1u << 2);
		if (!(waveform_output & (1 << 10))) shift_register &= ~(1u << 4);
		if (!(waveform_output & (1 << 9))) shift_register &= ~(1u << 8);
		if (!(waveform_output & (1 << 8))) shift_register &= ~(1u << 11);
		if (!(waveform_output & (1 << 7))) shift_register &= ~(1u << 13);
		if (!(waveform_output & (1 << 5))) shift_register &= ~(1u << 17);
		if (!(waveform_output & (1 << 4))) shift_register &= ~(1u << 20);
		if (!(waveform_output & 1)) shift_register &= ~(1u << 22);
	}
}

void waveform_generator::write_control(uint8_t control)
{
	const int waveform_new = (control >> 4) & 0x0F;
	waveform = waveform_new;

	if (model_wave && waveform_new < 4) wave = model_wave[waveform_new];

	if (model_pulldown && waveform_new >= 3)
	{
		const int idx = waveform_new - 3;
		pulldown = idx < 5 ? model_pulldown[idx] : nullptr;
	}
	else
	{
		pulldown = nullptr;
	}

	ring_msb_mask = (control & 0x04) ? 0x800000 : 0;
	sync = (control & 0x02) != 0;
	const bool test_old = test;
	test = (control & 0x08) != 0;

	if (waveform_new >= 8) { no_noise = 0; set_noise_output(); }
	else { no_noise = 0xFFF; set_no_noise_or_noise_output(); }

	no_pulse = (waveform_new >= 4 && waveform_new < 8) ? 0 : 0xFFF;

	if (!test_old && test)
	{
		accumulator = 0;
		shift_register_reset = is6581 ? 50000 : 986000;
		shift_pipeline = 0;
		pulse_output = 0xFFF;
	}
	else if (test_old && !test)
	{
		shift_latch = shift_register;
		shift_pipeline = 1;
		floating_output_ttl = is6581 ? 54000 : 800000;
	}
}

// ===========================================================================
// Envelope generator
// ===========================================================================

namespace
{
	const uint16_t ADSR_TABLE[16] = {
		0x007F, 0x3000, 0x1E00, 0x0660, 0x0182, 0x5573, 0x000E, 0x3805,
		0x2424, 0x2220, 0x090C, 0x0ECD, 0x010E, 0x23F7, 0x5237, 0x64A8
	};
}

void envelope_generator::reset()
{
	envelope_pipeline = 0;
	state_pipeline = 0;
	attack = decay = sustain = release = 0;
	gate = false;
	reset_lfsr = true;
	exponential_counter = 0;
	exponential_counter_period = 1;
	new_exponential_counter_period = 0;
	state = RELEASE;
	counter_enabled = false;
	rate = ADSR_TABLE[release];
	envelope_counter = 0;
	lfsr = 0x7FFF;
	exponential_pipeline = 0;
	env3 = 0;
	next_state = RELEASE;
}

void envelope_generator::clock()
{
	env3 = envelope_counter;

	if (new_exponential_counter_period > 0)
	{
		exponential_counter_period = new_exponential_counter_period;
		new_exponential_counter_period = 0;
	}

	if (state_pipeline) state_change();

	if (envelope_pipeline != 0 && (--envelope_pipeline == 0))
	{
		if (counter_enabled)
		{
			if (state == ATTACK)
			{
				if (++envelope_counter == 0xFF)
				{
					next_state = DECAY_SUSTAIN;
					state_pipeline = 3;
				}
			}
			else if (state == DECAY_SUSTAIN || state == RELEASE)
			{
				if (--envelope_counter == 0x00) counter_enabled = false;
			}
			set_exponential_counter();
		}
	}
	else if (exponential_pipeline != 0 && (--exponential_pipeline == 0))
	{
		exponential_counter = 0;
		if ((state == DECAY_SUSTAIN && envelope_counter != sustain) || state == RELEASE)
			envelope_pipeline = 1;
	}
	else if (reset_lfsr)
	{
		lfsr = 0x7FFF;
		reset_lfsr = false;
		if (state == ATTACK)
		{
			exponential_counter = 0;
			envelope_pipeline = 2;
		}
		else if (counter_enabled && (++exponential_counter == exponential_counter_period))
		{
			exponential_pipeline = exponential_counter_period != 1 ? 2 : 1;
		}
	}

	if (lfsr != rate)
	{
		const uint16_t feedback = ((lfsr << 14) ^ (lfsr << 13)) & 0x4000;
		lfsr = static_cast<uint16_t>(((lfsr >> 1) | feedback) & 0x7FFF);
	}
	else
	{
		reset_lfsr = true;
	}
}

void envelope_generator::state_change()
{
	state_pipeline--;
	switch (next_state)
	{
	case ATTACK:
		if (state_pipeline == 1) rate = ADSR_TABLE[decay];
		else if (state_pipeline == 0)
		{
			state = ATTACK;
			rate = ADSR_TABLE[attack];
			counter_enabled = true;
		}
		break;
	case DECAY_SUSTAIN:
		if (state_pipeline == 0)
		{
			state = DECAY_SUSTAIN;
			rate = ADSR_TABLE[decay];
		}
		break;
	case RELEASE:
		if ((state == ATTACK && state_pipeline == 0) ||
			(state == DECAY_SUSTAIN && state_pipeline == 1))
		{
			state = RELEASE;
			rate = ADSR_TABLE[release];
		}
		break;
	default:
		break;
	}
}

void envelope_generator::set_exponential_counter()
{
	switch (envelope_counter)
	{
	case 0xFF: case 0x00: new_exponential_counter_period = 1; break;
	case 0x5D: new_exponential_counter_period = 2; break;
	case 0x36: new_exponential_counter_period = 4; break;
	case 0x1A: new_exponential_counter_period = 8; break;
	case 0x0E: new_exponential_counter_period = 16; break;
	case 0x06: new_exponential_counter_period = 30; break;
	default: break;
	}
}

void envelope_generator::write_control(uint8_t control)
{
	const bool gate_next = (control & 0x01) != 0;
	if (gate_next == gate) return;
	gate = gate_next;

	if (gate_next)
	{
		next_state = ATTACK;
		state_pipeline = 2;
		if (reset_lfsr || exponential_pipeline == 2)
			envelope_pipeline = (exponential_counter_period == 1 || exponential_pipeline == 2) ? 2 : 4;
		else if (exponential_pipeline == 1)
			state_pipeline = 3;
	}
	else
	{
		next_state = RELEASE;
		state_pipeline = envelope_pipeline > 0 ? 3 : 2;
	}
}

void envelope_generator::write_attack_decay(uint8_t v)
{
	attack = (v >> 4) & 0x0F;
	decay = v & 0x0F;
	if (state == ATTACK) rate = ADSR_TABLE[attack];
	else if (state == DECAY_SUSTAIN) rate = ADSR_TABLE[decay];
}

void envelope_generator::write_sustain_release(uint8_t v)
{
	sustain = static_cast<uint8_t>((v & 0xF0) | ((v >> 4) & 0x0F));
	release = v & 0x0F;
	if (state == RELEASE) rate = ADSR_TABLE[release];
}

// ===========================================================================
// Filters
// ===========================================================================

namespace { constexpr int MAX_FILTER_STATE = 0x7FFFFF; }

sid_filter::sid_filter(bool is_6581) : is6581(is_6581)
{
	constexpr double filter_curve = 0.5;
	for (int i = 0; i < 2048; i++)
	{
		const double fc_n = i / 2048.0;
		if (is6581)
		{
			const double x = fc_n * (1 + filter_curve * 0.5);
			freq_table[i] = static_cast<float>(std::pow(x, 1.0 + filter_curve) * 0.04);
		}
		else
		{
			freq_table[i] = static_cast<float>(0.002 + fc_n * 0.045);
		}
	}
	reset();
}

void sid_filter::reset()
{
	write_fc_lo(0);
	write_fc_hi(0);
	write_mode_vol(0);
	write_res_filt(0);
	vhp = vbp = vlp = 0;
}

void sid_filter::write_res_filt(uint8_t v)
{
	filt = v;
	update_resonance((v >> 4) & 0x0F);
	if (enabled)
	{
		filt1 = (filt & 0x01) != 0;
		filt2 = (filt & 0x02) != 0;
		filt3 = (filt & 0x04) != 0;
		filtE = (filt & 0x08) != 0;
	}
}

void sid_filter::write_mode_vol(uint8_t v)
{
	vol = v & 0x0F;
	lp = (v & 0x10) != 0;
	bp = (v & 0x20) != 0;
	hp = (v & 0x40) != 0;
	voice3off = (v & 0x80) != 0;
}

void sid_filter::enable(bool e)
{
	enabled = e;
	if (e) write_res_filt(filt);
	else filt1 = filt2 = filt3 = filtE = false;
}

int sid_filter::normalized_voice(voice* v) const
{
	const int wav = v->wave.output();
	const int env = v->envelope.output();
	return ((wav - 2048) * env) >> 3;
}

void sid_filter::route_voices(voice& v1, voice& v2, voice& v3, int& vi, int& vo)
{
	const int a = normalized_voice(&v1);
	const int b = normalized_voice(&v2);
	int c;
	if (voice3off && !filt3) { v3.wave.output(); c = 0; }
	else c = normalized_voice(&v3);

	vi = 0;
	if (filt1) vi += a;
	if (filt2) vi += b;
	if (filt3) vi += c;
	if (filtE) vi += ve;

	vo = 0;
	if (!filt1) vo += a;
	if (!filt2) vo += b;
	if (!filt3 && !voice3off) vo += c;
	if (!filtE) vo += ve;
}

void sid_filter::apply_filter(int vi)
{
	const double q_inv = resonance_coeff;
	vhp = static_cast<int>(vi * q_inv - vlp - vbp * q_inv);
	vbp = static_cast<int>(vbp + w0 * vhp);
	vlp = static_cast<int>(vlp + w0 * vbp);

	vhp = std::clamp(vhp, -MAX_FILTER_STATE, MAX_FILTER_STATE);
	vbp = std::clamp(vbp, -MAX_FILTER_STATE, MAX_FILTER_STATE);
	vlp = std::clamp(vlp, -MAX_FILTER_STATE, MAX_FILTER_STATE);
}

int sid_filter::mix_output(int vo)
{
	if (lp) vo += vlp;
	if (bp) vo += vbp;
	if (hp) vo += vhp;

	const int output = (vo * vol) >> 4;
	const int digi_dc = static_cast<int>((vol - 7.5) * 1024);
	return std::clamp(output + digi_dc, -32768, 32767);
}

int sid_filter::clock(voice& v1, voice& v2, voice& v3)
{
	int vi, vo;
	route_voices(v1, v2, v3, vi, vo);
	apply_filter(vi);
	return mix_output(vo);
}

void external_filter::set_clock_frequency(double frequency)
{
	const double w0lp = 2 * 3.14159265358979323846 * 16000 / frequency;
	const double w0hp = 2 * 3.14159265358979323846 * 1.6 / frequency;
	w0lp_1_s7 = static_cast<int64_t>(std::lround(w0lp * (1 << 7)));
	w0hp_1_s17 = static_cast<int64_t>(std::lround(w0hp * (1 << 17)));
}

int external_filter::clock(int input)
{
	const int64_t vi = static_cast<int64_t>(input) << 11;
	const int64_t d_vlp = (w0lp_1_s7 * (vi - vlp)) >> 7;
	const int64_t d_vhp = (w0hp_1_s17 * (vlp - vhp)) >> 17;
	vlp += d_vlp;
	vhp += d_vhp;
	return static_cast<int>((vlp - vhp) >> 11);
}

// ===========================================================================
// SID chip
// ===========================================================================

sid_chip::sid_chip()
	: filter6581(true), filter8580(false), filter(&filter8580)
{
	sid_init_tables();

	voices[0].wave.set_other(&voices[2].wave, &voices[1].wave);
	voices[1].wave.set_other(&voices[0].wave, &voices[2].wave);
	voices[2].wave.set_other(&voices[1].wave, &voices[0].wave);

	set_sampling_parameters(985248, 44100);
	set_chip_model(sid_model::mos8580);
	reset();
}

void sid_chip::set_chip_model(sid_model m)
{
	if (m == sid_model::mos6581) { filter = &filter6581; scale_factor = 0.3; model_ttl = 0x01D00; }
	else { filter = &filter8580; scale_factor = 0.4; model_ttl = 0xA2000; }
	model = m;

	const bool is6581 = m == sid_model::mos6581;
	const int model_idx = is6581 ? 0 : 1;
	for (auto& v : voices)
	{
		v.wave.set_model(is6581);
		v.wave.set_waveform_models(g_wftable);
		v.wave.set_pulldown_models(g_pulldown[model_idx]);
	}
}

void sid_chip::set_sampling_parameters(double clk, double samp)
{
	clock_frequency = clk;
	sampling_frequency = samp;
	cycles_per_sample = static_cast<int>(std::floor(clk / samp * 1024));
	sample_offset = 0;
	ext_filter.set_clock_frequency(clk);
	init_fir_filter(clk, samp);
}

void sid_chip::reset()
{
	for (auto& v : voices) v.reset();
	filter6581.reset();
	filter8580.reset();
	ext_filter.reset();
	bus_value = 0;
	bus_value_ttl = 0;
	write_queue.clear();
	write_queue_index = 0;
	dc_offset = 0;
	std::memset(fir_buffer, 0, sizeof(fir_buffer));
	fir_index = 0;
	sample_offset = 0;
	output_value = 0;
	voice_sync(false);
}

uint8_t sid_chip::read(uint16_t offset)
{
	switch (offset)
	{
	case 0x19: case 0x1A:
		bus_value = 0xFF;
		bus_value_ttl = model_ttl;
		break;
	case 0x1B:
		bus_value = voices[2].wave.read_osc();
		bus_value_ttl = model_ttl;
		break;
	case 0x1C:
		bus_value = voices[2].envelope.read_env();
		bus_value_ttl = model_ttl;
		break;
	default:
		bus_value_ttl /= 2;
		break;
	}
	return bus_value;
}

void sid_chip::write(uint16_t offset, uint8_t value, int64_t cycle)
{
	write_queue.push_back({static_cast<uint8_t>(offset & 0x1F), value, cycle});
}

void sid_chip::apply_register_write(uint8_t offset, uint8_t value)
{
	bus_value = value;
	bus_value_ttl = model_ttl;

	switch (offset)
	{
	case 0x00: voices[0].wave.write_freq_lo(value); break;
	case 0x01: voices[0].wave.write_freq_hi(value); break;
	case 0x02: voices[0].wave.write_pw_lo(value); break;
	case 0x03: voices[0].wave.write_pw_hi(value); break;
	case 0x04: voices[0].write_control(value); break;
	case 0x05: voices[0].envelope.write_attack_decay(value); break;
	case 0x06: voices[0].envelope.write_sustain_release(value); break;
	case 0x07: voices[1].wave.write_freq_lo(value); break;
	case 0x08: voices[1].wave.write_freq_hi(value); break;
	case 0x09: voices[1].wave.write_pw_lo(value); break;
	case 0x0A: voices[1].wave.write_pw_hi(value); break;
	case 0x0B: voices[1].write_control(value); break;
	case 0x0C: voices[1].envelope.write_attack_decay(value); break;
	case 0x0D: voices[1].envelope.write_sustain_release(value); break;
	case 0x0E: voices[2].wave.write_freq_lo(value); break;
	case 0x0F: voices[2].wave.write_freq_hi(value); break;
	case 0x10: voices[2].wave.write_pw_lo(value); break;
	case 0x11: voices[2].wave.write_pw_hi(value); break;
	case 0x12: voices[2].write_control(value); break;
	case 0x13: voices[2].envelope.write_attack_decay(value); break;
	case 0x14: voices[2].envelope.write_sustain_release(value); break;
	case 0x15: filter6581.write_fc_lo(value); filter8580.write_fc_lo(value); break;
	case 0x16: filter6581.write_fc_hi(value); filter8580.write_fc_hi(value); break;
	case 0x17: filter6581.write_res_filt(value); filter8580.write_res_filt(value); break;
	case 0x18: filter6581.write_mode_vol(value); filter8580.write_mode_vol(value); break;
	default: break;
	}
	voice_sync(false);
}

void sid_chip::age_bus_value(int n)
{
	if (bus_value_ttl != 0)
	{
		bus_value_ttl -= n;
		if (bus_value_ttl <= 0) { bus_value = 0; bus_value_ttl = 0; }
	}
}

void sid_chip::voice_sync(bool sync)
{
	if (sync)
		for (auto& v : voices) v.wave.synchronize();

	next_voice_sync = 0x7FFFFFFF;
	for (auto& v : voices)
	{
		const uint16_t freq = v.wave.read_freq();
		if (v.wave.read_test() || freq == 0 || !v.wave.read_following_voice_sync()) continue;
		const uint32_t acc = v.wave.read_accumulator();
		const int64_t this_sync = static_cast<int64_t>(((0x7FFFFF - acc) & 0xFFFFFF) / freq + 1);
		if (this_sync < next_voice_sync) next_voice_sync = this_sync;
	}
}

void sid_chip::begin_frame()
{
	write_queue_index = 0;
	std::stable_sort(write_queue.begin(), write_queue.end(),
		[](const queued_write& a, const queued_write& b) { return a.cycle < b.cycle; });
}

void sid_chip::init_fir_filter(double clk, double samp)
{
	const double oversample = clk / samp;
	const double cutoff = 0.9 / oversample;
	const double center = (FIR_LEN - 1) / 2.0;
	double sum = 0;

	for (int i = 0; i < FIR_LEN; i++)
	{
		const double x = i - center;
		double sinc;
		if (std::abs(x) < 0.0001) sinc = 1.0;
		else { const double arg = 3.14159265358979323846 * x * cutoff * 2; sinc = std::sin(arg) / arg; }
		const double window = 0.42 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (FIR_LEN - 1))
			+ 0.08 * std::cos(4 * 3.14159265358979323846 * i / (FIR_LEN - 1));
		fir_coeffs[i] = static_cast<float>(sinc * window);
		sum += fir_coeffs[i];
	}
	for (int i = 0; i < FIR_LEN; i++) fir_coeffs[i] = static_cast<float>(fir_coeffs[i] / sum);

	std::memset(fir_buffer, 0, sizeof(fir_buffer));
	fir_index = 0;
}

bool sid_chip::resampler_input(double sample)
{
	fir_buffer[fir_index] = static_cast<float>(sample);
	fir_index = (fir_index + 1) % FIR_LEN;

	bool ready = false;
	if (sample_offset < 1024)
	{
		double filtered = 0;
		int buf_idx = fir_index;
		for (int i = 0; i < FIR_LEN; i++)
		{
			buf_idx = (buf_idx - 1 + FIR_LEN) % FIR_LEN;
			filtered += fir_buffer[buf_idx] * fir_coeffs[i];
		}
		output_value = filtered;
		ready = true;
		sample_offset += cycles_per_sample;
	}
	sample_offset -= 1024;
	return ready;
}

int sid_chip::resampler_output()
{
	int sample = static_cast<int>(output_value * scale_factor);
	sample = std::clamp(sample, -32768, 32767);
	const double dc_filtered = sample - dc_offset;
	dc_offset += dc_filtered * 0.005;
	return std::clamp(static_cast<int>(dc_filtered), -32768, 32767);
}

int sid_chip::clock(int cycles, int16_t* buf, int64_t start_cycle, int max_samples)
{
	age_bus_value(cycles);
	int s = 0;
	const int64_t end_cycle = start_cycle + cycles;
	int64_t current_cycle = start_cycle;

	while (current_cycle < end_cycle)
	{
		while (write_queue_index < write_queue.size() && write_queue[write_queue_index].cycle <= current_cycle)
		{
			const queued_write& cmd = write_queue[write_queue_index++];
			apply_register_write(cmd.offset, cmd.value);
		}

		int64_t next_event = end_cycle;
		if (write_queue_index < write_queue.size())
			next_event = std::min(next_event, write_queue[write_queue_index].cycle);

		const int64_t to_run = std::min(next_event - current_cycle, next_voice_sync);
		if (to_run <= 0) { current_cycle++; continue; }

		for (int64_t i = 0; i < to_run; i++)
		{
			voices[0].wave.clock();
			voices[1].wave.clock();
			voices[2].wave.clock();
			voices[0].envelope.clock();
			voices[1].envelope.clock();
			voices[2].envelope.clock();

			const int sid_output = filter->clock(voices[0], voices[1], voices[2]);
			const int c64_output = ext_filter.clock(sid_output);

			if (resampler_input(c64_output))
			{
				const int sample = resampler_output();
				if (s < max_samples) buf[s++] = static_cast<int16_t>(sample);
			}
		}

		current_cycle += to_run;
		next_voice_sync -= to_run;
		if (next_voice_sync <= 0) voice_sync(true);
	}

	if (write_queue_index >= write_queue.size())
	{
		write_queue.clear();
		write_queue_index = 0;
	}

	return s;
}
