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

namespace {

constexpr char kLogTag[] = "TCVR_MAME";

class tcvr_osd final : public osd_interface
{
public:
	void init(running_machine &) override { }
	void update(bool) override { }
	void input_update(bool) override { }
	void check_osd_inputs() override { }
	void set_verbose(bool) override { }
	void init_debugger() override { }
	void wait_for_debugger(device_t &, bool) override { }
	bool no_sound() override { return true; }
	bool sound_external_per_channel_volume() override { return false; }
	bool sound_split_streams_per_source() override { return false; }
	uint32_t sound_get_generation() override { return 1; }
	osd::audio_info sound_get_information() override { return {}; }
	uint32_t sound_stream_sink_open(uint32_t, std::string, uint32_t) override { return 0; }
	uint32_t sound_stream_source_open(uint32_t, std::string, uint32_t) override { return 0; }
	void sound_stream_close(uint32_t) override { }
	void sound_stream_sink_update(uint32_t, int16_t const *, int) override { }
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
		machine.add_notifier(MACHINE_NOTIFY_FRAME, machine_notify_delegate(&tcvr_machine_manager::on_frame, this));
	}

private:
	void on_frame()
	{
		++m_frame_count;
		if (m_frame_count == 1)
			__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 first emulated video frame");
	}

	int &m_frame_count;
	std::unique_ptr<ui_manager> m_ui;
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

// Runs a bounded, headless boot of a driver selected by the generic C ABI.
// The ROM archive remains at a caller-owned external path and is never copied
// into the application or this repository.
extern "C" int tcvr_mame_boot_smoke(const char *driver_id, const char *rom_path, int seconds, int *frame_count)
{
	if (!driver_id || !rom_path || seconds <= 0 || !frame_count)
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
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "TCVR_M2 boot result=%d frames=%d", result, *frame_count);
		return result;
	}
	catch (std::exception const &error)
	{
		__android_log_print(ANDROID_LOG_ERROR, kLogTag, "TCVR_M2 exception: %s", error.what());
		return EMU_ERR_FATALERROR;
	}
}
