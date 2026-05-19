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

		// Spawn a prefab tree by GUID. The record reserves ONE entity slot
		// in the ECB's entity table — the slot at `entityIndex` becomes
		// the prefab's ROOT. Child entities are bulk-created on the native
		// side during playback's pre-scan pass and are NOT addressable
		// from the managed `EntityRef` index space — managed callers reach
		// children via the root entity's hierarchy after playback (the
		// usual Entity.GetChildren API).
		//
		// Record layout:
		//   u8  opcode      = 3
		//   u32 entityIndex = ECB-local index of the root slot
		//   u32 typeIdU32   = 0 (unused — reserved for future per-spawn flags)
		//   u16 payloadSize = 8
		//   u8  payload[8]  = u64 prefabGuid (little-endian)
		//
		// The fixed 11-byte prefix matches Ecb_AddComponent so the wire's
		// merge-and-remap walker (managed EcbWire.CopyAndRemapCommands)
		// works without per-opcode branching — it rewrites the u32
		// entityIndex blindly and copies the payload through.
		Ecb_InstantiatePrefab = 3,

		// Add a component using its C++ default constructor. The record
		// carries NO payload — the native playback dispatches through
		// `ComponentInfo::defaultEmplace`, which calls
		// `registry.emplace<T>(handle)` so the C++ member-initializers fire
		// (e.g. Transform2DComponent::Scale{1,1}, SpriteRendererComponent::
		// Color{1,1,1,1}). This is what `EntityCommandBuffer.CreateEntityWith`
		// emits — `default(T)` on the C# side is zero-init, so memcpy'ing
		// it through Ecb_AddComponent would silently overwrite the engine
		// defaults.
		//
		// Record layout:
		//   u8  opcode      = 4
		//   u32 entityIndex = ECB-local index of the target slot
		//   u32 typeIdU32   = component to default-construct
		//   u16 payloadSize = 0
		//   (no payload bytes)
		//
		// The fixed 11-byte prefix matches every other opcode so the
		// merge-and-remap walker stays opcode-agnostic.
		Ecb_DefaultConstructComponent = 4,
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

	// The batch would push the live-entity count past EnTT's configured
	// entity_mask (the project-level entityBits setting decides this — see
	// index-project.json). Native logs the current count, the requested
	// addition, and the configured cap so the managed exception is
	// actionable ("raise entityBits and rebuild").
	constexpr int kEcbErrorEntityCapExceeded = -5;

	// An Ecb_InstantiatePrefab record referenced a prefab GUID that is not
	// registered (no .prefab asset with that GUID) OR whose template cannot
	// be baked (e.g. holds internal entity references the v1 cache can't
	// replay). Native logs the offending GUID; managed throws.
	constexpr int kEcbErrorBadPrefab = -6;

} // namespace Index
