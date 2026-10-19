#include "format.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

namespace roomy {

std::wstring sizeText(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + (bytes == 1 ? L" byte" : L" bytes");
    static const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = double(bytes) / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[32];
    swprintf(buf, 32, v >= 100 ? L"%.0f %s" : L"%.1f %s", v, units[u]);
    return buf;
}

std::wstring countText(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), 1, L',');
    return s;
}

std::wstring ageText(int64_t t) {
    if (t <= 0) return L"";
    int64_t d = (int64_t(std::time(nullptr)) - t) / 86400;
    auto plural = [](int64_t n, const wchar_t* unit) {
        return std::to_wstring(n) + L" " + unit + (n == 1 ? L"" : L"s") + L" ago";
    };
    if (d < 1) return L"today";
    if (d < 2) return L"yesterday";
    if (d < 14) return plural(d, L"day");
    if (d < 60) return plural(d / 7, L"week");
    if (d < 365) return plural(d / 30, L"month");
    return plural(d / 365, L"year");
}

std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string jsonString(const std::wstring& s) {
    std::string in = utf8(s), out = "\"";
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace roomy
