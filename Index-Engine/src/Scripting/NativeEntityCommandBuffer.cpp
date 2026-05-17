#include "pch.hpp"

#include "Scripting/NativeEntityCommandBuffer.hpp"

#include "Components/General/EntityMetaDataComponent.hpp"
#include "Scene/Scene.hpp"

namespace Index {

	NativeEntityCommandBuffer::~NativeEntityCommandBuffer() {
		// Clear() handles destructor invocation for any still-pending commands.
		Clear();
	}

	NativeEntityCommandBuffer::EntityRef NativeEntityCommandBuffer::CreateEntity() {
		EntityRef ref{ m_EntityCount };
		++m_EntityCount;
		return ref;
	}

	NativeEntityCommandBuffer::Command& NativeEntityCommandBuffer::AppendCommand(
		uint32_t entityIndex,
		void (*apply)(void*, entt::registry&, EntityHandle),
		void (*destroy)(void*))
	{
		Command& cmd = m_Commands.emplace_back();
		cmd.EntityIndex = entityIndex;
		cmd.Apply = apply;
		cmd.Destroy = destroy;
		// Storage stays uninitialized — the caller placement-news into it.
		return cmd;
	}

	int NativeEntityCommandBuffer::Playback(Scene& scene) {
		// Empty buffer is a valid no-op so callers wiring an ECB into a
		// generic spawn pipeline don't have to special-case "no work."
		if (m_EntityCount == 0) {
			return 0;
		}

		// Reserve the entity pool up-front. We pass an empty perTypeCounts
		// span — the per-component pool reserves are a separate optimization
		// (would require us to track types as we record). Even without it,
		// the single pool growth is the dominant win.
		scene.ReserveForLoadRuntime(m_EntityCount, {});

		// Bulk-create every entity in one call — collapses N pool growths
		// into one. Reuse m_CreatedHandles across Clear() cycles so a
		// per-frame spawn loop stays allocation-free.
		m_CreatedHandles.resize(m_EntityCount);
		scene.CreateEntitiesBulk(m_EntityCount, m_CreatedHandles);

		// Defer idempotent on_construct hooks (Transform2D, SpriteRenderer,
		// StaticTag, ParticleSystem2D) until the guard destructor. They fire
		// once per recorded entity in a tight loop at the end of the scope
		// instead of being interleaved with each emplace.
		Scene::LoadGuard guard(scene);

		// Stamp runtime metadata on every entity — same as the managed ECB
		// (ScriptBindingsEcb.cpp:153). NoFlags variant skips the per-entity
		// dirty pulse; we do a single MarkAllDirtyOnce() at the end.
		for (uint32_t i = 0; i < m_EntityCount; ++i) {
			scene.SetEntityMetaDataNoFlags(m_CreatedHandles[i], EntityOrigin::Runtime);
		}

		// Walk the command stream and apply each. The Apply thunk knows the
		// component type at template-instantiation time so this loop is just
		// "indirect call, indirect call, ..." — no type lookup, no
		// emplaceFromBytes registration requirement (which is why the native
		// path supports every C++ component, including ones the managed ECB
		// can't handle for lack of an emplacer).
		entt::registry& registry = scene.GetRegistry();
		for (Command& cmd : m_Commands) {
			// Range check is cheap insurance — a buggy caller can't corrupt
			// the registry by passing an out-of-range EntityRef.
			if (cmd.EntityIndex < m_EntityCount) {
				cmd.Apply(cmd.Storage, registry, m_CreatedHandles[cmd.EntityIndex]);
			}
		}

		// Guard goes out of scope here, flushing deferred hooks. The single
		// end-of-batch dirty pulse runs after the flush — matches the
		// managed ECB ordering (ScriptBindingsEcb.cpp:227).
		// (Falling out of the guard's scope happens automatically as we
		// return from the inner block.)
		scene.MarkAllDirtyOnce();

		return static_cast<int>(m_EntityCount);
	}

	void NativeEntityCommandBuffer::Clear() {
		// Run captured-state destructors for any non-trivial captures. Most
		// builtin uses (lambdas that capture only Vec2/float, value-payload
		// commands for POD components) hit the trivial-destructor fast path
		// where Destroy is nullptr and this loop is a no-op.
		for (Command& cmd : m_Commands) {
			if (cmd.Destroy != nullptr) {
				cmd.Destroy(cmd.Storage);
			}
		}
		m_Commands.clear();      // capacity retained
		m_EntityCount = 0;
		// m_CreatedHandles is kept sized to its peak so the next Playback
		// at the same scale doesn't realloc.
	}

	EntityHandle NativeEntityCommandBuffer::GetCreatedEntity(int i) const {
		if (i < 0 || static_cast<std::size_t>(i) >= m_CreatedHandles.size()) {
			return entt::null;
		}
		return m_CreatedHandles[static_cast<std::size_t>(i)];
	}

} // namespace Index
