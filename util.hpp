// Library-candidate helpers (jgus-org/tickets#112): generic pieces with no interposer coupling live here or in unique_fd.hpp.

#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace evi {

// EV_KEY event values (the kernel UAPI headers define no names for these).
inline constexpr std::int32_t kKeyRelease = 0;
inline constexpr std::int32_t kKeyPress = 1;
inline constexpr std::int32_t kKeyAutorepeat = 2;

inline std::string errstr() { return std::strerror(errno); }

// Tests a bit in an ioctl-filled bitmap (e.g. EVIOCGBIT).
inline bool has_bit(const unsigned char* bits, unsigned bit) { return bits[bit / 8] & (1u << (bit % 8)); }

}  // namespace evi
