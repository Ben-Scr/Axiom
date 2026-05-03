#include <pch.hpp>
#include "Gui/ComponentInspectors.hpp"

#include "Components/Components.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Texture2D.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <type_traits>
#include <variant>

namespace Axiom {

	namespace {

		// Common helper for hybrid inspectors: render every PropertyDescriptor
		// the component declared in BuiltInComponentRegistration.cpp, then the
		// caller appends its own extras. The component's display name is the
		// field-key prefix so reference fields don't collide with same-named
		// fields on other components.
		template <typename TComponent>
		void DrawPropertiesFor(std::span<const Entity> entities) {
			const auto* info = SceneManager::Get().GetComponentRegistry().GetInfo<TComponent>();
			if (!info || info->properties.empty()) return;
			PropertyDrawer::DrawAll(entities, info->properties, info->displayName);
		}

	} // namespace

	// ── SpriteRenderer ───────────────────────────────────────────────
	// Properties (Color, SortingOrder, SortingLayer, Texture) flow through
	// the auto-drawer. Texture picker uses the unified ReferencePicker
	// (thumbnail style). The extras here are the per-texture preview +
	// filter/wrap controls that live on Texture2D, not the component.

	void DrawSpriteRendererInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<SpriteRendererComponent>(entities);

		if (entities.empty()) return;

		// Preview only when the selection's textures are uniform — otherwise
		// "which texture's filter would we be editing?" has no sane answer.
		bool textureUniform = true;
		TextureHandle firstHandle = entities[0].GetComponent<SpriteRendererComponent>().TextureHandle;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (entities[i].GetComponent<SpriteRendererComponent>().TextureHandle != firstHandle) {
				textureUniform = false;
				break;
			}
		}

		if (textureUniform && firstHandle.IsValid()) {
			if (Texture2D* texture = TextureManager::GetTexture(firstHandle)) {
				const float texWidth = texture->GetWidth();
				const float texHeight = texture->GetHeight();
				ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texWidth, texHeight);
				ImGui::Text("%.0f x %.0f", texWidth, texHeight);

				ImGui::PushID("TextureSettings");
				ImGuiUtils::DrawEnumCombo<Filter>("Filter", texture->GetFilter(),
					[&texture](Filter newFilter) { texture->SetFilter(newFilter); });
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap U", texture->GetWrapU(),
					[&texture](Wrap wrapU) { texture->SetWrapU(wrapU); });
				ImGuiUtils::DrawEnumCombo<Wrap>("Wrap V", texture->GetWrapV(),
					[&texture](Wrap wrapV) { texture->SetWrapV(wrapV); });
				ImGui::PopID();
			}
		}
		else if (!textureUniform) {
			ImGui::TextDisabled("Mixed texture - pick to apply to all");
		}
	}

	// ── Camera2D ─────────────────────────────────────────────────────
	// Properties (Zoom, OrthographicSize, ClearColor) flow through the
	// auto-drawer. Read-only viewport size + world viewport are diagnostic
	// extras only meaningful when ONE camera is selected.

	void DrawCamera2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<Camera2DComponent>(entities);

		if (entities.size() == 1) {
			const auto& camera = entities[0].GetComponent<Camera2DComponent>();
			if (Viewport* vp = camera.GetViewport()) {
				ImGuiUtils::DrawVec2ReadOnly("Viewport Size", vp->GetSize());
			}
			ImGuiUtils::DrawVec2ReadOnly("World Viewport", camera.WorldViewPort());
		}
	}

	// ── FastBody2D (Axiom-Physics) ───────────────────────────────────
	// Properties (Type, Mass, UseGravity, BoundaryCheck) flow through the
	// auto-drawer. Runtime velocity + position are read each frame from the
	// physics body; only meaningful when one entity is selected.

	void DrawFastBody2DInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<FastBody2DComponent>(entities);

		if (entities.size() == 1) {
			const auto& body = entities[0].GetComponent<FastBody2DComponent>();
			if (body.IsValid()) {
				ImGui::Separator();
				ImGui::TextDisabled("Runtime");
				Vec2 vel = body.GetVelocity();
				ImGuiUtils::DrawVec2ReadOnly("Velocity", vel);
				Vec2 pos = body.GetPosition();
				ImGuiUtils::DrawVec2ReadOnly("Position", pos);
			}
		}
	}

	// ── ParticleSystem2D ─────────────────────────────────────────────
	// Stays fully custom: ParticleSettings has variant Shape (Circle/Square)
	// with per-shape sub-fields, plus a Play/Pause button that doesn't fit
	// the declarative property model. The component does NOT register
	// PropertyDescriptors — this drawInspector owns the entire inspector body.

	void DrawParticleSystem2DInspector(std::span<const Entity> entities)
	{
		// Play / Pause toggle.
		bool firstPlaying = false;
		bool playUniform = true;
		if (!entities.empty()) {
			firstPlaying = entities[0].GetComponent<ParticleSystem2DComponent>().IsPlaying();
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (entities[i].GetComponent<ParticleSystem2DComponent>().IsPlaying() != firstPlaying) {
					playUniform = false;
					break;
				}
			}
		}
		const std::string playbackLabel = (playUniform ? (firstPlaying ? "Pause" : "Play") : "Play / Pause") + std::string("##Value");
		if (ImGuiUtils::DrawInspectorControl("Playback", [&playbackLabel](const char*) {
			return ImGui::Button(playbackLabel.c_str());
		})) {
			for (const Entity& e : entities) {
				auto& ps = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>();
				if (ps.IsPlaying()) ps.Pause();
				else                ps.Play();
			}
		}

		ImGuiUtils::CheckboxMulti("Play On Awake", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().PlayOnAwake; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().PlayOnAwake = v; });

		ImGuiUtils::InputFloatMulti("LifeTime", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.LifeTime; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.LifeTime = v; });

		ImGuiUtils::InputFloatMulti("Scale", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Scale; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Scale = v; });

		ImGuiUtils::InputFloatMulti("Speed", entities,
			[](const Entity& e) -> float { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Speed; },
			[](const Entity& e, float v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Speed = v; });

		ImGuiUtils::CheckboxMulti("Gravity", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity = v; });

		bool gravityEnabledUniform = true;
		bool gravityEnabledFirst = false;
		if (!entities.empty()) {
			gravityEnabledFirst = entities[0].GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (entities[i].GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseGravity != gravityEnabledFirst) {
					gravityEnabledUniform = false;
					break;
				}
			}
		}
		ImGuiUtils::DrawEnabled(gravityEnabledUniform && gravityEnabledFirst, [&]() {
			ImGuiUtils::DragFloatNMulti("Gravity Value", entities, 2,
				[](const Entity& e, int c) -> float {
					const Vec2& g = e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.Gravity;
					return c == 0 ? g.x : g.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& g = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.Gravity;
					if (c == 0) g.x = v; else g.y = v;
				});
		});

		ImGuiUtils::CheckboxMulti("Random Colors", entities,
			[](const Entity& e) -> bool { return e.GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseRandomColors; },
			[](const Entity& e, bool v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().ParticleSettings.UseRandomColors = v; });

		if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID("Emission");
			ImGuiUtils::InputIntMulti("Emit Over Time", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().EmissionSettings.EmitOverTime); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().EmissionSettings.EmitOverTime = static_cast<uint16_t>(std::clamp(v, 0, 65535)); });

			using ShapeType = ParticleSystem2DComponent::ShapeType;
			auto shapeOf = [](const Entity& e) -> ShapeType {
				return std::visit([](auto&& s) -> ShapeType {
					using T = std::decay_t<decltype(s)>;
					if constexpr (std::is_same_v<T, ParticleSystem2DComponent::CircleParams>) return ShapeType::Circle;
					else                                                                       return ShapeType::Square;
					}, e.GetComponent<ParticleSystem2DComponent>().Shape);
			};
			ImGuiUtils::EnumComboMulti<ShapeType>("Shape Type", entities,
				shapeOf,
				[](const Entity& e, ShapeType v) {
					auto& ps = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>();
					if (v == ShapeType::Circle) ps.Shape = ParticleSystem2DComponent::CircleParams{ 1.f, false };
					else                        ps.Shape = ParticleSystem2DComponent::SquareParams{ Vec2{ 1.f, 1.f } };
				});

			ShapeType firstShape = entities.empty() ? ShapeType::Circle : shapeOf(entities[0]);
			bool shapeUniform = true;
			for (std::size_t i = 1; i < entities.size(); ++i) {
				if (shapeOf(entities[i]) != firstShape) { shapeUniform = false; break; }
			}
			if (!shapeUniform) {
				ImGui::TextDisabled("Mixed shape - pick one to apply to all");
			}
			else if (firstShape == ShapeType::Circle) {
				ImGuiUtils::InputFloatMulti("Radius", entities,
					[](const Entity& e) -> float { return std::get<ParticleSystem2DComponent::CircleParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).Radius; },
					[](const Entity& e, float v) { std::get<ParticleSystem2DComponent::CircleParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).Radius = v; });
				ImGuiUtils::CheckboxMulti("On Circle Edge", entities,
					[](const Entity& e) -> bool { return std::get<ParticleSystem2DComponent::CircleParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).IsOnCircle; },
					[](const Entity& e, bool v) { std::get<ParticleSystem2DComponent::CircleParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).IsOnCircle = v; });
			}
			else {
				ImGuiUtils::DragFloatNMulti("Half Extents", entities, 2,
					[](const Entity& e, int c) -> float {
						const Vec2& v = std::get<ParticleSystem2DComponent::SquareParams>(e.GetComponent<ParticleSystem2DComponent>().Shape).HalfExtends;
						return c == 0 ? v.x : v.y;
					},
					[](const Entity& e, int c, float v) {
						Vec2& he = std::get<ParticleSystem2DComponent::SquareParams>(const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().Shape).HalfExtends;
						if (c == 0) he.x = v; else he.y = v;
					});
			}
			ImGui::PopID();
		}

		if (ImGui::CollapsingHeader("Rendering")) {
			ImGui::PushID("Rendering");
			ImGuiUtils::ColorEdit4Multi("Color", entities,
				[](const Entity& e, int c) -> float {
					const Color& col = e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.Color;
					return (&col.r)[c];
				},
				[](const Entity& e, int c, float v) {
					Color& col = const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.Color;
					(&col.r)[c] = v;
				});

			ImGuiUtils::DragIntMulti("Max Particles", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.MaxParticles); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.MaxParticles = static_cast<uint32_t>(std::max(1, v)); },
				1.0f, 1, 10000);

			ImGuiUtils::InputIntMulti("Sorting Order", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingOrder); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingOrder = static_cast<int16_t>(v); });

			ImGuiUtils::InputIntMulti("Sorting Layer", entities,
				[](const Entity& e) -> int { return static_cast<int>(e.GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingLayer); },
				[](const Entity& e, int v) { const_cast<Entity&>(e).GetComponent<ParticleSystem2DComponent>().RenderingSettings.SortingLayer = static_cast<uint8_t>(std::clamp(v, 0, 255)); });

			if (entities.size() == 1) {
				const auto& ps = entities[0].GetComponent<ParticleSystem2DComponent>();
				TextureHandle previewHandle = ps.GetTextureHandle();
				if (!previewHandle.IsValid()) {
					previewHandle = TextureManager::GetDefaultTexture(DefaultTexture::Square);
				}
				if (Texture2D* texture = TextureManager::GetTexture(previewHandle)) {
					ImGuiUtils::DrawTexturePreview(texture->GetHandle(), texture->GetWidth(), texture->GetHeight());
				}
			}
			ImGui::PopID();
		}
	}

} // namespace Axiom
