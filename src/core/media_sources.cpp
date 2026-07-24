#include "ps5mc/media_sources.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ps5mc {
namespace {

char hex_digit(unsigned int value) {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
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

std::string hex_encode(std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char byte : value) {
        encoded.push_back(hex_digit(byte >> 4U));
        encoded.push_back(hex_digit(byte & 0x0fU));
    }
    return encoded;
}

bool hex_decode(std::string_view encoded, std::string& value) {
    if ((encoded.size() & 1U) != 0U) {
        return false;
    }
    value.clear();
    value.reserve(encoded.size() / 2U);
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = hex_value(encoded[index]);
        const int low = hex_value(encoded[index + 1U]);
        if (high < 0 || low < 0) {
            value.clear();
            return false;
        }
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return value.find('\0') == std::string::npos;
}

bool path_is_within(const std::string& path, const std::string& root) {
    if (path == root) {
        return true;
    }
    return path.size() > root.size() &&
           path.compare(0, root.size(), root) == 0 &&
           (root == "/" || path[root.size()] == '/');
}

} // namespace

std::string normalize_media_source_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1U && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::string media_source_default_title(const std::string& path) {
    const std::string normalized = normalize_media_source_path(path);
    if (normalized == "/") {
        return "Root";
    }
    const std::size_t slash = normalized.find_last_of('/');
    const std::string name =
        slash == std::string::npos ? normalized : normalized.substr(slash + 1U);
    if (name.empty()) {
        return "Media";
    }
    std::string title = name;
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}

std::string serialize_media_sources(const std::vector<MediaSource>& sources) {
    std::string encoded = "v1\n";
    for (const MediaSource& source : sources) {
        const std::string path = normalize_media_source_path(source.path);
        if (path.empty() || path[0] != '/') {
            continue;
        }
        encoded.push_back(source.kind == MediaSourceKind::tv_folder ? 'T' : 'M');
        encoded.push_back('\t');
        encoded += hex_encode(path);
        encoded.push_back('\t');
        encoded += hex_encode(
            source.title.empty() ? media_source_default_title(path) : source.title);
        encoded.push_back('\n');
    }
    return encoded;
}

std::vector<MediaSource> parse_media_sources(const std::string& encoded) {
    std::vector<MediaSource> sources;
    std::size_t line_begin = 0;
    bool first_line = true;
    while (line_begin <= encoded.size()) {
        const std::size_t line_end = encoded.find('\n', line_begin);
        const std::string_view line(
            encoded.data() + line_begin,
            (line_end == std::string::npos ? encoded.size() : line_end) - line_begin);
        line_begin = line_end == std::string::npos ? encoded.size() + 1U : line_end + 1U;
        if (first_line) {
            first_line = false;
            if (line != "v1") {
                return {};
            }
            continue;
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t first_tab = line.find('\t');
        const std::size_t second_tab =
            first_tab == std::string_view::npos
                ? std::string_view::npos
                : line.find('\t', first_tab + 1U);
        if (first_tab != 1U || second_tab == std::string_view::npos ||
            (line[0] != 'M' && line[0] != 'T')) {
            continue;
        }
        std::string path;
        std::string title;
        if (!hex_decode(line.substr(first_tab + 1U, second_tab - first_tab - 1U), path) ||
            !hex_decode(line.substr(second_tab + 1U), title)) {
            continue;
        }
        path = normalize_media_source_path(std::move(path));
        if (path.empty() || path[0] != '/') {
            continue;
        }
        const auto duplicate = std::find_if(
            sources.begin(), sources.end(),
            [&](const MediaSource& item) {
                return item.kind ==
                           (line[0] == 'T'
                                ? MediaSourceKind::tv_folder
                                : MediaSourceKind::movie_file) &&
                       item.path == path;
            });
        if (duplicate == sources.end()) {
            if (title.empty()) {
                title = media_source_default_title(path);
            }
            sources.push_back({
                line[0] == 'T'
                    ? MediaSourceKind::tv_folder
                    : MediaSourceKind::movie_file,
                std::move(path),
                std::move(title)});
        }
    }
    return sources;
}

void annotate_media_sources(
    std::vector<MediaEntry>& entries,
    const std::vector<MediaSource>& sources) {
    for (MediaEntry& entry : entries) {
        entry.series_root.clear();
        entry.series_title.clear();
        entry.explicit_movie = false;
        const std::string normalized_path = normalize_media_source_path(entry.path);
        for (const MediaSource& source : sources) {
            const std::string source_path = normalize_media_source_path(source.path);
            if (source.kind == MediaSourceKind::movie_file) {
                if (normalized_path == source_path) {
                    entry.explicit_movie = true;
                    entry.series_root.clear();
                    entry.series_title.clear();
                    break;
                }
                continue;
            }
            if (path_is_within(normalized_path, source_path)) {
                // Prefer the most specific selected TV folder.
                if (entry.series_root.empty() ||
                    source_path.size() > entry.series_root.size()) {
                    entry.series_root = source_path;
                    entry.series_title = source.title.empty()
                        ? media_source_default_title(source_path)
                        : source.title;
                }
            }
        }
    }
}

} // namespace ps5mc
