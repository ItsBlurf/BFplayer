#include "bfplayer/remote_control.hpp"

#include "bfplayer/diagnostics.hpp"
#include "bfplayer/source_uri.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <utility>

#if defined(BFPLAYER_PS5)
#include <SDL.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

#if defined(BFPLAYER_PS5)

constexpr std::size_t kMaximumRequestBytes = 16 * 1024;
constexpr std::size_t kMaximumCommandQueue = 32;
constexpr std::size_t kMaximumPathBytes = 4096;
constexpr const char* kAutomationPath = "/data/BFplayer/automation.json";

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(hex[(byte >> 4U) & 0x0fU]);
                    escaped.push_back(hex[byte & 0x0fU]);
                } else {
                    escaped.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    return escaped;
}

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool percent_decode(
    const std::string& encoded,
    std::size_t maximum,
    std::string& decoded) {
    decoded.clear();
    decoded.reserve(std::min(encoded.size(), maximum));
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const char current = encoded[index];
        if (current == '%') {
            if (index + 2 >= encoded.size()) {
                return false;
            }
            const int high = hex_value(encoded[index + 1]);
            const int low = hex_value(encoded[index + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            const char decoded_byte = static_cast<char>((high << 4) | low);
            if (decoded_byte == '\0') {
                return false;
            }
            decoded.push_back(decoded_byte);
            index += 2;
        } else if (current == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(current);
        }
        if (decoded.size() > maximum) {
            return false;
        }
    }
    return true;
}

bool query_argument(
    const std::string& query,
    const char* wanted,
    std::size_t maximum,
    std::string& value) {
    std::size_t offset = 0;
    while (offset <= query.size()) {
        const std::size_t end = query.find('&', offset);
        const std::string field = query.substr(
            offset,
            end == std::string::npos ? std::string::npos : end - offset);
        const std::size_t equals = field.find('=');
        const std::string name = field.substr(0, equals);
        if (name == wanted && equals != std::string::npos) {
            return percent_decode(field.substr(equals + 1), maximum, value);
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return false;
}

bool parse_finite_double(const std::string& text, double& value) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || !end || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

const char* command_name(RemoteCommandType type) {
    switch (type) {
        case RemoteCommandType::open:
            return "open";
        case RemoteCommandType::play:
            return "play";
        case RemoteCommandType::pause:
            return "pause";
        case RemoteCommandType::toggle_pause:
            return "toggle";
        case RemoteCommandType::seek_relative:
            return "seek";
        case RemoteCommandType::seek_absolute:
            return "seek-to";
        case RemoteCommandType::stop:
            return "stop";
        case RemoteCommandType::exit:
            return "exit";
    }
    return "unknown";
}

std::string status_json(const RemotePlaybackStatus& status) {
    char numeric[4096];
    const int length = std::snprintf(
        numeric,
        sizeof(numeric),
        "\"running\":%s,\"playing\":%s,\"paused\":%s,"
        "\"playerState\":%d,\"positionSeconds\":%.6f,"
        "\"durationSeconds\":%.6f,\"sourceFps\":%.6f,"
        "\"deliveredFps\":%.6f,\"loopAverageMs\":%.6f,"
        "\"loopMaxMs\":%.6f,\"videoPullAverageMs\":%.6f,"
        "\"videoPullMaxMs\":%.6f,\"renderAverageMs\":%.6f,"
        "\"renderMaxMs\":%.6f,\"presentAverageMs\":%.6f,"
        "\"presentMaxMs\":%.6f,\"presentP95Ms\":%.6f,"
        "\"presentP99Ms\":%.6f,\"audioPullAverageMs\":%.6f,"
        "\"audioPullMaxMs\":%.6f,\"cpuCoreEquivalents\":%.6f,"
        "\"userCpuMs\":%.6f,\"systemCpuMs\":%.6f,"
        "\"voluntaryContextSwitches\":%llu,"
        "\"involuntaryContextSwitches\":%llu,"
        "\"videoUpdates\":%llu,\"videoEmptyPolls\":%llu,"
        "\"estimatedMissedFrames\":%llu,\"peakRssKiB\":%llu,"
        "\"mediaBytesRead\":%llu,\"mediaReadCalls\":%llu,"
        "\"mediaReadTimeUs\":%llu,\"mediaSeekCalls\":%llu,"
        "\"mediaSeekTimeUs\":%llu,"
        "\"audioQueuedBytes\":%llu,"
        "\"videoFrames\":{\"length\":%u,\"capacity\":%u},"
        "\"videoPackets\":{\"length\":%u,\"capacity\":%u},"
        "\"audioFrames\":{\"length\":%u,\"capacity\":%u},"
        "\"audioPackets\":{\"length\":%u,\"capacity\":%u},"
        "\"sourceWidth\":%d,\"sourceHeight\":%d,"
        "\"outputWidth\":%d,\"outputHeight\":%d,\"hdrSource\":%s,"
        "\"hdrToneMapActive\":%s,\"hdrInputFullRange\":%s,"
        "\"hdrInputBt2020\":%s,\"hdrSourcePeakNits\":%.3f,"
        "\"hdrTargetPeakNits\":%.3f,\"hdrToneMapAverageMs\":%.6f,"
        "\"hdrToneMapFrames\":%llu,\"hdrToneMapTimeUs\":%llu,"
        "\"hdrToneMapWorkers\":%u",
        status.running ? "true" : "false",
        status.playing ? "true" : "false",
        status.paused ? "true" : "false",
        status.player_state,
        status.position_seconds,
        status.duration_seconds,
        status.source_fps,
        status.delivered_fps,
        status.loop_average_ms,
        status.loop_max_ms,
        status.video_pull_average_ms,
        status.video_pull_max_ms,
        status.render_average_ms,
        status.render_max_ms,
        status.present_average_ms,
        status.present_max_ms,
        status.present_p95_ms,
        status.present_p99_ms,
        status.audio_pull_average_ms,
        status.audio_pull_max_ms,
        status.cpu_core_equivalents,
        status.user_cpu_ms,
        status.system_cpu_ms,
        static_cast<unsigned long long>(status.voluntary_context_switches),
        static_cast<unsigned long long>(status.involuntary_context_switches),
        static_cast<unsigned long long>(status.video_updates),
        static_cast<unsigned long long>(status.video_empty_polls),
        static_cast<unsigned long long>(status.estimated_missed_frames),
        static_cast<unsigned long long>(status.peak_rss_kib),
        static_cast<unsigned long long>(status.media_bytes_read),
        static_cast<unsigned long long>(status.media_read_calls),
        static_cast<unsigned long long>(status.media_read_time_us),
        static_cast<unsigned long long>(status.media_seek_calls),
        static_cast<unsigned long long>(status.media_seek_time_us),
        static_cast<unsigned long long>(status.audio_queued_bytes),
        status.video_frames_length,
        status.video_frames_capacity,
        status.video_packets_length,
        status.video_packets_capacity,
        status.audio_frames_length,
        status.audio_frames_capacity,
        status.audio_packets_length,
        status.audio_packets_capacity,
        status.source_width,
        status.source_height,
        status.output_width,
        status.output_height,
        status.hdr_source ? "true" : "false",
        status.hdr_tone_map_active ? "true" : "false",
        status.hdr_input_full_range ? "true" : "false",
        status.hdr_input_bt2020 ? "true" : "false",
        status.hdr_source_peak_nits,
        status.hdr_target_peak_nits,
        status.hdr_tone_map_average_ms,
        static_cast<unsigned long long>(status.hdr_tone_map_frames),
        static_cast<unsigned long long>(status.hdr_tone_map_time_us),
        status.hdr_tone_map_workers);
    if (length < 0 || static_cast<std::size_t>(length) >= sizeof(numeric)) {
        return "{\"ok\":false,\"error\":\"status serialization failed\"}";
    }
    return "{\"ok\":true,\"phase\":\"" + json_escape(status.phase) +
        "\",\"mediaPath\":\"" + json_escape(status.media_path) + "\"," +
        numeric + ",\"hdrTransfer\":\"" +
        json_escape(status.hdr_transfer) + "\"}";
}

bool write_all(int descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const ssize_t written = ::write(descriptor, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            errno = ENOSPC;
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool random_token(std::string& token) {
    unsigned char random[32]{};
    int descriptor = ::open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < sizeof(random)) {
        const ssize_t count = ::read(
            descriptor, random + offset, sizeof(random) - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ::close(descriptor);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    ::close(descriptor);

    static constexpr char hex[] = "0123456789abcdef";
    token.resize(sizeof(random) * 2);
    for (std::size_t index = 0; index < sizeof(random); ++index) {
        token[index * 2] = hex[random[index] >> 4U];
        token[index * 2 + 1] = hex[random[index] & 0x0fU];
    }
    return true;
}

bool write_automation_file(
    std::uint16_t port,
    const std::string& token,
    const char* version,
    std::string& error) {
    if (::mkdir("/data/BFplayer", 0700) != 0 && errno != EEXIST) {
        error = "mkdir automation directory: errno=" + std::to_string(errno);
        return false;
    }
    const std::string temporary =
        std::string(kAutomationPath) + ".tmp." +
        std::to_string(static_cast<long long>(::getpid())) + "." +
        token.substr(0, 16);
    const std::string json =
        "{\"version\":1,\"pid\":" +
        std::to_string(static_cast<long long>(::getpid())) +
        ",\"port\":" + std::to_string(port) +
        ",\"token\":\"" + json_escape(token) +
        "\",\"build\":\"" +
        json_escape(version ? version : "development") + "\"}\n";
    const int descriptor = ::open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
        0600);
    if (descriptor < 0) {
        error = "open automation state: errno=" + std::to_string(errno);
        return false;
    }
    bool ok = write_all(descriptor, json.data(), json.size());
    if (ok && ::fsync(descriptor) != 0) {
        ok = false;
    }
    const int saved_error = errno;
    ::close(descriptor);
    if (!ok) {
        ::unlink(temporary.c_str());
        error = "write automation state: errno=" +
            std::to_string(saved_error ? saved_error : EIO);
        return false;
    }
    if (::rename(temporary.c_str(), kAutomationPath) != 0) {
        const int rename_error = errno;
        ::unlink(temporary.c_str());
        error = "rename automation state: errno=" +
            std::to_string(rename_error);
        return false;
    }
    int directory = ::open("/data/BFplayer", O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        (void)::fsync(directory);
        ::close(directory);
    }
    return true;
}

bool send_all_socket(int socket, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t sent = ::send(socket, data, size, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

void send_response(
    int socket,
    int status,
    const char* reason,
    const std::string& body) {
    const std::string header =
        "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    (void)send_all_socket(socket, header.data(), header.size());
    (void)send_all_socket(socket, body.data(), body.size());
}

bool header_value(
    const std::string& request,
    const char* wanted,
    std::string& value) {
    const std::string wanted_lower = lower_ascii(wanted);
    std::size_t offset = request.find("\r\n");
    if (offset == std::string::npos) {
        return false;
    }
    offset += 2;
    while (offset < request.size()) {
        const std::size_t end = request.find("\r\n", offset);
        if (end == std::string::npos || end == offset) {
            break;
        }
        const std::string line = request.substr(offset, end - offset);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos &&
            lower_ascii(line.substr(0, colon)) == wanted_lower) {
            std::size_t begin = colon + 1;
            while (begin < line.size() &&
                   (line[begin] == ' ' || line[begin] == '\t')) {
                ++begin;
            }
            std::size_t finish = line.size();
            while (finish > begin &&
                   (line[finish - 1] == ' ' || line[finish - 1] == '\t')) {
                --finish;
            }
            value = line.substr(begin, finish - begin);
            return true;
        }
        offset = end + 2;
    }
    return false;
}

bool constant_time_equal(
    const std::string& first,
    const std::string& second) {
    if (first.size() != second.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        difference |= static_cast<unsigned char>(
            static_cast<unsigned char>(first[index]) ^
            static_cast<unsigned char>(second[index]));
    }
    return difference == 0;
}

#endif

} // namespace

struct RemoteControlServer::Impl {
    std::string token;
    RemotePlaybackStatus status;
#if defined(BFPLAYER_PS5)
    int listen_socket = -1;
    SDL_Thread* thread = nullptr;
    SDL_mutex* mutex = nullptr;
    SDL_atomic_t stopping{};
    std::deque<RemoteCommand> commands;
    std::uint64_t next_sequence = 1;
#endif
};

#if defined(BFPLAYER_PS5)
namespace {

bool enqueue_command(
    RemoteControlServer::Impl& impl,
    RemoteCommand& command,
    std::string& error) {
    SDL_LockMutex(impl.mutex);
    if (impl.commands.size() >= kMaximumCommandQueue) {
        SDL_UnlockMutex(impl.mutex);
        error = "command queue is full";
        return false;
    }
    command.sequence = impl.next_sequence++;
    impl.commands.push_back(std::move(command));
    SDL_UnlockMutex(impl.mutex);
    return true;
}

void handle_client(RemoteControlServer::Impl& impl, int client) {
    timeval timeout{};
    timeout.tv_sec = 2;
    (void)::setsockopt(
        client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(
        client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string request;
    request.reserve(2048);
    char buffer[2048];
    while (request.size() < kMaximumRequestBytes) {
        const std::size_t available =
            std::min(sizeof(buffer), kMaximumRequestBytes - request.size());
        const ssize_t count = ::recv(client, buffer, available, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(count));
        if (request.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    if (request.find("\r\n\r\n") == std::string::npos) {
        send_response(
            client, 400, "Bad Request",
            "{\"ok\":false,\"error\":\"incomplete request\"}");
        return;
    }

    const std::size_t first_line_end = request.find("\r\n");
    const std::string first_line = request.substr(0, first_line_end);
    const std::size_t first_space = first_line.find(' ');
    const std::size_t second_space = first_line.find(' ', first_space + 1);
    if (first_space == std::string::npos ||
        second_space == std::string::npos) {
        send_response(
            client, 400, "Bad Request",
            "{\"ok\":false,\"error\":\"bad request line\"}");
        return;
    }
    const std::string method = first_line.substr(0, first_space);
    const std::string target = first_line.substr(
        first_space + 1, second_space - first_space - 1);

    std::string supplied_token;
    if (!header_value(request, "X-BFplayer-Token", supplied_token) ||
        !constant_time_equal(supplied_token, impl.token)) {
        diagnostics_log(
            DiagnosticLevel::warning,
            "remote-control rejected reason=authentication method=%s",
            method.c_str());
        send_response(
            client, 401, "Unauthorized",
            "{\"ok\":false,\"error\":\"unauthorized\"}");
        return;
    }

    const std::size_t question = target.find('?');
    const std::string route = target.substr(0, question);
    const std::string query = question == std::string::npos
        ? std::string{}
        : target.substr(question + 1);
    if (method == "GET" && route == "/v1/status") {
        RemotePlaybackStatus snapshot;
        SDL_LockMutex(impl.mutex);
        snapshot = impl.status;
        SDL_UnlockMutex(impl.mutex);
        send_response(client, 200, "OK", status_json(snapshot));
        return;
    }
    if (method != "POST") {
        send_response(
            client, 405, "Method Not Allowed",
            "{\"ok\":false,\"error\":\"method not allowed\"}");
        return;
    }

    RemoteCommand command;
    std::string error;
    if (route == "/v1/open") {
        command.type = RemoteCommandType::open;
        if (!query_argument(
                query, "path", kMaximumPathBytes, command.path) ||
            command.path.empty()) {
            error = "missing or invalid path";
        }
    } else if (route == "/v1/play") {
        command.type = RemoteCommandType::play;
    } else if (route == "/v1/pause") {
        command.type = RemoteCommandType::pause;
    } else if (route == "/v1/toggle") {
        command.type = RemoteCommandType::toggle_pause;
    } else if (route == "/v1/seek" || route == "/v1/seek-to") {
        command.type = route == "/v1/seek"
            ? RemoteCommandType::seek_relative
            : RemoteCommandType::seek_absolute;
        std::string seconds;
        if (!query_argument(query, "seconds", 64, seconds) ||
            !parse_finite_double(seconds, command.value)) {
            error = "missing or invalid seconds";
        }
    } else if (route == "/v1/stop") {
        command.type = RemoteCommandType::stop;
    } else if (route == "/v1/exit") {
        command.type = RemoteCommandType::exit;
    } else {
        send_response(
            client, 404, "Not Found",
            "{\"ok\":false,\"error\":\"unknown endpoint\"}");
        return;
    }
    if (!error.empty() || !enqueue_command(impl, command, error)) {
        send_response(
            client,
            error == "command queue is full" ? 429 : 400,
            error == "command queue is full" ? "Too Many Requests" : "Bad Request",
            "{\"ok\":false,\"error\":\"" + json_escape(error) + "\"}");
        return;
    }
    const std::string logged_path = command.path.empty()
        ? std::string{"<none>"}
        : redact_uri_secrets(command.path);
    diagnostics_log(
        DiagnosticLevel::info,
        "remote-command queued sequence=%llu command=%s value=%.3f path=%s",
        static_cast<unsigned long long>(command.sequence),
        command_name(command.type),
        command.value,
        logged_path.c_str());
    send_response(
        client,
        202,
        "Accepted",
        "{\"ok\":true,\"sequence\":" +
            std::to_string(command.sequence) + "}");
}

int remote_thread(void* opaque) {
    auto& impl = *static_cast<RemoteControlServer::Impl*>(opaque);
    while (!SDL_AtomicGet(&impl.stopping)) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(impl.listen_socket, &readable);
        timeval timeout{};
        timeout.tv_usec = 250000;
        const int selected = ::select(
            impl.listen_socket + 1,
            &readable,
            nullptr,
            nullptr,
            &timeout);
        if (selected < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!SDL_AtomicGet(&impl.stopping)) {
                diagnostics_log(
                    DiagnosticLevel::error,
                    "remote-control select failed errno=%d",
                    errno);
            }
            break;
        }
        if (selected == 0 || !FD_ISSET(impl.listen_socket, &readable)) {
            continue;
        }
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client = ::accept(
            impl.listen_socket,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_size);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!SDL_AtomicGet(&impl.stopping)) {
                diagnostics_log(
                    DiagnosticLevel::warning,
                    "remote-control accept failed errno=%d",
                    errno);
            }
            continue;
        }
        handle_client(impl, client);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }
    return 0;
}

} // namespace
#endif

RemoteControlServer::RemoteControlServer()
    : impl_(std::make_unique<Impl>()) {
}

RemoteControlServer::~RemoteControlServer() {
    stop();
}

bool RemoteControlServer::start(
    std::uint16_t port,
    const char* version,
    std::string& error) {
    error.clear();
#if !defined(BFPLAYER_PS5)
    (void)port;
    (void)version;
    error = "Remote control is available only in the PS5 build";
    return false;
#else
    if (impl_->thread) {
        error = "Remote control is already running";
        return false;
    }
    if (!random_token(impl_->token)) {
        error = "Unable to generate remote-control token";
        return false;
    }
    impl_->mutex = SDL_CreateMutex();
    if (!impl_->mutex) {
        error = std::string("SDL_CreateMutex: ") + SDL_GetError();
        impl_->token.clear();
        return false;
    }
    impl_->listen_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_socket < 0) {
        error = "socket: errno=" + std::to_string(errno);
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
        impl_->token.clear();
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(
        impl_->listen_socket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(
            impl_->listen_socket,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0 ||
        ::listen(impl_->listen_socket, 4) != 0) {
        error = "bind/listen: errno=" + std::to_string(errno);
        ::close(impl_->listen_socket);
        impl_->listen_socket = -1;
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
        impl_->token.clear();
        return false;
    }
    SDL_AtomicSet(&impl_->stopping, 0);
    if (!write_automation_file(port, impl_->token, version, error)) {
        ::close(impl_->listen_socket);
        impl_->listen_socket = -1;
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
        impl_->token.clear();
        return false;
    }
    impl_->thread = SDL_CreateThread(
        remote_thread, "bfplayer-remote", impl_.get());
    if (!impl_->thread) {
        error = std::string("SDL_CreateThread: ") + SDL_GetError();
        ::unlink(kAutomationPath);
        ::close(impl_->listen_socket);
        impl_->listen_socket = -1;
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
        impl_->token.clear();
        return false;
    }
    diagnostics_log(
        DiagnosticLevel::info,
        "remote-control started bind=0.0.0.0 port=%u state=%s auth=token",
        static_cast<unsigned int>(port),
        kAutomationPath);
    return true;
#endif
}

void RemoteControlServer::stop() noexcept {
#if defined(BFPLAYER_PS5)
    if (!impl_ || !impl_->thread) {
        return;
    }
    SDL_AtomicSet(&impl_->stopping, 1);
    if (impl_->listen_socket >= 0) {
        ::shutdown(impl_->listen_socket, SHUT_RDWR);
    }
    SDL_WaitThread(impl_->thread, nullptr);
    impl_->thread = nullptr;
    if (impl_->listen_socket >= 0) {
        ::close(impl_->listen_socket);
        impl_->listen_socket = -1;
    }
    ::unlink(kAutomationPath);
    if (impl_->mutex) {
        SDL_DestroyMutex(impl_->mutex);
        impl_->mutex = nullptr;
    }
    impl_->commands.clear();
    impl_->token.clear();
    diagnostics_log(DiagnosticLevel::info, "remote-control stopped");
#endif
}

bool RemoteControlServer::is_running() const noexcept {
#if defined(BFPLAYER_PS5)
    return impl_ && impl_->thread != nullptr;
#else
    return false;
#endif
}

const std::string& RemoteControlServer::token() const noexcept {
    return impl_->token;
}

bool RemoteControlServer::poll(RemoteCommand& command) {
#if !defined(BFPLAYER_PS5)
    (void)command;
    return false;
#else
    if (!impl_->mutex) {
        return false;
    }
    SDL_LockMutex(impl_->mutex);
    if (impl_->commands.empty()) {
        SDL_UnlockMutex(impl_->mutex);
        return false;
    }
    command = std::move(impl_->commands.front());
    impl_->commands.pop_front();
    SDL_UnlockMutex(impl_->mutex);
    return true;
#endif
}

void RemoteControlServer::update_status(
    const RemotePlaybackStatus& status) {
#if defined(BFPLAYER_PS5)
    if (!impl_->mutex) {
        return;
    }
    SDL_LockMutex(impl_->mutex);
    impl_->status = status;
    SDL_UnlockMutex(impl_->mutex);
#else
    impl_->status = status;
#endif
}

} // namespace bfplayer
