#pragma once
#include <imgui.h>
#include "Collections/Color.hpp"
#include "Collections/Vec2.hpp"
#include "Scene/Entity.hpp"
#include <magic_enum/magic_enum.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <type_traits>

namespace Axiom::ImGuiUtils {
	float GetInspectorLabelColumnWidth();
	void BeginInspectorFieldRow(const char* label);
	void DrawInspectorLabel(const char* label);

	template<typename Draw>
	bool DrawInspectorControl(const char* label, Draw&& draw)
	{
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = draw("##Value");
		ImGui::PopID();
		return changed;
	}

	template<typename Draw>
	void DrawInspectorValue(const char* label, Draw&& draw)
	{
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		draw("##Value");
		ImGui::PopID();
	}

	inline Color DrawColorPick4(const char* label, const Color& color)
	{
		float values[4] = { color.r, color.g, color.b, color.a };
		DrawInspectorValue(label, [&values](const char* id) {
			ImGui::ColorEdit4(id, values);
		});
		return Color(values[0], values[1], values[2], values[3]);
	}
	template<typename Draw>
	void DrawEnabled(bool enabled, Draw&& draw) {
				if (!enabled) {
			ImGui::BeginDisabled();
		}

		draw();

		if (!enabled) {
			ImGui::EndDisabled();
		}
	}

	template<typename TEnum, typename Setter>
	bool DrawEnumCombo(const char* label, TEnum currentValue, Setter&& setter)
	{
		static_assert(std::is_enum_v<TEnum>, "DrawEnumCombo requires an enum type.");

		auto previewView = magic_enum::enum_name(currentValue);
		const char* preview = previewView.empty() ? "Unknown" : previewView.data();

		bool changed = false;

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (TEnum value : magic_enum::enum_values<TEnum>()) {
				const bool isSelected = (currentValue == value);

				auto name = magic_enum::enum_name(value);
				if (name.empty()) {
					continue;
				}

				if (ImGui::Selectable(name.data(), isSelected)) {
					setter(value);
					changed = true;
				}

				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}

			ImGui::EndCombo();
		}
		ImGui::PopID();

		return changed;
	}

	template<typename TVec2>
	void DrawVec2ReadOnly(const char* id, const TVec2& vec2)
	{
		float values[2] = { static_cast<float>(vec2.x), static_cast<float>(vec2.y) };
		DrawInspectorValue(id, [&values](const char*) {
			const float componentWidth = std::max(45.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
			ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(componentWidth);
			ImGui::InputFloat("##X", &values[0], 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(componentWidth);
			ImGui::InputFloat("##Y", &values[1], 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_ReadOnly);
			ImGui::EndDisabled();
		});
	}

	template<typename TVec2>
	bool DrawVec2(const char* id, TVec2& vec2)
	{
		float values[2] = { vec2.x, vec2.y };
		bool changed = false;
		DrawInspectorValue(id, [&values, &changed](const char*) {
			const float componentWidth = std::max(45.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
			ImGui::SetNextItemWidth(componentWidth);
			changed |= ImGui::InputFloat("##X", &values[0], 0.0f, 0.0f, "%.3f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(componentWidth);
			changed |= ImGui::InputFloat("##Y", &values[1], 0.0f, 0.0f, "%.3f");
		});

		if (changed) {
			vec2.x = values[0];
			vec2.y = values[1];
		}

		return changed;
	}

	void DrawTexturePreview(unsigned int rendererId, float texWidth, float texHeight, float previewSize = 96.0f);
	std::string Ellipsize(const std::string& text, float maxWidth, bool* outTruncated = nullptr);
	void TextEllipsis(const std::string& text, float maxWidth = -1.0f);
	void TextDisabledEllipsis(const std::string& text, float maxWidth = -1.0f);
	bool SelectableEllipsis(const std::string& text, const char* id, bool selected = false,
		ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0.0f, 0.0f), float maxWidth = -1.0f);
	bool MenuItemEllipsis(const std::string& text, const char* id,
		const char* shortcut = nullptr, bool selected = false, bool enabled = true, float maxWidth = -1.0f);

	bool BeginComponentSection(const char* label, bool& removeRequested, const std::function<void()>& contextMenu = {});

	void EndComponentSection();

	// ── Multi-edit primitives ─────────────────────────────────────────
	//
	// All MultiEdit widgets sample the field across every entity in `entities`.
	// If all entities give the same value the widget renders normally; if any
	// channel differs it renders in a "mixed" state showing "—" (or an
	// indeterminate look for booleans / combos / strings). Editing always
	// writes through to ALL selected entities — for vectors and colors the
	// per-channel write mask matches the widget so editing only Y on a vec3
	// leaves X and Z untouched on every entity.
	//
	// All edits go through the caller-supplied setter, which is the same
	// mutation path single-entity edits use today (direct assignment or
	// component setter, plus scene.MarkDirty() at the inspector level).

	namespace MultiEdit {

		// Sample a scalar across all entities. Returns true if all entities
		// give the same value; false if mixed. `out` is set to the first
		// entity's value either way.
		template <typename T, typename Getter>
		bool SampleUniform(std::span<const Entity> entities, Getter&& get, T& out) {
			if (entities.empty()) {
				out = T{};
				return true;
			}
			out = get(entities[0]);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				T v = get(entities[i]);
				if (!(v == out)) {
					return false;
				}
			}
			return true;
		}

		// Sample N channels across all entities. `getChan(entity, channelIdx) -> float`
		// is invoked once per (entity, channel). `outValues[c]` receives the
		// first entity's channel value; `mixedMask[c]` is true if any other
		// entity's channel differs.
		template <std::size_t N, typename ChannelGet>
		void SamplePerChannel(std::span<const Entity> entities, ChannelGet&& getChan,
			float (&outValues)[N], std::array<bool, N>& mixedMask)
		{
			for (std::size_t c = 0; c < N; ++c) {
				mixedMask[c] = false;
			}
			if (entities.empty()) {
				for (std::size_t c = 0; c < N; ++c) outValues[c] = 0.0f;
				return;
			}
			for (std::size_t c = 0; c < N; ++c) {
				outValues[c] = getChan(entities[0], c);
			}
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (std::size_t c = 0; c < N; ++c) {
					if (!mixedMask[c]) {
						const float v = getChan(entities[i], c);
						if (v != outValues[c]) {
							mixedMask[c] = true;
						}
					}
				}
			}
		}

		// Layout for an N-channel inline row (replicates ImGui's internal
		// DragScalarN spacing: components separated by ItemInnerSpacing, last
		// component absorbs rounding error). Body runs per-channel and returns
		// true if it changed; the row returns true if any channel changed.
		template <typename Body>
		inline bool MultiItemRow(int components, Body&& body) {
			const ImGuiStyle& style = ImGui::GetStyle();
			const float fullWidth = ImGui::GetContentRegionAvail().x;
			const float spacing = style.ItemInnerSpacing.x;
			const float wItemOne = std::max(1.0f, std::floor(
				(fullWidth - spacing * static_cast<float>(components - 1)) / static_cast<float>(components)));
			const float wItemLast = std::max(1.0f, std::floor(
				fullWidth - (wItemOne + spacing) * static_cast<float>(components - 1)));
			bool any = false;
			for (int c = 0; c < components; ++c) {
				if (c > 0) ImGui::SameLine(0.0f, spacing);
				ImGui::SetNextItemWidth(c == components - 1 ? wItemLast : wItemOne);
				any = body(c) || any;
			}
			return any;
		}
	}

	// ── Scalar widgets ────────────────────────────────────────────────

	// `get(Entity) -> float`, `set(Entity, float)`. When mixed, displays "—"
	// but the underlying value is the first entity's value (drag math still
	// works). Edits propagate to ALL entities.
	template <typename Getter, typename Setter>
	bool DragFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float speed = 0.1f, float vmin = 0.0f, float vmax = 0.0f,
		const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragFloat("##Value", &v, speed, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool SliderFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float vmin, float vmax, const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::SliderFloat("##Value", &v, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool InputFloatMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float step = 0.0f, float stepFast = 0.0f, const char* format = "%.3f")
	{
		float v = 0.0f;
		const bool uniform = MultiEdit::SampleUniform<float>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::InputFloat("##Value", &v, step, stepFast, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool DragIntMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		float speed = 1.0f, int vmin = 0, int vmax = 0,
		const char* format = "%d")
	{
		int v = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragInt("##Value", &v, speed, vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename Getter, typename Setter>
	bool InputIntMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		int step = 1, int stepFast = 100)
	{
		int v = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		// InputInt has no format-string knob; when mixed we render via InputText
		// with a hint instead so the user sees "—" rather than "0".
		bool changed = false;
		if (uniform) {
			changed = ImGui::InputInt("##Value", &v, step, stepFast);
		}
		else {
			char buf[32] = "";
			const bool committed = ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf),
				ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
			if (committed && buf[0] != '\0') {
				v = std::atoi(buf);
				changed = true;
			}
		}
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	// Generic typed scalar — for int16, uint8, uint64, etc.
	template <typename T, typename Getter, typename Setter>
	bool DragScalarMulti(const char* label, std::span<const Entity> entities,
		ImGuiDataType dataType, Getter&& get, Setter&& set,
		float speed = 1.0f, const T* vmin = nullptr, const T* vmax = nullptr,
		const char* format = nullptr)
	{
		T v{};
		const bool uniform = MultiEdit::SampleUniform<T>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool changed = ImGui::DragScalar("##Value", dataType, &v, speed,
			vmin, vmax, uniform ? format : "-");
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	template <typename T, typename Getter, typename Setter>
	bool InputScalarMulti(const char* label, std::span<const Entity> entities,
		ImGuiDataType dataType, Getter&& get, Setter&& set,
		const char* format = nullptr)
	{
		T v{};
		const bool uniform = MultiEdit::SampleUniform<T>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		bool changed = false;
		if (uniform) {
			changed = ImGui::InputScalar("##Value", dataType, &v, nullptr, nullptr, format);
		}
		else {
			char buf[64] = "";
			const bool committed = ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf),
				ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
			if (committed && buf[0] != '\0') {
				if (dataType == ImGuiDataType_U64) {
					unsigned long long parsed = std::strtoull(buf, nullptr, 10);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				else if (dataType == ImGuiDataType_S64) {
					long long parsed = std::strtoll(buf, nullptr, 10);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				else {
					int parsed = std::atoi(buf);
					std::memcpy(&v, &parsed, sizeof(v) < sizeof(parsed) ? sizeof(v) : sizeof(parsed));
				}
				changed = true;
			}
		}
		if (changed) {
			for (const Entity& e : entities) set(e, v);
		}
		ImGui::PopID();
		return changed;
	}

	// Tri-state checkbox using ImGui's native MixedValue flag.
	template <typename Getter, typename Setter>
	bool CheckboxMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		bool v = false;
		const bool uniform = MultiEdit::SampleUniform<bool>(entities, get, v);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		// ImGuiItemFlags_MixedValue (1<<12) — declared in imgui_internal.h but
		// the value is part of the public ABI and used by Checkbox to draw the
		// indeterminate dash. Matches imgui_internal.h declaration.
		constexpr int kMixedValueFlag = 1 << 12;
		if (!uniform) {
			ImGui::PushItemFlag(static_cast<ImGuiItemFlags>(kMixedValueFlag), true);
		}
		bool tmp = v;
		const bool changed = ImGui::Checkbox("##Value", &tmp);
		if (!uniform) {
			ImGui::PopItemFlag();
		}
		if (changed) {
			for (const Entity& e : entities) set(e, tmp);
		}
		ImGui::PopID();
		return changed;
	}

	// String multi-edit. Buffer size is fixed at 256 chars to match the existing
	// NameComponent pattern. When mixed, the input shows "—" as a hint.
	template <typename Getter, typename Setter>
	bool InputTextMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		std::string sample;
		const bool uniform = MultiEdit::SampleUniform<std::string>(entities, get, sample);
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		char buf[256]{};
		if (uniform) {
			std::snprintf(buf, sizeof(buf), "%s", sample.c_str());
		}
		const bool changed = uniform
			? ImGui::InputText("##Value", buf, sizeof(buf))
			: ImGui::InputTextWithHint("##Value", "-", buf, sizeof(buf));
		if (changed) {
			std::string newValue(buf);
			for (const Entity& e : entities) set(e, newValue);
		}
		ImGui::PopID();
		return changed;
	}

	// Per-channel float vector multi-edit. Renders `componentCount` adjacent
	// drag widgets with separate mixed flags. `getChan(entity, channelIdx)`
	// reads the channel value; `setChan(entity, channelIdx, newValue)` writes
	// it on edit. Only channels the user actually touched are written, so
	// editing Y on a vec3 leaves X and Z alone on every selected entity.
	template <typename ChannelGet, typename ChannelSet>
	bool DragFloatNMulti(const char* label, std::span<const Entity> entities,
		int componentCount, ChannelGet&& getChan, ChannelSet&& setChan,
		float speed = 0.1f, float vmin = 0.0f, float vmax = 0.0f,
		const char* format = "%.3f")
	{
		if (componentCount < 1 || componentCount > 4) return false;

		float values[4] = {};
		std::array<bool, 4> mixedMask{};
		// Sample the up-to-4 channels by hand; SamplePerChannel<N> needs a
		// compile-time N so we just inline the loop here.
		for (int c = 0; c < componentCount; ++c) mixedMask[c] = false;
		if (!entities.empty()) {
			for (int c = 0; c < componentCount; ++c) values[c] = getChan(entities[0], c);
			for (std::size_t i = 1; i < entities.size(); ++i) {
				for (int c = 0; c < componentCount; ++c) {
					if (!mixedMask[c]) {
						const float v = getChan(entities[i], c);
						if (v != values[c]) mixedMask[c] = true;
					}
				}
			}
		}

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		const bool anyChanged = MultiEdit::MultiItemRow(componentCount, [&](int c) -> bool {
			ImGui::PushID(c);
			float channelValue = values[c];
			const char* fmt = mixedMask[c] ? "-" : format;
			const bool changed = ImGui::DragFloat("##c", &channelValue, speed, vmin, vmax, fmt);
			if (changed) {
				for (const Entity& e : entities) setChan(e, c, channelValue);
			}
			ImGui::PopID();
			return changed;
		});
		ImGui::PopID();
		return anyChanged;
	}

	// 4-channel color multi-edit, per-channel mixed-state.
	// `getChan(entity, channelIdx)` reads R/G/B/A in [0..1].
	// `setChan(entity, channelIdx, newValue)` writes the channel.
	template <typename ChannelGet, typename ChannelSet>
	bool ColorEdit4Multi(const char* label, std::span<const Entity> entities,
		ChannelGet&& getChan, ChannelSet&& setChan,
		ImGuiColorEditFlags flags = 0)
	{
		float values[4] = {};
		std::array<bool, 4> mixedMask{};
		MultiEdit::SamplePerChannel<4>(entities, getChan, values, mixedMask);
		const bool anyMixed = mixedMask[0] || mixedMask[1] || mixedMask[2] || mixedMask[3];

		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		bool anyChanged = false;
		if (!anyMixed) {
			float editVals[4] = { values[0], values[1], values[2], values[3] };
			if (ImGui::ColorEdit4("##Value", editVals, flags)) {
				for (int c = 0; c < 4; ++c) {
					if (editVals[c] != values[c]) {
						const float nv = editVals[c];
						for (const Entity& e : entities) setChan(e, c, nv);
						anyChanged = true;
					}
				}
			}
		}
		else {
			// Per-channel drag fallback when any channel differs across the
			// selection — ColorEdit4 cannot show per-channel "mixed" state.
			anyChanged = MultiEdit::MultiItemRow(4, [&](int c) -> bool {
				ImGui::PushID(c);
				float channelValue = values[c];
				const char* fmt = mixedMask[c] ? "-" : "%.3f";
				const bool changed = ImGui::DragFloat("##c", &channelValue, 0.005f, 0.0f, 1.0f, fmt);
				if (changed) {
					for (const Entity& e : entities) setChan(e, c, channelValue);
				}
				ImGui::PopID();
				return changed;
			});
		}
		ImGui::PopID();
		return anyChanged;
	}

	// Enum combo multi-edit. When mixed, the preview text is "—"; opening the
	// combo and picking a value writes it to all selected entities.
	template <typename TEnum, typename Getter, typename Setter>
	bool EnumComboMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set)
	{
		static_assert(std::is_enum_v<TEnum>, "EnumComboMulti requires an enum type.");

		TEnum sampled{};
		const bool uniform = MultiEdit::SampleUniform<TEnum>(entities, get, sampled);

		const char* preview;
		std::string previewBuf;
		if (uniform) {
			auto view = magic_enum::enum_name(sampled);
			previewBuf.assign(view.data(), view.size());
			preview = previewBuf.empty() ? "Unknown" : previewBuf.c_str();
		}
		else {
			preview = "-";
		}

		bool changed = false;
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (TEnum value : magic_enum::enum_values<TEnum>()) {
				auto name = magic_enum::enum_name(value);
				if (name.empty()) continue;
				const bool isSelected = uniform && (sampled == value);
				if (ImGui::Selectable(std::string(name).c_str(), isSelected)) {
					for (const Entity& e : entities) set(e, value);
					changed = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return changed;
	}

	// Combo backed by a name array (for non-magic_enum cases). When mixed,
	// preview shows "—".
	template <typename Getter, typename Setter>
	bool ComboMulti(const char* label, std::span<const Entity> entities,
		Getter&& get, Setter&& set,
		const char* const* itemNames, int itemCount)
	{
		int sampled = 0;
		const bool uniform = MultiEdit::SampleUniform<int>(entities, get, sampled);
		const char* preview = "-";
		if (uniform && sampled >= 0 && sampled < itemCount) {
			preview = itemNames[sampled];
		}
		bool changed = false;
		ImGui::PushID(label);
		BeginInspectorFieldRow(label);
		if (ImGui::BeginCombo("##Value", preview)) {
			for (int i = 0; i < itemCount; ++i) {
				const bool isSelected = uniform && (sampled == i);
				if (ImGui::Selectable(itemNames[i], isSelected)) {
					for (const Entity& e : entities) set(e, i);
					changed = true;
				}
				if (isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return changed;
	}
}
