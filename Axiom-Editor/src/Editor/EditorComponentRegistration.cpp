#include <pch.hpp>
#include "Editor/EditorComponentRegistration.hpp"

#include "Gui/ComponentInspectors.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptComponent.hpp"
#include "Scripting/ScriptComponentInspector.hpp"

#include "Components/Components.hpp"

#include <span>
#include <typeindex>

namespace Axiom {
	namespace {
		using InspectorFn = void (*)(std::span<const Entity>);

		struct InspectorBinding {
			std::type_index type;
			InspectorFn inspector;
		};

		template<typename T>
		InspectorBinding Bind(InspectorFn inspector) {
			return InspectorBinding{ std::type_index(typeid(T)), inspector };
		}

		// Editor-only: attach a draw-inspector callback to an already-registered
		// component. The component itself (display name, category, serializedName,
		// has/add/remove/copyTo) MUST be registered by the engine in
		// BuiltInComponentRegistration.cpp first; this file only paints in the UI
		// behavior on top. ScriptComponent is the one exception: it is registered
		// here because its inspector is the only thing the editor needs from it
		// and the engine never instantiates a ComponentInfo for it.
		void AttachInspector(SceneManager& sceneManager, std::type_index type, InspectorFn inspector) {
			bool attached = false;
			sceneManager.GetComponentRegistry().ForEachComponentInfo([&](const std::type_index& id, ComponentInfo& info) {
				if (id == type) {
					info.drawInspector = inspector;
					attached = true;
				}
			});
			AIM_CORE_ASSERT(attached, AxiomErrorCode::InvalidArgument,
				"AttachInspector: component type not registered. Register it in BuiltInComponentRegistration.cpp before attaching an inspector.");
		}
	}

	void RegisterEditorComponentInspectors(SceneManager& sceneManager) {
		// ScriptComponent is editor-only as far as registration goes: register it
		// here (it isn't part of BuiltInComponentRegistration) before attaching
		// its inspector below.
		{
			ComponentInfo scriptInfo{ "Scripts", "Scripting", ComponentCategory::Component };
			scriptInfo.serializedName = "Scripts";
			sceneManager.RegisterComponentType<ScriptComponent>(scriptInfo);
		}

		// Inspector-only attachments. The component metadata itself lives in
		// Axiom-Engine/src/Scene/BuiltInComponentRegistration.cpp — do not
		// re-declare display names, categories, or serialized names here.
		const InspectorBinding bindings[] = {
			Bind<NameComponent>(DrawNameComponentInspector),
			Bind<Transform2DComponent>(DrawTransform2DInspector),

			Bind<SpriteRendererComponent>(DrawSpriteRendererInspector),
			Bind<Camera2DComponent>(DrawCamera2DInspector),
			Bind<ParticleSystem2DComponent>(DrawParticleSystem2DInspector),

			Bind<BoxCollider2DComponent>(DrawBoxCollider2DInspector),
			Bind<Rigidbody2DComponent>(DrawRigidbody2DInspector),
			Bind<FastBody2DComponent>(DrawFastBody2DInspector),
			Bind<FastBoxCollider2DComponent>(DrawFastBoxCollider2DInspector),
			Bind<FastCircleCollider2DComponent>(DrawFastCircleCollider2DInspector),

			Bind<AudioSourceComponent>(DrawAudioSourceInspector),
			Bind<ScriptComponent>(DrawScriptComponentInspector),
		};

		for (const auto& b : bindings)
			AttachInspector(sceneManager, b.type, b.inspector);
	}
}
