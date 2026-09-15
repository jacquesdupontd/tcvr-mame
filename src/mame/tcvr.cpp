// license:BSD-3-Clause
// Native ArcadeXR boundary.  This deliberately owns MAME's small runtime glue;
// Android/OpenXR code never needs to know a MAME driver implementation detail.
#include "emu.h"
#include "drivenum.h"
#include "emuopts.h"
#include "main.h"
#include "osdepend.h"
#include "rendlay.h"
#include "render.h"
#include "video.h"

#include "ui/uimain.h"

#include <android/log.h>

#include <cstring>
#include <cstdlib>
#include <sys/system_properties.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace {

constexpr char kLogTag[] = "TCVR_MAME";
std::atomic<running_machine *> s_tcvr_machine{ nullptr };
std::atomic<bool> s_tcvr_exit_requested{ false };

// __system_property_get writes an EMPTY string and returns 0 when the property
// does not exist, so passing a pre-filled buffer as a "default" silently loses
// it. That mistake turned VIDEO_ALWAYS_UPDATE off on a build meant to keep it
// on, and the screen went black.
inline bool property_flag(const char *name, bool fallback)
{
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get(name, value) <= 0)
		return fallback;
	return value[0] == '1' || value[0] == 'y' || value[0] == 't';
}

inline int property_int(const char *name, int fallback)
{
	char value[PROP_VALUE_MAX] = {};
	if (__system_property_get(name, value) <= 0)
		return fallback;
	return atoi(value);
}

struct tcvr_video_store
{
	std::mutex mutex;
	// Triple buffer. The emulation thread fills `buffers[write_idx]` with NO lock
	// held, then swaps it into `published_idx` under the lock for a few
	// nanoseconds. The reader swaps `published_idx` into `read_idx` the same way
	// and uploads straight from it. The three indices are always distinct, so
	// nobody ever waits for a 1.2 MB copy -- which is what produced the 1.27 ms
	// spikes on the one thread whose speed is the whole problem.
	std::vector<std::uint32_t> buffers[3];
	std::uint64_t buffer_seq[3] = { 0, 0, 0 };
	int write_idx = 0;
	int published_idx = 1;
	int read_idx = 2;
	bool published_fresh = false;
	int width = 0;
	int height = 0;
	int stride = 0;
	std::uint64_t sequence = 0;

	// Published without the mutex, for readers that only want to know whether
	// anything changed. The XR renderer asks that question hundreds of times a
	// second -- once per eye of every presented frame, which at 207 Hz is over
	// 400 times a second -- while the emulator holds the mutex to copy a whole
	// 640x480 frame into `pixels`. Making the cheap question take the expensive
	// lock put the renderer squarely on the emulator's critical path: measured
	// on a Quest 3, MAME sat at 70% CPU with the machine 267% idle, blocked on
	// this lock, and ran at 58 fps instead of 60.
	std::atomic<std::uint64_t> published_sequence{ 0 };
	std::atomic<int> published_width{ 0 };
	std::atomic<int> published_height{ 0 };
	std::atomic<int> published_stride{ 0 };
	std::uint64_t capture_us = 0;
	std::uint64_t capture_worst_us = 0;
	std::uint64_t capture_frames = 0;
};

tcvr_video_store s_video;

struct tcvr_audio_store
{
	std::vector<std::int16_t> samples;
	std::atomic<std::uint64_t> write_frame{ 0 };
	int rate = 48000;
	int channels = 2;
	// Consumer-only state, touched exclusively by the AAudio callback.
	bool primed = false;
	bool started = false;                 // has anything ever been played?
	int last_push_frames = -1;            // to notice the producer changing cadence
	std::chrono::steady_clock::time_point last_push_at{};
	std::atomic<std::uint64_t> stalls{ 0 };
	std::atomic<std::uint32_t> worst_gap_ms{ 0 };
	std::uint64_t pushed_since = 0;
	std::chrono::steady_clock::time_point rate_window{};
	std::uint64_t consumed_since = 0;
	std::chrono::steady_clock::time_point consume_window{};
	std::uint32_t phase = 0;              // 0.16 fixed-point resampler phase
	std::int64_t ratio_fine = std::int64_t(65536) << 8;  // 16.16 << 8, smoothed playback ratio
	std::int64_t ratio_applied = std::int64_t(65536) << 8;  // slew-limited output ratio
	std::int64_t rate_measured = 65536;   // 16.16, the producer's measured rate
	std::atomic<int> policy_floor_ppm{ 990000 };
	std::atomic<bool> policy_follow_producer{ false };
	std::atomic<int> policy_cushion_ms{ 120 };
	std::uint64_t rate_last_write = 0;    // write cursor at the last measurement
	std::size_t rate_output = 0;          // output frames since the last measurement
	std::int16_t last[2] = { 0, 0 };
	// Counters, so "the sound crackles" can be a number instead of an opinion.
	// Written by the audio callback, read by anyone; relaxed is enough.
	std::atomic<std::uint64_t> callbacks{ 0 };
	std::atomic<std::uint64_t> underruns{ 0 };
	std::atomic<std::uint64_t> stretches{ 0 };
	std::atomic<std::uint64_t> shrinks{ 0 };
	std::atomic<std::uint64_t> silent{ 0 };
	std::atomic<std::uint32_t> cushion{ 0 };
	std::atomic<std::int32_t> ratio_ppm{ 1000000 };
	std::atomic<std::uint64_t> resyncs{ 0 };   // latency ceiling hit, cursor skipped forward

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
	// driving profile (System 22 racers): steering 0..1 (0.5 centre), pedals 0..1
	float steer = 0.5f, gas = 0.0f, brake = 0.0f;
	bool shift_up = false, shift_down = false, view = false;
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
		// How much of the emulation thread does OUR capture actually take? It
		// copies 1.2 MB under a mutex sixty times a second, on the one thread
		// whose speed is the whole problem. That has never been measured.
		auto const captureStart = std::chrono::steady_clock::now();
		// renderbitmap includes System22 CRT overscan. Export MAME's declared
		// visible area only, so the arcade image fills the XR presentation plane.
		int const width = visible.width();
		int const height = visible.height();
		std::vector<std::uint32_t> &target = s_video.buffers[s_video.write_idx];
		target.resize(std::size_t(width) * std::size_t(height));
		for (int y = 0; y < height; ++y)
			std::memcpy(target.data() + std::size_t(y) * width,
				&bitmap.pix(y + visible.min_y, visible.min_x), std::size_t(width) * sizeof(std::uint32_t));
		if (s_video.sequence == 0)
		{
			std::size_t nonzero = 0;
			for (std::uint32_t pixel : target)
				nonzero += (pixel & 0x00ffffffU) != 0;
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_M3 first framebuffer pixels=%zu/%zu visible=%dx%d+%d+%d first=0x%08x",
				nonzero, target.size(), width, height, visible.min_x, visible.min_y,
				target.empty() ? 0U : target.front());
		}
		{
			std::lock_guard lock(s_video.mutex);
			++s_video.sequence;
			s_video.buffer_seq[s_video.write_idx] = s_video.sequence;
			std::swap(s_video.write_idx, s_video.published_idx);
			s_video.published_fresh = true;
			s_video.width = width;
			s_video.height = height;
			s_video.stride = width;
		}
		s_video.published_width.store(width, std::memory_order_relaxed);
		s_video.published_height.store(height, std::memory_order_relaxed);
		s_video.published_stride.store(width, std::memory_order_relaxed);
		s_video.published_sequence.store(s_video.sequence, std::memory_order_release);

		{
			auto const took = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - captureStart).count();
			s_video.capture_us += std::uint64_t(took);
			if (std::uint64_t(took) > s_video.capture_worst_us)
				s_video.capture_worst_us = std::uint64_t(took);
			if (++s_video.capture_frames >= 60)
			{
				__android_log_print(ANDROID_LOG_INFO, kLogTag,
					"TCVR_SOUND framebuffer capture: mean %.2f ms, worst %.2f ms over %llu frames"
					" (a frame is 16.67 ms) | frameskip level=%d speed=%.1f%%",
					double(s_video.capture_us) / double(s_video.capture_frames) / 1000.0,
					double(s_video.capture_worst_us) / 1000.0,
					(unsigned long long)s_video.capture_frames,
					m_machine->video().effective_frameskip(), m_machine->video().speed_percent() * 100.0);
				s_video.capture_us = 0;
				s_video.capture_worst_us = 0;
				s_video.capture_frames = 0;
			}
		}
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
	uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override
	{
		// Reported because the failure the player describes happens "at exactly
		// the same place" in the game -- which is the signature of something the
		// GAME does, not of scheduling. A stream reopened at a different rate
		// mid-play would do exactly that, and would be invisible otherwise.
		__android_log_print(ANDROID_LOG_INFO, kLogTag,
			"TCVR_SOUND sink_open node=%u name=%s rate=%u (previous rate=%d)",
			node, name.c_str(), rate, s_audio.rate);
		s_audio.rate = int(rate);
		return 1;
	}
	uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	void sound_stream_close(uint32_t) override { }
	void sound_stream_sink_update(uint32_t, int16_t const *buffer, int samples_this_frame) override
	{
		// Does the PRODUCER stop, or does the consumer merely fall behind? Those
		// two need completely different fixes and every attempt so far assumed
		// the second. Measure the wall-clock gap between pushes: MAME emits
		// 960 frames at a time, so a healthy gap is 20 ms and anything past 40
		// means the emulation loop itself stalled -- and no audio existed to be
		// played, whatever the reader did.
		{
			auto const now = std::chrono::steady_clock::now();
			if (s_audio.last_push_at.time_since_epoch().count() != 0)
			{
				auto const gap = std::chrono::duration_cast<std::chrono::milliseconds>(
					now - s_audio.last_push_at).count();
				if (gap > 40)
				{
					s_audio.stalls.fetch_add(1, std::memory_order_relaxed);
					__android_log_print(ANDROID_LOG_WARN, kLogTag,
						"TCVR_SOUND producer stalled %lld ms (healthy is 20) totalStalls=%llu",
						(long long)gap,
						(unsigned long long)s_audio.stalls.load(std::memory_order_relaxed));
				}
				if (gap > s_audio.worst_gap_ms.load(std::memory_order_relaxed))
					s_audio.worst_gap_ms.store(std::uint32_t(gap), std::memory_order_relaxed);
			}
			s_audio.last_push_at = now;

			// Frames produced per wall-clock second, stated outright. The
			// feed-forward infers this from cursor arithmetic, and the inference
			// disagrees with what MAME reports as its own speed: 59.90 fps is
			// 99.8% of realtime, yet the inferred production rate sits near 95%.
			// One of the two is wrong, and until this line existed there was no
			// way to tell which.
			s_audio.pushed_since += std::uint64_t(samples_this_frame);
			if (s_audio.rate_window.time_since_epoch().count() == 0)
				s_audio.rate_window = now;
			auto const window = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - s_audio.rate_window).count();
			if (window >= 1000)
			{
				__android_log_print(ANDROID_LOG_INFO, kLogTag,
					"TCVR_SOUND produced %llu frames in %lld ms = %.0f/s (device wants %d/s)",
					(unsigned long long)s_audio.pushed_since, (long long)window,
					double(s_audio.pushed_since) * 1000.0 / double(window), s_audio.rate);
				s_audio.pushed_since = 0;
				s_audio.rate_window = now;
			}
		}
		if (samples_this_frame != s_audio.last_push_frames)
		{
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_SOUND push size changed %d -> %d frames", s_audio.last_push_frames, samples_this_frame);
			s_audio.last_push_frames = samples_this_frame;
		}
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
		// VIDEO_ALWAYS_UPDATE forces the screen update -- and therefore the whole
		// System 22 rasterisation -- on every single frame, which is precisely
		// what stops MAME from ever skipping one. On a heavy scene the emulation
		// loop then has no way to catch up: it runs late, and since the sound is
		// generated from that same loop, the audio runs late with it. That is the
		// mechanism behind a dropout that happens "at exactly the same place"
		// every time, which is the clue that made it findable.
		//
		// It was originally added to fix a black framebuffer, but the real fix
		// for that was reading renderbitmap() instead of curbitmap(), which is
		// what the capture does now. Kept behind a property so the two can be
		// compared rather than argued about.
		// Give MAME a render target, then let it skip frames.
		//
		// screen.cpp guards the screen update with TWO conditions, and only the
		// first is obvious:
		//
		//     if (!(m_video_attributes & VIDEO_ALWAYS_UPDATE)) {
		//         if (machine().video().skip_this_frame()) return false;
		//         if (!machine().render().is_live(*this))    return false;
		//     }
		//
		// A headless OSD has no render target, so is_live() is false and the
		// screen is never updated at all -- which is why simply dropping
		// VIDEO_ALWAYS_UPDATE blacked out the framebuffer rather than enabling
		// frameskip. Allocating a target makes the screen live, and MAME then
		// skips frames the ordinary way when it falls behind: the simulation and
		// the sound keep their original timing, only the picture skips. A skipped
		// picture is invisible here, because the XR renderer presents the last
		// image it has a hundred and twenty times a second regardless.
		//
		// This matters far beyond the picture. The emulator produced 49 blocks of
		// audio per second instead of 50 on heavy scenes, and no amount of work
		// in the audio reader can conjure the missing block.
		m_render_target = machine.render().target_alloc();
		const bool alwaysUpdate = property_flag("debug.tcvr.alwaysUpdate", false);
		if (screen_device *screen = screen_device_enumerator(machine.root_device()).first())
		{
			if (alwaysUpdate)
				screen->set_video_attributes(VIDEO_ALWAYS_UPDATE);
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_SOUND alwaysUpdate=%d renderTarget=%p screenLive=%d",
				alwaysUpdate ? 1 : 0, (void *)m_render_target,
				machine.render().is_live(*screen) ? 1 : 0);
		}
		machine.add_notifier(MACHINE_NOTIFY_FRAME, machine_notify_delegate(&tcvr_machine_manager::on_frame, this));
		m_machine = &machine;
		s_tcvr_machine.store(&machine, std::memory_order_release);
	}

private:
	render_target *m_render_target = nullptr;
	bool m_paused_by_property = false;

	void on_frame()
	{
		if (m_machine)
		{
			if (s_tcvr_exit_requested.exchange(false, std::memory_order_acq_rel))
			{
				__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 graceful game stop requested");
				m_machine->schedule_exit();
				return;
			}
			// Freeze the emulation on request, so the same frame can be looked at
			// under every display filter. A debug tool, read once a frame.
			const bool wantPause = property_flag("debug.tcvr.pause", false);
			if (wantPause != m_paused_by_property)
			{
				if (wantPause) m_machine->pause(); else m_machine->resume();
				m_paused_by_property = wantPause;
				__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_SOUND emulation %s by debug.tcvr.pause",
					wantPause ? "PAUSED" : "resumed");
			}
			bool coin, start, trigger, pedal, shift_up, shift_down, view;
			float gun_x, gun_y, steer, gas, brake;
			{
				std::lock_guard lock(s_input.mutex);
				coin = s_input.coin;
				start = s_input.start;
				trigger = s_input.trigger;
				pedal = s_input.pedal;
				gun_x = s_input.gun_x;
				gun_y = s_input.gun_y;
				steer = s_input.steer; gas = s_input.gas; brake = s_input.brake;
				shift_up = s_input.shift_up; shift_down = s_input.shift_down; view = s_input.view;
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
			if (find_port("STEER") && find_port("ACCEL"))
			{
				// Sega Model 2 driving board. Its I/O tags and START bit do not
				// match the System 22 ADC/INPUTS wiring; XR still supplies the same
				// generic steering and pedal values.
				set_button("IN0", 0x01, coin);
				set_button("IN0", 0x40, start);
				set_button("IN0", 0x20, view);
				set_axis("STEER", steer);
				set_axis("ACCEL", gas);
				set_axis("BRAKE", brake);
				set_axis("IN2", pedal ? 1.0f : 0.0f);
			}
			else if (find_port("ADC.0"))
			{
			set_button("INPUTS", 0x0001, coin);
				// A System 22 racer (Dirt Dash, Ridge Racer...): the INPUTS bits mean
				// view / shift, not trigger / pedal, and the game starts on the gas.
				set_axis("ADC.0", steer);
				set_axis("ADC.1", gas);
				set_axis("ADC.2", brake);
				set_button("INPUTS", 0x0010, view);
				set_button("INPUTS", 0x0020, shift_up);
				set_button("INPUTS", 0x0040, shift_down);
			}
			else
			{
			set_button("INPUTS", 0x0001, coin);
				set_button("INPUTS", 0x0010, trigger);
				set_button("INPUTS", 0x0020, pedal);
				set_axis("OPT.0", gun_x);
				set_axis("OPT.1", gun_y);
				set_button("INPUTS", 0x0100, start);
			}
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

extern "C" void tcvr_mame_request_exit()
{
	s_tcvr_exit_requested.store(true, std::memory_order_release);
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
		// Let MAME drop a video frame rather than fall behind. The simulation and
		// the sound keep their original timing; only the picture skips, and a
		// skipped picture is invisible here because the XR renderer already
		// presents the last image it has, a hundred and twenty times a second,
		// independently of the arcade clock.
		{
			const bool autoskip = property_flag("debug.tcvr.autoframeskip", true);
			const int fixedskip = property_int("debug.tcvr.frameskip", 0);
			options.set_value(OPTION_AUTOFRAMESKIP, autoskip, OPTION_PRIORITY_MAXIMUM);
			options.set_value(OPTION_FRAMESKIP, fixedskip, OPTION_PRIORITY_MAXIMUM);
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_SOUND autoframeskip=%d frameskip=%d", autoskip ? 1 : 0, fixedskip);
		}

		tcvr_osd osd;
		tcvr_machine_manager manager(options, osd, *frame_count);
		manager.start_http_server();
		machine_config config(driver_list::driver(driver_index), options);
		running_machine machine(config, manager);
		manager.set_machine(&machine);
		int const result = machine.run(true);
		s_tcvr_machine.store(nullptr, std::memory_order_release);
		s_tcvr_exit_requested.store(false, std::memory_order_release);
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
			std::vector<std::uint32_t> const &last = s_video.buffers[s_video.published_idx];
			std::size_t nonzero = 0;
			for (std::uint32_t pixel : last)
				nonzero += (pixel & 0x00ffffffU) != 0;
			__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M3 final framebuffer pixels=%zu/%zu first=0x%08x",
				nonzero, last.size(), last.empty() ? 0U : last.front());
		}
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 boot result=%d frames=%d video=%dx%d stride=%d seq=%llu", result, *frame_count, width, height, stride, static_cast<unsigned long long>(sequence));
		return result;
	}
	catch (std::exception const &error)
	{
		s_tcvr_machine.store(nullptr, std::memory_order_release);
		s_tcvr_exit_requested.store(false, std::memory_order_release);
		__android_log_print(ANDROID_LOG_ERROR, kLogTag, "TCVR_M2 exception: %s", error.what());
		return EMU_ERR_FATALERROR;
	}
}

extern "C" int tcvr_mame_latest_video_info(int *width, int *height, int *stride, std::uint64_t *sequence)
{
	if (!width || !height || !stride || !sequence)
		return 0;
	// Deliberately lock-free: see tcvr_video_store.
	std::uint64_t const published = s_video.published_sequence.load(std::memory_order_acquire);
	*width = s_video.published_width.load(std::memory_order_relaxed);
	*height = s_video.published_height.load(std::memory_order_relaxed);
	*stride = s_video.published_stride.load(std::memory_order_relaxed);
	*sequence = published;
	return published != 0 ? 1 : 0;
}

// Hands the reader the newest published buffer without copying it. The pointer
// stays valid until the next acquire, because the writer can never reach the
// buffer the reader currently holds. Returns nullptr before the first frame.
extern "C" const std::uint32_t *tcvr_mame_acquire_latest_video(int *width, int *height, int *stride, std::uint64_t *sequence)
{
	std::lock_guard lock(s_video.mutex);
	if (s_video.published_fresh)
	{
		std::swap(s_video.read_idx, s_video.published_idx);
		s_video.published_fresh = false;
	}
	std::vector<std::uint32_t> const &buffer = s_video.buffers[s_video.read_idx];
	if (buffer.empty())
		return nullptr;
	if (width) *width = s_video.width;
	if (height) *height = s_video.height;
	if (stride) *stride = s_video.stride;
	if (sequence) *sequence = s_video.buffer_seq[s_video.read_idx];
	return buffer.data();
}

extern "C" std::size_t tcvr_mame_copy_latest_video(std::uint32_t *destination, std::size_t destination_pixels)
{
	if (!destination)
		return 0;
	int w = 0, h = 0, st = 0;
	std::uint64_t seq = 0;
	const std::uint32_t *source = tcvr_mame_acquire_latest_video(&w, &h, &st, &seq);
	if (!source)
		return 0;
	std::size_t const count = std::min(destination_pixels, std::size_t(st) * std::size_t(h));
	std::memcpy(destination, source, count * sizeof(std::uint32_t));
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

// Generic audio policy chosen by the frontend profile, never by a hardcoded
// game ID in MAME. The default is the proven 1% cushion-only reader.
extern "C" void tcvr_mame_audio_configure(int floor_ppm, bool follow_producer, int cushion_ms)
{
	if (floor_ppm < 700000 || floor_ppm > 1000000) floor_ppm = 990000;
	if (cushion_ms < 80 || cushion_ms > 250) cushion_ms = 120;
	s_audio.policy_floor_ppm.store(floor_ppm, std::memory_order_release);
	s_audio.policy_follow_producer.store(follow_producer, std::memory_order_release);
	s_audio.policy_cushion_ms.store(cushion_ms, std::memory_order_release);
	__android_log_print(ANDROID_LOG_INFO, kLogTag,
		"TCVR_SOUND policy floorPpm=%d followProducer=%d cushionMs=%d",
		floor_ppm, follow_producer ? 1 : 0, cushion_ms);
}

// Raw counters, cumulative since start. No interpretation, no thresholds --
// whoever reads them decides what "often" means.
extern "C" void tcvr_mame_audio_stats(std::uint64_t *callbacks, std::uint64_t *underruns, std::uint64_t *stretches,
	std::uint64_t *shrinks, std::uint64_t *silent, std::uint32_t *cushion_frames, std::int32_t *ratio_ppm)
{
	if (callbacks) *callbacks = s_audio.callbacks.load(std::memory_order_relaxed);
	if (underruns) *underruns = s_audio.underruns.load(std::memory_order_relaxed);
	if (stretches) *stretches = s_audio.stretches.load(std::memory_order_relaxed);
	if (shrinks) *shrinks = s_audio.shrinks.load(std::memory_order_relaxed);
	if (silent) *silent = s_audio.silent.load(std::memory_order_relaxed);
	if (cushion_frames) *cushion_frames = s_audio.cushion.load(std::memory_order_relaxed);
	if (ratio_ppm) *ratio_ppm = s_audio.ratio_ppm.load(std::memory_order_relaxed);
	// Folded into `silent`, which already means "the reader could not serve the
	// buffer normally"; a separate ABI field would break every existing caller.
	if (silent) *silent += s_audio.resyncs.load(std::memory_order_relaxed);
	// Producer stalls ride along in `stretches`, which no longer means anything
	// on its own now that the resampler is continuous; the log line is the real
	// report. Encoded as stalls * 1000000 + worst gap so one number carries both.
	if (stretches)
		*stretches = s_audio.stalls.load(std::memory_order_relaxed) * 1000000ull +
			s_audio.worst_gap_ms.load(std::memory_order_relaxed);
}

// The emulator clock and the audio device clock are independent, and always
// will be: MAME advances on the original arcade timing while AAudio consumes at
// exactly 48 kHz. Worse, the emulator's speed is not even constant -- measured
// on a Quest 3, MAME holds 59.90 fps on a quiet scene and drops towards 58 when
// Area 1 gets busy, so the mismatch swings between roughly 0.2% and 3%.
//
// Two earlier shapes of this reader both failed, and for the same reason:
// they could only ever consume a whole number of frames. Draining the ring to
// empty turned every shortfall into a gap of silence. Trimming by one frame in
// 192 capped the correction at 0.52%, which cannot track a 3% deficit, so the
// cushion drained anyway and underran about once every ten seconds.
//
// This reader resamples instead. It keeps a target cushion, derives a playback
// ratio from how far the cushion has wandered, and reads the ring at that ratio
// with linear interpolation and a fixed-point phase carried across callbacks.
// A ratio within a couple of percent of unity is a pitch shift of a few tens of
// a semitone, held steady rather than jumping -- inaudible, and it never leaves
// a hole. The ratio is clamped so that a genuinely stalled emulator degrades
// into an honest underrun rather than into a slowed-down drone.
extern "C" std::size_t tcvr_mame_audio_read(std::int16_t *destination, std::size_t destination_frames, std::uint64_t *cursor)
{
	if (!destination || !cursor || !destination_frames)
		return 0;
	int const channels = s_audio.channels;
	std::size_t const capacity = s_audio.samples.size() / std::size_t(channels);
	std::uint64_t const write_frame = s_audio.write_frame.load(std::memory_order_acquire);

	// The cushion we aim to keep between the emulator and the speaker: enough to
	// ride out a slow frame, short enough not to be felt as lag.
	// 120 ms. At 80 ms a single emulator stumble emptied it; the extra 40 ms of
	// latency is well under what a gunshot's delay would make noticeable, and it
	// buys roughly half a second more tolerance to a speed dip.
	std::uint64_t const target = std::uint64_t(s_audio.rate) *
		std::uint64_t(s_audio.policy_cushion_ms.load(std::memory_order_acquire)) / 1000u;

	s_audio.callbacks.fetch_add(1, std::memory_order_relaxed);
	{
		auto const now = std::chrono::steady_clock::now();
		s_audio.consumed_since += destination_frames;
		if (s_audio.consume_window.time_since_epoch().count() == 0)
			s_audio.consume_window = now;
		auto const window = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - s_audio.consume_window).count();
		if (window >= 1000)
		{
			__android_log_print(ANDROID_LOG_INFO, kLogTag,
				"TCVR_SOUND device asked for %llu frames in %lld ms = %.0f/s",
				(unsigned long long)s_audio.consumed_since, (long long)window,
				double(s_audio.consumed_since) * 1000.0 / double(window));
			s_audio.consumed_since = 0;
			s_audio.consume_window = now;
		}
	}

	if (!s_audio.primed)
	{
		if (!s_audio.started)
		{
			// First fill. Nothing has been heard yet, so starting a cushion's
			// worth behind the writer costs nothing.
			if (write_frame < target)
			{
				s_audio.silent.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
			*cursor = write_frame - target;
			s_audio.started = true;
		}
		else
		{
			// Re-priming after an underrun. The listener has ALREADY HEARD
			// everything up to the cursor, so moving it back to build a cushion
			// replays that audio -- which is the echo the player reported after
			// every dropout. Wait for the producer to get ahead of where we
			// stopped instead, and resume from exactly there: nothing repeated,
			// nothing skipped.
			if (write_frame < *cursor + target)
			{
				s_audio.silent.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
		}
		s_audio.phase = 0;
		// Do not touch the ratio here at all.
		//
		// Seeding it from the measured rate looked reasonable and was the source
		// of the "voice pitching downwards": the rate measured DURING a stall can
		// be 0.88, and seeding the filter with it sent the applied ratio sliding
		// twelve percent down over the next several seconds. Every other path
		// clamps the aim to +/-4%, so leaving the loop alone keeps the ratio
		// inside that band by construction, whatever the stall measured.
		s_audio.rate_last_write = write_frame;
		s_audio.rate_output = 0;
		s_audio.primed = true;
	}

	// Never read samples the producer has already overwritten.
	std::uint64_t const oldest = (write_frame > capacity) ? write_frame - capacity : 0;
	if (*cursor < oldest)
	{
		*cursor = oldest;
		s_audio.phase = 0;
	}

	std::uint64_t available = (write_frame > *cursor) ? write_frame - *cursor : 0;

	// Hard ceiling on latency, independent of the control loop.
	//
	// The loop is slow on purpose, so it must not be the only thing standing
	// between a reader that has fallen behind and a buffer full of stale audio.
	// Measured with a slew limit set fifty times too slow: the cushion reached
	// the ring's full two seconds, and the player heard gunshots arriving three
	// seconds after the shot. A single skip forward is a click; two seconds of
	// delay makes the game unplayable.
	std::uint64_t const ceiling = target * 2;   // 240 ms, hard maximum latency
	if (available > ceiling)
	{
		*cursor = write_frame - target;
		s_audio.phase = 0;
		available = target;
		s_audio.resyncs.fetch_add(1, std::memory_order_relaxed);
	}
	s_audio.cushion.store(std::uint32_t(available > 0xffffffffu ? 0xffffffffu : available), std::memory_order_relaxed);

	// Playback ratio in 16.16 fixed point: input frames consumed per output frame.
	//
	// What makes this audible or not is the SPEED of the change, not its size.
	//
	// Version one corrected over half a second and allowed +/-3%: the ratio swung
	// between -1.6% and +1.1% and moved on every callback, which is tremolo.
	// Version two kept the slow smoothing but clamped to +/-0.5%: no tremolo, but
	// measured on a real attract sequence MAME runs at 57-58 fps, four to eight
	// percent below realtime, and half a percent cannot follow that -- the
	// cushion drained and it underran roughly every two seconds.
	//
	// The clamp was the wrong lever, and for a reason worth writing down: when
	// the emulator runs at 96% of realtime, its music genuinely IS slower. Playing
	// its samples back at 96% is not a distortion, it is faithful to what the
	// machine is doing, and pitch-shifting them back to 100% would be the lie.
	//
	// So the clamp is wide enough to follow the emulator, and the SMOOTHING is
	// what keeps it inaudible: a one-pole filter with a time constant near a
	// second, far below the few hertz the ear hears as tremolo. The ratio tracks
	// the emulator's real speed and creeps there over seconds.
	// The feed-forward is GONE, and this is why.
	//
	// It inferred the producer's speed from cursor arithmetic and reported 0.95,
	// sometimes 0.87. The control loop believed it and pitched the audio down by
	// up to six percent to match -- which is precisely the wandering pitch the
	// player kept reporting. It was chasing a drift that does not exist.
	//
	// Measured directly in wall clock, at the two places where frames actually
	// cross the boundary:
	//
	//     produced      47952 to 48094 frames per second
	//     device asked  48000 to 48048 frames per second
	//
	// The two clocks agree to a tenth of a percent. There is no drift to correct,
	// and every gram of pitch modulation this file produced was self-inflicted.
	//
	// So: drive on the one quantity that IS measured directly here -- the depth
	// of the cushion -- and clamp the correction to one percent, which is about
	// seventeen cents and only ever used to walk the cushion back to its target
	// after a real stall. The raw inference is still computed and logged, purely
	// so the discrepancy stays visible rather than being forgotten.
	// Feed-forward, kept only as a diagnostic.
	//
	// A purely proportional loop cannot do this. Its correction is a function of
	// the error, so it only produces the -4% needed to follow a 96% emulator once
	// the error is large -- and "large" here meant the cushion had drained to 388
	// frames of the 3840 it was aiming for, eight milliseconds from silence. That
	// is textbook steady-state droop, and it was plainly visible in the counters.
	//
	// The producer's rate is not a mystery to be inferred: write_frame says it
	// outright. Measure it over a quarter of a second, use it directly, and leave
	// the proportional term with the only job it is good at -- nudging the cushion
	// back to where it should sit.
	s_audio.rate_output += destination_frames;
	const bool follow_producer = s_audio.policy_follow_producer.load(std::memory_order_acquire);
	if (s_audio.rate_output >= std::size_t(s_audio.rate) / (follow_producer ? 1 : 4))
	{
		if (s_audio.rate_last_write != 0 && write_frame > s_audio.rate_last_write)
		{
			std::uint64_t const produced = write_frame - s_audio.rate_last_write;
			std::int64_t const measured = std::int64_t((produced << 16) / s_audio.rate_output);
			// Ignore a reading that could only come from a stall or a wrap.
			// Wide enough to include an emulator genuinely running below realtime,
			// which this one does: 95.2% was measured during the failure and a
			// narrower window would simply have frozen the feed-forward at the
			// last value it happened to accept.
			if (measured > 65536 * 85 / 100 && measured < 65536 * 115 / 100)
				s_audio.rate_measured = measured;
		}
		s_audio.rate_last_write = write_frame;
		s_audio.rate_output = 0;
	}

	std::int64_t const error = std::int64_t(available) - std::int64_t(target);
	std::int64_t const span = std::int64_t(s_audio.rate) * 4;
	std::int64_t aim = (follow_producer ? s_audio.rate_measured : 65536) +
		(span > 0 ? (error * 65536) / span : 0);
	// Asymmetric on purpose. Consuming too slowly costs latency, and the ceiling
	// above already bounds that. Consuming too fast costs SILENCE, which is what
	// is actually being heard. Measured during the failure, MAME was producing at
	// 95.2% of realtime while this floor was pinned at 96%: the reader was
	// structurally faster than the producer, so the buffer could only ever empty.
	// One percent, about seventeen cents. Enough to walk a drained cushion back
	// to target over a few seconds, far too little to be heard as a bend.
	std::int64_t const lowest = std::int64_t(65536) *
		s_audio.policy_floor_ppm.load(std::memory_order_acquire) / 1000000;
	std::int64_t const highest = 65536 + 65536 / 100;
	if (aim < lowest) aim = lowest;
	if (aim > highest) aim = highest;
	// One-pole smoothing, time constant a few hundred callbacks -- a second or so.
	//
	// Carried at eight extra bits of precision. A first version smoothed the
	// 16.16 value directly, and integer division then truncated every step
	// smaller than 256 counts to zero: a dead zone of 0.39%, wider than the
	// 0.5% clamp around it, so the correction never moved at all and the ratio
	// sat at exactly 1.0 while the cushion quietly drifted. The counters said
	// so -- ratioPpm=1000000 with the cushion 384 frames below target -- which
	// is the whole reason for printing them.
	s_audio.ratio_fine += ((aim << 8) - s_audio.ratio_fine) / 256;
	// Hard ceiling on how fast the pitch may move, whatever the loop wants.
	// Following the emulator's real speed is right; gliding there audibly is
	// not, and a glide is exactly what "it slows down and speeds up like a
	// vinyl" describes. At this limit a full 4% correction takes about four
	// seconds to apply, which the ear reads as a steady offset rather than a
	// bend.
	{
		// Sized so a full four percent correction lands in about three seconds.
		// The first value here was 16, which worked out at 167 seconds: the
		// ratio could not move, the loop was effectively open, and the buffer
		// filled to the brim. A slew limit must be slow enough to be inaudible
		// and fast enough to still be a control system.
		std::int64_t const maximumStep =
			(follow_producer && available < target / 2) ? 25000 : 900;
		std::int64_t delta = s_audio.ratio_fine - s_audio.ratio_applied;
		if (delta > maximumStep) delta = maximumStep;
		if (delta < -maximumStep) delta = -maximumStep;
		s_audio.ratio_applied += delta;
	}
	std::int64_t const ratio = s_audio.ratio_applied >> 8;
	s_audio.ratio_ppm.store(std::int32_t((ratio * 1000000) / 65536), std::memory_order_relaxed);
	if (ratio < 65536) s_audio.stretches.fetch_add(1, std::memory_order_relaxed);
	else if (ratio > 65536) s_audio.shrinks.fetch_add(1, std::memory_order_relaxed);

	// Frames the ring must hold for this buffer, plus one for the interpolation
	// to have something to reach towards.
	std::uint64_t const needed = std::uint64_t((ratio * std::int64_t(destination_frames)) >> 16) + 2u;

	if (available < needed)
	{
		// A genuine underrun. Emit what the ring holds, then hold the last sample
		// instead of dropping to zero -- a held level is far less audible than a
		// square edge into silence -- and re-prime so the cushion is rebuilt
		// rather than chased forever.
		std::uint64_t const count = s_audio.underruns.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count <= 40 || (count % 50) == 0)
		{
			__android_log_print(ANDROID_LOG_WARN, kLogTag,
				"TCVR_SOUND underrun #%llu available=%llu need=%llu target=%llu rateMeasured=%lld ratioPpm=%d",
				(unsigned long long)count, (unsigned long long)available, (unsigned long long)needed,
				(unsigned long long)target, (long long)((s_audio.rate_measured * 1000000) / 65536),
				(int)s_audio.ratio_ppm.load(std::memory_order_relaxed));
		}
		std::size_t const copied = std::size_t(available < destination_frames ? available : destination_frames);
		for (std::size_t frame = 0; frame < copied; ++frame)
		{
			std::size_t const slot = ((*cursor + frame) % capacity) * std::size_t(channels);
			std::memcpy(destination + frame * std::size_t(channels), s_audio.samples.data() + slot,
				std::size_t(channels) * sizeof(std::int16_t));
		}
		if (copied)
		{
			std::size_t const slot = ((*cursor + copied - 1) % capacity) * std::size_t(channels);
			for (int channel = 0; channel < channels && channel < 2; ++channel)
				s_audio.last[channel] = s_audio.samples[slot + channel];
		}
		for (std::size_t frame = copied; frame < destination_frames; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				destination[frame * std::size_t(channels) + channel] = s_audio.last[channel < 2 ? channel : 1];
		// Advance by what was actually consumed, not to the writer. Jumping the
		// cursor forward here would drop whatever the producer wrote in the
		// meantime; the held samples covered the gap, they did not consume it.
		// Keep going. Do NOT de-prime.
		//
		// De-priming looked prudent and was the single worst thing in this file:
		// the re-prime below then waited for a FULL cushion to rebuild before
		// playing anything, which is thirty-odd callbacks of pure silence. The
		// counters said so outright -- silent climbing by 32 every second
		// alongside one underrun a second -- and that is what the player heard as
		// "it cuts, then a bit later it cuts again".
		//
		// A shortfall costs exactly the frames it was short. The loop rebuilds
		// the cushion by consuming very slightly slower, inaudibly, over seconds.
		*cursor += copied;
		s_audio.phase = 0;
		return destination_frames;
	}

	std::uint64_t position = (*cursor << 16) | std::uint64_t(s_audio.phase);
	for (std::size_t frame = 0; frame < destination_frames; ++frame)
	{
		std::uint64_t const index = position >> 16;
		std::int32_t const weight = std::int32_t(position & 0xffffu);
		std::size_t const slot = std::size_t(index % capacity) * std::size_t(channels);
		std::size_t const next = std::size_t((index + 1) % capacity) * std::size_t(channels);
		for (int channel = 0; channel < channels; ++channel)
		{
			std::int32_t const a = s_audio.samples[slot + channel];
			std::int32_t const b = s_audio.samples[next + channel];
			destination[frame * std::size_t(channels) + channel] = std::int16_t(a + (((b - a) * weight) >> 16));
		}
		position += std::uint64_t(ratio);
	}
	for (int channel = 0; channel < channels && channel < 2; ++channel)
		s_audio.last[channel] = destination[(destination_frames - 1) * std::size_t(channels) + channel];

	*cursor = position >> 16;
	s_audio.phase = std::uint32_t(position & 0xffffu);
	return destination_frames;
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
	else if (!std::strcmp(id, "shift_up"))
		s_input.shift_up = pressed;
	else if (!std::strcmp(id, "shift_down"))
		s_input.shift_down = pressed;
	else if (!std::strcmp(id, "view"))
		s_input.view = pressed;
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
	else if (!std::strcmp(id, "steer"))
		s_input.steer = value;
	else if (!std::strcmp(id, "gas"))
		s_input.gas = value;
	else if (!std::strcmp(id, "brake"))
		s_input.brake = value;
}

// ---------------------------------------------------------------------------
// TCVR scene recorder (see tcvr_scene.h). Triple-buffered like the video.
// ---------------------------------------------------------------------------
#include "tcvr_scene.h"
#include "namco/namcos22.h"
namespace {
struct tcvr_scene_store
{
	struct slot
	{
		std::vector<tcvr_scene_vertex> vertices;
		std::vector<tcvr_scene_prim> prims;
		std::vector<uint32_t> pens;
		std::vector<uint8_t> czram;
		std::vector<uint16_t> text;
		std::vector<uint8_t> gamma;   // 3 x 256
		std::vector<uint8_t> pri;     // text mask, width x height
		std::vector<uint16_t> spotram;
		tcvr_scene_frame frame{};
	};
	std::mutex mutex;
	slot slots[3];
	int write_idx = 0, published_idx = 1, read_idx = 2;
	bool fresh = false;
	uint64_t sequence = 0;
	bool recording = false;
	std::atomic<int> enabled{0};
	// static assets, built once
	std::vector<uint8_t> sprite_atlas;
	bool assets_ready = false;
	tcvr_scene_assets assets{};
};
tcvr_scene_store s_scene;
}

extern "C" void tcvr_mame_scene_reset()
{
	std::lock_guard lock(s_scene.mutex);
	s_scene.recording = false;
	s_scene.fresh = false;
	s_scene.write_idx = 0;
	s_scene.published_idx = 1;
	s_scene.read_idx = 2;
	for (auto &slot : s_scene.slots)
	{
		slot.vertices.clear(); slot.prims.clear(); slot.pens.clear(); slot.czram.clear();
		slot.text.clear(); slot.gamma.clear(); slot.pri.clear(); slot.spotram.clear();
		slot.frame = {};
	}
	s_scene.sprite_atlas.clear();
	s_scene.assets = {};
	s_scene.assets_ready = false;
}

extern "C" void tcvr_mame_scene_enable(int enabled)
{
	// 0 = disabled, 1 = record alongside the CPU rasteriser, 2 = record and
	// let the Quest GPU rasterise.  Do not coerce this to bool: doing so made
	// mode 2 unreachable and paid for both rasterisers in immersive mode.
	s_scene.enabled.store(std::clamp(enabled, 0, 2), std::memory_order_relaxed);
}
extern "C" int tcvr_scene_mode(void) { return s_scene.enabled.load(std::memory_order_relaxed); }
extern "C" void tcvr_scene_begin(void)
{
	if (!s_scene.enabled.load(std::memory_order_relaxed)) { s_scene.recording = false; return; }
	auto &w = s_scene.slots[s_scene.write_idx];
	w.vertices.clear(); w.prims.clear();
	s_scene.recording = true;
}
extern "C" void tcvr_scene_poly(const tcvr_scene_vertex *v, int count, const tcvr_scene_prim &p)
{
	if (!s_scene.recording) return;
	auto &w = s_scene.slots[s_scene.write_idx];
	tcvr_scene_prim q = p; q.first_vertex = uint32_t(w.vertices.size()); q.vertex_count = uint32_t(count);
	w.vertices.insert(w.vertices.end(), v, v + count);
	w.prims.push_back(q);
}
extern "C" void tcvr_scene_sprite(const tcvr_scene_vertex *v, const tcvr_scene_prim &p)
{
	tcvr_scene_poly(v, 4, p);
}
extern "C" void tcvr_scene_end(const tcvr_scene_frame &fp)
{
	if (!s_scene.recording) return;
	s_scene.recording = false;
	auto &w = s_scene.slots[s_scene.write_idx];
	w.pens.assign(fp.pens, fp.pens + fp.pen_count);
	w.czram.assign(fp.czram, fp.czram + fp.cz_banks * fp.cz_entries);
	if (fp.text) { w.text.resize(size_t(fp.width) * fp.height); for (int y = 0; y < fp.height; y++) std::memcpy(w.text.data() + size_t(y) * fp.width, fp.text + size_t(y) * fp.text_stride, size_t(fp.width) * 2); }
	// The gamma tables live in the mixer's u32 words and are read with nthbyte(): byte i is at i ^ 3.
	w.gamma.resize(768); for (int i = 0; i < 256; i++) { w.gamma[i] = fp.gamma_r[i ^ 3]; w.gamma[256 + i] = fp.gamma_g[i ^ 3]; w.gamma[512 + i] = fp.gamma_b[i ^ 3]; }
	if (fp.pri) { w.pri.resize(size_t(fp.width) * fp.height); for (int y = 0; y < fp.height; y++) std::memcpy(w.pri.data() + size_t(y) * fp.width, fp.pri + size_t(y) * fp.pri_stride, size_t(fp.width)); }
	if (fp.spotram) w.spotram.assign(fp.spotram, fp.spotram + 0x400); else w.spotram.clear();
	w.frame = fp;
	w.frame.vertices = w.vertices.data(); w.frame.vertex_count = uint32_t(w.vertices.size());
	w.frame.prims = w.prims.data(); w.frame.prim_count = uint32_t(w.prims.size());
	w.frame.pens = w.pens.data(); w.frame.czram = w.czram.data();
	w.frame.text = w.text.empty() ? nullptr : w.text.data(); w.frame.text_stride = uint32_t(fp.width);
	w.frame.gamma_r = w.gamma.data(); w.frame.gamma_g = w.gamma.data() + 256; w.frame.gamma_b = w.gamma.data() + 512;
	w.frame.pri = w.pri.empty() ? nullptr : w.pri.data(); w.frame.pri_stride = uint32_t(fp.width);
	w.frame.spotram = w.spotram.empty() ? nullptr : w.spotram.data();
	std::lock_guard lock(s_scene.mutex);
	w.frame.sequence = ++s_scene.sequence;
	std::swap(s_scene.write_idx, s_scene.published_idx);
	s_scene.fresh = true;
}
extern "C" const tcvr_scene_frame *tcvr_mame_acquire_scene(void)
{
	std::lock_guard lock(s_scene.mutex);
	if (s_scene.fresh) { std::swap(s_scene.read_idx, s_scene.published_idx); s_scene.fresh = false; }
	auto &r = s_scene.slots[s_scene.read_idx];
	return r.frame.sequence ? &r.frame : nullptr;
}
extern "C" int tcvr_mame_scene_assets(tcvr_scene_assets *out)
{
	if (!out) return 0;
	running_machine *mp = s_tcvr_machine.load(std::memory_order_acquire);
	if (!mp) return 0;
	running_machine &machine = *mp;
	if (!s_scene.assets_ready)
	{
		namcos22_state *state = dynamic_cast<namcos22_state *>(&machine.root_device());
		if (!state) return 0;
		memory_region *const textile = machine.root_device().memregion("textile");
		gfx_element *const gfx = state->tcvr_gfx(2);
		// create_custom publishes the machine before video_start has built these
		// tables. Do not permanently cache that transient, half-initialised view.
		// The first recorded scene is published only after video_start, so a later
		// call will see the complete immutable asset set.
		if (!textile || !state->tcvr_texture_tilemap() || !state->tcvr_texture_tileattr() ||
			!state->tcvr_texture_ayx() || !gfx || !gfx->elements())
			return 0;
		auto &a = s_scene.assets;
		a.tiledata = textile->base(); a.tiledata_bytes = uint32_t(textile->bytes());
		a.tilemap = state->tcvr_texture_tilemap(); a.tilemap_entries = 0x100000;
		a.tileattr = state->tcvr_texture_tileattr(); a.tileattr_entries = 0x100000;
		a.ayx = state->tcvr_texture_ayx(); a.ayx_entries = 16 * 16 * 16;
		if (gfx)
		{
			a.sprite_width = gfx->width(); a.sprite_height = gfx->height(); a.sprite_count = gfx->elements();
			s_scene.sprite_atlas.resize(size_t(a.sprite_count) * a.sprite_width * a.sprite_height);
			for (uint32_t e = 0; e < a.sprite_count; e++)
			{
				const u8 *src = gfx->get_data(e);
				for (uint32_t y = 0; y < a.sprite_height; y++)
					std::memcpy(s_scene.sprite_atlas.data() + (size_t(e) * a.sprite_height + y) * a.sprite_width, src + y * gfx->rowbytes(), a.sprite_width);
			}
			a.sprites = s_scene.sprite_atlas.data();
		}
		a.pen_count = state->tcvr_pen_count();
		s_scene.assets_ready = true;
	}
	*out = s_scene.assets;
	return 1;
}

// ---------------------------------------------------------------------------
// TCVR Model 2 scene recorder (see sega/tcvr_m2_scene.h).
//
// Same triple-buffered shape as the System 22 recorder above, and for the same
// reason: the emulation thread must never block on a reader, and the XR thread
// must never see a half-written frame. What differs is only what a Model 2
// frame carries -- there is no czram, no Namco tile store, no sprite atlas;
// the colour chain is palram, colorxlat, lumaram and a gamma ramp.
//
// This records; it does not decide. It publishes the same walk MAME's own
// rasteriser makes, in the same painter's order, and reports what it had to
// drop so a consumer can tell a complete scene from a truncated one instead of
// trusting one.
// ---------------------------------------------------------------------------
#include "sega/tcvr_m2_scene.h"
namespace {
// A frame of Sega Rally attract runs 500-1800 polygons; the caps are set well
// above what has been observed so that hitting one is a reportable anomaly
// rather than normal operation. Exceeding them truncates the tail and says so.
constexpr uint32_t k_m2_max_prims = 16384;
constexpr uint32_t k_m2_max_vertices = 131072;

struct tcvr_m2_scene_store
{
	struct slot
	{
		std::vector<tcvr_m2_vertex> vertices;
		std::vector<tcvr_m2_prim> prims;
		std::vector<uint16_t> palram;
		std::vector<uint16_t> colorxlat;
		std::vector<uint8_t> lumaram;
		std::vector<uint8_t> gamma;
		std::vector<uint32_t> dirty[2];
		tcvr_m2_frame frame{};
	};
	std::mutex mutex;
	slot slots[3];
	int write_idx = 0, published_idx = 1, read_idx = 2;
	bool fresh = false;
	bool recording = false;
	uint64_t sequence = 0;
	uint32_t dropped_prims = 0, dropped_vertices = 0;
	std::atomic<int> enabled{0};
};
tcvr_m2_scene_store s_m2_scene;
}

extern "C" void tcvr_m2_scene_enable(int mode)
{
	s_m2_scene.enabled.store(std::clamp(mode, 0, 1), std::memory_order_relaxed);
}

extern "C" int tcvr_m2_scene_mode(void)
{
	return s_m2_scene.enabled.load(std::memory_order_relaxed);
}

extern "C" void tcvr_m2_scene_begin(int width, int height)
{
	if (!s_m2_scene.enabled.load(std::memory_order_relaxed)) { s_m2_scene.recording = false; return; }
	auto &w = s_m2_scene.slots[s_m2_scene.write_idx];
	w.vertices.clear();
	w.prims.clear();
	w.frame.width = width;
	w.frame.height = height;
	s_m2_scene.dropped_prims = 0;
	s_m2_scene.dropped_vertices = 0;
	s_m2_scene.recording = true;
}

extern "C" void tcvr_m2_scene_poly(const tcvr_m2_vertex *v, int count, const tcvr_m2_prim *p)
{
	if (!s_m2_scene.recording || v == nullptr || p == nullptr || count < 3) return;
	auto &w = s_m2_scene.slots[s_m2_scene.write_idx];
	// A cap never rejects the frame: it drops the tail and records how much, so
	// the number is visible instead of the scene silently disagreeing with the
	// CPU raster.
	if (w.prims.size() >= k_m2_max_prims || w.vertices.size() + count > k_m2_max_vertices)
	{
		s_m2_scene.dropped_prims++;
		s_m2_scene.dropped_vertices += uint32_t(count);
		return;
	}
	tcvr_m2_prim q = *p;
	q.first_vertex = uint32_t(w.vertices.size());
	q.vertex_count = uint32_t(count);
	w.vertices.insert(w.vertices.end(), v, v + count);
	w.prims.push_back(q);
}

extern "C" void tcvr_m2_scene_end(const tcvr_m2_frame *fp)
{
	if (!s_m2_scene.recording) return;
	s_m2_scene.recording = false;
	if (fp == nullptr) return;
	auto &w = s_m2_scene.slots[s_m2_scene.write_idx];
	if (fp->palram) w.palram.assign(fp->palram, fp->palram + fp->palram_entries); else w.palram.clear();
	if (fp->colorxlat) w.colorxlat.assign(fp->colorxlat, fp->colorxlat + fp->colorxlat_entries); else w.colorxlat.clear();
	if (fp->lumaram) w.lumaram.assign(fp->lumaram, fp->lumaram + fp->lumaram_entries); else w.lumaram.clear();
	if (fp->gamma) w.gamma.assign(fp->gamma, fp->gamma + fp->gamma_entries); else w.gamma.clear();

	w.frame = *fp;
	w.frame.vertices = w.vertices.data();
	w.frame.vertex_count = uint32_t(w.vertices.size());
	w.frame.prims = w.prims.data();
	w.frame.prim_count = uint32_t(w.prims.size());
	w.frame.palram = w.palram.empty() ? nullptr : w.palram.data();
	w.frame.colorxlat = w.colorxlat.empty() ? nullptr : w.colorxlat.data();
	w.frame.lumaram = w.lumaram.empty() ? nullptr : w.lumaram.data();
	w.frame.gamma = w.gamma.empty() ? nullptr : w.gamma.data();
	w.frame.dropped_prims = s_m2_scene.dropped_prims;
	w.frame.dropped_vertices = s_m2_scene.dropped_vertices;
	// The texture sheets are not copied: 4 MB a frame would cost more than the
	// whole emulation. The driver owns them and only rewrites them through
	// tex0_w/tex1_w, so a consumer reads them in place and uses the dirty mask
	// to know what moved. The masks ARE copied, because the driver clears them
	// as soon as this frame is published.
	for (int sheet = 0; sheet < 2; sheet++)
	{
		if (fp->dirty[sheet] && fp->dirty_words)
			w.dirty[sheet].assign(fp->dirty[sheet], fp->dirty[sheet] + fp->dirty_words);
		else
			w.dirty[sheet].clear();
		w.frame.dirty[sheet] = w.dirty[sheet].empty() ? nullptr : w.dirty[sheet].data();
	}

	std::lock_guard lock(s_m2_scene.mutex);
	w.frame.sequence = ++s_m2_scene.sequence;
	std::swap(s_m2_scene.write_idx, s_m2_scene.published_idx);
	s_m2_scene.fresh = true;
}

extern "C" const tcvr_m2_frame *tcvr_m2_acquire_scene(void)
{
	std::lock_guard lock(s_m2_scene.mutex);
	if (s_m2_scene.fresh) { std::swap(s_m2_scene.read_idx, s_m2_scene.published_idx); s_m2_scene.fresh = false; }
	auto &r = s_m2_scene.slots[s_m2_scene.read_idx];
	return r.frame.sequence ? &r.frame : nullptr;
}
