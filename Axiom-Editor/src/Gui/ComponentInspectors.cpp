#include <pch.hpp>
#include "Gui/ComponentInspectors.hpp"

#include "Components/Components.hpp"
#include "Components/UI/ButtonComponent.hpp"
#include "Components/UI/InspectorEventBinding.hpp"
#include "Editor/EditorComponentRegistration.hpp"
#include "Graphics/TextureManager.hpp"
#include "Graphics/Texture2D.hpp"
#include "Gui/HierarchyDragData.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/PropertyDrawer.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Math/Trigonometry.hpp"
#include "Scene/ComponentRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/InspectorEventDispatch.hpp"
#include "Scripting/ScriptComponent.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
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

		// ── RectTransform2D layout helpers ───────────────────────────────
		// Sample N channels across `entities` via getChan(entity, channel).
		// Out: per-channel value of entities[0] + per-channel mixed mask.
		// Used by both DrawColumnLabeledFloatRow and DrawAxisLabeledVecMulti.
		template <int N, typename ChannelGet>
		void SampleVecChannels(std::span<const Entity> entities, ChannelGet&& getChan,
			float (&outValues)[N], bool (&outMixed)[N])
		{
			for (int c = 0; c < N; ++c) { outValues[c] = 0.0f; outMixed[c] = false; }
			if (entities.empty()) return;
			for (int c = 0; c < N; ++c) outValues[c] = getChan(entities[0], c);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (int c = 0; c < N; ++c) {
					if (!outMixed[c] && getChan(entities[i], c) != outValues[c]) {
						outMixed[c] = true;
					}
				}
			}
		}

		// Layout: full inspector width split into N columns; each column is
		// a stacked (header text, drag-float) pair (Unity's Pos X / Pos Y).
		// On edit, only the touched channel is written per entity so
		// untouched channels survive across the selection.
		// `perColumnDisabled` (optional, may be null) gates each column
		// individually behind ImGui::BeginDisabled — used by the
		// RectTransform inspector to lock Width when ContentSizeFitter
		// or a no-wrap TextRenderer owns the rect, without losing the
		// drag affordance on the sibling Height column.
		template <int N, typename ChannelGet, typename ChannelSet>
		bool DrawColumnLabeledFloatRow(const char* rowId,
			const char* (&columnLabels)[N],
			std::span<const Entity> entities,
			ChannelGet&& getChan, ChannelSet&& setChan,
			float speed = 0.5f, const char* format = "%.3f",
			const bool* perColumnDisabled = nullptr)
		{
			float values[N];
			bool  mixed[N];
			SampleVecChannels<N>(entities, getChan, values, mixed);

			ImGui::PushID(rowId);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;
			const float colWidth = std::max(1.0f, std::floor(
				(fullWidth - spacing * static_cast<float>(N - 1)) / static_cast<float>(N)));

			// Header row — labels positioned at each column's left edge.
			const float startX = ImGui::GetCursorPosX();
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine();
				ImGui::SetCursorPosX(startX + static_cast<float>(c) * (colWidth + spacing));
				ImGui::TextUnformatted(columnLabels[c]);
			}
			// Cursor naturally advances to a new line for the value row.

			bool anyChanged = false;
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(c);
				ImGui::SetNextItemWidth(colWidth);
				const bool disabled = perColumnDisabled && perColumnDisabled[c];
				if (disabled) ImGui::BeginDisabled();
				float channelValue = values[c];
				const char* fmt = mixed[c] ? "-" : format;
				if (ImGui::DragFloat("##c", &channelValue, speed, 0.0f, 0.0f, fmt)) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
					ImGuiUtils::MarkSelectionDirty(entities);
					anyChanged = true;
				}
				if (disabled) ImGui::EndDisabled();
				ImGui::PopID();
			}

			ImGui::PopID();
			return anyChanged;
		}

		// Layout: standard inspector row (label column + value column), with
		// the value column carrying N (axis-letter, drag-float) pairs side
		// by side. Used for the Min / Max / Pivot / Scale rows where the
		// row's identity comes from the left label.
		template <int N, typename ChannelGet, typename ChannelSet>
		bool DrawAxisLabeledVecMulti(const char* rowLabel,
			const char* (&axisLabels)[N],
			std::span<const Entity> entities,
			ChannelGet&& getChan, ChannelSet&& setChan,
			float speed = 0.01f, const char* format = "%.3f")
		{
			float values[N];
			bool  mixed[N];
			SampleVecChannels<N>(entities, getChan, values, mixed);

			ImGui::PushID(rowLabel);
			ImGuiUtils::BeginInspectorFieldRow(rowLabel);

			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;

			float labelWidths[N];
			float totalLabelWidth = 0.0f;
			for (int c = 0; c < N; ++c) {
				labelWidths[c] = ImGui::CalcTextSize(axisLabels[c]).x;
				totalLabelWidth += labelWidths[c];
			}
			// (label + spacing + field) per cell, plus spacing between cells.
			const float remaining = fullWidth - totalLabelWidth
				- spacing * static_cast<float>(2 * N - 1);
			const float fieldWidth = std::max(1.0f, std::floor(remaining / static_cast<float>(N)));

			bool anyChanged = false;
			for (int c = 0; c < N; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(axisLabels[c]);
				ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(c);
				ImGui::SetNextItemWidth(fieldWidth);
				float channelValue = values[c];
				const char* fmt = mixed[c] ? "-" : format;
				if (ImGui::DragFloat("##c", &channelValue, speed, 0.0f, 0.0f, fmt)) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
					ImGuiUtils::MarkSelectionDirty(entities);
					anyChanged = true;
				}
				ImGui::PopID();
			}

			ImGui::PopID();
			return anyChanged;
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
	// Hybrid: every field is declared in BuiltInComponentRegistration.cpp
	// (including the Shape variant via Properties::MakeVariantWith and the
	// Gravity-Value EnabledIf gate). Properties flow through the auto-drawer.
	// The two non-property bits are the Play / Pause button (a runtime
	// toggle, not a serialized field) and the texture preview (only sane
	// with one entity selected).

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

		// All declarative fields — auto-drawer renders the standard rows
		// with proper EnabledIf / Variant / Clamp behavior.
		DrawPropertiesFor<ParticleSystem2DComponent>(entities);

		// Texture preview is only meaningful with one entity selected.
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
	}

	// True when this entity is referenced as the FillEntity of any
	// SliderComponent in its scene. The slider system writes the
	// fill's SizeDelta.x each frame; locking the anchors here keeps
	// the user from authoring a rect setup that the slider's
	// "anchor=(0,0.5), pivot=(0,0.5), grow rightward" math would
	// silently break.
	bool IsEntitySliderFill(const Entity& entity) {
		const Scene* scene = entity.GetScene();
		if (!scene) return false;
		entt::registry& registry = const_cast<entt::registry&>(scene->GetRegistry());
		const EntityHandle target = entity.GetHandle();
		auto view = registry.view<SliderComponent>();
		for (auto&& [_, slider] : view.each()) {
			if (slider.FillEntity == target) return true;
		}
		return false;
	}

	bool AnyEntitySliderFill(std::span<const Entity> entities) {
		for (const auto& e : entities) {
			if (IsEntitySliderFill(e)) return true;
		}
		return false;
	}

	// ── RectTransform2D ──────────────────────────────────────────────
	// Unity-style layout: stacked-header columns for Position + Size, then
	// a collapsible Anchors group with Min / Max, followed by Pivot,
	// Rotation, and Scale. The auto-drawer fallback can't express the
	// per-row sub-labels or the column-header-above-value rows, so this
	// component opts into a fully custom drawer (its property descriptors
	// are dropped — custom drawer wins over properties anyway).
	void DrawRectTransform2DInspector(std::span<const Entity> entities)
	{
		using RTC = RectTransform2DComponent;

		// Lock the slider-driven rect fields when this entity is a
		// SliderComponent's FillEntity. The slider system rewrites
		// SizeDelta.x every frame and assumes the fill is point-anchored
		// at the parent's left edge with pivot at the fill's left edge —
		// editing those fields manually would either be overwritten on
		// the next tick (Width) or would break the fill's geometry
		// (anchors / pivot / left-edge AnchoredPosition).
		const bool fillReadOnly = AnyEntitySliderFill(entities);

		// Position (Pos X / Pos Y) — column-header-above-value layout.
		const char* posLabels[] = { "Pos X", "Pos Y" };
		DrawColumnLabeledFloatRow<2>("##Position", posLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& p = e.GetComponent<RTC>().AnchoredPosition;
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& p = const_cast<Entity&>(e).GetComponent<RTC>().AnchoredPosition;
				(c == 0 ? p.x : p.y) = v;
			});

		// Size (Width / Height) — same column-header layout. Drag speed of
		// 1.0f matches the integer-pixel feel users expect for size fields.
		// Each axis is disabled when something else owns it:
		//   • ContentSizeFitter with HorizontalFit / VerticalFit — fitter
		//     overwrites SizeDelta from children every frame, so manual
		//     edits would just snap back next tick.
		//   • TextRendererComponent with WrapMode::None — the text's
		//     measured natural size is written into SizeDelta by
		//     UILayoutSystem::FitTextNaturalSize each frame; same snap-
		//     back risk on both axes if the user edited.
		// On a multi-selection any entity in the list claiming an axis
		// disables that axis for the whole row — safer than allowing a
		// partial write some entities would silently revert.
		bool sizeAxisDisabled[2] = { false, false };
		for (const Entity& e : entities) {
			if (e.HasComponent<ContentSizeFitterComponent>()) {
				const auto& csf = e.GetComponent<ContentSizeFitterComponent>();
				if (csf.HorizontalFit) sizeAxisDisabled[0] = true;
				if (csf.VerticalFit)   sizeAxisDisabled[1] = true;
			}
			if (e.HasComponent<TextRendererComponent>()) {
				const auto& text = e.GetComponent<TextRendererComponent>();
				if (text.WrapMode == TextWrapMode::None) {
					sizeAxisDisabled[0] = true;
					sizeAxisDisabled[1] = true;
				}
			}
		}

		const char* sizeLabels[] = { "Width", "Height" };
		DrawColumnLabeledFloatRow<2>("##Size", sizeLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& s = e.GetComponent<RTC>().SizeDelta;
				return c == 0 ? s.x : s.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& s = const_cast<Entity&>(e).GetComponent<RTC>().SizeDelta;
				(c == 0 ? s.x : s.y) = v;
			},
			1.0f, "%.3f", sizeAxisDisabled);

		// Axis sub-labels reused by every "label + Vec2" row below.
		const char* xyLabels[] = { "X", "Y" };

		// Anchors group — collapsible to mirror Unity's foldout. Slow drag
		// speed because anchors live in [0, 1]. When the entity is a
		// slider Fill, the rows render disabled (read-only) since the
		// slider's geometry math depends on a fixed anchor configuration.
		if (ImGui::CollapsingHeader("Anchors", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID("Anchors");
			ImGui::Indent(8.0f);

			if (fillReadOnly) ImGui::BeginDisabled();

			DrawAxisLabeledVecMulti<2>("Min", xyLabels, entities,
				[](const Entity& e, int c) -> float {
					const Vec2& a = e.GetComponent<RTC>().AnchorMin;
					return c == 0 ? a.x : a.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& a = const_cast<Entity&>(e).GetComponent<RTC>().AnchorMin;
					(c == 0 ? a.x : a.y) = v;
				});

			DrawAxisLabeledVecMulti<2>("Max", xyLabels, entities,
				[](const Entity& e, int c) -> float {
					const Vec2& a = e.GetComponent<RTC>().AnchorMax;
					return c == 0 ? a.x : a.y;
				},
				[](const Entity& e, int c, float v) {
					Vec2& a = const_cast<Entity&>(e).GetComponent<RTC>().AnchorMax;
					(c == 0 ? a.x : a.y) = v;
				});

			if (fillReadOnly) ImGui::EndDisabled();

			ImGui::Unindent(8.0f);
			ImGui::PopID();
		}

		DrawAxisLabeledVecMulti<2>("Pivot", xyLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& p = e.GetComponent<RTC>().Pivot;
				return c == 0 ? p.x : p.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& p = const_cast<Entity&>(e).GetComponent<RTC>().Pivot;
				(c == 0 ? p.x : p.y) = v;
			});

		// Rotation: 2D engines only need the Z component, edited in degrees
		// to match the Transform2D inspector (component stores radians).
		// Edits write LocalRotation — UILayoutSystem composes it against
		// the parent's world rotation each frame and writes the result
		// into the rect's Rotation field.
		ImGuiUtils::DragFloatMulti("Rotation", entities,
			[](const Entity& e) -> float {
				return Degrees(e.GetComponent<RTC>().LocalRotation);
			},
			[](const Entity& e, float v) {
				const_cast<Entity&>(e).GetComponent<RTC>().LocalRotation = Radians(v);
			},
			1.0f);

		// Scale: edits write LocalScale; UILayoutSystem composes it
		// (Hadamard) with the parent's world scale into the rect's Scale.
		DrawAxisLabeledVecMulti<2>("Scale", xyLabels, entities,
			[](const Entity& e, int c) -> float {
				const Vec2& s = e.GetComponent<RTC>().LocalScale;
				return c == 0 ? s.x : s.y;
			},
			[](const Entity& e, int c, float v) {
				Vec2& s = const_cast<Entity&>(e).GetComponent<RTC>().LocalScale;
				(c == 0 ? s.x : s.y) = v;
			},
			0.1f);
	}

	// ── Inspector event list editor (used by Button.OnClick + the
	//   Slider / Toggle / InputField / Dropdown event lists) ────────
	namespace {

		// Compare two binding vectors element-by-element so multi-select
		// uniformity check is value-based, not address-based. Includes
		// the typed argument fields so a class change that also reset
		// the arg counts as "different" across the selection.
		bool EventListsEqual(const InspectorEventList& a, const InspectorEventList& b) {
			if (a.Bindings.size() != b.Bindings.size()) return false;
			for (std::size_t i = 0; i < a.Bindings.size(); ++i) {
				const auto& x = a.Bindings[i];
				const auto& y = b.Bindings[i];
				if (x.TargetEntityUUID != y.TargetEntityUUID) return false;
				if (x.ScriptClassName  != y.ScriptClassName)  return false;
				if (x.MethodName       != y.MethodName)       return false;
				if (x.Enabled          != y.Enabled)          return false;
				if (x.ArgumentKind     != y.ArgumentKind)     return false;
				if (x.ArgumentValue    != y.ArgumentValue)    return false;
			}
			return true;
		}

		// Resolve a binding's target-entity UUID into a display string for
		// the entity-picker button. UUID 0 is the "self" sentinel and shows
		// as "(Self)" — same UX as Unity's None-but-meaningful fallback.
		std::string DescribeEventTarget(uint64_t uuid) {
			if (uuid == 0) return "(Self)";
			bool missing = false;
			std::string secondary;
			std::string display = ReferencePicker::ResolveEntityDisplay(uuid, missing, &secondary);
			if (missing || display.empty()) return "(Missing)";
			return display;
		}

		// Distinct script class names on `entity`'s ScriptComponent. The
		// editor needs ALL classes the entity has authored, regardless of
		// whether a live managed instance exists yet — managed instances
		// only get created on play-mode entry, but bindings have to be
		// authored in edit mode. Empty list means no ScriptComponent.
		//
		// Includes both EntityScript-derived `Scripts` and managed-Component
		// entries (`ManagedComponents`). The C# reflection helper resolves
		// either kind by name.
		std::vector<std::string> CollectTargetScriptClasses(const Scene& scene, EntityHandle target) {
			std::vector<std::string> out;
			if (target == entt::null) return out;
			ScriptComponent* sc = nullptr;
			if (!const_cast<Scene&>(scene).TryGetComponent<ScriptComponent>(target, sc) || sc == nullptr) {
				return out;
			}
			out.reserve(sc->Scripts.size() + sc->ManagedComponents.size());
			for (const ScriptInstance& s : sc->Scripts) {
				if (s.GetClassName().empty()) continue;
				if (std::find(out.begin(), out.end(), s.GetClassName()) == out.end()) {
					out.push_back(s.GetClassName());
				}
			}
			for (const std::string& className : sc->ManagedComponents) {
				if (className.empty()) continue;
				if (std::find(out.begin(), out.end(), className) == out.end()) {
					out.push_back(className);
				}
			}
			return out;
		}

		// Resolve a row's TargetEntityUUID — including "self" — to a
		// concrete EntityHandle in the scene the inspector is editing.
		// Used by the class / method combos to query the right scripts.
		EntityHandle ResolveRowTarget(const Scene& scene, EntityHandle ownerEntity, uint64_t uuid) {
			if (uuid == 0) return ownerEntity;
			EntityHandle resolved = entt::null;
			const_cast<Scene&>(scene).TryResolveEntityRef(uuid, resolved);
			return resolved;
		}

		// Wraps the BeginDragDropTarget pattern so the entity-binding rows
		// can accept hierarchy drops without duplicating the boilerplate.
		// Decodes the HIERARCHY_ENTITY payload, resolves the live handle
		// against the active scene, and converts to the persistent UUID
		// the binding stores. Returns the UUID on a successful drop, or
		// std::nullopt if no payload arrived this frame. Mirrors the
		// pattern in PropertyDrawer::DrawEntityRef.
		std::optional<uint64_t> AcceptEntityDrop() {
			std::optional<uint64_t> result;
			if (!ImGui::BeginDragDropTarget()) return result;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
				if (payload->DataSize == sizeof(HierarchyDragData)) {
					const auto* data = static_cast<const HierarchyDragData*>(payload->Data);
					const EntityHandle handle = static_cast<EntityHandle>(data->EntityHandle);
					const Scene* scene = SceneManager::Get().GetActiveScene();
					if (scene && scene->IsValid(handle)) {
						const uint64_t uuid = scene->GetEntityPersistentID(handle);
						if (uuid != 0) result = uuid;
					}
				}
			}
			ImGui::EndDragDropTarget();
			return result;
		}

		// Method-name combo entries arrive from the C# side as
		// "<methodName>:<argKindByte>". Split into a bare display name +
		// the InspectorEventArgKind so the inspector can render the
		// correct value editor for the chosen method.
		struct MethodEntry {
			std::string Name;
			InspectorEventArgKind Kind = InspectorEventArgKind::Void;
		};

		std::vector<MethodEntry> ParseMethodEntries(const std::vector<std::string>& raw) {
			std::vector<MethodEntry> out;
			out.reserve(raw.size());
			for (const std::string& entry : raw) {
				const std::size_t sep = entry.find_last_of(':');
				MethodEntry me;
				if (sep == std::string::npos) {
					me.Name = entry;
					me.Kind = InspectorEventArgKind::Void;
				}
				else {
					me.Name = entry.substr(0, sep);
					int parsed = 0;
					try { parsed = std::stoi(entry.substr(sep + 1)); } catch (...) { parsed = 0; }
					if (parsed < 0 || parsed > static_cast<int>(InspectorEventArgKind::EntityRef)) {
						parsed = 0;
					}
					me.Kind = static_cast<InspectorEventArgKind>(parsed);
				}
				out.push_back(std::move(me));
			}
			return out;
		}

		const char* ArgKindLabel(InspectorEventArgKind k) {
			switch (k) {
				case InspectorEventArgKind::Void:      return "void";
				case InspectorEventArgKind::Bool:      return "bool";
				case InspectorEventArgKind::Int:       return "int";
				case InspectorEventArgKind::Float:     return "float";
				case InspectorEventArgKind::Double:    return "double";
				case InspectorEventArgKind::String:    return "string";
				case InspectorEventArgKind::Vec2:      return "Vector2";
				case InspectorEventArgKind::Color:     return "Color";
				case InspectorEventArgKind::EntityRef: return "Entity";
			}
			return "?";
		}

		// Decode argument string for a numeric kind. Falls back to 0 on
		// any parse failure — same shape as the C# side, so a saved
		// binding survives a load-edit-save round-trip even when the
		// authored value was empty.
		float DecodeFloat(const std::string& s, float fallback = 0.0f) {
			if (s.empty()) return fallback;
			try { return std::stof(s); } catch (...) { return fallback; }
		}
		int DecodeInt(const std::string& s, int fallback = 0) {
			if (s.empty()) return fallback;
			try { return std::stoi(s); } catch (...) { return fallback; }
		}
		double DecodeDouble(const std::string& s, double fallback = 0.0) {
			if (s.empty()) return fallback;
			try { return std::stod(s); } catch (...) { return fallback; }
		}
		uint64_t DecodeUInt64(const std::string& s, uint64_t fallback = 0) {
			if (s.empty()) return fallback;
			try { return std::stoull(s); } catch (...) { return fallback; }
		}
		std::array<float, 2> DecodeVec2(const std::string& s) {
			std::array<float, 2> out{ 0.0f, 0.0f };
			const std::size_t sep = s.find(',');
			if (sep == std::string::npos) return out;
			out[0] = DecodeFloat(s.substr(0, sep));
			out[1] = DecodeFloat(s.substr(sep + 1));
			return out;
		}
		std::array<float, 4> DecodeColor(const std::string& s) {
			std::array<float, 4> out{ 1.0f, 1.0f, 1.0f, 1.0f };
			std::size_t i = 0;
			std::size_t start = 0;
			std::size_t channel = 0;
			while (channel < 4 && start <= s.size()) {
				const std::size_t sep = s.find(',', start);
				const std::string slice = (sep == std::string::npos)
					? s.substr(start) : s.substr(start, sep - start);
				out[channel++] = DecodeFloat(slice, channel <= 3 ? 1.0f : 1.0f);
				if (sep == std::string::npos) break;
				start = sep + 1;
				++i;
			}
			return out;
		}

		// Render the value editor for a binding's typed argument. Called
		// inline next to the method combo. Mutates the binding via
		// `setValue` (which calls back into the host's mutate-all path
		// so multi-select edits stay in lockstep). Width is the field
		// width that was reserved for this column by the row layout.
		template <typename SetValue>
		void DrawArgValueEditor(const InspectorEventBinding& row, float width, SetValue&& setValue) {
			const InspectorEventArgKind k = row.ArgumentKind;
			ImGui::SetNextItemWidth(width);
			switch (k) {
				case InspectorEventArgKind::Void:
					// No editable argument — display a disabled stub so
					// the layout still allocates a column (keeps row
					// alignment consistent across mixed kinds).
					ImGui::BeginDisabled();
					ImGui::Button("(no arg)##NoArg", ImVec2(width, 0.0f));
					ImGui::EndDisabled();
					break;
				case InspectorEventArgKind::Bool: {
					bool v = row.ArgumentValue == "1";
					if (ImGui::Checkbox("##ArgBool", &v)) setValue(v ? "1" : "0");
					break;
				}
				case InspectorEventArgKind::Int: {
					int v = DecodeInt(row.ArgumentValue);
					if (ImGui::DragInt("##ArgInt", &v)) setValue(std::to_string(v));
					break;
				}
				case InspectorEventArgKind::Float: {
					float v = DecodeFloat(row.ArgumentValue);
					if (ImGui::DragFloat("##ArgFloat", &v, 0.1f)) {
						char buf[32];
						std::snprintf(buf, sizeof(buf), "%g", v);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Double: {
					double v = DecodeDouble(row.ArgumentValue);
					if (ImGui::InputDouble("##ArgDouble", &v, 0.0, 0.0, "%.6g")) {
						char buf[32];
						std::snprintf(buf, sizeof(buf), "%.10g", v);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::String: {
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", row.ArgumentValue.c_str());
					if (ImGui::InputText("##ArgString", buf, sizeof(buf))) {
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Vec2: {
					auto vv = DecodeVec2(row.ArgumentValue);
					float v[2] = { vv[0], vv[1] };
					if (ImGui::DragFloat2("##ArgVec2", v, 0.1f)) {
						char buf[64];
						std::snprintf(buf, sizeof(buf), "%g,%g", v[0], v[1]);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::Color: {
					auto cc = DecodeColor(row.ArgumentValue);
					float v[4] = { cc[0], cc[1], cc[2], cc[3] };
					if (ImGui::ColorEdit4("##ArgColor", v,
							ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
					{
						char buf[96];
						std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", v[0], v[1], v[2], v[3]);
						setValue(std::string(buf));
					}
					break;
				}
				case InspectorEventArgKind::EntityRef: {
					// Entity reference renders as a click-to-pick button
					// just like the row's target-entity button. The
					// modal picker drives selection through the standard
					// OpenForFieldKey / ConsumeSelection / RenderPopup
					// loop; we encode the chosen UUID into ArgumentValue.
					const uint64_t uuid = DecodeUInt64(row.ArgumentValue);
					std::string label = uuid == 0 ? "(None)" : DescribeEventTarget(uuid);
					ImGui::Button((label + "##ArgEntity").c_str(), ImVec2(width, 0.0f));
					// Picker open is wired by the caller via setValue
					// keys — left as a placeholder so the value can
					// still display. The full picker hookup in the
					// generic editor below opens the modal on click.
					break;
				}
			}
		}

	} // namespace

	// Render an "On X ()" foldout body for an arbitrary InspectorEventList
	// reachable through a per-component getter / mutator. Used by the
	// Button + Slider + Toggle + InputField + Dropdown inspectors so the
	// row layout, multi-select uniform-check, and typed-arg editor stay
	// in one place. Caller supplies:
	//   `label` — visible foldout label ("On Click ()").
	//   `idSuffix` — unique ImGui ID suffix so two foldouts on the same
	//                inspector (e.g. InputField's OnValueChanged + OnSubmitted)
	//                don't collide.
	//   `getList` — read-only accessor returning a const InspectorEventList&
	//                for an entity in the selection.
	//   `mutate` — write-side accessor; calls a closure with a mutable
	//                InspectorEventList& for every entity in the selection
	//                (and marks the scene dirty).
	template <typename GetList, typename MutateAll>
	void DrawEventListFoldout(const char* label, const char* idSuffix,
		std::span<const Entity> entities,
		GetList&& getList, MutateAll&& mutate)
	{
		const std::string foldoutId = std::string(label) + "##" + idSuffix;
		if (!ImGui::CollapsingHeader(foldoutId.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			return;
		}
		if (entities.empty()) return;

		// Scope every control inside this foldout to its own ID space.
		// Two event lists on the same inspector (e.g. InputField's
		// OnValueChanged + OnSubmitted) share row indices i = 0..N-1
		// and bare label IDs ("##Enabled", "##NoArg", "##Method", ...);
		// without an outer push the row-0 controls of both lists
		// hash to the same ImGui ID and trigger the
		// "ID stack collision" assertion. Paired with PopID at the
		// matching end-of-function early-return points below.
		ImGui::PushID(idSuffix);

		// Uniformity check — same shape as the original Button helper.
		const InspectorEventList& first = getList(entities[0]);
		bool uniform = true;
		for (std::size_t i = 1; i < entities.size(); ++i) {
			if (!EventListsEqual(first, getList(entities[i]))) {
				uniform = false;
				break;
			}
		}
		if (!uniform) {
			ImGui::TextDisabled("Multiple values");
			ImGui::SameLine();
			std::string applyLabel = std::string("Apply primary's list to all##Apply") + idSuffix;
			if (ImGui::SmallButton(applyLabel.c_str())) {
				const InspectorEventList src = first;
				mutate([&src](InspectorEventList& dst) { dst = src; });
			}
			ImGui::PopID();
			return;
		}

		const Scene* scene = entities[0].GetScene();
		const EntityHandle owner = entities[0].GetHandle();
		const auto& bindings = first.Bindings;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float spacing = style.ItemInnerSpacing.x;
		const float minusWidth = ImGui::CalcTextSize("-").x + style.FramePadding.x * 2.0f;
		const float checkWidth = ImGui::GetFrameHeight();

		ImGui::Indent();
		if (bindings.empty()) {
			ImGui::TextDisabled("List is Empty");
		}
		else {
			std::optional<std::size_t> rowToRemove;
			for (std::size_t i = 0; i < bindings.size(); ++i) {
				const InspectorEventBinding& row = bindings[i];
				ImGui::PushID(static_cast<int>(i));

				// Column widths: 4 main combos (target / class / method
				// / arg-value) + checkbox + minus, separated by 5
				// inner-spacings. Min 80 px per combo so the names
				// remain readable on narrow inspectors.
				const float totalWidth = ImGui::GetContentRegionAvail().x;
				const float fieldsWidth = std::max(160.0f,
					totalWidth - checkWidth - minusWidth - spacing * 5.0f);
				const float fieldWidth = std::floor(fieldsWidth / 4.0f);

				bool enabled = row.Enabled;
				if (ImGui::Checkbox("##Enabled", &enabled)) {
					mutate([i, enabled](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].Enabled = enabled;
					});
				}
				ImGui::SameLine(0.0f, spacing);

				// Target picker.
				const std::string targetKey = std::string(idSuffix) + ".target." + std::to_string(i);
				if (auto picked = ReferencePicker::ConsumeSelection(targetKey)) {
					uint64_t newUuid = 0;
					const std::string& s = *picked;
					if (s.rfind("prefab:", 0) != 0 && !s.empty()) {
						newUuid = std::strtoull(s.c_str(), nullptr, 10);
					}
					mutate([i, newUuid](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].TargetEntityUUID = newUuid;
					});
				}
				const std::string targetLabel = DescribeEventTarget(row.TargetEntityUUID);
				bool truncatedTarget = false;
				const std::string targetText = ImGuiUtils::Ellipsize(targetLabel,
					fieldWidth - style.FramePadding.x * 2.0f, &truncatedTarget);
				if (ImGui::Button((targetText + "##Target").c_str(), ImVec2(fieldWidth, 0.0f))) {
					ReferencePicker::OpenForFieldKey(targetKey, "Select Entity",
						ReferencePicker::CollectEntities());
				}
				if (auto droppedUuid = AcceptEntityDrop()) {
					mutate([i, uuid = *droppedUuid](InspectorEventList& list) {
						if (i < list.Bindings.size()) list.Bindings[i].TargetEntityUUID = uuid;
					});
				}
				if (truncatedTarget && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", targetLabel.c_str());
				}
				ImGui::SameLine(0.0f, spacing);

				// Class combo.
				const EntityHandle resolvedTarget = scene
					? ResolveRowTarget(*scene, owner, row.TargetEntityUUID)
					: entt::null;
				const std::vector<std::string> classes = scene
					? CollectTargetScriptClasses(*scene, resolvedTarget)
					: std::vector<std::string>{};
				const char* classPreview = row.ScriptClassName.empty()
					? (classes.empty() ? "<no scripts>" : "<select>")
					: row.ScriptClassName.c_str();
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::BeginCombo("##Class", classPreview)) {
					for (const std::string& className : classes) {
						const bool selected = (className == row.ScriptClassName);
						if (ImGui::Selectable(className.c_str(), selected)) {
							mutate([i, &className](InspectorEventList& list) {
								if (i < list.Bindings.size()) {
									list.Bindings[i].ScriptClassName = className;
									list.Bindings[i].MethodName.clear();
									list.Bindings[i].ArgumentKind = InspectorEventArgKind::Void;
									list.Bindings[i].ArgumentValue.clear();
								}
							});
						}
						if (selected) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine(0.0f, spacing);

				// Method combo. The C# side reports each method as
				// "name:argKind". Display the bare name; on selection
				// stamp ArgumentKind from the parsed kind so the value
				// editor that follows knows what widget to render.
				std::string methodPreview;
				if (row.MethodName.empty()) {
					methodPreview = row.ScriptClassName.empty() ? "<pick class>" : "<select>";
				}
				else {
					methodPreview = row.MethodName;
					if (row.ArgumentKind != InspectorEventArgKind::Void) {
						methodPreview += " (";
						methodPreview += ArgKindLabel(row.ArgumentKind);
						methodPreview += ")";
					}
				}
				ImGui::SetNextItemWidth(fieldWidth);
				if (ImGui::BeginCombo("##Method", methodPreview.c_str())) {
					if (!row.ScriptClassName.empty()) {
						const std::vector<std::string> raw =
							InspectorEvents::GetInvokableMethods(row.ScriptClassName);
						const std::vector<MethodEntry> methods = ParseMethodEntries(raw);
						if (methods.empty()) {
							ImGui::TextDisabled("(no invokable methods)");
						}
						for (const MethodEntry& m : methods) {
							std::string display = m.Name;
							if (m.Kind != InspectorEventArgKind::Void) {
								display += " (";
								display += ArgKindLabel(m.Kind);
								display += ")";
							}
							const bool selected = (m.Name == row.MethodName);
							if (ImGui::Selectable(display.c_str(), selected)) {
								mutate([i, m](InspectorEventList& list) {
									if (i < list.Bindings.size()) {
										auto& b = list.Bindings[i];
										b.MethodName = m.Name;
										// Reset value when arg type changes
										// — the old encoded payload makes
										// no sense for the new kind.
										if (b.ArgumentKind != m.Kind) {
											b.ArgumentValue.clear();
										}
										b.ArgumentKind = m.Kind;
									}
								});
							}
							if (selected) ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine(0.0f, spacing);

				// Typed argument editor for the picked method's parameter.
				// Entity-ref kind needs the picker hookup that the inline
				// helper can't do (it doesn't have access to the picker
				// keys / mutate closure), so route that case here.
				if (row.ArgumentKind == InspectorEventArgKind::EntityRef) {
					const std::string argKey = std::string(idSuffix) + ".arg." + std::to_string(i);
					if (auto picked = ReferencePicker::ConsumeSelection(argKey)) {
						uint64_t newUuid = 0;
						const std::string& s = *picked;
						if (s.rfind("prefab:", 0) != 0 && !s.empty()) {
							newUuid = std::strtoull(s.c_str(), nullptr, 10);
						}
						mutate([i, newUuid](InspectorEventList& list) {
							if (i < list.Bindings.size()) {
								list.Bindings[i].ArgumentValue = std::to_string(newUuid);
							}
						});
					}
					const uint64_t argUuid = DecodeUInt64(row.ArgumentValue);
					std::string argLabel = argUuid == 0 ? "(None)" : DescribeEventTarget(argUuid);
					bool truncatedArg = false;
					const std::string argText = ImGuiUtils::Ellipsize(argLabel,
						fieldWidth - style.FramePadding.x * 2.0f, &truncatedArg);
					if (ImGui::Button((argText + "##ArgEntityPick").c_str(), ImVec2(fieldWidth, 0.0f))) {
						ReferencePicker::OpenForFieldKey(argKey, "Select Entity",
							ReferencePicker::CollectEntities());
					}
					if (auto droppedUuid = AcceptEntityDrop()) {
						mutate([i, uuid = *droppedUuid](InspectorEventList& list) {
							if (i < list.Bindings.size()) {
								list.Bindings[i].ArgumentValue = std::to_string(uuid);
							}
						});
					}
					if (truncatedArg && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", argLabel.c_str());
					}
				}
				else {
					DrawArgValueEditor(row, fieldWidth, [&mutate, i](std::string newValue) {
						mutate([i, value = std::move(newValue)](InspectorEventList& list) {
							if (i < list.Bindings.size()) list.Bindings[i].ArgumentValue = value;
						});
					});
				}
				ImGui::SameLine(0.0f, spacing);

				if (ImGui::Button("-##Remove", ImVec2(minusWidth, 0.0f))) {
					rowToRemove = i;
				}
				ImGui::PopID();
			}
			if (rowToRemove) {
				const std::size_t i = *rowToRemove;
				mutate([i](InspectorEventList& list) {
					if (i < list.Bindings.size()) list.Bindings.erase(list.Bindings.begin() + i);
				});
			}
		}
		ImGui::Unindent();

		ImGui::Spacing();
		std::string addLabel = std::string("+##Add") + idSuffix;
		if (ImGui::Button(addLabel.c_str())) {
			mutate([](InspectorEventList& list) {
				list.Bindings.emplace_back();
			});
		}

		// Closes the PushID(idSuffix) at the top of the function — paired
		// here so every control inside the foldout (including the
		// per-row PushID(i) blocks above) ends up under a foldout-unique
		// ID-stack root.
		ImGui::PopID();
	}

	// Render an event-list foldout against a specific component. Used by
	// every UI component that owns one or more InspectorEventLists —
	// keeps the per-component inspector down to a single line per list.
	template <typename TComponent, typename ListPtr>
	static void DrawComponentEventList(const char* label, const char* idSuffix,
		std::span<const Entity> entities, ListPtr listPtr)
	{
		auto getList = [listPtr](const Entity& e) -> const InspectorEventList& {
			return e.GetComponent<TComponent>().*listPtr;
		};
		auto mutateAll = [listPtr, entities](auto&& fn) {
			for (const Entity& e : entities) {
				auto& c = const_cast<Entity&>(e).GetComponent<TComponent>();
				fn(c.*listPtr);
				if (Scene* s = const_cast<Entity&>(e).GetScene()) s->MarkDirty();
			}
		};
		DrawEventListFoldout(label, idSuffix, entities, getList, mutateAll);
	}

	void DrawButtonInspector(std::span<const Entity> entities)
	{
		// Standard fields (target graphic, transition, colors, sprites)
		// flow through the auto-drawer just like every other component.
		DrawPropertiesFor<ButtonComponent>(entities);
		if (entities.empty()) return;

		ImGui::Spacing();
		DrawComponentEventList<ButtonComponent>("On Click ()", "Button.OnClick",
			entities, &ButtonComponent::OnClick);

		// Picker modal — must be rendered exactly once per frame, after
		// every field that could open it.
		ReferencePicker::RenderPopup();
	}

	// Slider / Toggle / InputField / Dropdown share the same hybrid
	// "auto-drawn properties + inspector-event-list foldout" shape.
	// Each adds one or more typed event lists below the standard fields.

	void DrawSliderInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<SliderComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<SliderComponent>("On Value Changed (float)",
			"Slider.OnValueChanged", entities, &SliderComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

	void DrawToggleInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<ToggleComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<ToggleComponent>("On Value Changed (bool)",
			"Toggle.OnValueChanged", entities, &ToggleComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

	void DrawInputFieldInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<InputFieldComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<InputFieldComponent>("On Value Changed (string)",
			"InputField.OnValueChanged", entities, &InputFieldComponent::OnValueChanged);
		ImGui::Spacing();
		DrawComponentEventList<InputFieldComponent>("On Submitted (string)",
			"InputField.OnSubmitted", entities, &InputFieldComponent::OnSubmitted);
		ReferencePicker::RenderPopup();
	}

	void DrawDropdownInspector(std::span<const Entity> entities)
	{
		DrawPropertiesFor<DropdownComponent>(entities);
		if (entities.empty()) return;
		ImGui::Spacing();
		DrawComponentEventList<DropdownComponent>("On Value Changed (int)",
			"Dropdown.OnValueChanged", entities, &DropdownComponent::OnValueChanged);
		ReferencePicker::RenderPopup();
	}

} // namespace Axiom
