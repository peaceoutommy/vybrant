// Minimal, self-written declarations of the CLAP 1.2 C ABI needed to host plugins. Written from the public ABI
// description; contains no CLAP SDK code. Only the subset the host uses is declared, but every struct keeps the complete
// member list and order so layouts match the official headers. Names follow the official ones (minus the _t typedefs).
#pragma once
#include <cstdint>

namespace clap {

using clap_id = uint32_t;
constexpr clap_id CLAP_INVALID_ID = UINT32_MAX;
constexpr int CLAP_NAME_SIZE = 256;
constexpr int CLAP_PATH_SIZE = 1024;

struct clap_version {
    uint32_t major, minor, revision;
};
constexpr clap_version CLAP_VERSION = {1, 2, 2};
inline bool clap_version_is_compatible(clap_version v) { return v.major >= 1; }

// ---------------------------------------------------------------- events
struct clap_event_header {
    uint32_t size;   // event size including this header
    uint32_t time;   // sample offset within the block
    uint16_t space_id;
    uint16_t type;
    uint32_t flags;
};
constexpr uint16_t CLAP_CORE_EVENT_SPACE_ID = 0;
enum : uint16_t {
    CLAP_EVENT_NOTE_ON = 0,
    CLAP_EVENT_NOTE_OFF = 1,
    CLAP_EVENT_NOTE_CHOKE = 2,
    CLAP_EVENT_NOTE_END = 3,
    CLAP_EVENT_NOTE_EXPRESSION = 4,
    CLAP_EVENT_PARAM_VALUE = 5,
    CLAP_EVENT_PARAM_MOD = 6,
    CLAP_EVENT_PARAM_GESTURE_BEGIN = 7,
    CLAP_EVENT_PARAM_GESTURE_END = 8,
    CLAP_EVENT_TRANSPORT = 9,
    CLAP_EVENT_MIDI = 10,
    CLAP_EVENT_MIDI_SYSEX = 11,
    CLAP_EVENT_MIDI2 = 12,
};
struct clap_event_note {
    clap_event_header header;
    int32_t note_id;   // -1 = wildcard / unspecified, same for port_index, channel, key
    int16_t port_index;
    int16_t channel;
    int16_t key;
    double velocity;
};
struct clap_event_param_value {
    clap_event_header header;
    clap_id param_id;
    void* cookie;
    int32_t note_id;
    int16_t port_index;
    int16_t channel;
    int16_t key;
    double value;
};
struct clap_event_midi {
    clap_event_header header;
    uint16_t port_index;
    uint8_t data[3];
};
using clap_beattime = int64_t;
using clap_sectime = int64_t;
constexpr int64_t CLAP_BEATTIME_FACTOR = 1LL << 31;
constexpr int64_t CLAP_SECTIME_FACTOR = 1LL << 31;
constexpr uint32_t CLAP_TRANSPORT_HAS_TEMPO = 1 << 0, CLAP_TRANSPORT_HAS_BEATS_TIMELINE = 1 << 1,
                   CLAP_TRANSPORT_HAS_SECONDS_TIMELINE = 1 << 2, CLAP_TRANSPORT_HAS_TIME_SIGNATURE = 1 << 3,
                   CLAP_TRANSPORT_IS_PLAYING = 1 << 4, CLAP_TRANSPORT_IS_RECORDING = 1 << 5, CLAP_TRANSPORT_IS_LOOP_ACTIVE = 1 << 6,
                   CLAP_TRANSPORT_IS_WITHIN_PRE_ROLL = 1 << 7;
struct clap_event_transport {
    clap_event_header header;
    uint32_t flags;
    clap_beattime song_pos_beats;
    clap_sectime song_pos_seconds;
    double tempo;
    double tempo_inc;
    clap_beattime loop_start_beats;
    clap_beattime loop_end_beats;
    clap_sectime loop_start_seconds;
    clap_sectime loop_end_seconds;
    clap_beattime bar_start;
    int32_t bar_number;
    uint16_t tsig_num;
    uint16_t tsig_denom;
};
struct clap_input_events {
    void* ctx;
    uint32_t (*size)(const clap_input_events* list);
    const clap_event_header* (*get)(const clap_input_events* list, uint32_t index);
};
struct clap_output_events {
    void* ctx;
    bool (*try_push)(const clap_output_events* list, const clap_event_header* event);
};

// ---------------------------------------------------------------- process
struct clap_audio_buffer {
    float** data32;
    double** data64;
    uint32_t channel_count;
    uint32_t latency;
    uint64_t constant_mask;
};
using clap_process_status = int32_t;
enum : clap_process_status {
    CLAP_PROCESS_ERROR = 0,
    CLAP_PROCESS_CONTINUE = 1,
    CLAP_PROCESS_CONTINUE_IF_NOT_QUIET = 2,
    CLAP_PROCESS_TAIL = 3,
    CLAP_PROCESS_SLEEP = 4,
};
struct clap_process {
    int64_t steady_time;
    uint32_t frames_count;
    const clap_event_transport* transport;
    const clap_audio_buffer* audio_inputs;
    clap_audio_buffer* audio_outputs;
    uint32_t audio_inputs_count;
    uint32_t audio_outputs_count;
    const clap_input_events* in_events;
    const clap_output_events* out_events;
};

// ---------------------------------------------------------------- host, plugin, factory, entry
struct clap_host {
    clap::clap_version clap_version;   // qualified: the member shares the type's name
    void* host_data;
    const char* name;
    const char* vendor;
    const char* url;
    const char* version;
    const void* (*get_extension)(const clap_host* host, const char* extension_id);
    void (*request_restart)(const clap_host* host);
    void (*request_process)(const clap_host* host);
    void (*request_callback)(const clap_host* host);
};
struct clap_plugin_descriptor {
    clap::clap_version clap_version;   // qualified: the member shares the type's name
    const char* id;
    const char* name;
    const char* vendor;
    const char* url;
    const char* manual_url;
    const char* support_url;
    const char* version;
    const char* description;
    const char* const* features;   // null-terminated
};
struct clap_plugin {
    const clap_plugin_descriptor* desc;
    void* plugin_data;
    bool (*init)(const clap_plugin* plugin);
    void (*destroy)(const clap_plugin* plugin);
    bool (*activate)(const clap_plugin* plugin, double sample_rate, uint32_t min_frames_count, uint32_t max_frames_count);
    void (*deactivate)(const clap_plugin* plugin);
    bool (*start_processing)(const clap_plugin* plugin);
    void (*stop_processing)(const clap_plugin* plugin);
    void (*reset)(const clap_plugin* plugin);
    clap_process_status (*process)(const clap_plugin* plugin, const clap_process* process);
    const void* (*get_extension)(const clap_plugin* plugin, const char* id);
    void (*on_main_thread)(const clap_plugin* plugin);
};
constexpr char CLAP_PLUGIN_FACTORY_ID[] = "clap.plugin-factory";
struct clap_plugin_factory {
    uint32_t (*get_plugin_count)(const clap_plugin_factory* factory);
    const clap_plugin_descriptor* (*get_plugin_descriptor)(const clap_plugin_factory* factory, uint32_t index);
    const clap_plugin* (*create_plugin)(const clap_plugin_factory* factory, const clap_host* host, const char* plugin_id);
};
struct clap_plugin_entry {   // exported by every plugin as `clap_entry`
    clap::clap_version clap_version;   // qualified: the member shares the type's name
    bool (*init)(const char* plugin_path);
    void (*deinit)();
    const void* (*get_factory)(const char* factory_id);
};
constexpr char CLAP_PLUGIN_FEATURE_INSTRUMENT[] = "instrument";

// ---------------------------------------------------------------- streams
struct clap_istream {
    void* ctx;
    int64_t (*read)(const clap_istream* stream, void* buffer, uint64_t size);
};
struct clap_ostream {
    void* ctx;
    int64_t (*write)(const clap_ostream* stream, const void* buffer, uint64_t size);
};

// ---------------------------------------------------------------- extensions (one id string names both halves)
constexpr char CLAP_EXT_PARAMS[] = "clap.params";
enum : uint32_t {
    CLAP_PARAM_IS_STEPPED = 1 << 0,
    CLAP_PARAM_IS_PERIODIC = 1 << 1,
    CLAP_PARAM_IS_HIDDEN = 1 << 2,
    CLAP_PARAM_IS_READONLY = 1 << 3,
    CLAP_PARAM_IS_BYPASS = 1 << 4,
    CLAP_PARAM_IS_AUTOMATABLE = 1 << 5,
};
enum : uint32_t {
    CLAP_PARAM_RESCAN_VALUES = 1 << 0,
    CLAP_PARAM_RESCAN_TEXT = 1 << 1,
    CLAP_PARAM_RESCAN_INFO = 1 << 2,
    CLAP_PARAM_RESCAN_ALL = 1 << 3,
};
struct clap_param_info {
    clap_id id;
    uint32_t flags;
    void* cookie;
    char name[CLAP_NAME_SIZE];
    char module[CLAP_PATH_SIZE];
    double min_value;
    double max_value;
    double default_value;
};
struct clap_plugin_params {
    uint32_t (*count)(const clap_plugin* plugin);
    bool (*get_info)(const clap_plugin* plugin, uint32_t param_index, clap_param_info* param_info);
    bool (*get_value)(const clap_plugin* plugin, clap_id param_id, double* out_value);
    bool (*value_to_text)(const clap_plugin* plugin, clap_id param_id, double value, char* out_buffer, uint32_t out_buffer_capacity);
    bool (*text_to_value)(const clap_plugin* plugin, clap_id param_id, const char* param_value_text, double* out_value);
    void (*flush)(const clap_plugin* plugin, const clap_input_events* in, const clap_output_events* out);
};
struct clap_host_params {
    void (*rescan)(const clap_host* host, uint32_t flags);
    void (*clear)(const clap_host* host, clap_id param_id, uint32_t flags);
    void (*request_flush)(const clap_host* host);
};

constexpr char CLAP_EXT_AUDIO_PORTS[] = "clap.audio-ports";
enum : uint32_t { CLAP_AUDIO_PORT_IS_MAIN = 1 << 0 };
struct clap_audio_port_info {
    clap_id id;
    char name[CLAP_NAME_SIZE];
    uint32_t flags;
    uint32_t channel_count;
    const char* port_type;
    clap_id in_place_pair;
};
struct clap_plugin_audio_ports {
    uint32_t (*count)(const clap_plugin* plugin, bool is_input);
    bool (*get)(const clap_plugin* plugin, uint32_t index, bool is_input, clap_audio_port_info* info);
};

constexpr char CLAP_EXT_NOTE_PORTS[] = "clap.note-ports";
enum : uint32_t {
    CLAP_NOTE_DIALECT_CLAP = 1 << 0,
    CLAP_NOTE_DIALECT_MIDI = 1 << 1,
    CLAP_NOTE_DIALECT_MIDI_MPE = 1 << 2,
    CLAP_NOTE_DIALECT_MIDI2 = 1 << 3,
};
struct clap_note_port_info {
    clap_id id;
    uint32_t supported_dialects;
    uint32_t preferred_dialect;
    char name[CLAP_NAME_SIZE];
};
struct clap_plugin_note_ports {
    uint32_t (*count)(const clap_plugin* plugin, bool is_input);
    bool (*get)(const clap_plugin* plugin, uint32_t index, bool is_input, clap_note_port_info* info);
};

constexpr char CLAP_EXT_STATE[] = "clap.state";
struct clap_plugin_state {
    bool (*save)(const clap_plugin* plugin, const clap_ostream* stream);
    bool (*load)(const clap_plugin* plugin, const clap_istream* stream);
};
struct clap_host_state {
    void (*mark_dirty)(const clap_host* host);
};

constexpr char CLAP_EXT_LATENCY[] = "clap.latency";
struct clap_plugin_latency {
    uint32_t (*get)(const clap_plugin* plugin);
};
struct clap_host_latency {
    void (*changed)(const clap_host* host);
};

constexpr char CLAP_EXT_GUI[] = "clap.gui";
#ifdef _WIN32
constexpr char CLAP_WINDOW_API_EMBED[] = "win32";
#else
constexpr char CLAP_WINDOW_API_EMBED[] = "x11";
#endif
struct clap_window {
    const char* api;
    union {
        void* cocoa;
        unsigned long x11;
        void* win32;
        void* ptr;
    };
};
struct clap_gui_resize_hints {
    bool can_resize_horizontally;
    bool can_resize_vertically;
    bool preserve_aspect_ratio;
    uint32_t aspect_ratio_width;
    uint32_t aspect_ratio_height;
};
struct clap_plugin_gui {
    bool (*is_api_supported)(const clap_plugin* plugin, const char* api, bool is_floating);
    bool (*get_preferred_api)(const clap_plugin* plugin, const char** api, bool* is_floating);
    bool (*create)(const clap_plugin* plugin, const char* api, bool is_floating);
    void (*destroy)(const clap_plugin* plugin);
    bool (*set_scale)(const clap_plugin* plugin, double scale);
    bool (*get_size)(const clap_plugin* plugin, uint32_t* width, uint32_t* height);
    bool (*can_resize)(const clap_plugin* plugin);
    bool (*get_resize_hints)(const clap_plugin* plugin, clap_gui_resize_hints* hints);
    bool (*adjust_size)(const clap_plugin* plugin, uint32_t* width, uint32_t* height);
    bool (*set_size)(const clap_plugin* plugin, uint32_t width, uint32_t height);
    bool (*set_parent)(const clap_plugin* plugin, const clap_window* window);
    bool (*set_transient)(const clap_plugin* plugin, const clap_window* window);
    void (*suggest_title)(const clap_plugin* plugin, const char* title);
    bool (*show)(const clap_plugin* plugin);
    bool (*hide)(const clap_plugin* plugin);
};
struct clap_host_gui {
    void (*resize_hints_changed)(const clap_host* host);
    bool (*request_resize)(const clap_host* host, uint32_t width, uint32_t height);
    bool (*request_show)(const clap_host* host);
    bool (*request_hide)(const clap_host* host);
    void (*closed)(const clap_host* host, bool was_destroyed);
};

constexpr char CLAP_EXT_TIMER_SUPPORT[] = "clap.timer-support";
struct clap_plugin_timer_support {
    void (*on_timer)(const clap_plugin* plugin, clap_id timer_id);
};
struct clap_host_timer_support {
    bool (*register_timer)(const clap_host* host, uint32_t period_ms, clap_id* timer_id);
    bool (*unregister_timer)(const clap_host* host, clap_id timer_id);
};

constexpr char CLAP_EXT_POSIX_FD_SUPPORT[] = "clap.posix-fd-support";
enum : uint32_t {
    CLAP_POSIX_FD_READ = 1 << 0,
    CLAP_POSIX_FD_WRITE = 1 << 1,
    CLAP_POSIX_FD_ERROR = 1 << 2,
};
struct clap_plugin_posix_fd_support {
    void (*on_fd)(const clap_plugin* plugin, int fd, uint32_t flags);
};
struct clap_host_posix_fd_support {
    bool (*register_fd)(const clap_host* host, int fd, uint32_t flags);
    bool (*modify_fd)(const clap_host* host, int fd, uint32_t flags);
    bool (*unregister_fd)(const clap_host* host, int fd);
};

constexpr char CLAP_EXT_THREAD_CHECK[] = "clap.thread-check";
struct clap_host_thread_check {
    bool (*is_main_thread)(const clap_host* host);
    bool (*is_audio_thread)(const clap_host* host);
};

constexpr char CLAP_EXT_LOG[] = "clap.log";
enum : int32_t {
    CLAP_LOG_DEBUG = 0,
    CLAP_LOG_INFO = 1,
    CLAP_LOG_WARNING = 2,
    CLAP_LOG_ERROR = 3,
    CLAP_LOG_FATAL = 4,
    CLAP_LOG_HOST_MISBEHAVING = 5,
    CLAP_LOG_PLUGIN_MISBEHAVING = 6,
};
struct clap_host_log {
    void (*log)(const clap_host* host, int32_t severity, const char* msg);
};

}   // namespace clap
