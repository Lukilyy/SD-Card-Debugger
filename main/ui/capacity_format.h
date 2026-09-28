#pragma once

#include <cstddef>
#include <cstdint>

enum class CapacityUnitMode : uint8_t {
    Decimal,
    Binary,
};

// Formats byte counts for UI display only. Decimal mode uses powers of 1000
// and KB/MB/GB; binary mode uses powers of 1024 and KiB/MiB/GiB.
void format_capacity(uint64_t bytes,
                     CapacityUnitMode mode,
                     char *output,
                     size_t output_size);
