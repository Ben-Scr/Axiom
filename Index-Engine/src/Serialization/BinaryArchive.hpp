#pragma once

#include "Serialization/Archive.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace Index::Serialization {

	// File-format constants for the true-binary scene/prefab encoding. The
	// magic and version live in the 16-byte header at the start of every
	// IDXBIN2 file; bump kBinaryFileVersion only when the *framing* changes
	// (e.g. a new flag bit or a new entity field). Per-component schema
	// evolution rides on ComponentInfo::serializationVersion, not here.
	constexpr char     kBinaryMagic[8] = { 'I', 'D', 'X', 'B', 'I', 'N', '2', '\x1A' };
	constexpr std::uint32_t kBinaryFileVersion = 1;
	constexpr std::uint32_t kBinaryFlagCompressedBody = 0x1u; // reserved; zstd path not wired yet

	enum class BinaryKind : std::uint8_t {
		Scene  = 0,
		Prefab = 1
	};

	// BinaryArchive is the true-binary IArchive backend. It owns a
	// std::vector<std::uint8_t> on write, or holds a non-owning span on read.
	//
	// The IArchive contract handles the per-component framing (fieldHash +
	// tag + value). Higher-level framing — file header, entity records,
	// override blocks — is the responsibility of SceneSerializer's binary
	// path, which calls the public raw helpers below to lay down bytes in
	// between component frames.
	//
	// Read-mode field lookup is linear over a small per-component vector
	// (typical component has <20 fields; a hashmap would be slower because
	// of allocation and overhead). Fields are addressed by their 32-bit
	// FieldHash, so component-author field reorderings are non-breaking.
	class INDEX_API BinaryArchive final : public IArchive {
	public:
		// ── Constructors / factory ─────────────────────────────────────────
		static BinaryArchive ForWriting();
		static BinaryArchive ForReading(const std::uint8_t* data, std::size_t size);

		// ── Buffer access (write mode only) ────────────────────────────────
		// Moves the accumulated buffer out of the archive. The archive is
		// left empty; call only once at the end of a serialization pass.
		std::vector<std::uint8_t> TakeBuffer() { return std::move(m_WriteBuffer); }
		const std::vector<std::uint8_t>& Buffer() const { return m_WriteBuffer; }
		std::size_t Size() const { return IsWriting() ? m_WriteBuffer.size() : m_ReadSize; }

		// ── Raw byte-level helpers used by SceneSerializer for framing ─────
		// These are NOT part of IArchive; they exist so the binary file
		// header / entity records can be written without inventing a fake
		// "component" frame just to call into IArchive::Field.
		void RawWriteU8(std::uint8_t v);
		void RawWriteU16(std::uint16_t v);
		void RawWriteU32(std::uint32_t v);
		void RawWriteU64(std::uint64_t v);
		void RawWriteI32(std::int32_t v) { RawWriteU32(static_cast<std::uint32_t>(v)); }
		void RawWriteF32(float v) { std::uint32_t u; std::memcpy(&u, &v, 4); RawWriteU32(u); }
		void RawWriteVarint(std::uint64_t v);
		void RawWriteBytes(const void* data, std::size_t size);
		void RawWriteString(std::string_view s); // varint length + utf8 bytes

		// Cursor-position fixup used by length-prefix backpatching (e.g.
		// payloadLen). Reserve a placeholder, get its offset, write the body,
		// then patch the placeholder once the body's size is known.
		std::size_t ReserveU32() { std::size_t p = m_WriteBuffer.size(); RawWriteU32(0); return p; }
		void PatchU32At(std::size_t offset, std::uint32_t value);

		// ── Raw byte-level read helpers ────────────────────────────────────
		// Each returns false (and leaves outValue untouched) on EOF / malformed
		// input. Cursor advances on success only. Callers MUST check the bool.
		[[nodiscard]] bool RawReadU8(std::uint8_t& out);
		[[nodiscard]] bool RawReadU16(std::uint16_t& out);
		[[nodiscard]] bool RawReadU32(std::uint32_t& out);
		[[nodiscard]] bool RawReadU64(std::uint64_t& out);
		[[nodiscard]] bool RawReadI32(std::int32_t& out);
		[[nodiscard]] bool RawReadF32(float& out);
		[[nodiscard]] bool RawReadVarint(std::uint64_t& out);
		[[nodiscard]] bool RawReadBytes(void* dst, std::size_t size);
		[[nodiscard]] bool RawReadString(std::string& out);
		[[nodiscard]] bool RawSkipBytes(std::size_t size);

		std::size_t Cursor() const { return m_ReadCursor; }
		void SeekTo(std::size_t cursor) { m_ReadCursor = cursor; }
		bool AtEnd() const { return m_ReadCursor >= m_ReadSize; }

		// ── IArchive ───────────────────────────────────────────────────────
		std::uint16_t ComponentVersion() const override { return m_CurrentComponentVersion; }

		void BeginComponent(std::uint64_t nameHash, std::uint16_t version) override;
		void EndComponent() override;

		void Field(std::string_view name, bool& v) override;
		void Field(std::string_view name, std::int32_t& v) override;
		void Field(std::string_view name, std::int64_t& v) override;
		void Field(std::string_view name, std::uint32_t& v) override;
		void Field(std::string_view name, std::uint64_t& v) override;
		void Field(std::string_view name, float& v) override;
		void Field(std::string_view name, double& v) override;
		void Field(std::string_view name, std::string& v) override;
		void Field(std::string_view name, UUID& v) override;
		void Field(std::string_view name, Color& v) override;

		void Object(std::string_view name, void* userData,
			void (*fn)(IArchive&, void*)) override;
		void Array(std::string_view name, std::uint32_t& count,
			void* userData,
			void (*fn)(IArchive&, std::uint32_t, void*)) override;

	private:
		explicit BinaryArchive(ArchiveMode mode);

		// One entry in the per-component read-side field index. Built once
		// at BeginComponent on read; payloadStart points at the first byte
		// AFTER the tag, payloadLen is the value's encoded length.
		struct FieldIndexEntry {
			std::uint32_t fieldHash;
			TypeTag tag;
			std::size_t payloadStart;
			std::size_t payloadLen;
		};

		// Read-side context for an Object / Array sub-scope. Stack-allocated
		// via std::vector<Scope>; the field index is rebuilt per scope so a
		// nested Object's reads don't see the parent scope's fields by hash.
		struct Scope {
			// Write-mode: offset of the payloadLen u32 placeholder that
			// frames this scope's payload. EndScope backpatches it.
			std::size_t writePayloadLenOffset = 0;
			// Read-mode: byte range covering this scope's payload.
			std::size_t readStart = 0;
			std::size_t readEnd = 0;
			std::vector<FieldIndexEntry> fields;
		};

		void BeginScope(bool indexFields); // write: emit u32 length placeholder, push.
		void EndScope();   // write: backpatch length. read: restore cursor + pop.

		// Read-mode helpers
		const FieldIndexEntry* FindField(std::string_view name) const;
		void BuildFieldIndex(Scope& scope);
		// Skip past a single field's value given its tag. Returns false on malformed data.
		bool SkipFieldValue(TypeTag tag, std::size_t& cursor) const;

		// Field hash encoder used on write (cached to avoid recomputing
		// per call site — though FieldHash itself is constexpr-on-literals).
		std::uint32_t HashFieldName(std::string_view name) const { return FieldHash(name); }

		// Write-mode field record: 4 bytes hash + 1 byte tag + payload bytes.
		void WriteFieldHeader(std::string_view name, TypeTag tag);

	private:
		// Write state
		std::vector<std::uint8_t> m_WriteBuffer;

		// Read state (non-owning span)
		const std::uint8_t* m_ReadData = nullptr;
		std::size_t m_ReadSize = 0;
		std::size_t m_ReadCursor = 0;

		std::vector<Scope> m_ScopeStack;
		std::uint16_t m_CurrentComponentVersion = 1;
	};

} // namespace Index::Serialization
