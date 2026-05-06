#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <limits>

namespace Axiom {
    // Stable handle into FontManager's slot table. Mirrors TextureHandle's
    // shape (16-bit slot index + 16-bit generation) so the same patterns
    // (`IsValid`, equality, `Invalid()`) carry over to font references.
    struct AXIOM_API FontHandle {
        uint16_t index;
        uint16_t generation;

        static constexpr uint16_t k_InvalidIndex = std::numeric_limits<uint16_t>::max();
        static FontHandle Invalid() { return FontHandle(k_InvalidIndex, 0); }

        FontHandle(uint16_t index, uint16_t generation) : index{ index }, generation{ generation } {}
        FontHandle() : index{ k_InvalidIndex }, generation{ 0 } {}

        bool IsValid() const { return index != k_InvalidIndex; }

        bool operator==(const FontHandle& other) const {
            return index == other.index && generation == other.generation;
        }

        bool operator!=(const FontHandle& other) const {
            return !(*this == other);
        }
    };
}
