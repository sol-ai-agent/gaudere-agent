#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace gaudere_agent {

// Presentation only: never change the bytes of an accepted dialogue response,
// Task report, request ID or canonical proof. Keep printable UTF-8 and
// intentional line breaks, but neutralize terminal control sequences in text
// received from a transport or provider before writing it to stdout/stderr.
[[nodiscard]] inline std::string gaudere_chat_terminal_safe(
    const std::string_view text)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    const auto escaped_byte = [&](const unsigned char byte) {
        out += "\\x";
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        // Unicode C1 controls (U+0080..U+009F), including CSI U+009B,
        // are encoded as C2 80..9F in UTF-8. Preserve other UTF-8.
        if (byte == 0xc2 && i + 1 < text.size()) {
            const auto next = static_cast<unsigned char>(text[i + 1]);
            if (next >= 0x80 && next <= 0x9f) {
                escaped_byte(byte);
                escaped_byte(next);
                ++i;
                continue;
            }
        }
        // Allow line breaks and tabs for human-readable multi-line dialogue,
        // not CR, ESC, BEL, backspace, DEL or other C0 control bytes.
        if ((byte < 0x20 && byte != '\n' && byte != '\t')
            || byte == 0x7f) {
            escaped_byte(byte);
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    return out;
}

} // namespace gaudere_agent
