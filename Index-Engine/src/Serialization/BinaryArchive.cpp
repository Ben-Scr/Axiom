#include "pch.hpp"
#include "Serialization/BinaryArchive.hpp"

#include <cstring>
#include <utility>

namespace Index::Serialization {

	// ─────────────────────────────────────────────────────────────────────────
	// Construction
	// ─────────────────────────────────────────────────────────────────────────

	BinaryArchive::BinaryArchive(ArchiveMode mode)
		: IArchive(mode) {}

	BinaryArchive BinaryArchive::ForWriting() {
		BinaryArchive ar(ArchiveMode::Write);
		// Pre-reserve a modest amount; SceneSerializer grows it as needed.
		ar.m_WriteBuffer.reserve(64 * 1024);
		return ar;
	}

	BinaryArchive BinaryArchive::ForReading(const std::uint8_t* data, std::size_t size) {
		BinaryArchive ar(ArchiveMode::Read);
		ar.m_ReadData = data;
		ar.m_ReadSize = size;
		ar.m_ReadCursor = 0;
		return ar;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Raw write helpers
	// ─────────────────────────────────────────────────────────────────────────

	void BinaryArchive::RawWriteU8(std::uint8_t v) {
		m_WriteBuffer.push_back(v);
	}

	void BinaryArchive::RawWriteU16(std::uint16_t v) {
		m_WriteBuffer.push_back(static_cast<std::uint8_t>(v & 0xFFu));
		m_WriteBuffer.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	}

	void BinaryArchive::RawWriteU32(std::uint32_t v) {
		m_WriteBuffer.push_back(static_cast<std::uint8_t>(v & 0xFFu));
		m_WriteBuffer.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
		m_WriteBuffer.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
		m_WriteBuffer.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
	}

	void BinaryArchive::RawWriteU64(std::uint64_t v) {
		for (int i = 0; i < 8; ++i) {
			m_WriteBuffer.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
		}
	}

	void BinaryArchive::RawWriteVarint(std::uint64_t v) {
		WriteULEB128(m_WriteBuffer, v);
	}

	void BinaryArchive::RawWriteBytes(const void* data, std::size_t size) {
		const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
		m_WriteBuffer.insert(m_WriteBuffer.end(), p, p + size);
	}

	void BinaryArchive::RawWriteString(std::string_view s) {
		RawWriteVarint(static_cast<std::uint64_t>(s.size()));
		RawWriteBytes(s.data(), s.size());
	}

	void BinaryArchive::PatchU32At(std::size_t offset, std::uint32_t value) {
		m_WriteBuffer[offset    ] = static_cast<std::uint8_t>(value & 0xFFu);
		m_WriteBuffer[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
		m_WriteBuffer[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
		m_WriteBuffer[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Raw read helpers
	// ─────────────────────────────────────────────────────────────────────────

	bool BinaryArchive::RawReadU8(std::uint8_t& out) {
		if (m_ReadCursor + 1 > m_ReadSize) return false;
		out = m_ReadData[m_ReadCursor++];
		return true;
	}

	bool BinaryArchive::RawReadU16(std::uint16_t& out) {
		if (m_ReadCursor + 2 > m_ReadSize) return false;
		out = static_cast<std::uint16_t>(m_ReadData[m_ReadCursor]) |
		      (static_cast<std::uint16_t>(m_ReadData[m_ReadCursor + 1]) << 8);
		m_ReadCursor += 2;
		return true;
	}

	bool BinaryArchive::RawReadU32(std::uint32_t& out) {
		if (m_ReadCursor + 4 > m_ReadSize) return false;
		out =  static_cast<std::uint32_t>(m_ReadData[m_ReadCursor]) |
		      (static_cast<std::uint32_t>(m_ReadData[m_ReadCursor + 1]) << 8) |
		      (static_cast<std::uint32_t>(m_ReadData[m_ReadCursor + 2]) << 16) |
		      (static_cast<std::uint32_t>(m_ReadData[m_ReadCursor + 3]) << 24);
		m_ReadCursor += 4;
		return true;
	}

	bool BinaryArchive::RawReadU64(std::uint64_t& out) {
		if (m_ReadCursor + 8 > m_ReadSize) return false;
		std::uint64_t v = 0;
		for (int i = 0; i < 8; ++i) {
			v |= static_cast<std::uint64_t>(m_ReadData[m_ReadCursor + i]) << (i * 8);
		}
		m_ReadCursor += 8;
		out = v;
		return true;
	}

	bool BinaryArchive::RawReadI32(std::int32_t& out) {
		std::uint32_t u;
		if (!RawReadU32(u)) return false;
		out = static_cast<std::int32_t>(u);
		return true;
	}

	bool BinaryArchive::RawReadF32(float& out) {
		std::uint32_t u;
		if (!RawReadU32(u)) return false;
		std::memcpy(&out, &u, 4);
		return true;
	}

	bool BinaryArchive::RawReadVarint(std::uint64_t& out) {
		return ReadULEB128(m_ReadData, m_ReadSize, m_ReadCursor, out);
	}

	bool BinaryArchive::RawReadBytes(void* dst, std::size_t size) {
		// Overflow-safe bound check: comparing as `cursor + size > readSize`
		// wraps when `size` (originating from an attacker-controlled varint)
		// is near SIZE_MAX, letting the check pass before memcpy reads OOB.
		if (m_ReadCursor > m_ReadSize || size > m_ReadSize - m_ReadCursor) return false;
		std::memcpy(dst, m_ReadData + m_ReadCursor, size);
		m_ReadCursor += size;
		return true;
	}

	bool BinaryArchive::RawReadString(std::string& out) {
		std::uint64_t len;
		if (!RawReadVarint(len)) return false;
		// Overflow-safe bound: see RawReadBytes. `len` is a 64-bit varint from
		// the buffer; cast to size_t for the comparison after the subtraction.
		if (m_ReadCursor > m_ReadSize) return false;
		const std::size_t remaining = m_ReadSize - m_ReadCursor;
		if (len > static_cast<std::uint64_t>(remaining)) return false;
		const std::size_t lenSz = static_cast<std::size_t>(len);
		out.assign(reinterpret_cast<const char*>(m_ReadData + m_ReadCursor), lenSz);
		m_ReadCursor += lenSz;
		return true;
	}

	bool BinaryArchive::RawSkipBytes(std::size_t size) {
		// Overflow-safe bound: see RawReadBytes.
		if (m_ReadCursor > m_ReadSize || size > m_ReadSize - m_ReadCursor) return false;
		m_ReadCursor += size;
		return true;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Scope / component framing
	// ─────────────────────────────────────────────────────────────────────────

	void BinaryArchive::BeginScope(bool indexFields) {
		Scope scope;
		if (IsWriting()) {
			scope.writePayloadLenOffset = ReserveU32();
		} else {
			std::uint32_t payloadLen = 0;
			if (!RawReadU32(payloadLen)) {
				// Corrupt: leave an empty scope on the stack so the matching
				// EndScope still pops cleanly.
				scope.readStart = m_ReadCursor;
				scope.readEnd = m_ReadCursor;
				m_ScopeStack.push_back(std::move(scope));
				return;
			}
			scope.readStart = m_ReadCursor;
			scope.readEnd = m_ReadCursor + payloadLen;
			if (scope.readEnd > m_ReadSize) {
				scope.readEnd = m_ReadSize; // clamp on truncation
			}
			if (indexFields) {
				BuildFieldIndex(scope);
				// Field-indexed scopes don't need the cursor — leave it at
				// readEnd so the parent's framing reads cleanly when this
				// scope is popped.
				m_ReadCursor = scope.readEnd;
			}
			// Non-field-indexed scopes (Array) leave cursor at readStart so
			// the caller can read raw bytes (count, element scopes...).
		}
		m_ScopeStack.push_back(std::move(scope));
	}

	void BinaryArchive::EndScope() {
		if (m_ScopeStack.empty()) return;
		if (IsWriting()) {
			const std::size_t lenOffset = m_ScopeStack.back().writePayloadLenOffset;
			const std::size_t payloadStart = lenOffset + 4;
			const std::size_t payloadEnd = m_WriteBuffer.size();
			PatchU32At(lenOffset, static_cast<std::uint32_t>(payloadEnd - payloadStart));
		} else {
			// On read, always snap cursor to scope end so non-indexed scopes
			// (Array) and field-indexed scopes share the same exit invariant.
			m_ReadCursor = m_ScopeStack.back().readEnd;
		}
		m_ScopeStack.pop_back();
	}

	void BinaryArchive::BeginComponent(std::uint64_t nameHash, std::uint16_t version) {
		// SceneSerializer owns the outer component framing: <u64 nameHash>
		// <u16 version> are written/read by the entity-record loop, BEFORE
		// it calls BeginComponent. That layout lets the reader dispatch to
		// the right ComponentInfo (or skip cleanly if the hash is unknown)
		// before this archive ever sees the payload. BeginComponent only
		// frames the payload itself (<u32 payloadLen><payload...>) so the
		// behavior is symmetric between write and read.
		(void)nameHash;
		m_CurrentComponentVersion = version;
		BeginScope(/*indexFields=*/true);
	}

	void BinaryArchive::EndComponent() {
		EndScope();
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Read-side field index
	// ─────────────────────────────────────────────────────────────────────────

	bool BinaryArchive::SkipFieldValue(TypeTag tag, std::size_t& cursor) const {
		auto require = [&](std::size_t n) {
			// Overflow-safe bound check: see RawReadBytes — varint-derived `n`
			// near SIZE_MAX would wrap `cursor + n` and let the check pass.
			if (cursor > m_ReadSize || n > m_ReadSize - cursor) return false;
			cursor += n;
			return true;
		};
		switch (tag) {
		case TypeTag::Null:                              return true;
		case TypeTag::Bool:   return require(1);
		case TypeTag::I32:
		case TypeTag::U32:
		case TypeTag::F32:    return require(4);
		case TypeTag::I64:
		case TypeTag::U64:
		case TypeTag::F64:
		case TypeTag::UUID:   return require(8);
		case TypeTag::Vec2:   return require(8);
		case TypeTag::Vec3:   return require(12);
		case TypeTag::Color:  return require(16);
		case TypeTag::String:
		case TypeTag::Bytes: {
			std::uint64_t len;
			std::size_t before = cursor;
			if (!ReadULEB128(m_ReadData, m_ReadSize, cursor, len)) {
				cursor = before;
				return false;
			}
			return require(static_cast<std::size_t>(len));
		}
		case TypeTag::Object:
		case TypeTag::Array: {
			// Both carry a u32 length prefix immediately, identical to the
			// component scope: skip 4 + length bytes.
			if (cursor + 4 > m_ReadSize) return false;
			std::uint32_t len = static_cast<std::uint32_t>(m_ReadData[cursor]) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 1]) << 8) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 2]) << 16) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 3]) << 24);
			cursor += 4;
			return require(len);
		}
		}
		return false; // unknown tag → malformed
	}

	void BinaryArchive::BuildFieldIndex(Scope& scope) {
		std::size_t cursor = scope.readStart;
		while (cursor < scope.readEnd) {
			if (cursor + 5 > scope.readEnd) break; // 4 (hash) + 1 (tag)
			std::uint32_t fieldHash = static_cast<std::uint32_t>(m_ReadData[cursor]) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 1]) << 8) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 2]) << 16) |
				(static_cast<std::uint32_t>(m_ReadData[cursor + 3]) << 24);
			cursor += 4;
			TypeTag tag = static_cast<TypeTag>(m_ReadData[cursor]);
			cursor += 1;
			FieldIndexEntry entry;
			entry.fieldHash = fieldHash;
			entry.tag = tag;
			entry.payloadStart = cursor;
			std::size_t after = cursor;
			if (!SkipFieldValue(tag, after) || after > scope.readEnd) {
				// Malformed; stop indexing further fields. Already-indexed
				// entries remain valid and lookups still work for them.
				break;
			}
			entry.payloadLen = after - cursor;
			scope.fields.push_back(entry);
			cursor = after;
		}
	}

	const BinaryArchive::FieldIndexEntry* BinaryArchive::FindField(std::string_view name) const {
		if (m_ScopeStack.empty()) return nullptr;
		const std::uint32_t hash = FieldHash(name);
		const Scope& top = m_ScopeStack.back();
		for (const auto& e : top.fields) {
			if (e.fieldHash == hash) return &e;
		}
		return nullptr;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// IArchive::Field overloads
	// ─────────────────────────────────────────────────────────────────────────

	void BinaryArchive::WriteFieldHeader(std::string_view name, TypeTag tag) {
		RawWriteU32(FieldHash(name));
		RawWriteU8(static_cast<std::uint8_t>(tag));
	}

	void BinaryArchive::Field(std::string_view name, bool& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::Bool);
			RawWriteU8(v ? 1u : 0u);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::Bool || e->payloadLen < 1) return;
			v = (m_ReadData[e->payloadStart] != 0);
		}
	}

	void BinaryArchive::Field(std::string_view name, std::int32_t& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::I32);
			RawWriteU32(static_cast<std::uint32_t>(v));
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->payloadLen < 4) return;
			// Accept I32 / U32 — written sizes match.
			if (e->tag != TypeTag::I32 && e->tag != TypeTag::U32) return;
			std::uint32_t u = static_cast<std::uint32_t>(m_ReadData[e->payloadStart]) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 1]) << 8) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 2]) << 16) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 3]) << 24);
			v = static_cast<std::int32_t>(u);
		}
	}

	void BinaryArchive::Field(std::string_view name, std::int64_t& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::I64);
			RawWriteU64(static_cast<std::uint64_t>(v));
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->payloadLen < 8) return;
			if (e->tag != TypeTag::I64 && e->tag != TypeTag::U64) return;
			std::uint64_t u = 0;
			for (int i = 0; i < 8; ++i) {
				u |= static_cast<std::uint64_t>(m_ReadData[e->payloadStart + i]) << (i * 8);
			}
			v = static_cast<std::int64_t>(u);
		}
	}

	void BinaryArchive::Field(std::string_view name, std::uint32_t& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::U32);
			RawWriteU32(v);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->payloadLen < 4) return;
			if (e->tag != TypeTag::I32 && e->tag != TypeTag::U32) return;
			v = static_cast<std::uint32_t>(m_ReadData[e->payloadStart]) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 1]) << 8) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 2]) << 16) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 3]) << 24);
		}
	}

	void BinaryArchive::Field(std::string_view name, std::uint64_t& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::U64);
			RawWriteU64(v);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->payloadLen < 8) return;
			if (e->tag != TypeTag::I64 && e->tag != TypeTag::U64) return;
			std::uint64_t u = 0;
			for (int i = 0; i < 8; ++i) {
				u |= static_cast<std::uint64_t>(m_ReadData[e->payloadStart + i]) << (i * 8);
			}
			v = u;
		}
	}

	void BinaryArchive::Field(std::string_view name, float& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::F32);
			RawWriteF32(v);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::F32 || e->payloadLen < 4) return;
			std::uint32_t u = static_cast<std::uint32_t>(m_ReadData[e->payloadStart]) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 1]) << 8) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 2]) << 16) |
				(static_cast<std::uint32_t>(m_ReadData[e->payloadStart + 3]) << 24);
			std::memcpy(&v, &u, 4);
		}
	}

	void BinaryArchive::Field(std::string_view name, double& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::F64);
			std::uint64_t u;
			std::memcpy(&u, &v, 8);
			RawWriteU64(u);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::F64 || e->payloadLen < 8) return;
			std::uint64_t u = 0;
			for (int i = 0; i < 8; ++i) {
				u |= static_cast<std::uint64_t>(m_ReadData[e->payloadStart + i]) << (i * 8);
			}
			std::memcpy(&v, &u, 8);
		}
	}

	void BinaryArchive::Field(std::string_view name, std::string& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::String);
			RawWriteString(v);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::String) return;
			// Re-decode the length so we don't have to store it in the index.
			std::size_t cursor = e->payloadStart;
			std::uint64_t len;
			if (!ReadULEB128(m_ReadData, m_ReadSize, cursor, len)) return;
			// Overflow-safe bound: the prior `cursor + len > payloadStart + payloadLen`
			// could wrap both sides on adversarial varint length. Recompute the
			// remaining bytes within this field's payload and compare directly.
			const std::size_t payloadEnd = e->payloadStart + e->payloadLen;
			if (cursor > payloadEnd) return;
			const std::size_t remaining = payloadEnd - cursor;
			if (len > static_cast<std::uint64_t>(remaining)) return;
			v.assign(reinterpret_cast<const char*>(m_ReadData + cursor),
				static_cast<std::size_t>(len));
		}
	}

	void BinaryArchive::Field(std::string_view name, UUID& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::UUID);
			RawWriteU64(static_cast<std::uint64_t>(v));
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::UUID || e->payloadLen < 8) return;
			std::uint64_t u = 0;
			for (int i = 0; i < 8; ++i) {
				u |= static_cast<std::uint64_t>(m_ReadData[e->payloadStart + i]) << (i * 8);
			}
			v = UUID(u);
		}
	}

	void BinaryArchive::Field(std::string_view name, Color& v) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::Color);
			RawWriteF32(v.r);
			RawWriteF32(v.g);
			RawWriteF32(v.b);
			RawWriteF32(v.a);
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::Color || e->payloadLen < 16) return;
			auto load = [&](std::size_t off) -> float {
				std::uint32_t u = static_cast<std::uint32_t>(m_ReadData[off]) |
					(static_cast<std::uint32_t>(m_ReadData[off + 1]) << 8) |
					(static_cast<std::uint32_t>(m_ReadData[off + 2]) << 16) |
					(static_cast<std::uint32_t>(m_ReadData[off + 3]) << 24);
				float f; std::memcpy(&f, &u, 4); return f;
			};
			v.r = load(e->payloadStart);
			v.g = load(e->payloadStart + 4);
			v.b = load(e->payloadStart + 8);
			v.a = load(e->payloadStart + 12);
		}
	}

	void BinaryArchive::Object(std::string_view name, void* userData,
		void (*fn)(IArchive&, void*)) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::Object);
			BeginScope(/*indexFields=*/true);
			fn(*this, userData);
			EndScope();
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::Object) return;
			// Object payload layout (after the field hash+tag header):
			// <u32 lenInsideObject><inner-payload-of-that-length>.
			// payloadStart points at the u32, payloadLen = 4 + len.
			// Position cursor at the length prefix, let BeginScope consume
			// it and build the inner field index.
			const std::size_t savedCursor = m_ReadCursor;
			m_ReadCursor = e->payloadStart;
			BeginScope(/*indexFields=*/true);
			fn(*this, userData);
			EndScope();
			m_ReadCursor = savedCursor;
		}
	}

	void BinaryArchive::Array(std::string_view name, std::uint32_t& count,
		void* userData,
		void (*fn)(IArchive&, std::uint32_t, void*)) {
		if (IsWriting()) {
			WriteFieldHeader(name, TypeTag::Array);
			BeginScope(/*indexFields=*/false);
			RawWriteU32(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				// Each element is itself a length-prefixed field-indexed scope
				// so the reader can skip unknown elements cleanly AND look up
				// per-element fields by name.
				BeginScope(/*indexFields=*/true);
				fn(*this, i, userData);
				EndScope();
			}
			EndScope();
		} else {
			const FieldIndexEntry* e = FindField(name);
			if (e == nullptr || e->tag != TypeTag::Array) { count = 0; return; }
			const std::size_t savedCursor = m_ReadCursor;
			m_ReadCursor = e->payloadStart;
			BeginScope(/*indexFields=*/false);
			std::uint32_t storedCount = 0;
			if (!RawReadU32(storedCount)) { EndScope(); m_ReadCursor = savedCursor; count = 0; return; }
			count = storedCount;
			for (std::uint32_t i = 0; i < storedCount; ++i) {
				BeginScope(/*indexFields=*/true);
				fn(*this, i, userData);
				EndScope();
			}
			EndScope();
			m_ReadCursor = savedCursor;
		}
	}

} // namespace Index::Serialization
