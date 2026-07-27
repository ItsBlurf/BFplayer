#pragma once

#include <string>
#include <string_view>

namespace bfplayer {

// These helpers deliberately avoid decoding or normalizing the URI. FFmpeg
// must receive the original source string, while logs and persistence must not
// retain credentials, signed-query tokens, or fragments.
[[nodiscard]] bool is_network_uri(std::string_view value) noexcept;
[[nodiscard]] bool is_supported_stream_uri(std::string_view value) noexcept;
[[nodiscard]] bool uri_has_credentials(std::string_view value) noexcept;
[[nodiscard]] bool uri_has_sensitive_components(std::string_view value) noexcept;
[[nodiscard]] std::string redact_uri_secrets(std::string_view value);

} // namespace bfplayer
