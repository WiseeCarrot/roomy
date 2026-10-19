#pragma once

#include <cstdint>
#include <string>

namespace roomy {

std::wstring sizeText(uint64_t bytes);       // "38.2 GB"
std::wstring countText(uint64_t n);          // "1,234"
std::wstring ageText(int64_t unixTime);      // "3 weeks ago"
std::string utf8(const std::wstring& s);
std::wstring wide(const std::string& s);
std::string jsonString(const std::wstring& s);  // quoted and escaped

}  // namespace roomy
