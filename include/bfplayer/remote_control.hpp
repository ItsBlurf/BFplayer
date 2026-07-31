#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace bfplayer {

enum class RemoteCommandType {
    open,
    play,
    pause,
    toggle_pause,
    seek_relative,
    seek_absolute,
    cycle_audio,
    cycle_subtitle,
    button,
    stop,
    exit,
};

struct RemoteCommand {
    RemoteCommandType type = RemoteCommandType::stop;
    std::uint64_t sequence = 0;
    double value = 0.0;
    int button = -1;
    std::string path;
};

struct RemotePlaybackStatus {
    std::string phase = "starting";
    std::string media_path;
    bool running = true;
    bool playing = false;
    bool paused = false;
    int player_state = 0;
    double position_seconds = 0.0;
    double duration_seconds = 0.0;
    double source_fps = 0.0;
    double delivered_fps = 0.0;
    double loop_average_ms = 0.0;
    double loop_max_ms = 0.0;
    double video_pull_average_ms = 0.0;
    double video_pull_max_ms = 0.0;
    double render_average_ms = 0.0;
    double render_max_ms = 0.0;
    double present_average_ms = 0.0;
    double present_max_ms = 0.0;
    double present_p95_ms = 0.0;
    double present_p99_ms = 0.0;
    double audio_pull_average_ms = 0.0;
    double audio_pull_max_ms = 0.0;
    double cpu_core_equivalents = 0.0;
    double user_cpu_ms = 0.0;
    double system_cpu_ms = 0.0;
    std::uint64_t voluntary_context_switches = 0;
    std::uint64_t involuntary_context_switches = 0;
    std::uint64_t video_updates = 0;
    std::uint64_t video_empty_polls = 0;
    std::uint64_t estimated_missed_frames = 0;
    std::uint64_t peak_rss_kib = 0;
    std::uint64_t media_bytes_read = 0;
    std::uint64_t media_read_calls = 0;
    std::uint64_t media_read_time_us = 0;
    std::uint64_t media_seek_calls = 0;
    std::uint64_t media_seek_time_us = 0;
    std::uint64_t audio_queued_bytes = 0;
    unsigned int video_frames_length = 0;
    unsigned int video_frames_capacity = 0;
    unsigned int video_packets_length = 0;
    unsigned int video_packets_capacity = 0;
    unsigned int audio_frames_length = 0;
    unsigned int audio_frames_capacity = 0;
    unsigned int audio_packets_length = 0;
    unsigned int audio_packets_capacity = 0;
    int audio_stream = -1;
    int subtitle_stream = -1;
    int source_width = 0;
    int source_height = 0;
    int output_width = 0;
    int output_height = 0;
    int display_width = 0;
    int display_height = 0;
    bool hdr_source = false;
    bool native_hdr_output = false;
    std::string hdr_transfer;
    std::string hdr_output_policy;
    bool hdr_tone_map_active = false;
    bool hdr_input_full_range = false;
    bool hdr_input_bt2020 = false;
    double hdr_source_peak_nits = 0.0;
    double hdr_target_peak_nits = 0.0;
    double hdr_tone_map_average_ms = 0.0;
    std::uint64_t hdr_tone_map_frames = 0;
    std::uint64_t hdr_tone_map_time_us = 0;
    unsigned int hdr_tone_map_workers = 0;
};

// Authenticated, bounded LAN control used for repeatable PS5 hardware tests.
// The per-process token and port are written atomically to
// /data/BFplayer/automation.json.
class RemoteControlServer {
public:
    struct Impl;

    RemoteControlServer();
    ~RemoteControlServer();

    RemoteControlServer(const RemoteControlServer&) = delete;
    RemoteControlServer& operator=(const RemoteControlServer&) = delete;

    bool start(std::uint16_t port, const char* version, std::string& error);
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] const std::string& token() const noexcept;

    bool poll(RemoteCommand& command);
    void update_status(const RemotePlaybackStatus& status);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace bfplayer
