/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
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

#include "radio.hpp"

#include "rf_path.hpp"

#include "rffc507x.hpp"
#include "max2839.hpp"
#include "max5864.hpp"
#include "baseband_cpld.hpp"

#include "tuning.hpp"

#include "spi_arbiter.hpp"

#include "hackrf_hal.hpp"
#include "hackrf_gpio.hpp"
using namespace hackrf::one;

#include "portapack.hpp"

namespace radio {

static constexpr uint32_t ssp1_cpsr      = 2;

static constexpr uint32_t ssp_scr(
	const float pclk_f,
	const uint32_t cpsr,
	const float spi_f
) {
	return static_cast<uint8_t>(pclk_f / cpsr / spi_f - 1);
}

static constexpr SPIConfig ssp_config_max2839 = {
	.end_cb = NULL,
	.ssport = gpio_max2839_select.port(),
	.sspad = gpio_max2839_select.pad(),
	.cr0 =
		  CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max2839_spi_f))
		| CR0_FRFSPI
		| CR0_DSS16BIT
		,
	.cpsr = ssp1_cpsr,
};

static constexpr SPIConfig ssp_config_max5864 = {
	.end_cb = NULL,
	.ssport = gpio_max5864_select.port(),
	.sspad = gpio_max5864_select.pad(),
	.cr0 =
		  CR0_CLOCKRATE(ssp_scr(ssp1_pclk_f, ssp1_cpsr, max5864_spi_f))
		| CR0_FRFSPI
		| CR0_DSS8BIT
		,
	.cpsr = ssp1_cpsr,
};

static spi::arbiter::Arbiter ssp1_arbiter(portapack::ssp1);

static spi::arbiter::Target ssp1_target_max2839 {
	ssp1_arbiter,
	ssp_config_max2839
};

static spi::arbiter::Target ssp1_target_max5864 {
	ssp1_arbiter,
	ssp_config_max5864
};

static rf::path::Path rf_path;
rffc507x::RFFC507x first_if;
max2839::MAX2839 second_if { ssp1_target_max2839 };
static max5864::MAX5864 baseband_codec { ssp1_target_max5864 };
static baseband::CPLD baseband_cpld;

static rf::Direction direction { rf::Direction::Receive };
static bool baseband_invert = false;
static bool mixer_invert = false;

void init() {
	gpio_not_ant_pwr.write(1);
	gpio_not_ant_pwr.output();
	rf_path.init();
	first_if.init();
	second_if.init();
	baseband_codec.init();
	baseband_cpld.init();
}

void set_direction(const rf::Direction new_direction) {
	/* TODO: Refactor all the various "Direction" enumerations into one. */
	/* TODO: Only make changes if direction changes, but beware of clock enabling. */
	direction = new_direction;

	/*
	 * HackRF One r9 inverts analog baseband only for RX. Previous hardware
	 * revisions inverted analog baseband for neither direction because of
	 * compensation in the CPLD. If we ever simplify the CPLD to handle RX
	 * and TX the same way, we will need to update this baseband_invert
	 * logic.
	 */
	baseband_invert = (direction == rf::Direction::Receive);
	baseband_cpld.set_invert(mixer_invert ^ baseband_invert);

	second_if.set_mode((direction == rf::Direction::Transmit) ? max283x::Mode::Transmit : max283x::Mode::Receive);
	rf_path.set_direction(direction);

	baseband_codec.set_mode((direction == rf::Direction::Transmit) ? max5864::Mode::Transmit : max5864::Mode::Receive);
}

bool set_tuning_frequency(const rf::Frequency frequency) {
	const auto tuning_config = tuning::config::create(frequency);
	if( tuning_config.is_valid() ) {
		first_if.disable();

		if( tuning_config.first_lo_frequency ) {
			first_if.set_frequency(tuning_config.first_lo_frequency);
			first_if.enable();
		}

		const auto result_second_if = second_if.set_frequency(tuning_config.second_lo_frequency);

		rf_path.set_band(tuning_config.rf_path_band);
		mixer_invert = tuning_config.mixer_invert;
		baseband_cpld.set_invert(mixer_invert ^ baseband_invert);

		return result_second_if;
	} else {
		return false;
	}
}

void set_rf_amp(const bool rf_amp) {
	rf_path.set_rf_amp(rf_amp);
}

void set_lna_gain(const int_fast8_t db) {
	second_if.set_lna_gain(db);
}

void set_vga_gain(const int_fast8_t db) {
	second_if.set_vga_gain(db);
}

void set_tx_gain(const int_fast8_t db) {
	second_if.set_tx_vga_gain(db);
}

void set_baseband_filter_bandwidth(const uint32_t bandwidth_minimum) {
	second_if.set_lpf_rf_bandwidth(bandwidth_minimum);
}

void set_baseband_rate(const uint32_t rate) {
	portapack::clock_manager.set_sampling_frequency(rate);
}

void set_antenna_bias(const bool on) {
	/* Pull MOSFET gate low to turn on antenna bias. */
	gpio_not_ant_pwr.write(on ? 0 : 1);
}

void disable() {
	set_antenna_bias(false);
	baseband_codec.set_mode(max5864::Mode::Shutdown);
	second_if.set_mode(max283x::Mode::Standby);
	first_if.disable();
	set_rf_amp(false);
}

void enable(Configuration configuration) {
	configure(configuration);
}

void configure(Configuration configuration) {
	set_tuning_frequency(configuration.tuning_frequency);
	set_rf_amp(configuration.rf_amp);
	set_lna_gain(configuration.lna_gain);
	set_vga_gain(configuration.vga_gain);
	set_baseband_rate(configuration.baseband_rate);
	set_baseband_filter_bandwidth(configuration.baseband_filter_bandwidth);
	set_direction(configuration.direction);
}

namespace debug {

namespace first_if {

uint32_t register_read(const size_t register_number) {
	return radio::first_if.read(register_number);
}

} /* namespace first_if */

namespace second_if {

uint32_t register_read(const size_t register_number) {
	return radio::second_if.read(register_number);
}

uint8_t temp_sense() {
	return radio::second_if.temp_sense() & 0x1f;
}

} /* namespace second_if */

} /* namespace debug */

} /* namespace radio */
