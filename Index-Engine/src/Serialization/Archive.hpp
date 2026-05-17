#pragma once

#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Collections/Color.hpp"
#include "Serialization/FieldHash.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Index::Serialization {

	enum class ArchiveMode : std::uint8_t {
		Write,
		Read
	};

	// On-wire type tag for a single Field record in BinaryArchive. The values
	// are persisted in saved scenes — DO NOT renumber or remove existing tags
	// without bumping the file's fileVersion in the IDXBIN2 header.
	//
	// Tags are also used by CaptureArchive (in-memory) so JsonArchive's
	// override capture path emits typed records without needing a parallel
	// enum. Keep the set minimal — anything componentSerialize wants to write
	// reduces to one of these primitive shapes, or to a length-prefixed
	// OBJECT / ARRAY recursion.
	enum class TypeTag : std::uint8_t {
		Null    = 0,
		Bool    = 1,
		I32     = 2,
		I64     = 3,
		U32     = 4,
		U64     = 5,
		F32     = 6,
		F64     = 7,
		String  = 8,
		UUID    = 9,
		Color   = 10,
		Vec2    = 11,
		Vec3    = 12,
		Bytes   = 13,
		Object  = 14,
		Array   = 15
	};

	// IArchive is the bidirectional, format-agnostic visitor that every
	// component implements once. The same Serialize(IArchive&) method is
	// called for both writing and reading — concrete backends (JsonArchive,
	// BinaryArchive, CaptureArchive) decide what each Field(...) call means.
	//
	// Read semantics for Field(name, v&): when reading and the named field
	// is absent in the payload, `v` is left unchanged. This matches the
	// existing JSON deserialization pattern (AsDoubleOr(c.x)) and gives
	// forward-compatible field evolution for free — adding a new Field call
	// in a component method does NOT invalidate older saved files.
	//
	// BeginComponent / EndComponent are framing calls used by SceneSerializer,
	// not by the component author. The component's Serialize body only calls
	// Field / Object / Array.
	class INDEX_API IArchive {
	public:
		virtual ~IArchive() = default;

		bool IsWriting() const { return m_Mode == ArchiveMode::Write; }
		bool IsReading() const { return m_Mode == ArchiveMode::Read; }
		ArchiveMode Mode() const { return m_Mode; }

		// Version of the component currently being serialized. Components
		// branch on this to handle field renames / type changes across
		// schema versions. Only valid between BeginComponent / EndComponent.
		virtual std::uint16_t ComponentVersion() const = 0;

		// Framing (driven by SceneSerializer, not by component authors)
		virtual void BeginComponent(std::uint64_t nameHash, std::uint16_t version) = 0;
		virtual void EndComponent() = 0;

		// Primitives
		virtual void Field(std::string_view name, bool& v) = 0;
		virtual void Field(std::string_view name, std::int32_t& v) = 0;
		virtual void Field(std::string_view name, std::int64_t& v) = 0;
		virtual void Field(std::string_view name, std::uint32_t& v) = 0;
		virtual void Field(std::string_view name, std::uint64_t& v) = 0;
		virtual void Field(std::string_view name, float& v) = 0;
		virtual void Field(std::string_view name, double& v) = 0;
		virtual void Field(std::string_view name, std::string& v) = 0;
		virtual void Field(std::string_view name, UUID& v) = 0;
		virtual void Field(std::string_view name, Color& v) = 0;
		// glm::vec2 / glm::vec3 are intentionally NOT in this interface for v1:
		// the existing JSON layout flattens them as scalar fields (posX/posY,
		// not pos:{x,y}), and components migrating from the hardcoded path need
		// to call Field twice with those exact scalar names to preserve byte
		// parity with old saves. The Vec2 / Vec3 TypeTag values are reserved
		// for a future revision where the binary backend can pack them.

		// Nested object: backend wraps a named scope; `fn` calls Field/Object/Array.
		// On read, missing objects skip the fn call entirely (no defaults overwritten).
		virtual void Object(std::string_view name, void* userData,
			void (*fn)(IArchive&, void*)) = 0;

		// Header-only convenience: forwards a closure as the trampoline.
		template <typename Fn>
		void Object(std::string_view name, Fn&& fn) {
			Object(name, &fn,
				[](IArchive& ar, void* p) { (*static_cast<Fn*>(p))(ar); });
		}

		// Homogeneous array: caller pushes/pops elements through the trampoline.
		// `count` is round-tripped (in/out) — on read it tells the caller how
		// many elements to iterate.
		virtual void Array(std::string_view name, std::uint32_t& count,
			void* userData,
			void (*fn)(IArchive&, std::uint32_t, void*)) = 0;

		template <typename Fn>
		void Array(std::string_view name, std::uint32_t& count, Fn&& fn) {
			Array(name, count, &fn,
				[](IArchive& ar, std::uint32_t i, void* p) { (*static_cast<Fn*>(p))(ar, i); });
		}

	protected:
		explicit IArchive(ArchiveMode mode) : m_Mode(mode) {}
		ArchiveMode m_Mode;
	};

	// ULEB128 helpers for variable-length integer encoding. Public so both
	// BinaryArchive and the legacy IDXSCNB reader can share one implementation.
	inline void WriteULEB128(std::vector<std::uint8_t>& buf, std::uint64_t value) {
		do {
			std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
			value >>= 7;
			if (value != 0) byte |= 0x80u;
			buf.push_back(byte);
		} while (value != 0);
	}

	// Returns true on success and advances `cursor`. Returns false if the
	// payload ends mid-varint or exceeds the 10-byte max (corrupt input).
	inline bool ReadULEB128(const std::uint8_t* data, std::size_t size,
		std::size_t& cursor, std::uint64_t& outValue) {
		std::uint64_t result = 0;
		int shift = 0;
		for (int i = 0; i < 10; ++i) {
			if (cursor >= size) return false;
			std::uint8_t byte = data[cursor++];
			result |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
			if ((byte & 0x80u) == 0) {
				outValue = result;
				return true;
			}
			shift += 7;
		}
		return false; // malformed varint
	}

} // namespace Index::Serialization
