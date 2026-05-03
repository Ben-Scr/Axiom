#include "pch.hpp"
#include "GizmosDebugSystem.hpp"
#include "Graphics/Gizmo.hpp"
#include "Scene/SceneManager.hpp"
#include "Scene/Scene.hpp"


#include "Components/Physics/BoxCollider2DComponent.hpp"

namespace Axiom {
	void GizmosDebugSystem::OnUpdate(Application& app, float dt) {
		// TODO(E34, audit-arch): This system currently lives in Axiom-Engine but
		// the work it does (drawing collider outlines + per-entity transform
		// boxes for every loaded scene) is editor/debug-time visualization, not
		// runtime gameplay. Two specific things should move to Axiom-Editor:
		//   1) The Transform2DComponent overlay (gray boxes around every entity)
		//      is purely an authoring aid — runtime games never want it.
		//   2) The BoxCollider2DComponent outline is more defensible as a
		//      runtime debug toggle, but its driving toggle/UI lives in the
		//      editor today, so it should be wired through there too.
		// Until that split happens, gate everything behind a runtime check so
		// shipped runtime builds incur zero gizmo cost. The block below is
		// disabled wholesale because the editor already has its own gizmo
		// rendering path; turning this on would double-draw.
		(void)app;
		(void)dt;
		return;

		SceneManager::Get().ForeachLoadedScene([](const Scene& scene) {
			Gizmo::SetColor(Color::Green());

			for (auto [ent, boxCollider] : scene.GetRegistry().view<BoxCollider2DComponent>().each()) {
				Gizmo::DrawSquare(boxCollider.GetBodyPosition(), boxCollider.GetScale(), boxCollider.GetRotationDegrees());
			}

			Gizmo::SetColor(Color::Gray());

			for (auto [ent, tr] : scene.GetRegistry().view<Transform2DComponent>().each()) {
				Gizmo::DrawSquare(tr.Position, tr.Scale, tr.GetRotationDegrees());
			}
		});
	}
}
