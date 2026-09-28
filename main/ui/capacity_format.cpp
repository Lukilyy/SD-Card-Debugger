#include "capacity_format.h"

#include <cstdio>

void format_capacity(uint64_t bytes,
                     CapacityUnitMode mode,
                     char *output,
                     size_t output_size)
{
    if (output == nullptr || output_size == 0U) {
        return;
    }

    const uint64_t kilo = mode == CapacityUnitMode::Decimal ? 1000ULL
                                                            : 1024ULL;
    const uint64_t mega = kilo * kilo;
    const uint64_t giga = mega * kilo;
    const char *kilo_label = mode == CapacityUnitMode::Decimal ? "KB" : "KiB";
    const char *mega_label = mode == CapacityUnitMode::Decimal ? "MB" : "MiB";
    const char *giga_label = mode == CapacityUnitMode::Decimal ? "GB" : "GiB";

    if (bytes >= giga) {
        std::snprintf(output, output_size, "%.2f %s",
                      static_cast<double>(bytes) / giga, giga_label);
    } else if (bytes >= mega) {
        std::snprintf(output, output_size, "%.1f %s",
                      static_cast<double>(bytes) / mega, mega_label);
    } else if (bytes >= kilo) {
        std::snprintf(output, output_size, "%.1f %s",
                      static_cast<double>(bytes) / kilo, kilo_label);
    } else {
        std::snprintf(output, output_size, "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    output[output_size - 1U] = '\0';
}
