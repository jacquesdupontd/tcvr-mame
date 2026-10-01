// license:BSD-3-Clause
// copyright-holders:R. Belmont
/***************************************************************************

  Sega Model 1 sound board (68000 + 2x 315-5560 "MultiPCM")

  used for Model 1 and early Model 2 games

***************************************************************************/

#include "emu.h"
#include "segam1audio.h"

#include "machine/clock.h"
#include "speaker.h"

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace {

// TCVR (01/10, Daytona USA): the edge-pulse UART clock of the Model 2 main board (model2.cpp, 23/09), on the
// sound board's end of the same link. MAME's 500 kHz clock toggles a timer a million times a second, each one a
// scheduler slice in which every CPU of the machine is re-entered (Daytona: 1.52M clock events a second, 190 ms of
// callbacks, each CPU entered ~1M times). The i8251 only acts on edges (receive on rising, transmit on falling): a
// 250 kHz clock whose every toggle delivers one full pulse gives it the same pulses per second with half the
// events (transmit 1 us earlier inside a 32 us bit). debug.tcvr.m2.uartPulse=0 restores MAME's clock.
bool tcvr_uart_pulse_on()
{
#if defined(__ANDROID__)
	char value[PROP_VALUE_MAX] = {};
	// "" (the bench's way to release a property) counts as unset
	if (__system_property_get("debug.tcvr.m2.uartPulse", value) <= 0 || !value[0] || value[0] == '"') return true;
	return value[0] == '1';
#else
	return false;
#endif
}

// debug.tcvr.m2.uartN: pulses per clock event (1, 2 or 4), the same switch as the Model 2 main board's (model2.cpp)
int tcvr_uart_pulses()
{
#if defined(__ANDROID__)
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.tcvr.m2.uartN", value) <= 0 || !value[0] || value[0] == '"') return 1;
	int const n = atoi(value);
	return (n == 1 || n == 2 || n == 4) ? n : 1;
#else
	return 1;
#endif
}

int g_tcvr_m1_uart_n = 1;

} // anonymous namespace

void segam1audio_device::segam1audio_map(address_map &map)
{
	map(0x000000, 0x03ffff).rom();
	map(0x080000, 0x09ffff).rom().region("sndcpu", 0x20000); // mirror of upper ROM socket
	map(0xc20000, 0xc20003).rw(m_uart, FUNC(i8251_device::read), FUNC(i8251_device::write)).umask16(0x00ff);
	map(0xc40000, 0xc40007).rw(m_multipcm_1, FUNC(multipcm_device::read), FUNC(multipcm_device::write)).umask16(0x00ff);
	map(0xc40012, 0xc40013).nopw();
	map(0xc50000, 0xc50001).w(FUNC(segam1audio_device::m1_snd_mpcm_bnk1_w));
	map(0xc60000, 0xc60007).rw(m_multipcm_2, FUNC(multipcm_device::read), FUNC(multipcm_device::write)).umask16(0x00ff);
	map(0xc70000, 0xc70001).w(FUNC(segam1audio_device::m1_snd_mpcm_bnk2_w));
	map(0xd00000, 0xd00007).rw(m_ym, FUNC(ym3438_device::read), FUNC(ym3438_device::write)).umask16(0x00ff);
	map(0xf00000, 0xf0ffff).ram(); // real PCB actually has 2x 8kBx8-bit SRAMs (16kB total)
}

void segam1audio_device::mpcm1_map(address_map &map)
{
	map(0x000000, 0x0fffff).rom();
	map(0x100000, 0x1fffff).bankr(m_mpcmbank1);
}

void segam1audio_device::mpcm2_map(address_map &map)
{
	map(0x000000, 0x0fffff).rom();
	map(0x100000, 0x1fffff).bankr(m_mpcmbank2);
}

//**************************************************************************
//  GLOBAL VARIABLES
//**************************************************************************

DEFINE_DEVICE_TYPE(SEGAM1AUDIO, segam1audio_device, "segam1audio", "Sega Model 1 Sound Board")

//-------------------------------------------------
// device_add_mconfig - add device configuration
//-------------------------------------------------

void segam1audio_device::device_add_mconfig(machine_config &config)
{
	M68000(config, m_audiocpu, 20_MHz_XTAL / 2);  // verified on real h/w
	m_audiocpu->set_addrmap(AS_PROGRAM, &segam1audio_device::segam1audio_map);

	SPEAKER(config, "speaker", 2).front();

	YM3438(config, m_ym, 16_MHz_XTAL / 2);
	m_ym->add_route(0, "speaker", 0.30, 0);
	m_ym->add_route(1, "speaker", 0.30, 1);

	MULTIPCM(config, m_multipcm_1, 20_MHz_XTAL / 2);
	m_multipcm_1->set_addrmap(0, &segam1audio_device::mpcm1_map);
	m_multipcm_1->add_route(0, "speaker", 0.5, 0);
	m_multipcm_1->add_route(1, "speaker", 0.5, 1);

	MULTIPCM(config, m_multipcm_2, 20_MHz_XTAL / 2);
	m_multipcm_2->set_addrmap(0, &segam1audio_device::mpcm2_map);
	m_multipcm_2->add_route(0, "speaker", 0.5, 0);
	m_multipcm_2->add_route(1, "speaker", 0.5, 1);

	I8251(config, m_uart, 16_MHz_XTAL / 2); // T82C51
	m_uart->rxrdy_handler().set_inputline(m_audiocpu, M68K_IRQ_2);
	m_uart->txd_handler().set(FUNC(segam1audio_device::output_txd));

	if (tcvr_uart_pulse_on())
	{
		g_tcvr_m1_uart_n = tcvr_uart_pulses();
		clock_device &uart_clock(CLOCK(config, "uart_clock", 16_MHz_XTAL / 2 / 32 / g_tcvr_m1_uart_n));
		uart_clock.signal_handler().set(FUNC(segam1audio_device::tcvr_uart_pulse_w));
	}
	else
	{
		clock_device &uart_clock(CLOCK(config, "uart_clock", 16_MHz_XTAL / 2 / 16)); // 16 times 31.25kHz (standard Sega/MIDI sound data rate)
		uart_clock.signal_handler().set(m_uart, FUNC(i8251_device::write_txc));
		uart_clock.signal_handler().append(m_uart, FUNC(i8251_device::write_rxc));
	}

	// DAC output clocks measures:
	// BYTECLK = 10/8 (1.25MHz)
	// WORDCLK = 10/8/28 (44.642857kHz)
}

//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

//-------------------------------------------------
//  segam1audio_device - constructor
//-------------------------------------------------

segam1audio_device::segam1audio_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	device_t(mconfig, SEGAM1AUDIO, tag, owner, clock),
	m_audiocpu(*this, "sndcpu"),
	m_multipcm_1(*this, "pcm1"),
	m_multipcm_2(*this, "pcm2"),
	m_ym(*this, "ymsnd"),
	m_uart(*this, "uart"),
	m_multipcm1_region(*this, "pcm1"),
	m_multipcm2_region(*this, "pcm2"),
	m_mpcmbank1(*this, "m1pcm1_bank"),
	m_mpcmbank2(*this, "m1pcm2_bank"),
	m_rxd_handler(*this)
{
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void segam1audio_device::device_start()
{
	m_mpcmbank1->configure_entries(0, 4, m_multipcm1_region->base(), 0x100000);
	m_mpcmbank2->configure_entries(0, 4, m_multipcm2_region->base(), 0x100000);
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void segam1audio_device::device_reset()
{
	m_uart->write_cts(0);
}

void segam1audio_device::m1_snd_mpcm_bnk1_w(uint16_t data)
{
	m_mpcmbank1->set_entry(data & 3);
}

void segam1audio_device::m1_snd_mpcm_bnk2_w(uint16_t data)
{
	m_mpcmbank2->set_entry(data & 3);
}

void segam1audio_device::write_txd(int state)
{
	m_uart->write_rxd(state);
}

void segam1audio_device::tcvr_uart_pulse_w(int state)
{
	for (int i = 0; i < g_tcvr_m1_uart_n; i++)
	{
		m_uart->write_txc(1);
		m_uart->write_rxc(1);   // rising: receive
		m_uart->write_txc(0);   // falling: transmit
		m_uart->write_rxc(0);
	}
}

void segam1audio_device::output_txd(int state)
{
	m_rxd_handler(state);
}
