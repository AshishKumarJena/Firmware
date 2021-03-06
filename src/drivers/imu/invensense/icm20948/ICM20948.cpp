/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "ICM20948.hpp"

#include "AKM_AK09916_registers.hpp"

using namespace time_literals;

static constexpr int16_t combine(uint8_t msb, uint8_t lsb)
{
	return (msb << 8u) | lsb;
}

ICM20948::ICM20948(I2CSPIBusOption bus_option, int bus, uint32_t device, enum Rotation rotation, int bus_frequency,
		   spi_mode_e spi_mode, spi_drdy_gpio_t drdy_gpio, bool enable_magnetometer) :
	SPI(DRV_IMU_DEVTYPE_ICM20948, MODULE_NAME, bus, device, spi_mode, bus_frequency),
	I2CSPIDriver(MODULE_NAME, px4::device_bus_to_wq(get_device_id()), bus_option, bus),
	_drdy_gpio(drdy_gpio),
	_px4_accel(get_device_id(), ORB_PRIO_DEFAULT, rotation),
	_px4_gyro(get_device_id(), ORB_PRIO_DEFAULT, rotation)
{
	ConfigureSampleRate(_px4_gyro.get_max_rate_hz());

	if (enable_magnetometer) {
		_slave_ak09916_magnetometer = new AKM_AK09916::ICM20948_AK09916(*this, rotation);

		if (_slave_ak09916_magnetometer) {
			for (auto &r : _register_bank3_cfg) {
				if (r.reg == Register::BANK_3::I2C_SLV4_CTRL) {
					r.set_bits = I2C_SLV4_CTRL_BIT::I2C_MST_DLY;

				} else if (r.reg == Register::BANK_3::I2C_MST_CTRL) {
					r.set_bits = I2C_MST_CTRL_BIT::I2C_MST_P_NSR | I2C_MST_CTRL_BIT::I2C_MST_CLK_400_kHz;

				} else if (r.reg == Register::BANK_3::I2C_MST_DELAY_CTRL) {
					r.set_bits = I2C_MST_DELAY_CTRL_BIT::I2C_SLVX_DLY_EN;
				}
			}
		}
	}
}

ICM20948::~ICM20948()
{
	perf_free(_transfer_perf);
	perf_free(_bad_register_perf);
	perf_free(_bad_transfer_perf);
	perf_free(_fifo_empty_perf);
	perf_free(_fifo_overflow_perf);
	perf_free(_fifo_reset_perf);
	perf_free(_drdy_interval_perf);

	delete _slave_ak09916_magnetometer;
}

int ICM20948::init()
{
	int ret = SPI::init();

	if (ret != PX4_OK) {
		DEVICE_DEBUG("SPI::init failed (%i)", ret);
		return ret;
	}

	return Reset() ? 0 : -1;
}

bool ICM20948::Reset()
{
	_state = STATE::RESET;
	ScheduleClear();
	ScheduleNow();
	return true;
}

void ICM20948::exit_and_cleanup()
{
	DataReadyInterruptDisable();
	I2CSPIDriverBase::exit_and_cleanup();
}

void ICM20948::print_status()
{
	I2CSPIDriverBase::print_status();
	PX4_INFO("FIFO empty interval: %d us (%.3f Hz)", _fifo_empty_interval_us,
		 static_cast<double>(1000000 / _fifo_empty_interval_us));

	perf_print_counter(_transfer_perf);
	perf_print_counter(_bad_register_perf);
	perf_print_counter(_bad_transfer_perf);
	perf_print_counter(_fifo_empty_perf);
	perf_print_counter(_fifo_overflow_perf);
	perf_print_counter(_fifo_reset_perf);
	perf_print_counter(_drdy_interval_perf);

	_px4_accel.print_status();
	_px4_gyro.print_status();

	if (_slave_ak09916_magnetometer) {
		_slave_ak09916_magnetometer->PrintInfo();
	}
}

int ICM20948::probe()
{
	const uint8_t whoami = RegisterRead(Register::BANK_0::WHO_AM_I);

	if (whoami != WHOAMI) {
		DEVICE_DEBUG("unexpected WHO_AM_I 0x%02x", whoami);
		return PX4_ERROR;
	}

	return PX4_OK;
}

void ICM20948::RunImpl()
{
	switch (_state) {
	case STATE::RESET:
		// PWR_MGMT_1: Device Reset
		RegisterWrite(Register::BANK_0::PWR_MGMT_1, PWR_MGMT_1_BIT::DEVICE_RESET);
		_reset_timestamp = hrt_absolute_time();
		_state = STATE::WAIT_FOR_RESET;
		ScheduleDelayed(10_ms);
		break;

	case STATE::WAIT_FOR_RESET:

		// The reset value is 0x00 for all registers other than the registers below
		if ((RegisterRead(Register::BANK_0::WHO_AM_I) == WHOAMI)
		    && (RegisterRead(Register::BANK_0::PWR_MGMT_1) == 0x41)) {

			// if reset succeeded then configure
			_state = STATE::CONFIGURE;
			ScheduleDelayed(10_ms);

		} else {
			// RESET not complete
			if (hrt_elapsed_time(&_reset_timestamp) > 100_ms) {
				PX4_DEBUG("Reset failed, retrying");
				_state = STATE::RESET;
				ScheduleDelayed(100_ms);

			} else {
				PX4_DEBUG("Reset not complete, check again in 10 ms");
				ScheduleDelayed(10_ms);
			}
		}

		break;

	case STATE::CONFIGURE:
		if (Configure()) {

			// start AK09916 magnetometer (I2C aux)
			if (_slave_ak09916_magnetometer) {
				_slave_ak09916_magnetometer->Reset();
			}

			// if configure succeeded then start reading from FIFO
			_state = STATE::FIFO_READ;

			if (DataReadyInterruptConfigure()) {
				_data_ready_interrupt_enabled = true;

				// backup schedule as a watchdog timeout
				ScheduleDelayed(10_ms);

			} else {
				_data_ready_interrupt_enabled = false;
				ScheduleOnInterval(_fifo_empty_interval_us, _fifo_empty_interval_us);
			}

			FIFOReset();

		} else {
			PX4_DEBUG("Configure failed, retrying");
			// try again in 10 ms
			ScheduleDelayed(10_ms);
		}

		break;

	case STATE::FIFO_READ: {
			hrt_abstime timestamp_sample = 0;
			uint8_t samples = 0;

			if (_data_ready_interrupt_enabled) {
				// re-schedule as watchdog timeout
				ScheduleDelayed(10_ms);

				// timestamp set in data ready interrupt
				if (!_force_fifo_count_check) {
					samples = _fifo_read_samples.load();

				} else {
					const uint16_t fifo_count = FIFOReadCount();
					samples = (fifo_count / sizeof(FIFO::DATA) / SAMPLES_PER_TRANSFER) * SAMPLES_PER_TRANSFER; // round down to nearest
				}

				timestamp_sample = _fifo_watermark_interrupt_timestamp;
			}

			bool failure = false;

			// manually check FIFO count if no samples from DRDY or timestamp looks bogus
			if (!_data_ready_interrupt_enabled || (samples == 0)
			    || (hrt_elapsed_time(&timestamp_sample) > (_fifo_empty_interval_us / 2))) {

				// use the time now roughly corresponding with the last sample we'll pull from the FIFO
				timestamp_sample = hrt_absolute_time();
				const uint16_t fifo_count = FIFOReadCount();
				samples = (fifo_count / sizeof(FIFO::DATA) / SAMPLES_PER_TRANSFER) * SAMPLES_PER_TRANSFER; // round down to nearest
			}

			if (samples > FIFO_MAX_SAMPLES) {
				// not technically an overflow, but more samples than we expected or can publish
				perf_count(_fifo_overflow_perf);
				failure = true;
				FIFOReset();

			} else if (samples >= SAMPLES_PER_TRANSFER) {
				// require at least SAMPLES_PER_TRANSFER (we want at least 1 new accel sample per transfer)
				if (!FIFORead(timestamp_sample, samples)) {
					failure = true;
					_px4_accel.increase_error_count();
					_px4_gyro.increase_error_count();
				}

			} else if (samples == 0) {
				failure = true;
				perf_count(_fifo_empty_perf);
			}

			if (failure || hrt_elapsed_time(&_last_config_check_timestamp) > 10_ms) {
				// check BANK_0 & BANK_2 registers incrementally
				if (RegisterCheck(_register_bank0_cfg[_checked_register_bank0], true)
				    && RegisterCheck(_register_bank2_cfg[_checked_register_bank2], true)
				    && RegisterCheck(_register_bank3_cfg[_checked_register_bank3], true)
				   ) {
					_last_config_check_timestamp = timestamp_sample;
					_checked_register_bank0 = (_checked_register_bank0 + 1) % size_register_bank0_cfg;
					_checked_register_bank2 = (_checked_register_bank2 + 1) % size_register_bank2_cfg;
					_checked_register_bank3 = (_checked_register_bank3 + 1) % size_register_bank3_cfg;

				} else {
					// register check failed, force reconfigure
					PX4_DEBUG("Health check failed, reconfiguring");
					_state = STATE::CONFIGURE;
					ScheduleNow();
				}

			} else {
				// periodically update temperature (1 Hz)
				if (hrt_elapsed_time(&_temperature_update_timestamp) > 1_s) {
					UpdateTemperature();
					_temperature_update_timestamp = timestamp_sample;
				}
			}
		}

		break;
	}
}

void ICM20948::ConfigureAccel()
{
	const uint8_t ACCEL_FS_SEL = RegisterRead(Register::BANK_2::ACCEL_CONFIG) & (Bit2 | Bit1); // 2:1 ACCEL_FS_SEL[1:0]

	switch (ACCEL_FS_SEL) {
	case ACCEL_FS_SEL_2G:
		_px4_accel.set_scale(CONSTANTS_ONE_G / 16384.f);
		_px4_accel.set_range(2.f * CONSTANTS_ONE_G);
		break;

	case ACCEL_FS_SEL_4G:
		_px4_accel.set_scale(CONSTANTS_ONE_G / 8192.f);
		_px4_accel.set_range(4.f * CONSTANTS_ONE_G);
		break;

	case ACCEL_FS_SEL_8G:
		_px4_accel.set_scale(CONSTANTS_ONE_G / 4096.f);
		_px4_accel.set_range(8.f * CONSTANTS_ONE_G);
		break;

	case ACCEL_FS_SEL_16G:
		_px4_accel.set_scale(CONSTANTS_ONE_G / 2048.f);
		_px4_accel.set_range(16.f * CONSTANTS_ONE_G);
		break;
	}
}

void ICM20948::ConfigureGyro()
{
	const uint8_t GYRO_FS_SEL = RegisterRead(Register::BANK_2::GYRO_CONFIG_1) & (Bit2 | Bit1); // 2:1 GYRO_FS_SEL[1:0]

	switch (GYRO_FS_SEL) {
	case GYRO_FS_SEL_250_DPS:
		_px4_gyro.set_scale(math::radians(1.f / 131.f));
		_px4_gyro.set_range(math::radians(250.f));
		break;

	case GYRO_FS_SEL_500_DPS:
		_px4_gyro.set_scale(math::radians(1.f / 65.5f));
		_px4_gyro.set_range(math::radians(500.f));
		break;

	case GYRO_FS_SEL_1000_DPS:
		_px4_gyro.set_scale(math::radians(1.f / 32.8f));
		_px4_gyro.set_range(math::radians(1000.f));
		break;

	case GYRO_FS_SEL_2000_DPS:
		_px4_gyro.set_scale(math::radians(1.f / 16.4f));
		_px4_gyro.set_range(math::radians(2000.f));
		break;
	}
}

void ICM20948::ConfigureSampleRate(int sample_rate)
{
	if (sample_rate == 0) {
		sample_rate = 800; // default to ~800 Hz
	}

	// round down to nearest FIFO sample dt * SAMPLES_PER_TRANSFER
	const float min_interval = SAMPLES_PER_TRANSFER * FIFO_SAMPLE_DT;
	_fifo_empty_interval_us = math::max(roundf((1e6f / (float)sample_rate) / min_interval) * min_interval, min_interval);

	_fifo_gyro_samples = roundf(math::min((float)_fifo_empty_interval_us / (1e6f / GYRO_RATE), (float)FIFO_MAX_SAMPLES));

	// recompute FIFO empty interval (us) with actual gyro sample limit
	_fifo_empty_interval_us = _fifo_gyro_samples * (1e6f / GYRO_RATE);

	_fifo_accel_samples = roundf(math::min(_fifo_empty_interval_us / (1e6f / ACCEL_RATE), (float)FIFO_MAX_SAMPLES));

	_px4_accel.set_update_rate(1e6f / _fifo_empty_interval_us);
	_px4_gyro.set_update_rate(1e6f / _fifo_empty_interval_us);
}

void ICM20948::SelectRegisterBank(enum REG_BANK_SEL_BIT bank)
{
	if (bank != _last_register_bank) {
		// select BANK_0
		uint8_t cmd_bank_sel[2] {};
		cmd_bank_sel[0] = static_cast<uint8_t>(Register::BANK_0::REG_BANK_SEL);
		cmd_bank_sel[1] = bank;
		transfer(cmd_bank_sel, cmd_bank_sel, sizeof(cmd_bank_sel));

		_last_register_bank = bank;
	}
}

bool ICM20948::Configure()
{
	bool success = true;

	for (const auto &reg : _register_bank0_cfg) {
		if (!RegisterCheck(reg)) {
			success = false;
		}
	}

	for (const auto &reg : _register_bank2_cfg) {
		if (!RegisterCheck(reg)) {
			success = false;
		}
	}

	for (const auto &reg : _register_bank3_cfg) {
		if (!RegisterCheck(reg)) {
			success = false;
		}
	}

	ConfigureAccel();
	ConfigureGyro();

	return success;
}

int ICM20948::DataReadyInterruptCallback(int irq, void *context, void *arg)
{
	static_cast<ICM20948 *>(arg)->DataReady();
	return 0;
}

void ICM20948::DataReady()
{
	perf_count(_drdy_interval_perf);

	if (_data_ready_count.fetch_add(1) >= (_fifo_gyro_samples - 1)) {
		_data_ready_count.store(0);
		_fifo_watermark_interrupt_timestamp = hrt_absolute_time();
		_fifo_read_samples.store(_fifo_gyro_samples);
		ScheduleNow();
	}
}

bool ICM20948::DataReadyInterruptConfigure()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	// Setup data ready on falling edge
	return px4_arch_gpiosetevent(_drdy_gpio, false, true, true, &DataReadyInterruptCallback, this) == 0;
}

bool ICM20948::DataReadyInterruptDisable()
{
	if (_drdy_gpio == 0) {
		return false;
	}

	return px4_arch_gpiosetevent(_drdy_gpio, false, false, false, nullptr, nullptr) == 0;
}

template <typename T>
bool ICM20948::RegisterCheck(const T &reg_cfg, bool notify)
{
	bool success = true;

	const uint8_t reg_value = RegisterRead(reg_cfg.reg);

	if (reg_cfg.set_bits && ((reg_value & reg_cfg.set_bits) != reg_cfg.set_bits)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not set)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.set_bits);
		success = false;
	}

	if (reg_cfg.clear_bits && ((reg_value & reg_cfg.clear_bits) != 0)) {
		PX4_DEBUG("0x%02hhX: 0x%02hhX (0x%02hhX not cleared)", (uint8_t)reg_cfg.reg, reg_value, reg_cfg.clear_bits);
		success = false;
	}

	if (!success) {
		RegisterSetAndClearBits(reg_cfg.reg, reg_cfg.set_bits, reg_cfg.clear_bits);

		if (notify) {
			perf_count(_bad_register_perf);
			_px4_accel.increase_error_count();
			_px4_gyro.increase_error_count();
		}
	}

	return success;
}

template <typename T>
uint8_t ICM20948::RegisterRead(T reg)
{
	SelectRegisterBank(reg);

	uint8_t cmd[2] {};
	cmd[0] = static_cast<uint8_t>(reg) | DIR_READ;
	transfer(cmd, cmd, sizeof(cmd));
	return cmd[1];
}

template <typename T>
void ICM20948::RegisterWrite(T reg, uint8_t value)
{
	SelectRegisterBank(reg);

	uint8_t cmd[2] { (uint8_t)reg, value };
	transfer(cmd, cmd, sizeof(cmd));
}

template <typename T>
void ICM20948::RegisterSetAndClearBits(T reg, uint8_t setbits, uint8_t clearbits)
{
	const uint8_t orig_val = RegisterRead(reg);
	uint8_t val = orig_val;

	if (setbits) {
		val |= setbits;
	}

	if (clearbits) {
		val &= ~clearbits;
	}

	RegisterWrite(reg, val);
}

uint16_t ICM20948::FIFOReadCount()
{
	SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

	// read FIFO count
	uint8_t fifo_count_buf[3] {};
	fifo_count_buf[0] = static_cast<uint8_t>(Register::BANK_0::FIFO_COUNTH) | DIR_READ;

	if (transfer(fifo_count_buf, fifo_count_buf, sizeof(fifo_count_buf)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return 0;
	}

	return combine(fifo_count_buf[1], fifo_count_buf[2]);
}

bool ICM20948::FIFORead(const hrt_abstime &timestamp_sample, uint16_t samples)
{
	perf_begin(_transfer_perf);

	SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

	FIFOTransferBuffer buffer{};
	const size_t transfer_size = math::min(samples * sizeof(FIFO::DATA) + 3, FIFO::SIZE);

	if (transfer((uint8_t *)&buffer, (uint8_t *)&buffer, transfer_size) != PX4_OK) {
		perf_end(_transfer_perf);
		perf_count(_bad_transfer_perf);
		return false;
	}

	perf_end(_transfer_perf);

	const uint16_t fifo_count_bytes = combine(buffer.FIFO_COUNTH, buffer.FIFO_COUNTL);
	const uint16_t fifo_count_samples = fifo_count_bytes / sizeof(FIFO::DATA);

	if (fifo_count_samples == 0) {
		perf_count(_fifo_empty_perf);
		return false;
	}

	if (fifo_count_bytes >= FIFO::SIZE) {
		perf_count(_fifo_overflow_perf);
		FIFOReset();
		return false;
	}

	const uint16_t valid_samples = math::min(samples, fifo_count_samples);

	if (fifo_count_samples < samples) {
		// force check if there is somehow fewer samples actually in the FIFO (potentially a serious error)
		_force_fifo_count_check = true;

	} else if (fifo_count_samples >= samples + 2) {
		// if we're more than a couple samples behind force FIFO_COUNT check
		_force_fifo_count_check = true;

	} else {
		// skip earlier FIFO_COUNT and trust DRDY count if we're in sync
		_force_fifo_count_check = false;
	}

	if (valid_samples > 0) {
		ProcessGyro(timestamp_sample, buffer, valid_samples);

		if (ProcessAccel(timestamp_sample, buffer, valid_samples)) {
			return true;
		}
	}

	// force FIFO count check if there was any other error
	_force_fifo_count_check = true;

	return false;
}

void ICM20948::FIFOReset()
{
	perf_count(_fifo_reset_perf);

	// FIFO_RST: reset FIFO
	RegisterSetBits(Register::BANK_0::FIFO_RST, FIFO_RST_BIT::FIFO_RESET);
	RegisterClearBits(Register::BANK_0::FIFO_RST, FIFO_RST_BIT::FIFO_RESET);

	// reset while FIFO is disabled
	_data_ready_count.store(0);
	_fifo_watermark_interrupt_timestamp = 0;
	_fifo_read_samples.store(0);
}

static bool fifo_accel_equal(const FIFO::DATA &f0, const FIFO::DATA &f1)
{
	return (memcmp(&f0.ACCEL_XOUT_H, &f1.ACCEL_XOUT_H, 6) == 0);
}

bool ICM20948::ProcessAccel(const hrt_abstime &timestamp_sample, const FIFOTransferBuffer &buffer,
			    const uint8_t samples)
{
	PX4Accelerometer::FIFOSample accel;
	accel.timestamp_sample = timestamp_sample;
	accel.dt = _fifo_empty_interval_us / _fifo_accel_samples;

	bool bad_data = false;

	// accel data is doubled in FIFO, but might be shifted
	int accel_first_sample = 1;

	if (samples >= 4) {
		if (fifo_accel_equal(buffer.f[0], buffer.f[1]) && fifo_accel_equal(buffer.f[2], buffer.f[3])) {
			// [A0, A1, A2, A3]
			//  A0==A1, A2==A3
			accel_first_sample = 1;

		} else if (fifo_accel_equal(buffer.f[1], buffer.f[2])) {
			// [A0, A1, A2, A3]
			//  A0, A1==A2, A3
			accel_first_sample = 0;

		} else {
			perf_count(_bad_transfer_perf);
			bad_data = true;
		}
	}

	int accel_samples = 0;

	for (int i = accel_first_sample; i < samples; i = i + 2) {
		const FIFO::DATA &fifo_sample = buffer.f[i];
		int16_t accel_x = combine(fifo_sample.ACCEL_XOUT_H, fifo_sample.ACCEL_XOUT_L);
		int16_t accel_y = combine(fifo_sample.ACCEL_YOUT_H, fifo_sample.ACCEL_YOUT_L);
		int16_t accel_z = combine(fifo_sample.ACCEL_ZOUT_H, fifo_sample.ACCEL_ZOUT_L);

		// sensor's frame is +x forward, +y left, +z up
		//  flip y & z to publish right handed with z down (x forward, y right, z down)
		accel.x[accel_samples] = accel_x;
		accel.y[accel_samples] = (accel_y == INT16_MIN) ? INT16_MAX : -accel_y;
		accel.z[accel_samples] = (accel_z == INT16_MIN) ? INT16_MAX : -accel_z;
		accel_samples++;
	}

	accel.samples = accel_samples;

	_px4_accel.updateFIFO(accel);

	return !bad_data;
}

void ICM20948::ProcessGyro(const hrt_abstime &timestamp_sample, const FIFOTransferBuffer &buffer, const uint8_t samples)
{
	PX4Gyroscope::FIFOSample gyro;
	gyro.timestamp_sample = timestamp_sample;
	gyro.samples = samples;
	gyro.dt = _fifo_empty_interval_us / _fifo_gyro_samples;

	for (int i = 0; i < samples; i++) {
		const FIFO::DATA &fifo_sample = buffer.f[i];

		const int16_t gyro_x = combine(fifo_sample.GYRO_XOUT_H, fifo_sample.GYRO_XOUT_L);
		const int16_t gyro_y = combine(fifo_sample.GYRO_YOUT_H, fifo_sample.GYRO_YOUT_L);
		const int16_t gyro_z = combine(fifo_sample.GYRO_ZOUT_H, fifo_sample.GYRO_ZOUT_L);

		// sensor's frame is +x forward, +y left, +z up
		//  flip y & z to publish right handed with z down (x forward, y right, z down)
		gyro.x[i] = gyro_x;
		gyro.y[i] = (gyro_y == INT16_MIN) ? INT16_MAX : -gyro_y;
		gyro.z[i] = (gyro_z == INT16_MIN) ? INT16_MAX : -gyro_z;
	}

	_px4_gyro.updateFIFO(gyro);
}

void ICM20948::UpdateTemperature()
{
	SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

	// read current temperature
	uint8_t temperature_buf[3] {};
	temperature_buf[0] = static_cast<uint8_t>(Register::BANK_0::TEMP_OUT_H) | DIR_READ;

	if (transfer(temperature_buf, temperature_buf, sizeof(temperature_buf)) != PX4_OK) {
		perf_count(_bad_transfer_perf);
		return;
	}

	const int16_t TEMP_OUT = combine(temperature_buf[1], temperature_buf[2]);
	const float TEMP_degC = (TEMP_OUT / TEMPERATURE_SENSITIVITY) + TEMPERATURE_OFFSET;

	if (PX4_ISFINITE(TEMP_degC)) {
		_px4_accel.set_temperature(TEMP_degC);
		_px4_gyro.set_temperature(TEMP_degC);

		if (_slave_ak09916_magnetometer) {
			_slave_ak09916_magnetometer->set_temperature(TEMP_degC);
		}
	}
}

void ICM20948::I2CSlaveRegisterStartRead(uint8_t slave_i2c_addr, uint8_t reg)
{
	I2CSlaveExternalSensorDataEnable(slave_i2c_addr, reg, 1);
}

void ICM20948::I2CSlaveRegisterWrite(uint8_t slave_i2c_addr, uint8_t reg, uint8_t val)
{
	RegisterWrite(Register::BANK_3::I2C_SLV0_ADDR, slave_i2c_addr);
	RegisterWrite(Register::BANK_3::I2C_SLV0_REG, reg);
	RegisterWrite(Register::BANK_3::I2C_SLV0_DO, val);
	RegisterSetBits(Register::BANK_3::I2C_SLV0_CTRL, 1);
}

void ICM20948::I2CSlaveExternalSensorDataEnable(uint8_t slave_i2c_addr, uint8_t reg, uint8_t size)
{
	//RegisterWrite(Register::I2C_SLV0_ADDR, 0); // disable slave
	RegisterWrite(Register::BANK_3::I2C_SLV0_ADDR, slave_i2c_addr | I2C_SLV0_ADDR_BIT::I2C_SLV0_RNW);
	RegisterWrite(Register::BANK_3::I2C_SLV0_REG, reg);
	RegisterWrite(Register::BANK_3::I2C_SLV0_CTRL, size | I2C_SLV0_CTRL_BIT::I2C_SLV0_EN);
}

bool ICM20948::I2CSlaveExternalSensorDataRead(uint8_t *buffer, uint8_t length)
{
	bool ret = false;

	if (buffer != nullptr && length <= 24) {
		SelectRegisterBank(REG_BANK_SEL_BIT::USER_BANK_0);

		// max EXT_SENS_DATA 24 bytes
		uint8_t transfer_buffer[24 + 1] {};
		transfer_buffer[0] = static_cast<uint8_t>(Register::BANK_0::EXT_SLV_SENS_DATA_00) | DIR_READ;

		if (transfer(transfer_buffer, transfer_buffer, length + 1) == PX4_OK) {
			ret = true;
		}

		// copy data after cmd back to return buffer
		memcpy(buffer, &transfer_buffer[1], length);
	}

	return ret;
}
