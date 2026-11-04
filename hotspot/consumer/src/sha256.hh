// SHA-256（FIPS 180-4）。run.ini 的 artifact.sha256 需要它，而本工程不引第三方库。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hotspot {

class Sha256 {
  public:
    Sha256();
    void update(const std::uint8_t *data, std::size_t len);
    void update(std::string_view text);
    std::string hex_digest();

  private:
    void compress(const std::uint8_t block[64]);

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffered_ = 0;
    std::uint64_t total_bits_ = 0;
};

std::string sha256_hex(const std::uint8_t *data, std::size_t len);

} // namespace hotspot
