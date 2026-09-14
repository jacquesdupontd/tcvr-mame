// license:BSD-3-Clause
// Native ArcadeXR boundary.  This deliberately owns MAME's small runtime glue;
// Android/OpenXR code never needs to know a MAME driver implementation detail.
#include "emu.h"
#include "drivenum.h"
#include "emuopts.h"
#include "main.h"
#include "osdepend.h"
#include "rendlay.h"

#include "ui/uimain.h"

#include <android/log.h>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace {

constexpr char kLogTag[] = "TCVR_MAME";

struct tcvr_video_store
{
	std::mutex mutex;
	std::vector<std::uint32_t> pixels;
	int width = 0;
	int height = 0;
	int stride = 0;
	std::uint64_t sequence = 0;
};

tcvr_video_store s_video;

struct tcvr_audio_store
{
	std::vector<std::int16_t> samples;
	std::atomic<std::uint64_t> write_frame{ 0 };
	int rate = 48000;
	int channels = 2;

	tcvr_audio_store()
		: samples(std::size_t(48000) * 2 * std::size_t(2), 0)
	{
	}

	void push(std::int16_t const *source, int frames)
	{
		if (!source || frames <= 0)
			return;
		// Single MAME producer, single AAudio callback consumer. Publishing the
		// write cursor after filling samples keeps the realtime callback lock-free.
		std::uint64_t const base = write_frame.load(std::memory_order_relaxed);
		for (int frame = 0; frame < frames; ++frame)
		{
			std::size_t const slot = ((base + std::uint64_t(frame)) % (samples.size() / channels)) * channels;
			for (int channel = 0; channel < channels; ++channel)
				samples[slot + channel] = source[frame * channels + channel];
		}
		write_frame.store(base + std::uint64_t(frames), std::memory_order_release);
	}
};

tcvr_audio_store s_audio;

struct tcvr_input_store
{
	std::mutex mutex;
	bool coin = false;
	bool start = false;
	bool trigger = false;
	bool pedal = false;
	float gun_x = 0.5f;
	float gun_y = 0.5f;
};

tcvr_input_store s_input;

class tcvr_osd final : public osd_interface
{
public:
	void init(running_machine &machine) override { m_machine = &machine; }
	void update(bool) override
	{
		if (!m_machine)
			return;
		screen_device *screen = screen_device_enumerator(m_machine->root_device()).first();
		if (!screen || screen->renderbitmap().format() != BITMAP_FORMAT_RGB32)
			return;

		bitmap_rgb32 &bitmap = screen->renderbitmap().as_rgb32();
		rectangle const visible = screen->visible_area();
		std::lock_guard lock(s_video.mutex);
		// renderbitmap includes System22 CRT overscan. Export MAME's declared
		// visible area only, so the arcade image fills the XR presentation plane.
		s_video.width = visible.width();
		s_video.height = visible.height();
		s_video.stride = s_video.width;
		s_video.pixels.resize(std::size_t(s_video.stride) * std::size_t(s_video.height));
		for (int y = 0; y < s_video.height; ++y)
			std::memcpy(s_video.pixels.data() + std::size_t(y) * s_video.stride,
				&bitmap.pix(y + visible.min_y, visible.min_x), std::size_t(s_video.width) * sizeof(std::uint32_t));
		if (s_video.sequence == 0)
		{
			std::size_t nonzero = 0;
			for (std::uint32_t pixel : s_video.pixels)
				nonzero += (pixel & 0x00ffffffU) != 0;
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_M3 first framebuffer pixels=%zu/%zu visible=%dx%d+%d+%d first=0x%08x",
				nonzero, s_video.pixels.size(), visible.width(), visible.height(), visible.min_x, visible.min_y,
				s_video.pixels.empty() ? 0U : s_video.pixels.front());
		}
		++s_video.sequence;
	}
	void input_update(bool) override { }
	void check_osd_inputs() override { }
	void set_verbose(bool) override { }
	void init_debugger() override { }
	void wait_for_debugger(device_t &, bool) override { }
	bool no_sound() override { return false; }
	bool sound_external_per_channel_volume() override { return false; }
	bool sound_split_streams_per_source() override { return false; }
	uint32_t sound_get_generation() override { return 1; }
	osd::audio_info sound_get_information() override
	{
		osd::audio_info result;
		result.m_generation = 1;
		result.m_default_sink = 1;
		result.m_default_source = 0;
		osd::audio_info::node_info node;
		node.m_name = "tcvr-aaudio";
		node.m_display_name = "ArcadeXR audio";
		node.m_id = 1;
		node.m_rate = osd::audio_rate_range{ 48000, 48000, 48000 };
		node.m_sinks = 2;
		node.m_sources = 0;
		node.m_port_names = { "FL", "FR" };
		node.m_port_positions = { osd::channel_position::FL(), osd::channel_position::FR() };
		result.m_nodes.push_back(std::move(node));
		return result;
	}
	uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t rate) override
	{
		s_audio.rate = int(rate);
		return 1;
	}
	uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	void sound_stream_close(uint32_t) override { }
	void sound_stream_sink_update(uint32_t, int16_t const *buffer, int samples_this_frame) override
	{
		s_audio.push(buffer, samples_this_frame);
	}
	void sound_stream_source_update(uint32_t, int16_t *, int) override { }
	void sound_stream_set_volumes(uint32_t, std::vector<float> const &) override { }
	void sound_begin_update() override { }
	void sound_end_update() override { }
	void customize_input_type_list(std::vector<input_type_entry> &) override { }
	void add_audio_to_recording(int16_t const *, int) override { }
	std::vector<ui::menu_item> get_slider_list() override { return {}; }
	osd_font::ptr font_alloc() override { return {}; }
	bool get_font_families(std::string const &, std::vector<std::pair<std::string, std::string>> &) override { return false; }
	bool execute_command(char const *) override { return false; }
	std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view) override { return {}; }
	std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view) override { return {}; }
	std::vector<osd::midi_port_info> list_midi_ports() override { return {}; }
	std::unique_ptr<osd::network_device> open_network_device(int, osd::network_handler &) override { return {}; }
	std::vector<osd::network_device_info> list_network_devices() override { return {}; }

private:
	running_machine *m_machine = nullptr;
};

class tcvr_machine_manager final : public machine_manager
{
public:
	tcvr_machine_manager(emu_options &options, osd_interface &osd, int &frame_count)
		: machine_manager(options, osd)
		, m_frame_count(frame_count)
	{
	}

	ui_manager *create_ui(running_machine &machine) override
	{
		m_ui = std::make_unique<ui_manager>(machine);
		return m_ui.get();
	}

	void create_custom(running_machine &machine) override
	{
		if (screen_device *screen = screen_device_enumerator(machine.root_device()).first())
			screen->set_video_attributes(VIDEO_ALWAYS_UPDATE);
		machine.add_notifier(MACHINE_NOTIFY_FRAME, machine_notify_delegate(&tcvr_machine_manager::on_frame, this));
		m_machine = &machine;
	}

private:
	void on_frame()
	{
		if (m_machine)
		{
			bool coin, start, trigger, pedal;
			float gun_x, gun_y;
			{
				std::lock_guard lock(s_input.mutex);
				coin = s_input.coin;
				start = s_input.start;
				trigger = s_input.trigger;
				pedal = s_input.pedal;
				gun_x = s_input.gun_x;
				gun_y = s_input.gun_y;
			}
			ioport_list const &ports = m_machine->ioport().ports();
			auto find_port = [&ports](char const *tag) -> ioport_port *
			{
				auto it = ports.find(tag);
				if (it == ports.end() && tag[0] != ':')
					it = ports.find(std::string(":") + tag);
				return (it == ports.end()) ? nullptr : it->second.get();
			};
			auto set_button = [&find_port](char const *tag, ioport_value mask, bool pressed)
			{
				if (ioport_port *port = find_port(tag))
					if (ioport_field *field = port->field(mask))
						// set_value is a programmatic pressed-state override. It is
						// independent of the port's IP_ACTIVE_LOW electrical polarity.
						field->set_value(pressed ? 1 : 0);
			};
			auto set_axis = [&find_port](char const *tag, float normalized)
			{
				if (ioport_port *port = find_port(tag))
					if (ioport_field *field = port->field(0xffff))
					{
						ioport_value const range = field->maxval() - field->minval();
						field->set_value(field->minval() + ioport_value(std::clamp(normalized, 0.0f, 1.0f) * float(range)));
					}
			};
			set_button("INPUTS", 0x0001, coin);
			set_button("INPUTS", 0x0010, trigger);
			set_button("INPUTS", 0x0020, pedal);
			set_axis("OPT.0", gun_x);
			set_axis("OPT.1", gun_y);
			set_button("INPUTS", 0x0100, start);
			if (!m_inputLogged || coin != m_lastCoin || start != m_lastStart || trigger != m_lastTrigger || pedal != m_lastPedal)
			{
				__android_log_print(ANDROID_LOG_INFO, kLogTag,
					"TCVR_M6 MAME inputs coin=%d start=%d trigger=%d pedal=%d", coin, start, trigger, pedal);
				m_inputLogged = true;
				m_lastCoin = coin;
				m_lastStart = start;
				m_lastTrigger = trigger;
				m_lastPedal = pedal;
			}
		}
		++m_frame_count;
		if (m_frame_count == 1)
			__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 first emulated video frame");
		if ((m_frame_count % 300) == 0)
		{
			auto const now = std::chrono::steady_clock::now();
			auto const elapsed = std::chrono::duration<float>(now - m_lastRateTime).count();
			if (elapsed > 0.0f)
				__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_PERF MAME fps=%.2f", 300.0f / elapsed);
			m_lastRateTime = now;
		}
	}

	int &m_frame_count;
	running_machine *m_machine = nullptr;
	std::unique_ptr<ui_manager> m_ui;
	bool m_inputLogged = false;
	bool m_lastCoin = false;
	bool m_lastStart = false;
	bool m_lastTrigger = false;
	bool m_lastPedal = false;
	std::chrono::steady_clock::time_point m_lastRateTime = std::chrono::steady_clock::now();
};

} // namespace

const char *emulator_info::get_appname() { return "ArcadeXR"; }
const char *emulator_info::get_appname_lower() { return "arcadexr"; }
const char *emulator_info::get_configname() { return "tcvr"; }
const char *emulator_info::get_copyright() { return "ArcadeXR runtime"; }
const char *emulator_info::get_copyright_info() { return "ArcadeXR runtime"; }
const char *emulator_info::get_bare_build_version() { return "M2"; }
const char *emulator_info::get_build_version() { return "ArcadeXR M2"; }
void emulator_info::display_ui_chooser(running_machine &) { }
int emulator_info::start_frontend(emu_options &, osd_interface &, std::vector<std::string> &) { return EMU_ERR_FATALERROR; }
int emulator_info::start_frontend(emu_options &, osd_interface &, int, char *[]) { return EMU_ERR_FATALERROR; }
bool emulator_info::draw_user_interface(running_machine &) { return false; }
void emulator_info::periodic_check() { }
bool emulator_info::frame_hook() { return false; }
void emulator_info::sound_hook(const std::map<std::string, std::vector<std::pair<const float *, int>>> &) { }
void emulator_info::layout_script_cb(layout_file &, const char *) { }
bool emulator_info::standalone() { return true; }

extern "C" int tcvr_mame_core_abi_version()
{
	return 1;
}

extern "C" int tcvr_mame_has_driver(const char *driver_id)
{
	return driver_id && driver_list::find(driver_id) >= 0;
}

// Runs a headless driver selected by the generic C ABI. A positive `seconds`
// is a test-only bound; zero is the normal continuous arcade runtime. The ROM
// archive remains at a caller-owned external path and is never copied into the
// application or this repository.
extern "C" int tcvr_mame_boot_smoke(const char *driver_id, const char *rom_path, int seconds, int *frame_count)
{
	if (!driver_id || !rom_path || seconds < 0 || !frame_count)
		return EMU_ERR_INVALID_CONFIG;

	int const driver_index = driver_list::find(driver_id);
	if (driver_index < 0)
		return EMU_ERR_NO_SUCH_SYSTEM;

	*frame_count = 0;
	__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 boot driver=%s rompath=%s seconds=%d", driver_id, rom_path, seconds);

	try
	{
		emu_options options(emu_options::option_support::GENERAL_AND_SYSTEM);
		options.set_system_name(driver_id);
		options.set_value(OPTION_MEDIAPATH, rom_path, OPTION_PRIORITY_MAXIMUM);
		if (seconds > 0)
			options.set_value(OPTION_SECONDS_TO_RUN, seconds, OPTION_PRIORITY_MAXIMUM);
		options.set_value(OPTION_SKIP_GAMEINFO, 1, OPTION_PRIORITY_MAXIMUM);
		options.set_value(OPTION_READCONFIG, 0, OPTION_PRIORITY_MAXIMUM);
		options.set_value(OPTION_WRITECONFIG, 0, OPTION_PRIORITY_MAXIMUM);
		options.set_value(OPTION_NVRAM_SAVE, 0, OPTION_PRIORITY_MAXIMUM);

		tcvr_osd osd;
		tcvr_machine_manager manager(options, osd, *frame_count);
		manager.start_http_server();
		machine_config config(driver_list::driver(driver_index), options);
		running_machine machine(config, manager);
		manager.set_machine(&machine);
		int const result = machine.run(true);
		int width = 0;
		int height = 0;
		int stride = 0;
		std::uint64_t sequence = 0;
		{
			std::lock_guard lock(s_video.mutex);
			width = s_video.width;
			height = s_video.height;
			stride = s_video.stride;
			sequence = s_video.sequence;
			std::size_t nonzero = 0;
			for (std::uint32_t pixel : s_video.pixels)
				nonzero += (pixel & 0x00ffffffU) != 0;
			__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M3 final framebuffer pixels=%zu/%zu first=0x%08x",
				nonzero, s_video.pixels.size(), s_video.pixels.empty() ? 0U : s_video.pixels.front());
		}
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 boot result=%d frames=%d video=%dx%d stride=%d seq=%llu", result, *frame_count, width, height, stride, static_cast<unsigned long long>(sequence));
		return result;
	}
	catch (std::exception const &error)
	{
		__android_log_print(ANDROID_LOG_ERROR, kLogTag, "TCVR_M2 exception: %s", error.what());
		return EMU_ERR_FATALERROR;
	}
}

extern "C" int tcvr_mame_latest_video_info(int *width, int *height, int *stride, std::uint64_t *sequence)
{
	if (!width || !height || !stride || !sequence)
		return 0;
	std::lock_guard lock(s_video.mutex);
	*width = s_video.width;
	*height = s_video.height;
	*stride = s_video.stride;
	*sequence = s_video.sequence;
	return (!s_video.pixels.empty()) ? 1 : 0;
}

extern "C" std::size_t tcvr_mame_copy_latest_video(std::uint32_t *destination, std::size_t destination_pixels)
{
	if (!destination)
		return 0;
	std::lock_guard lock(s_video.mutex);
	std::size_t const count = std::min(destination_pixels, s_video.pixels.size());
	std::memcpy(destination, s_video.pixels.data(), count * sizeof(std::uint32_t));
	return count;
}

extern "C" int tcvr_mame_audio_info(int *rate, int *channels, std::uint64_t *write_frame)
{
	if (!rate || !channels || !write_frame)
		return 0;
	*rate = s_audio.rate;
	*channels = s_audio.channels;
	*write_frame = s_audio.write_frame.load(std::memory_order_acquire);
	return 1;
}

extern "C" std::size_t tcvr_mame_audio_read(std::int16_t *destination, std::size_t destination_frames, std::uint64_t *cursor)
{
	if (!destination || !cursor || !destination_frames)
		return 0;
	std::size_t const capacity = s_audio.samples.size() / std::size_t(s_audio.channels);
	std::uint64_t const write_frame = s_audio.write_frame.load(std::memory_order_acquire);
	std::uint64_t const oldest = (write_frame > capacity) ? write_frame - capacity : 0;
	if (*cursor < oldest)
		*cursor = oldest;
	std::uint64_t const available = write_frame - *cursor;
	std::size_t const frames = std::min<std::size_t>(destination_frames, std::size_t(available));
	for (std::size_t frame = 0; frame < frames; ++frame)
	{
		std::size_t const slot = ((*cursor + frame) % capacity) * std::size_t(s_audio.channels);
		std::memcpy(destination + frame * std::size_t(s_audio.channels), s_audio.samples.data() + slot,
			std::size_t(s_audio.channels) * sizeof(std::int16_t));
	}
	*cursor += frames;
	return frames;
}

extern "C" void tcvr_mame_set_digital(char const *id, bool pressed)
{
	if (!id)
		return;
	std::lock_guard lock(s_input.mutex);
	if (!std::strcmp(id, "coin"))
		s_input.coin = pressed;
	else if (!std::strcmp(id, "start"))
		s_input.start = pressed;
	else if (!std::strcmp(id, "trigger"))
		s_input.trigger = pressed;
	else if (!std::strcmp(id, "pedal"))
		s_input.pedal = pressed;
}

extern "C" void tcvr_mame_set_analog(char const *id, float value)
{
	if (!id)
		return;
	std::lock_guard lock(s_input.mutex);
	if (!std::strcmp(id, "gun_x"))
		s_input.gun_x = value;
	else if (!std::strcmp(id, "gun_y"))
		s_input.gun_y = value;
}
