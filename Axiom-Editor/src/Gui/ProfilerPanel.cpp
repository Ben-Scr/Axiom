#include "Gui/ProfilerPanel.hpp"

#include <imgui.h>

#ifdef AXIOM_PROFILER_ENABLED
#include "Profiling/Profiler.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"

#include <algorithm>
#include <cstdio>
#endif

namespace Axiom {

#ifdef AXIOM_PROFILER_ENABLED

	void ProfilerPanel::Initialize() {
		// Defaults are read from the project on first Render() — by the time
		// Initialize runs, the active project may not be loaded yet.
		m_SettingsLoaded = false;
	}

	void ProfilerPanel::Shutdown() {
		// Ensure modules stop collecting if the editor is closing while
		// the panel was visible.
		Profiler::SetPanelVisible(false);
	}

	void ProfilerPanel::LoadSettingsFromProject() {
		const AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) return;

		const auto& src = project->Profiler;
		m_SamplingHz = src.SamplingHz;
		m_TrackingSpan = src.TrackingSpan;
		m_TrackInBackground = src.TrackInBackground;

		Profiler::SetSamplingHz(m_SamplingHz);
		Profiler::SetTrackingSpan(m_TrackingSpan);
		Profiler::SetBackgroundTracking(m_TrackInBackground);

		// Apply per-module enable overrides. Modules not in the saved list
		// stay at their default (enabled) state, so newly-added engine
		// modules don't need a settings migration.
		for (const auto& [name, enabled] : src.ModuleEnabled) {
			Profiler::SetModuleEnabled(name, enabled);
		}
	}

	void ProfilerPanel::SaveSettingsToProject() {
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) return;

		auto& dst = project->Profiler;
		dst.SamplingHz = m_SamplingHz;
		dst.TrackingSpan = m_TrackingSpan;
		dst.TrackInBackground = m_TrackInBackground;

		// Snapshot per-module enabled state from the live profiler. Only
		// non-default (disabled) entries get persisted; default-enabled
		// modules stay implicit so the JSON file doesn't grow with every
		// new engine module.
		dst.ModuleEnabled.clear();
		for (const auto& m : Profiler::AllModules()) {
			if (!m.Enabled) {
				dst.ModuleEnabled.emplace_back(m.Name, false);
			}
		}

		project->Save();
	}

	namespace {
		// Per-module display label — name shown in the panel may differ from
		// the internal registration name (e.g. "Total Memory" registered ->
		// "Total Used Memory" displayed). Map captures both.
		struct ModuleDisplay {
			const char* RegisteredName;
			const char* DisplayLabel;
			const char* Unit; // "ms", "MB", or "" for counts
		};

		// Category layout. Order here is the panel's display order.
		struct Category {
			const char* Header;
			std::vector<ModuleDisplay> Modules;
		};

		const std::vector<Category>& GetCategoryLayout() {
			static const std::vector<Category> kLayout = {
				{
					"CPU Usage",
					{
						{ "Rendering", "Rendering",  "ms" },
						{ "Scripts",   "Scripts",    "ms" },
						{ "Physics",   "Physics",    "ms" },
						{ "VSync",     "VSync",      "ms" },
						{ "Others",    "Others",     "ms" },
					}
				},
				{
					"Rendering",
					{
						{ "Batches",   "Batches Count",   "" },
						{ "Triangles", "Triangles Count", "" },
						{ "Vertices",  "Vertices Count",  "" },
					}
				},
				{
					"Memory",
					{
						{ "Total Memory",   "Total Used Memory", "MB" },
						{ "Texture Memory", "Texture Memory",    "MB" },
						{ "Entity Count",   "Entity Count",      "" },
					}
				},
				{
					"Audio",
					{
						{ "Playing Sources", "Playing Audio Sources", "" },
					}
				}
			};
			return kLayout;
		}
	}

	void ProfilerPanel::RenderModuleRow(const std::string& moduleName) {
		// Public single-row entry point. Looks up the display info from
		// the category layout (so unit + label stay in sync with the
		// structured panel rendering). No-op for unknown names.
		for (const auto& cat : GetCategoryLayout()) {
			for (const auto& m : cat.Modules) {
				if (moduleName == m.RegisteredName) {
					RenderModuleRowImpl(m.RegisteredName, m.DisplayLabel, m.Unit);
					return;
				}
			}
		}
	}

	void ProfilerPanel::RenderModuleRowImpl(const char* registeredName,
		const char* displayLabel,
		const char* unit) {
		ProfilerModule* m = Profiler::Find(registeredName);
		if (!m) return;

		ImGui::PushID(registeredName);

		bool enabled = m->Enabled;
		if (ImGui::Checkbox("##enable", &enabled)) {
			Profiler::SetModuleEnabled(registeredName, enabled);
			SaveSettingsToProject();
		}
		ImGui::SameLine();

		// One-line label: "Display Label    Cur X UNIT    Avg Y    Min Z    Max W"
		// Counts (no unit) format as integers; ms/MB format with one decimal.
		const bool isCount = unit[0] == '\0';
		char valueLabel[256];
		if (isCount) {
			std::snprintf(valueLabel, sizeof(valueLabel),
				"%-22s  Cur %.0f    Avg %.0f    Min %.0f    Max %.0f",
				displayLabel,
				m->CurrentValue, m->AvgValue, m->MinValue, m->MaxValue);
		}
		else {
			std::snprintf(valueLabel, sizeof(valueLabel),
				"%-22s  Cur %.1f %s    Avg %.1f    Min %.1f    Max %.1f",
				displayLabel,
				m->CurrentValue, unit,
				m->AvgValue, m->MinValue, m->MaxValue);
		}
		ImGui::TextUnformatted(valueLabel);

		// Scrolling line graph. PlotLines accepts a head offset, so we don't
		// need to unwrap the ring buffer.
		if (m->Count > 0) {
			ImGui::PlotLines("##graph",
				m->Samples.data(),
				static_cast<int>(m->Count),
				static_cast<int>(m->Head + m->Samples.size() - m->Count) % static_cast<int>(m->Samples.size()),
				nullptr,
				FLT_MAX, FLT_MAX,
				ImVec2(-1.0f, 40.0f));
		}
		else {
			ImGui::Dummy(ImVec2(-1.0f, 40.0f));
		}

		ImGui::Separator();
		ImGui::PopID();
	}

	void ProfilerPanel::Render(bool* pOpen) {
		if (pOpen && !*pOpen) {
			// Panel is hidden — make sure collection drops out.
			Profiler::SetPanelVisible(false);
			return;
		}

		ImGui::SetNextWindowSize(ImVec2(600, 600), ImGuiCond_FirstUseEver);
		const bool windowOpen = ImGui::Begin("Profiler", pOpen);
		// `windowOpen == false` means the window is collapsed (still the
		// chrome is being drawn, but the body is skipped). Either case
		// means we don't want module collection running.
		Profiler::SetPanelVisible(windowOpen);

		if (!windowOpen) {
			ImGui::End();
			return;
		}

		// First render after open: hydrate state from the project.
		if (!m_SettingsLoaded) {
			LoadSettingsFromProject();
			m_SettingsLoaded = true;
		}

		// Header — controls that apply to all modules.
		bool dirty = false;
		if (ImGui::Checkbox("Track in Background", &m_TrackInBackground)) {
			Profiler::SetBackgroundTracking(m_TrackInBackground);
			dirty = true;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80.0f);
		if (ImGui::InputInt("Sampling Hz", &m_SamplingHz, 0, 0)) {
			m_SamplingHz = std::max(1, m_SamplingHz);
			Profiler::SetSamplingHz(m_SamplingHz);
			dirty = true;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(100.0f);
		if (ImGui::InputInt("Tracking Span", &m_TrackingSpan, 0, 0)) {
			m_TrackingSpan = std::max(8, m_TrackingSpan);
			Profiler::SetTrackingSpan(m_TrackingSpan);
			dirty = true;
		}
		if (dirty) {
			SaveSettingsToProject();
		}

		ImGui::Separator();

		// Body — four collapsible category headers, each grouping its modules
		// from the static layout map. Header state defaults to OPEN on first
		// render; ImGui persists the user's collapsed/expanded preference via
		// imgui.ini between sessions automatically.
		for (const auto& category : GetCategoryLayout()) {
			ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
			if (ImGui::CollapsingHeader(category.Header)) {
				ImGui::Indent();
				for (const auto& mod : category.Modules) {
					RenderModuleRowImpl(mod.RegisteredName, mod.DisplayLabel, mod.Unit);
				}
				ImGui::Unindent();
			}
		}

		ImGui::End();
	}

#else // !AXIOM_PROFILER_ENABLED — stub so menu item still resolves

	void ProfilerPanel::Initialize() {}
	void ProfilerPanel::Shutdown()   {}
	void ProfilerPanel::RenderModuleRow(const std::string&) {}
	void ProfilerPanel::LoadSettingsFromProject() {}
	void ProfilerPanel::SaveSettingsToProject()   {}

	void ProfilerPanel::Render(bool* pOpen) {
		if (pOpen && !*pOpen) return;
		ImGui::SetNextWindowSize(ImVec2(360, 90), ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Profiler", pOpen)) {
			ImGui::TextDisabled("Profiler is disabled in this build.");
			ImGui::TextDisabled("Rebuild without --no-profiler to enable.");
		}
		ImGui::End();
	}

#endif

} // namespace Axiom
