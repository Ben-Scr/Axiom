#pragma once

#include <cstdint>
#include <string_view>

namespace Index::Serialization {

	// FNV-1a 32-bit. Stable across builds, platforms, and ABIs — safe to embed
	// in serialized assets. constexpr so field-name literals in component
	// Serialize(IArchive&) methods hash at compile time.
	//
	// The companion 64-bit variant lives at Index::detail::FnvHash64 (see
	// ComponentRegistry.hpp) and is used for the component-name table. Fields
	// only need 32 bits because the collision space is scoped under a single
	// component's payload — collisions across different components don't
	// matter, because the binary reader resolves field names only after the
	// outer component header (componentHash + version) has selected which
	// ComponentInfo's Serialize() will be invoked.
	//
	// A debug-build invariant in ComponentRegistry::Register validates that
	// every field name registered through Serialize(IArchive&) on a single
	// component hashes to a unique 32-bit value.
	constexpr std::uint32_t FieldHash(std::string_view name) noexcept {
		constexpr std::uint32_t kOffsetBasis = 0x811c9dc5u;
		constexpr std::uint32_t kPrime = 0x01000193u;
		std::uint32_t h = kOffsetBasis;
		for (char c : name) {
			h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
			h *= kPrime;
		}
		return h;
	}

} // namespace Index::Serialization
