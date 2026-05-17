#pragma once
#include "Core/Export.hpp"

#include <cstdint>
#include <limits>

namespace Index {
    struct INDEX_API ShaderHandle {
        uint16_t index;
        uint16_t generation;

        static constexpr uint16_t k_InvalidIndex = std::numeric_limits<uint16_t>::max();
        static ShaderHandle Invalid() { return ShaderHandle(k_InvalidIndex, 0); }

        ShaderHandle(uint16_t index, uint16_t generation) : index{ index }, generation{ generation } {}
        ShaderHandle() : index{ k_InvalidIndex }, generation{ 0 } {}

        bool IsValid() const { return index != k_InvalidIndex; }

        bool operator==(const ShaderHandle& other) const {
            return index == other.index && generation == other.generation;
        }

        bool operator!=(const ShaderHandle& other) const {
            return !(*this == other);
        }
    };
}

namespace std {
    template<>
    struct hash<Index::ShaderHandle> {
        size_t operator()(const Index::ShaderHandle& h) const noexcept {
            return (static_cast<size_t>(h.index) << 16) ^ static_cast<size_t>(h.generation);
        }
    };
}
