#include "bfplayer/playlist.hpp"

#include "bfplayer/source_uri.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <filesystem>
#include <fstream>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bfplayer {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string extension_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    return ascii_lower(path.substr(dot + 1));
}

std::string trim(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    std::string result(value.substr(start, end - start));
    if (result.size() >= 2 &&
        ((result.front() == '"' && result.back() == '"') ||
         (result.front() == '\'' && result.back() == '\''))) {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

std::string directory_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

bool is_absolute_path(const std::string& path) {
    if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        return true;
    }
    return path.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

int hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

bool percent_decode_path(std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            decoded.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            return false;
        }
        const int high = hex_digit(value[index + 1]);
        const int low = hex_digit(value[index + 2]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    value = std::move(decoded);
    return true;
}

bool xml_unescape(std::string_view value, std::string& output) {
    output.clear();
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        if (value[index] != '&') {
            output.push_back(value[index++]);
            continue;
        }
        const std::size_t end = value.find(';', index + 1);
        if (end == std::string_view::npos || end - index > 6) {
            return false;
        }
        const std::string_view entity = value.substr(index, end - index + 1);
        if (entity == "&amp;") {
            output.push_back('&');
        } else if (entity == "&lt;") {
            output.push_back('<');
        } else if (entity == "&gt;") {
            output.push_back('>');
        } else if (entity == "&quot;") {
            output.push_back('"');
        } else if (entity == "&apos;") {
            output.push_back('\'');
        } else {
            return false;
        }
        index = end + 1;
    }
    return true;
}

std::string resolve_item(const std::string& playlist_path, std::string item) {
    item = trim(item);
    if (item.empty() || item.size() > 8192 || item.find('\0') != std::string::npos) {
        return {};
    }

    const std::string lowered = ascii_lower(item);
    if (lowered.rfind("file://", 0) == 0) {
        item.erase(0, 7);
        const std::string lower_authority = ascii_lower(item);
        if (lower_authority.rfind("localhost/", 0) == 0) {
            item.erase(0, std::strlen("localhost"));
        } else if (item.empty() || item.front() != '/') {
            return {}; // A remote file authority is not a local PS5 path.
        }
        if (!percent_decode_path(item)) {
            return {};
        }
    } else if (is_network_uri(item)) {
        return is_supported_stream_uri(item) && !uri_has_credentials(item)
            ? item
            : std::string{};
    }

    if (is_absolute_path(item)) {
        return item;
    }
    std::string result = directory_of(playlist_path);
    if (!result.empty() && result.back() != '/' && result.back() != '\\') {
        result.push_back('/');
    }
    result += item;
    return result;
}

bool read_bounded_regular_file(
    const std::string& path,
    std::size_t max_bytes,
    std::string& content,
    std::string& error) {
#if defined(_WIN32)
    std::error_code status_error;
    const std::u8string path_utf8(
        reinterpret_cast<const char8_t*>(path.data()),
        reinterpret_cast<const char8_t*>(path.data() + path.size()));
    const std::filesystem::path native(path_utf8);
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(native, status_error);
    if (status_error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        error = "Playlist is not a regular non-symlink file";
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(native, status_error);
    if (status_error || size > max_bytes) {
        error = "Playlist exceeds the size limit";
        return false;
    }
    std::ifstream input(native, std::ios::binary);
    if (!input) {
        error = "Unable to open playlist";
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad() || content.size() > max_bytes) {
        error = "Unable to read playlist within the size limit";
        return false;
    }
    return true;
#else
    struct stat before {};
    if (lstat(path.c_str(), &before) != 0 || !S_ISREG(before.st_mode) ||
        S_ISLNK(before.st_mode)) {
        error = "Playlist is not a regular non-symlink file";
        return false;
    }
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > max_bytes) {
        error = "Playlist exceeds the size limit";
        return false;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(path.c_str(), flags);
    if (descriptor < 0) {
        error = "Unable to open playlist: errno=" + std::to_string(errno);
        return false;
    }
    struct stat opened {};
    if (fstat(descriptor, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino) {
        error = "Playlist changed while opening";
        close(descriptor);
        return false;
    }
    char buffer[16384];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "Unable to read playlist: errno=" + std::to_string(errno);
            close(descriptor);
            return false; // Fatal descriptor errors are never retried.
        }
        if (static_cast<std::size_t>(count) > max_bytes - content.size()) {
            error = "Playlist exceeds the size limit";
            close(descriptor);
            return false;
        }
        content.append(buffer, static_cast<std::size_t>(count));
    }
    close(descriptor);
    return true;
#endif
}

void append_item(
    PlaylistLoadResult& result,
    const std::string& playlist_path,
    std::string value,
    std::size_t max_items) {
    if (result.items.size() >= max_items) {
        result.truncated = true;
        return;
    }
    value = resolve_item(playlist_path, std::move(value));
    if (!value.empty()) {
        result.items.push_back(std::move(value));
    }
}

void parse_m3u(
    PlaylistLoadResult& result,
    const std::string& path,
    std::string_view content,
    std::size_t max_items) {
    std::size_t start = 0;
    bool first_line = true;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        std::string line = trim(content.substr(
            start, (end == std::string_view::npos ? content.size() : end) - start));
        if (first_line && line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3);
        }
        first_line = false;
        if (!line.empty() && line.front() != '#') {
            append_item(result, path, std::move(line), max_items);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

void parse_pls(
    PlaylistLoadResult& result,
    const std::string& path,
    std::string_view content,
    std::size_t max_items) {
    std::size_t start = 0;
    while (start <= content.size()) {
        const std::size_t end = content.find('\n', start);
        const std::string line = trim(content.substr(
            start, (end == std::string_view::npos ? content.size() : end) - start));
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string key = ascii_lower(trim(std::string_view(line).substr(0, equals)));
            if (key.rfind("file", 0) == 0 && key.size() > 4 &&
                std::all_of(key.begin() + 4, key.end(), [](unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
                append_item(result, path, line.substr(equals + 1), max_items);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

void parse_xspf(
    PlaylistLoadResult& result,
    const std::string& path,
    const std::string& content,
    std::size_t max_items) {
    const std::string lowered = ascii_lower(content);
    std::size_t position = 0;
    while ((position = lowered.find("<location", position)) != std::string::npos) {
        const std::size_t open_end = lowered.find('>', position + 9);
        if (open_end == std::string::npos) {
            break;
        }
        const std::size_t close = lowered.find("</location>", open_end + 1);
        if (close == std::string::npos) {
            break;
        }
        std::string unescaped;
        if (xml_unescape(
                std::string_view(content).substr(open_end + 1, close - open_end - 1),
                unescaped)) {
            append_item(result, path, std::move(unescaped), max_items);
        }
        position = close + std::strlen("</location>");
    }
}

} // namespace

bool is_generic_playlist_path(const std::string& path) {
    const std::string extension = extension_of(path);
    return extension == "m3u" || extension == "pls" || extension == "xspf";
}

PlaylistLoadResult load_generic_playlist(
    const std::string& path,
    std::size_t max_bytes,
    std::size_t max_items) {
    PlaylistLoadResult result{};
    result.recognized = is_generic_playlist_path(path);
    if (!result.recognized) {
        return result;
    }
    if (max_bytes == 0 || max_items == 0) {
        result.error = "Invalid playlist limits";
        return result;
    }

    std::string content;
    if (!read_bounded_regular_file(path, max_bytes, content, result.error)) {
        return result;
    }
    if (content.find('\0') != std::string::npos) {
        result.error = "Playlist contains binary data";
        return result;
    }

    const std::string extension = extension_of(path);
    if (extension == "m3u") {
        parse_m3u(result, path, content, max_items);
    } else if (extension == "pls") {
        parse_pls(result, path, content, max_items);
    } else {
        parse_xspf(result, path, content, max_items);
    }
    if (result.items.empty()) {
        result.error = "Playlist contains no supported media entries";
    }
    return result;
}

} // namespace bfplayer
