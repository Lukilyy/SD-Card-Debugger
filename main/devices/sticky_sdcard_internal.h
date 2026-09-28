#pragma once

#include "sticky_sdcard.h"

namespace sdcard_internal {

inline const char *partition_scheme_name(StickySdPartitionScheme scheme)
{
    switch (scheme) {
    case StickySdPartitionScheme::Superfloppy:
        return "SUPERFLOPPY";
    case StickySdPartitionScheme::Mbr:
        return "MBR";
    case StickySdPartitionScheme::Gpt:
        return "GPT";
    case StickySdPartitionScheme::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

}  // namespace sdcard_internal
