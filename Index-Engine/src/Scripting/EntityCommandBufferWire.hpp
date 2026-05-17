#pragma once

#include <cstdint>

// EntityCommandBuffer wire format — single source of truth shared by the
// managed recorder (Index-ScriptCore/Source/Index/Scene/EntityCommandBuffer.cs)
// and the native playback path (ScriptBindingsEcb.cpp). The layout is
// byte-packed and little-endian; cross-boundary correctness depends on
// these constants matching on both sides.
//
// Buffer layout:
//   [Header — 8 bytes]
//     u32 entityCount     number of entity slots created by this batch
//     u32 commandCount    number of command records that follow
//   [Entity table — entityCount × 4 bytes]
//     u32 nameOffset      0xFFFFFFFF = no name; else byte offset of the
//                         name's pool slot (relative to bufferStart)
//   [Command stream — commandCount records, variable size]
//     u8  opcode          see EcbOpcode
//     u32 entityIndex     0-based into the entity table
//     u32 typeIdU32       stable component ID from ComponentRegistry
//     u16 payloadSize     bytes of `payload` that follow
//     u8[payloadSize] payload   raw component memcpy
//
// Name pool slots are NOT length-prefixed in v1 — they are NUL-terminated
// UTF-8 stored anywhere after the command stream; the name table holds the
// absolute byte offset to the slot's first byte. (Names are optional and
// rare; not worth a separate alignment scheme.)

namespace Index {

	struct EcbHeader {
		uint32_t entityCount;
		uint32_t commandCount;
	};
	static_assert(sizeof(EcbHeader) == 8, "ECB header layout drift would corrupt the managed/native binding");

	enum EcbOpcode : uint8_t {
		Ecb_AddComponent = 1,
		// Set on an existing component (replaces stored value). Same payload
		// as AddComponent but skips dependency-add logic on the assumption
		// the component is already present. Reserved for future use.
		Ecb_SetComponent = 2,
	};

	// Sentinel "no name" in the entity table.
	constexpr uint32_t kEcbNoName = 0xFFFFFFFFu;

	// Negative return codes from Ecb_Playback. Positive returns are the
	// number of entities created.
	constexpr int kEcbErrorTruncated = -1;
	constexpr int kEcbErrorNoScene   = -2;
	constexpr int kEcbErrorOutputTooSmall = -3;
	// An AddComponent command referenced a typeId that's either unknown
	// (no component registered with that ID) OR the registered component
	// has no `emplaceFromBytes` callback. The latter case means the
	// component holds non-memcpy-safe state and needs a custom emplacer
	// — see the ComponentRegistry contract comment. The native side
	// IDX_CORE_WARN_TAG-logs the offending typeId before returning this
	// code so the managed exception carries a finger-pointable id.
	constexpr int kEcbErrorUnknownComponent = -4;

} // namespace Index
