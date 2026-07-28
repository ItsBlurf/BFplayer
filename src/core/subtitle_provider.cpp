#include "bfplayer/subtitle_provider.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

#if defined(BFPLAYER_PS5)
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#else
#include <filesystem>
#endif

namespace bfplayer {
namespace {

constexpr std::size_t kMaximumJsonBytes = 1024 * 1024;
constexpr std::size_t kMaximumSubtitleBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumJsonNodes = 30000;
constexpr int kMaximumJsonDepth = 32;

enum class JsonKind {
    null_value,
    boolean,
    number,
    string,
    array,
    object,
};

struct JsonValue {
    JsonKind kind = JsonKind::null_value;
    bool boolean = false;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& output, std::string& error) {
        skip_space();
        if (!parse_value(output, 0)) {
            error = error_.empty() ? "Invalid provider response" : error_;
            return false;
        }
        skip_space();
        if (position_ != input_.size()) {
            error = "Unexpected data after provider response";
            return false;
        }
        return true;
    }

private:
    void skip_space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool allocate_node(int depth) {
        if (depth > kMaximumJsonDepth) {
            error_ = "Provider response is nested too deeply";
            return false;
        }
        if (++nodes_ > kMaximumJsonNodes) {
            error_ = "Provider response is too complex";
            return false;
        }
        return true;
    }

    static void append_utf8(std::string& output, unsigned int value) {
        if (value <= 0x7fU) {
            output.push_back(static_cast<char>(value));
        } else if (value <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
            output.push_back(
                static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
    }

    bool parse_hex4(unsigned int& output) {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        output = 0;
        for (int index = 0; index < 4; ++index) {
            const char value = input_[position_++];
            output <<= 4U;
            if (value >= '0' && value <= '9') {
                output |= static_cast<unsigned int>(value - '0');
            } else if (value >= 'a' && value <= 'f') {
                output |= static_cast<unsigned int>(value - 'a' + 10);
            } else if (value >= 'A' && value <= 'F') {
                output |= static_cast<unsigned int>(value - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parse_string(std::string& output) {
        skip_space();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20U) {
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escape = input_[position_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escape);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    unsigned int codepoint = 0;
                    if (!parse_hex4(codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU &&
                        position_ + 6 <= input_.size() &&
                        input_[position_] == '\\' &&
                        input_[position_ + 1] == 'u') {
                        position_ += 2;
                        unsigned int low = 0;
                        if (!parse_hex4(low) ||
                            low < 0xdc00U || low > 0xdfffU) {
                            return false;
                        }
                        codepoint =
                            0x10000U +
                            ((codepoint - 0xd800U) << 10U) +
                            (low - 0xdc00U);
                        if (codepoint <= 0xffffU) {
                            return false;
                        }
                        output.push_back(
                            static_cast<char>(
                                0xf0U | (codepoint >> 18U)));
                        output.push_back(
                            static_cast<char>(
                                0x80U | ((codepoint >> 12U) & 0x3fU)));
                        output.push_back(
                            static_cast<char>(
                                0x80U | ((codepoint >> 6U) & 0x3fU)));
                        output.push_back(
                            static_cast<char>(
                                0x80U | (codepoint & 0x3fU)));
                    } else if (codepoint >= 0xd800U &&
                               codepoint <= 0xdfffU) {
                        return false;
                    } else {
                        append_utf8(output, codepoint);
                    }
                    break;
                }
                default:
                    return false;
            }
            if (output.size() > kMaximumJsonBytes) {
                return false;
            }
        }
        return false;
    }

    bool parse_number(std::string& output) {
        skip_space();
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        const std::size_t integer_begin = position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == integer_begin) {
            return false;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_begin = position_;
            while (position_ < input_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (position_ == fraction_begin) {
                return false;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_begin = position_;
            while (position_ < input_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (position_ == exponent_begin) {
                return false;
            }
        }
        output.assign(input_.substr(begin, position_ - begin));
        return true;
    }

    bool parse_value(JsonValue& output, int depth) {
        if (!allocate_node(depth)) {
            return false;
        }
        skip_space();
        if (position_ >= input_.size()) {
            return false;
        }
        if (input_[position_] == '"') {
            output.kind = JsonKind::string;
            return parse_string(output.text);
        }
        if (input_[position_] == '{') {
            output.kind = JsonKind::object;
            ++position_;
            skip_space();
            if (consume('}')) {
                return true;
            }
            for (;;) {
                std::string key;
                JsonValue value;
                if (!parse_string(key) || !consume(':') ||
                    !parse_value(value, depth + 1)) {
                    return false;
                }
                output.object.emplace_back(std::move(key), std::move(value));
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (input_[position_] == '[') {
            output.kind = JsonKind::array;
            ++position_;
            skip_space();
            if (consume(']')) {
                return true;
            }
            for (;;) {
                JsonValue value;
                if (!parse_value(value, depth + 1)) {
                    return false;
                }
                output.array.push_back(std::move(value));
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            output.kind = JsonKind::boolean;
            output.boolean = true;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            output.kind = JsonKind::boolean;
            output.boolean = false;
            return true;
        }
        if (input_.substr(position_, 4) == "null") {
            position_ += 4;
            output.kind = JsonKind::null_value;
            return true;
        }
        output.kind = JsonKind::number;
        return parse_number(output.text);
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;
    std::string error_;
};

const JsonValue* member(
    const JsonValue& value,
    std::initializer_list<std::string_view> names) {
    if (value.kind != JsonKind::object) {
        return nullptr;
    }
    for (const std::string_view name : names) {
        for (const auto& [key, child] : value.object) {
            if (key == name) {
                return &child;
            }
        }
    }
    return nullptr;
}

std::string text_member(
    const JsonValue& value,
    std::initializer_list<std::string_view> names) {
    const JsonValue* child = member(value, names);
    if (!child) {
        return {};
    }
    if (child->kind == JsonKind::string ||
        child->kind == JsonKind::number) {
        return child->text;
    }
    return {};
}

bool bool_member(
    const JsonValue& value,
    std::initializer_list<std::string_view> names) {
    const JsonValue* child = member(value, names);
    if (!child) {
        return false;
    }
    if (child->kind == JsonKind::boolean) {
        return child->boolean;
    }
    return (child->kind == JsonKind::number ||
            child->kind == JsonKind::string) &&
           (child->text == "1" || child->text == "true");
}

std::string bounded_text(std::string value, std::size_t maximum) {
    if (value.size() > maximum) {
        value.resize(maximum);
        while (!value.empty() &&
               (static_cast<unsigned char>(value.back()) & 0xc0U) == 0x80U) {
            value.pop_back();
        }
    }
    return value;
}

bool valid_id(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(
            value.begin(),
            value.end(),
            [](unsigned char character) {
                return std::isalnum(character) ||
                       character == '-' || character == '_';
            });
}

bool trusted_download_url(const std::string& value) {
    constexpr std::string_view prefix =
        "https://dl.subdl.com/subtitle/";
    if (!value.starts_with(prefix) || value.size() > 2048) {
        return false;
    }
    return value.find('\r') == std::string::npos &&
           value.find('\n') == std::string::npos &&
           value.find('#') == std::string::npos;
}

std::string normalize_download_url(std::string value) {
    if (value.starts_with("/subtitle/")) {
        value.insert(0, "https://dl.subdl.com");
    }
    return trusted_download_url(value) ? value : std::string{};
}

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string normalized_extension(std::string value) {
    value = lowercase_ascii(std::move(value));
    if (!value.empty() && value.front() == '.') {
        value.erase(value.begin());
    }
    static constexpr std::array<std::string_view, 13> supported{
        "srt", "ass", "ssa", "vtt", "sub", "sup", "idx",
        "smi", "sami", "ttml", "dfxp", "stl", "txt"};
    return std::find(supported.begin(), supported.end(), value) !=
            supported.end()
        ? value
        : "srt";
}

void append_candidate(
    const JsonValue& value,
    const std::string& inherited_id,
    OnlineSubtitleSearch& output) {
    std::string id = text_member(value, {"n_id", "nId", "subtitle_id"});
    if (!valid_id(id)) {
        id = inherited_id;
    }
    std::string url = normalize_download_url(
        text_member(value, {"download_url", "downloadUrl", "url"}));
    if (!valid_id(id) && url.empty()) {
        return;
    }

    OnlineSubtitle subtitle;
    subtitle.id = std::move(id);
    subtitle.download_url = std::move(url);
    subtitle.language = bounded_text(
        text_member(
            value,
            {"language", "language_code", "lang", "language_name"}),
        64);
    subtitle.release_name = bounded_text(
        text_member(
            value,
            {"release_name", "file_name", "filename", "name", "title"}),
        256);
    subtitle.format = normalized_extension(
        text_member(value, {"format", "extension", "ext"}));
    subtitle.fps = bounded_text(
        text_member(value, {"fps", "framerate"}),
        24);
    subtitle.hearing_impaired =
        bool_member(value, {"hi", "hearing_impaired"});
    if (subtitle.release_name.empty()) {
        subtitle.release_name = "SubDL subtitle";
    }
    const auto duplicate = std::find_if(
        output.subtitles.begin(),
        output.subtitles.end(),
        [&](const OnlineSubtitle& current) {
            return current.id == subtitle.id &&
                   current.download_url == subtitle.download_url &&
                   current.release_name == subtitle.release_name;
        });
    if (duplicate == output.subtitles.end() &&
        output.subtitles.size() < 100) {
        output.subtitles.push_back(std::move(subtitle));
    }
}

void collect_subtitles(
    const JsonValue& value,
    const std::string& inherited_id,
    bool inside_subtitles,
    OnlineSubtitleSearch& output,
    int depth) {
    if (depth > kMaximumJsonDepth || output.subtitles.size() >= 100) {
        return;
    }
    if (value.kind == JsonKind::array) {
        for (const JsonValue& child : value.array) {
            collect_subtitles(
                child,
                inherited_id,
                inside_subtitles,
                output,
                depth + 1);
        }
        return;
    }
    if (value.kind != JsonKind::object) {
        return;
    }
    std::string local_id =
        text_member(value, {"n_id", "nId", "subtitle_id"});
    if (!valid_id(local_id)) {
        local_id = inherited_id;
    }
    const JsonValue* unpack_files =
        member(value, {"unpack_files", "files"});
    if (inside_subtitles && unpack_files &&
        unpack_files->kind == JsonKind::array) {
        for (const JsonValue& child : unpack_files->array) {
            if (child.kind == JsonKind::object) {
                append_candidate(child, local_id, output);
            }
        }
    } else if (inside_subtitles) {
        append_candidate(value, local_id, output);
    }
    for (const auto& [key, child] : value.object) {
        const bool child_is_subtitles =
            inside_subtitles ||
            key == "subtitles" ||
            key == "subtitle_results";
        if (child.kind == JsonKind::array ||
            child.kind == JsonKind::object) {
            collect_subtitles(
                child,
                local_id,
                child_is_subtitles,
                output,
                depth + 1);
        }
    }
}

std::string provider_error(const JsonValue& root) {
    const JsonValue* error = member(root, {"error", "message"});
    if (!error) {
        return {};
    }
    if (error->kind == JsonKind::string) {
        return bounded_text(error->text, 256);
    }
    if (error->kind == JsonKind::object) {
        return bounded_text(
            text_member(*error, {"message", "code"}),
            256);
    }
    return {};
}

std::string percent_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() * 3);
    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' ||
            character == '_' || character == '.' || character == '~') {
            output.push_back(static_cast<char>(character));
        } else {
            output.push_back('%');
            output.push_back(hex[character >> 4U]);
            output.push_back(hex[character & 0x0fU]);
        }
    }
    return output;
}

#if defined(BFPLAYER_PS5)
bool valid_api_key(const std::string& api_key) {
    return !api_key.empty() && api_key.size() <= 256 &&
        std::none_of(
            api_key.begin(),
            api_key.end(),
            [](unsigned char character) {
                return character < 0x21U || character > 0x7eU ||
                       character == '\r' || character == '\n';
            });
}
#endif

#if defined(BFPLAYER_PS5)
bool read_https(
    const std::string& url,
    const std::string& api_key,
    bool authenticate,
    std::size_t maximum,
    std::vector<std::uint8_t>& output,
    std::string& error) {
    output.clear();
    if (authenticate && !valid_api_key(api_key)) {
        error = "A valid SubDL API key is required";
        return false;
    }
    if ((!url.starts_with("https://api.subdl.com/") &&
         !url.starts_with("https://dl.subdl.com/")) ||
        url.find('\r') != std::string::npos ||
        url.find('\n') != std::string::npos) {
        error = "Subtitle provider returned an unsafe URL";
        return false;
    }
    AVIOContext* context = nullptr;
    AVDictionary* options = nullptr;
    std::string headers =
        "User-Agent: BFplayer/" BFPLAYER_VERSION "\r\n";
    if (authenticate) {
        headers =
            "Authorization: Bearer " + api_key +
            "\r\nX-API-Key: " + api_key + "\r\n" + headers;
        // Never forward a credential-bearing request to another host.
        av_dict_set(&options, "max_redirects", "0", 0);
    }
    av_dict_set(&options, "headers", headers.c_str(), 0);
    av_dict_set(&options, "rw_timeout", "10000000", 0);
    av_dict_set(&options, "reconnect", "1", 0);
    const int open_result =
        avio_open2(&context, url.c_str(), AVIO_FLAG_READ, nullptr, &options);
    av_dict_free(&options);
    if (open_result < 0 || !context) {
        char message[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(open_result, message, sizeof(message));
        error = "Unable to connect to SubDL: " + std::string(message);
        return false;
    }
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        const int count =
            avio_read(context, buffer.data(), static_cast<int>(buffer.size()));
        if (count == AVERROR_EOF || count == 0) {
            break;
        }
        if (count < 0) {
            char message[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(count, message, sizeof(message));
            error = "Subtitle download failed: " + std::string(message);
            avio_closep(&context);
            output.clear();
            return false;
        }
        if (output.size() >
            maximum - static_cast<std::size_t>(count)) {
            error = "Subtitle provider response is too large";
            avio_closep(&context);
            output.clear();
            return false;
        }
        output.insert(
            output.end(),
            buffer.begin(),
            buffer.begin() + count);
    }
    avio_closep(&context);
    if (output.empty()) {
        error = "Subtitle provider returned an empty response";
        return false;
    }
    return true;
}
#endif

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_hex(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::uint64_t hash = fnv1a(value);
    std::string output(16, '0');
    for (int index = 15; index >= 0; --index) {
        output[static_cast<std::size_t>(index)] = hex[hash & 0x0fU];
        hash >>= 4U;
    }
    return output;
}

} // namespace

std::string normalize_subtitle_languages(std::string value) {
    std::string output;
    bool last_comma = true;
    for (unsigned char character : value) {
        if (std::isspace(character) || character == ';') {
            character = ',';
        }
        if (character == ',') {
            if (!last_comma && !output.empty()) {
                output.push_back(',');
                last_comma = true;
            }
            continue;
        }
        if (!std::isalnum(character) && character != '-') {
            continue;
        }
        if (output.size() >= 96) {
            break;
        }
        output.push_back(static_cast<char>(std::tolower(character)));
        last_comma = false;
    }
    while (!output.empty() && output.back() == ',') {
        output.pop_back();
    }
    return output.empty() ? "en" : output;
}

std::string subdl_search_url(
    const std::string& media_filename,
    const std::string& languages) {
    std::string filename = media_filename;
    const std::size_t slash = filename.find_last_of("/\\");
    if (slash != std::string::npos) {
        filename.erase(0, slash + 1);
    }
    if (filename.size() > 512) {
        filename.resize(512);
    }
    return
        "https://api.subdl.com/api/v2/files/search?filename=" +
        percent_encode(filename) +
        "&languages=" +
        percent_encode(normalize_subtitle_languages(languages)) +
        "&subs_per_page=30";
}

std::string subdl_title_search_url(
    const std::string& title,
    const std::string& languages) {
    std::string bounded_title = title;
    if (bounded_title.size() > 256) {
        bounded_title.resize(256);
    }
    return
        "https://api.subdl.com/api/v2/subtitles/search?film_name=" +
        percent_encode(bounded_title) +
        "&languages=" +
        percent_encode(normalize_subtitle_languages(languages)) +
        "&subs_per_page=30&unpack=1";
}

OnlineSubtitleSearch parse_subdl_search_json(const std::string& json) {
    OnlineSubtitleSearch output;
    if (json.empty() || json.size() > kMaximumJsonBytes) {
        output.error = "Subtitle provider response has an invalid size";
        return output;
    }
    JsonValue root;
    if (!JsonParser(json).parse(root, output.error)) {
        return output;
    }
    const std::string error = provider_error(root);
    if (!error.empty()) {
        output.error = error;
        return output;
    }
    collect_subtitles(root, {}, false, output, 0);
    if (output.subtitles.empty()) {
        output.error = "No matching subtitles were found";
    }
    return output;
}

OnlineSubtitleSearch search_subdl(
    const std::string& api_key,
    const std::string& media_filename,
    const std::string& languages) {
#if defined(BFPLAYER_PS5)
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_https(
            subdl_search_url(media_filename, languages),
            api_key,
            true,
            kMaximumJsonBytes,
            bytes,
            error)) {
        return {{}, std::move(error)};
    }
    return parse_subdl_search_json(
        std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()));
#else
    (void)api_key;
    (void)media_filename;
    (void)languages;
    return {{}, "SubDL network access is available only in the PS5 build"};
#endif
}

OnlineSubtitleSearch search_subdl_title(
    const std::string& api_key,
    const std::string& title,
    const std::string& languages) {
#if defined(BFPLAYER_PS5)
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_https(
            subdl_title_search_url(title, languages),
            api_key,
            true,
            kMaximumJsonBytes,
            bytes,
            error)) {
        return {{}, std::move(error)};
    }
    return parse_subdl_search_json(
        std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()));
#else
    (void)api_key;
    (void)title;
    (void)languages;
    return {{}, "SubDL network access is available only in the PS5 build"};
#endif
}

OnlineSubtitleDownload download_subdl(
    const std::string& api_key,
    const OnlineSubtitle& subtitle) {
    OnlineSubtitleDownload output;
    std::string url = subtitle.download_url;
    [[maybe_unused]] bool authenticate = false;
    url = normalize_download_url(std::move(url));
    if (url.empty()) {
        if (!valid_id(subtitle.id)) {
            output.error = "Subtitle result has no safe download identifier";
            return output;
        }
        url =
            "https://api.subdl.com/api/v2/subtitles/" +
            subtitle.id +
            "/download?format=file";
        authenticate = true;
    }
#if defined(BFPLAYER_PS5)
    if (!read_https(
            url,
            api_key,
            authenticate,
            kMaximumSubtitleBytes,
            output.bytes,
            output.error)) {
        return output;
    }
    if (output.bytes.size() >= 4 &&
        output.bytes[0] == 'P' && output.bytes[1] == 'K' &&
        output.bytes[2] == 3 && output.bytes[3] == 4) {
        output.bytes.clear();
        output.error =
            "This result is a subtitle pack; choose a single-file result";
        return output;
    }
    if (!output.bytes.empty() &&
        (output.bytes.front() == '{' || output.bytes.front() == '[')) {
        const std::string response(
            reinterpret_cast<const char*>(output.bytes.data()),
            output.bytes.size());
        const OnlineSubtitleSearch parsed =
            parse_subdl_search_json(response);
        output.bytes.clear();
        output.error = parsed.error.empty()
            ? "SubDL returned metadata instead of a subtitle file"
            : parsed.error;
        return output;
    }
    const std::size_t prefix_size =
        std::min<std::size_t>(output.bytes.size(), 256);
    std::string prefix(
        reinterpret_cast<const char*>(output.bytes.data()),
        prefix_size);
    prefix = lowercase_ascii(std::move(prefix));
    if (prefix.find("<html") != std::string::npos ||
        prefix.find("<!doctype html") != std::string::npos) {
        output.bytes.clear();
        output.error =
            "SubDL returned a web page instead of a subtitle file";
        return output;
    }
    output.extension = normalized_extension(subtitle.format);
    return output;
#else
    (void)api_key;
    output.error =
        "SubDL network access is available only in the PS5 build";
    return output;
#endif
}

bool save_downloaded_subtitle(
    const std::string& root,
    const std::string& media_path,
    const OnlineSubtitle& subtitle,
    const OnlineSubtitleDownload& download,
    std::string& saved_path,
    std::string& error) {
    saved_path.clear();
    if (!download.ok() || download.bytes.size() > kMaximumSubtitleBytes) {
        error = download.error.empty()
            ? "Downloaded subtitle is invalid"
            : download.error;
        return false;
    }
    const std::string extension =
        normalized_extension(download.extension);
    const std::string identity =
        media_path + "\n" + subtitle.id + "\n" +
        subtitle.download_url + "\n" + subtitle.release_name;
    const std::string filename =
        "subdl-" + hash_hex(identity) + "." + extension;
    const std::string normalized_root =
        !root.empty() && root.back() == '/'
        ? root.substr(0, root.size() - 1)
        : root;
    if (normalized_root.empty()) {
        error = "Subtitle storage path is invalid";
        return false;
    }
    const std::string final_path = normalized_root + "/" + filename;
    const std::string temporary_path =
        final_path + ".tmp-" + hash_hex(filename + media_path);

#if defined(BFPLAYER_PS5)
    struct stat root_status {};
    if (lstat(normalized_root.c_str(), &root_status) != 0) {
        if (errno != ENOENT ||
            mkdir(normalized_root.c_str(), 0755) != 0) {
            error =
                "Unable to create subtitle storage (errno " +
                std::to_string(errno) + ")";
            return false;
        }
        if (lstat(normalized_root.c_str(), &root_status) != 0) {
            error = "Unable to verify subtitle storage";
            return false;
        }
    }
    if (!S_ISDIR(root_status.st_mode) || S_ISLNK(root_status.st_mode)) {
        error = "Subtitle storage is not a safe directory";
        return false;
    }
    struct statvfs space {};
    if (statvfs(normalized_root.c_str(), &space) != 0) {
        error =
            "Unable to check subtitle storage space (errno " +
            std::to_string(errno) + ")";
        return false;
    }
    const std::uint64_t block_size =
        static_cast<std::uint64_t>(
            space.f_frsize != 0 ? space.f_frsize : space.f_bsize);
    if (block_size == 0 ||
        static_cast<std::uint64_t>(space.f_bavail) >
            std::numeric_limits<std::uint64_t>::max() / block_size) {
        error = "Subtitle storage space report is invalid";
        return false;
    }
    const std::uint64_t available =
        static_cast<std::uint64_t>(space.f_bavail) * block_size;
    if (available < download.bytes.size() + 1024 * 1024) {
        error = "Not enough free space for the subtitle";
        return false;
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(temporary_path.c_str(), flags, 0600);
    if (descriptor < 0) {
        error =
            "Unable to stage subtitle (errno " +
            std::to_string(errno) + ")";
        return false;
    }
    std::size_t written = 0;
    while (written < download.bytes.size()) {
        const ssize_t count = ::write(
            descriptor,
            download.bytes.data() + written,
            download.bytes.size() - written);
        if (count <= 0) {
            const int value = count == 0 ? ENOSPC : errno;
            ::close(descriptor);
            ::unlink(temporary_path.c_str());
            error =
                "Unable to save subtitle (errno " +
                std::to_string(value) + ")";
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0 || ::close(descriptor) != 0) {
        const int value = errno;
        ::unlink(temporary_path.c_str());
        error =
            "Unable to finalize subtitle (errno " +
            std::to_string(value) + ")";
        return false;
    }
    if (::rename(temporary_path.c_str(), final_path.c_str()) != 0) {
        const int value = errno;
        ::unlink(temporary_path.c_str());
        error =
            "Unable to install subtitle (errno " +
            std::to_string(value) + ")";
        return false;
    }
#else
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        std::filesystem::path(normalized_root),
        filesystem_error);
    if (filesystem_error) {
        error = "Unable to create subtitle storage";
        return false;
    }
    {
        std::ofstream stream(
            std::filesystem::path(temporary_path),
            std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to stage subtitle";
            return false;
        }
        stream.write(
            reinterpret_cast<const char*>(download.bytes.data()),
            static_cast<std::streamsize>(download.bytes.size()));
        if (!stream) {
            stream.close();
            std::filesystem::remove(
                std::filesystem::path(temporary_path),
                filesystem_error);
            error = "Unable to save subtitle";
            return false;
        }
    }
    std::filesystem::rename(
        std::filesystem::path(temporary_path),
        std::filesystem::path(final_path),
        filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(
            std::filesystem::path(temporary_path),
            filesystem_error);
        error = "Unable to install subtitle";
        return false;
    }
#endif
    saved_path = final_path;
    return true;
}

} // namespace bfplayer
