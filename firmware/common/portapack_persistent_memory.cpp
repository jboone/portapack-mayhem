/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "portapack_persistent_memory.hpp"

#include "portapack.hpp"
#include "hal.h"

#include "utility.hpp"

#include "memory_map.hpp"

#include "crc.hpp"

#include <algorithm>
#include <utility>

namespace portapack {
namespace persistent_memory {

constexpr rf::Frequency tuned_frequency_reset_value { 100000000 };

using ppb_range_t = range_t<ppb_t>;
constexpr ppb_range_t ppb_range { -99000, 99000 };
constexpr ppb_t ppb_reset_value { 0 };

using tone_mix_range_t = range_t<int32_t>;
constexpr tone_mix_range_t tone_mix_range { 10, 99 };
constexpr int32_t tone_mix_reset_value { 20 };

using afsk_freq_range_t = range_t<int32_t>;
constexpr afsk_freq_range_t afsk_freq_range { 1, 4000 };
constexpr int32_t afsk_mark_reset_value { 1200 };
constexpr int32_t afsk_space_reset_value { 2200 };

using modem_baudrate_range_t = range_t<int32_t>;
constexpr modem_baudrate_range_t modem_baudrate_range { 50, 9600 };
constexpr int32_t modem_baudrate_reset_value { 1200 };

/*using modem_bw_range_t = range_t<int32_t>;
constexpr modem_bw_range_t modem_bw_range { 1000, 50000 };
constexpr int32_t modem_bw_reset_value { 15000 };*/

using modem_repeat_range_t = range_t<int32_t>;
constexpr modem_repeat_range_t modem_repeat_range { 1, 99 };
constexpr int32_t modem_repeat_reset_value { 5 };

using clkout_freq_range_t = range_t<uint32_t>;
constexpr clkout_freq_range_t clkout_freq_range { 10, 60000 };
constexpr uint32_t clkout_freq_reset_value { 10000 };

static const uint32_t TOUCH_CALIBRATION_MAGIC = 0x074af82f;

/* struct must pack the same way on M4 and M0 cores. */
struct data_t {
	int64_t tuned_frequency;
	int32_t correction_ppb;
	uint32_t touch_calibration_magic;
	touch::Calibration touch_calibration;

	// Modem
	uint32_t modem_def_index;
	serial_format_t serial_format;
	int32_t modem_bw;
	int32_t afsk_mark_freq;
	int32_t afsk_space_freq;
	int32_t modem_baudrate;
	int32_t modem_repeat;

	// Play dead unlock
	uint32_t playdead_magic;
	uint32_t playing_dead;
	uint32_t playdead_sequence;
	
	// UI
	uint32_t ui_config;
	
	uint32_t pocsag_last_address;
	uint32_t pocsag_ignore_address;
	
	int32_t tone_mix;

	// Hardware
	uint32_t hardware_config;

	constexpr data_t() :
		tuned_frequency(tuned_frequency_reset_value),
		correction_ppb(ppb_reset_value),
		touch_calibration_magic(TOUCH_CALIBRATION_MAGIC),
		touch_calibration(touch::Calibration()),

		modem_def_index(0),			// TODO: Unused?
		serial_format(),
		modem_bw(15000),			// TODO: Unused?
		afsk_mark_freq(afsk_mark_reset_value),
		afsk_space_freq(afsk_space_reset_value),
		modem_baudrate(modem_baudrate_reset_value),
		modem_repeat(modem_repeat_reset_value),

		playdead_magic(),			// TODO: Unused?
		playing_dead(),				// TODO: Unused?
		playdead_sequence(),		// TODO: Unused?

		ui_config(					// TODO: Use `constexpr` setters in this constructor.
		      (1 << 31)							// Show splash
			| (1 << 28)							// Disable speaker
			| (clkout_freq_reset_value << 4)	// CLKOUT frequency.
			| (7 << 0)							// Backlight timer at maximum.
		),

		pocsag_last_address(0),		// TODO: A better default?
		pocsag_ignore_address(0),	// TODO: A better default?

		tone_mix(tone_mix_reset_value),

		hardware_config(0)
	{
	}
};

struct backup_ram_t {
private:
	uint32_t regfile[63];
	uint32_t check_value;

	static void copy(const backup_ram_t& src, backup_ram_t& dst) {
		for(size_t i=0; i<63; i++) {
			dst.regfile[i] = src.regfile[i];
		}
		dst.check_value = src.check_value;
	}

	static void copy_from_data_t(const data_t& src, backup_ram_t& dst) {
		const uint32_t* const src_words = (uint32_t*)&src;
		const size_t word_count = (sizeof(data_t) + 3) / 4;
		for(size_t i=0; i<63; i++) {
			if(i<word_count) {
				dst.regfile[i] = src_words[i];
			} else {
				dst.regfile[i] = 0;
			}
		}
	}

	uint32_t compute_check_value() {
		CRC<32> crc { 0x04c11db7, 0xffffffff, 0xffffffff };
		for(size_t i=0; i<63; i++) {
			const auto word = regfile[i];
			crc.process_byte((word >>  0) & 0xff);
			crc.process_byte((word >>  8) & 0xff);
			crc.process_byte((word >> 16) & 0xff);
			crc.process_byte((word >> 24) & 0xff);
		}
		return crc.checksum();
	}

public:
	/* default constructor */
	backup_ram_t() :
		check_value(0)
	{
		const data_t defaults = data_t();
		copy_from_data_t(defaults, *this);
	}

	/* copy-assignment operator */
	backup_ram_t& operator=(const backup_ram_t& src) {
		copy(src, *this);
		return *this;
	}

	/* Calculate a check value from `this`, and check against
	 * the stored value.
	 */
	bool is_valid() {
		return compute_check_value() == check_value;
	}

	/* Assuming `this` contains valid data, update the checksum
	 * and copy to the destination.
	 */
	void persist_to(backup_ram_t& dst) {
		check_value = compute_check_value();
		copy(*this, dst);
	}
};

static_assert(sizeof(backup_ram_t) == memory::map::backup_ram.size());
static_assert(sizeof(data_t) <= sizeof(backup_ram_t) - sizeof(uint32_t));

static backup_ram_t* const backup_ram = reinterpret_cast<backup_ram_t*>(memory::map::backup_ram.base());

static backup_ram_t cached_backup_ram;
static data_t* const data = reinterpret_cast<data_t*>(&cached_backup_ram);

namespace cache {

void defaults() {
	cached_backup_ram = backup_ram_t();
}

void init() {
	if(backup_ram->is_valid()) {
		// Copy valid persistent data into cache.
		cached_backup_ram = *backup_ram;
	} else {
		// Copy defaults into cache.
		defaults();
	}
}

void persist() {
	cached_backup_ram.persist_to(*backup_ram);
}

} /* namespace cache */

rf::Frequency tuned_frequency() {
	rf::tuning_range.reset_if_outside(data->tuned_frequency, tuned_frequency_reset_value);
	return data->tuned_frequency;
}

void set_tuned_frequency(const rf::Frequency new_value) {
	data->tuned_frequency = rf::tuning_range.clip(new_value);
}

ppb_t correction_ppb() {
	ppb_range.reset_if_outside(data->correction_ppb, ppb_reset_value);
	return data->correction_ppb;
}

void set_correction_ppb(const ppb_t new_value) {
	const auto clipped_value = ppb_range.clip(new_value);
	data->correction_ppb = clipped_value;
	portapack::clock_manager.set_reference_ppb(clipped_value);
}

void set_touch_calibration(const touch::Calibration& new_value) {
	data->touch_calibration = new_value;
	data->touch_calibration_magic = TOUCH_CALIBRATION_MAGIC;
}

const touch::Calibration& touch_calibration() {
	if( data->touch_calibration_magic != TOUCH_CALIBRATION_MAGIC ) {
		set_touch_calibration(touch::Calibration());
	}
	return data->touch_calibration;
}

int32_t tone_mix() {
	tone_mix_range.reset_if_outside(data->tone_mix, tone_mix_reset_value);
	return data->tone_mix;
}

void set_tone_mix(const int32_t new_value) {
	data->tone_mix = tone_mix_range.clip(new_value);
}

int32_t afsk_mark_freq() {
	afsk_freq_range.reset_if_outside(data->afsk_mark_freq, afsk_mark_reset_value);
	return data->afsk_mark_freq;
}

void set_afsk_mark(const int32_t new_value) {
	data->afsk_mark_freq = afsk_freq_range.clip(new_value);
}

int32_t afsk_space_freq() {
	afsk_freq_range.reset_if_outside(data->afsk_space_freq, afsk_space_reset_value);
	return data->afsk_space_freq;
}

void set_afsk_space(const int32_t new_value) {
	data->afsk_space_freq = afsk_freq_range.clip(new_value);
}

int32_t modem_baudrate() {
	modem_baudrate_range.reset_if_outside(data->modem_baudrate, modem_baudrate_reset_value);
	return data->modem_baudrate;
}

void set_modem_baudrate(const int32_t new_value) {
	data->modem_baudrate = modem_baudrate_range.clip(new_value);
}

/*int32_t modem_bw() {
	modem_bw_range.reset_if_outside(data->modem_bw, modem_bw_reset_value);
	return data->modem_bw;
}

void set_modem_bw(const int32_t new_value) {
	data->modem_bw = modem_bw_range.clip(new_value);
}*/

uint8_t modem_repeat() {
	modem_repeat_range.reset_if_outside(data->modem_repeat, modem_repeat_reset_value);
	return data->modem_repeat;
}

void set_modem_repeat(const uint32_t new_value) {
	data->modem_repeat = modem_repeat_range.clip(new_value);
}

serial_format_t serial_format() {
	return data->serial_format;
}

void set_serial_format(const serial_format_t new_value) {
	data->serial_format = new_value;
}

// ui_config is an uint32_t var storing information bitwise
// bits 0-2 store the backlight timer
// bits 4-19 (16 bits) store the clkout frequency
// bits 21-31 store the different single bit configs depicted below
// bit 20 store the display state of the gui return icon, hidden (0) or shown (1)

bool show_gui_return_icon(){ // add return icon in touchscreen menue
return data->ui_config & (1 << 20);
}

bool load_app_settings() { // load (last saved) app settings on startup of app
	return data->ui_config & (1 << 21);
}

bool save_app_settings() { // save app settings when closing app
	return data->ui_config & (1 << 22);
}
  
bool show_bigger_qr_code() { // show bigger QR code
	return data->ui_config & (1 << 23);
}

bool disable_touchscreen() { // Option to disable touch screen
	return data->ui_config & (1 << 24);
}

bool hide_clock() { // clock hidden from main menu
	return data->ui_config & (1 << 25);
}

bool clock_with_date() { // show clock with date, if not hidden
	return data->ui_config & (1 << 26);
}

bool clkout_enabled() {
	return data->ui_config & (1 << 27);
}

bool config_speaker() {
	return data->ui_config & (1 << 28);
}
bool stealth_mode() {
	return data->ui_config & (1 << 29);
}

bool config_login() {
	return data->ui_config & (1 << 30);
}

bool config_splash() {
	return data->ui_config & (1 << 31);
}

uint8_t config_cpld() {
	return data->hardware_config;
}

Optional<uint32_t> config_backlight_timer() {
	const auto table_index = data->ui_config & 7;
	if(table_index == 0) {
		return {};
	}

	const uint32_t timer_seconds[8] = { 0, 5, 15, 30, 60, 180, 300, 600 };
	return timer_seconds[table_index];
}

void set_gui_return_icon(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 20)) | (v << 20);
}

void set_load_app_settings(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 21)) | (v << 21);
}

void set_save_app_settings(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 22)) | (v << 22);
}

void set_show_bigger_qr_code(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 23)) | (v << 23);
}

void set_disable_touchscreen(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 24)) | (v << 24);
}

void set_clock_hidden(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 25)) | (v << 25);
}

void set_clock_with_date(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 26)) | (v << 26);
}

void set_clkout_enabled(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 27)) | (v << 27);
}

void set_config_speaker(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 28)) | (v << 28);
}

void set_stealth_mode(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 29)) | (v << 29);
}

void set_config_login(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 30)) | (v << 30);
}

void set_config_splash(bool v) {
	data->ui_config = (data->ui_config & ~(1 << 31)) | (v << 31);
}

void set_config_cpld(uint8_t i) {
	data->hardware_config = i;
}

void set_config_backlight_timer(uint32_t i) {
	data->ui_config = (data->ui_config & ~7) | (i & 7);
}

/*void set_config_textentry(uint8_t new_value) {
	data->ui_config = (data->ui_config & ~0b100) | ((new_value & 1) << 2);
}

uint8_t ui_config_textentry() {
	return ((data->ui_config >> 2) & 1);
}*/

/*void set_ui_config(const uint32_t new_value) {
	data->ui_config = new_value;
}*/

uint32_t pocsag_last_address() {
	return data->pocsag_last_address;
}

void set_pocsag_last_address(uint32_t address) {
	data->pocsag_last_address = address;
}

uint32_t pocsag_ignore_address() {
	return data->pocsag_ignore_address;
}

void set_pocsag_ignore_address(uint32_t address) {
	data->pocsag_ignore_address = address;
}

uint32_t clkout_freq() {
	uint16_t freq = (data->ui_config & 0x000FFFF0) >> 4;
	if(freq < clkout_freq_range.minimum || freq > clkout_freq_range.maximum) {
		data->ui_config = (data->ui_config & ~0x000FFFF0) | clkout_freq_reset_value << 4;
		return clkout_freq_reset_value;
	}
	else {
		return freq;
	}
}

void set_clkout_freq(uint32_t freq) {
	data->ui_config = (data->ui_config & ~0x000FFFF0) | (clkout_freq_range.clip(freq) << 4);
}


} /* namespace persistent_memory */
} /* namespace portapack */
