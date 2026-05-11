#include "ImGuiContextLayer.hpp"

#include "Core/Application.hpp"
#include "Core/Assert.hpp"
#include "Core/Window.hpp"
#include "Events/AxiomEvent.hpp"
#include "Packages/PackageImGuiBridge.hpp"
#include "Serialization/Path.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
// ImGui backend lives inside Axiom-Engine.dll
// (Axiom-Engine/src/Gui/ImGuiImplWebGPU.{hpp,cpp}). The editor previously
// static-linked imgui_impl_opengl3, but the engine's window has no GL
// context (GLFW_NO_API) so that path can't init. Going through engine.dll's
// AXIOM_API exports keeps editor and engine sharing one wgpu::Device.
#include "Gui/ImGuiImplWebGPU.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

namespace Axiom {
	namespace {
		constexpr const char* k_ImGuiIniFileName = "imgui.ini";

		// Set by Reset/Load preset paths; consumed at the very start
		// of the next OnPreRender, BEFORE NewFrame. ImGui's mid-frame
		// ClearIniSettings + LoadIniSettingsFromDisk fights the dock
		// context: dock nodes touched earlier in the frame (notably
		// the dockspace's own root, which was bound during
		// RenderDockspaceRoot) don't get properly re-attached when
		// the new dock tree is built, leaving every dockable window
		// visually floating. Deferring the reload to the start of
		// the next frame side-steps that — by then EndFrame has
		// torn down per-frame dock state, so Clear + Load gets a
		// clean slate to apply the tree into and Begin() picks up
		// the new DockId on each window's first call this frame.
		std::string s_PendingLayoutReloadPath;

		// Defensive DockId sync. ImGui's docking branch has a state
		// divergence we've reproduced: window->DockNode can stay
		// pointing at a live dock node while window->DockId gets
		// cleared to 0 (we've observed it specifically for Inspector
		// / Settings / Debug Info, which are submitted from overlay
		// layers running after the dockspace's host window; suspect
		// the path is one of BeginDocked's undock branches that
		// clears DockId without nulling DockNode in this particular
		// submission order). WindowSettingsHandler_WriteAll reads
		// window->DockId verbatim, so in the diverged state the ini
		// gets a [Window] section with no DockId= line — the next
		// launch loads it as floating. Force the two back into sync
		// from the live DockNode pointer right before each save.
		void SyncDockIdsFromLiveNodes() {
			ImGuiContext* ctx = ImGui::GetCurrentContext();
			if (ctx == nullptr) return;
			for (int n = 0; n < ctx->Windows.Size; n++) {
				ImGuiWindow* window = ctx->Windows[n];
				if (window->DockNode != nullptr &&
					window->DockId != window->DockNode->ID) {
					window->DockId = window->DockNode->ID;
				}
			}
		}

		std::filesystem::path GetCanonicalFileIfExists(const std::filesystem::path& path) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) || ec) {
				return {};
			}

			std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
			return ec ? path : canonicalPath;
		}

		std::filesystem::path FindDefaultEditorIniFile() {
			std::filesystem::path executableDir;
			std::error_code ec;

			try {
				executableDir = Path::ExecutableDir();
			} catch (...) {
				executableDir.clear();
			}

			const std::filesystem::path currentDir = std::filesystem::current_path(ec);
			const std::array<std::filesystem::path, 4> candidates = {
				currentDir / "Axiom-Editor" / k_ImGuiIniFileName,
				executableDir / ".." / ".." / ".." / "Axiom-Editor" / k_ImGuiIniFileName,
				currentDir / k_ImGuiIniFileName,
				executableDir / k_ImGuiIniFileName
			};

			for (const std::filesystem::path& candidate : candidates) {
				std::filesystem::path defaultIniFile = GetCanonicalFileIfExists(candidate);
				if (!defaultIniFile.empty()) {
					return defaultIniFile;
				}
			}

			return {};
		}

		std::filesystem::path GetEditorUserIniFilePath() {
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData)) /
					"Axiom" / "Editor" / k_ImGuiIniFileName;
			} catch (...) {
				return std::filesystem::path(k_ImGuiIniFileName);
			}
		}

		std::string ResolveEditorIniFilePath() {
			std::filesystem::path userIniFile = GetEditorUserIniFilePath();
			std::error_code ec;

			if (!userIniFile.parent_path().empty()) {
				std::filesystem::create_directories(userIniFile.parent_path(), ec);
			}

			if (!std::filesystem::is_regular_file(userIniFile, ec)) {
				if (std::filesystem::path defaultIniFile = FindDefaultEditorIniFile(); !defaultIniFile.empty()) {
					ec.clear();
					std::filesystem::copy_file(defaultIniFile, userIniFile, std::filesystem::copy_options::none, ec);
				}
			}

			return userIniFile.make_preferred().string();
		}

		std::filesystem::path GetLayoutPresetsDirectory() {
			// Co-located with the live imgui.ini so a user nuking
			// %LOCALAPPDATA%\Axiom\Editor for a clean-slate test wipes
			// presets in the same gesture (matches user expectation:
			// "I reset the editor → all my layout state went with it").
			try {
				return std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
					/ "Axiom" / "Editor" / "Layouts";
			} catch (...) {
				return std::filesystem::path("Layouts");
			}
		}

		std::filesystem::path GetLayoutPresetPath(const std::string& name) {
			return GetLayoutPresetsDirectory() / (name + ".ini");
		}
	}

	void ImGuiContextLayer::OnAttach(Application& app) {
		if (m_IsInitialized) {
			return;
		}

		Window* window = app.GetWindow();
		AIM_ASSERT(window != nullptr, AxiomErrorCode::InvalidHandle, "ImGuiContextLayer requires an active Window");
		GLFWwindow* glfwWindow = window->GetGLFWWindow();
		AIM_ASSERT(glfwWindow != nullptr, AxiomErrorCode::InvalidHandle, "Window has no GLFW handle");

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		// Publish our ImGui context + allocator to package DLLs. Without
		// this, any package that links ImGui statically (every engine_core
		// package today) gets its own null GImGui and crashes on the first
		// inspector ImGui call. Packages pick this up lazily on first use
		// via PackageImGuiBridge::GetContext.
		{
			ImGuiMemAllocFunc allocFn = nullptr;
			ImGuiMemFreeFunc  freeFn = nullptr;
			void* userData = nullptr;
			ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
			PackageImGuiBridge::Publish(
				reinterpret_cast<void*>(ImGui::GetCurrentContext()),
				reinterpret_cast<void*>(allocFn),
				reinterpret_cast<void*>(freeFn),
				userData);
		}

		ImGuiIO& io = ImGui::GetIO();
		m_IniFilePath = ResolveEditorIniFilePath();
		io.IniFilename = m_IniFilePath.c_str();
		// Reduce ImGui's lazy-save timer from 5s → 1s. With the long
		// timer a user who docked / resized / closed a window inside
		// the last 5 seconds and then rage-quit (kill from Task
		// Manager, BSOD, segfault during a hot-reload script,
		// frame-loop exception, …) loses the layout because the
		// auto-save timer never expired and the OnDetach explicit
		// save never ran. 1s is short enough that the dirty window
		// is small without thrashing the disk on every drag pixel
		// (ImGui only re-evaluates layout state on widget completion,
		// not on every mouse move).
		io.IniSavingRate = 1.0f;
		// Enable docking BEFORE LoadIniSettingsFromDisk. The ini's
		// [Docking][Data] section parses its dock-node tree through
		// the SettingsHandler that's only registered when the docking
		// config flag is set. Loading first leaves every window's
		// saved DockId orphaned — the windows fall back to floating
		// placement and the next save scrubs the [Docking][Data]
		// section, silently destroying the persisted layout. This
		// must run before the load below; the rest of the docking-
		// adjacent config (resize grips, etc.) can stay where it is.
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// Belt-and-suspenders load. ImGui's NewFrame would auto-load
		// on the first frame anyway, but doing it explicitly here
		// (a) makes it easy to confirm via the log whether the file
		// was found at the resolved path and (b) means any code path
		// that touches ImGui state BEFORE the first NewFrame (e.g.
		// PackageImGuiBridge plumbing) operates against the loaded
		// settings rather than ImGui's compiled-in defaults.
		std::error_code loadEc;
		const bool iniFileExists = std::filesystem::is_regular_file(m_IniFilePath, loadEc);
		const std::uintmax_t iniFileSize = iniFileExists
			? std::filesystem::file_size(m_IniFilePath, loadEc)
			: 0u;
		if (iniFileExists) {
			ImGui::LoadIniSettingsFromDisk(m_IniFilePath.c_str());
		}
		AIM_CORE_INFO_TAG("ImGui",
			"Editor layout file: {} ({}, {} bytes)",
			m_IniFilePath,
			iniFileExists ? "loaded" : "missing — defaults will be used",
			iniFileSize);
		// Edge-resize left at default (true). Forcing it false combined with the
		// transparent ResizeGrip colors below made undocked floating windows
		// effectively non-resizable.

		// One-time HiDPI scale captured from the window's monitor. Applied below
		// to the default font and to the theme via ScaleAllSizes. Mid-session
		// monitor moves are not handled — wire glfwSetWindowContentScaleCallback
		// if that becomes a real symptom.
		float xScale = 1.0f, yScale = 1.0f;
		glfwGetWindowContentScale(glfwWindow, &xScale, &yScale);
		const float dpiScale = std::max(1.0f, xScale);

		ImFontConfig fontCfg;
		fontCfg.SizePixels = 13.0f * dpiScale;
		io.Fonts->AddFontDefault(&fontCfg);

		// Merge a Latin-1 supplement glyph fallback onto the default font.
		// ImGui's bundled ProggyClean only ships ASCII glyphs, so anything
		// 0xA0-0xFF (German umlauts, French/Spanish accents, ©/®/°/±/etc.)
		// renders as '?'. We additionally point AddFontFromFileTTF at a
		// real TTF with Latin-1 coverage and merge ONLY that range so the
		// editor font otherwise looks identical to before. NotoSans-
		// Regular.ttf is the engine-bundled fallback (large character set,
		// SIL OFL).
		const std::string notoPath = Path::Combine(Path::ResolveAxiomAssets("Fonts"), "NotoSans-Regular.ttf");
		if (std::filesystem::exists(notoPath)) {
			ImFontConfig latinCfg;
			latinCfg.MergeMode = true;
			latinCfg.SizePixels = 13.0f * dpiScale;
			latinCfg.PixelSnapH = true;
			static const ImWchar latin1Range[] = { 0x00A0, 0x00FF, 0 };
			io.Fonts->AddFontFromFileTTF(notoPath.c_str(),
				13.0f * dpiScale, &latinCfg, latin1Range);
		}

		// GLFW_NO_API means there's no GL context; use ImGui's "Other"
		// GLFW init that doesn't bind one.
		AIM_VERIFY(ImGui_ImplGlfw_InitForOther(glfwWindow, true),
			"Failed to init glfw for imgui (WebGPU backend)!");
		AIM_VERIFY(ImGuiImplWebGPU::Init(),
			"Failed to init WebGPU imgui backend (device not ready?)");

		ApplyAxiomTheme();
		// Must run after the theme — ScaleAllSizes is multiplicative on the
		// current style values.
		ImGui::GetStyle().ScaleAllSizes(dpiScale);

		m_IsInitialized = true;
	}

	void ImGuiContextLayer::OnDetach(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}

		// Flush pending layout changes to imgui.ini BEFORE DestroyContext.
		// ImGui's auto-save is timer-driven (`g.SettingsDirtyTimer`
		// decays at IniSavingRate, lowered to 1s in OnAttach above);
		// a user who docks a panel and immediately closes the editor
		// would otherwise lose that change because the timer never
		// reached 0. Calling SaveIniSettingsToDisk explicitly drains
		// the dirty flag regardless of timer state. Skipped when
		// there's no IniFilename or the path is empty. We also
		// confirm the file appeared on disk + log the path so a "I
		// quit and the layout reset" report has actionable evidence
		// (the path the editor wrote to + whether the write actually
		// produced a file).
		const ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename != nullptr && *io.IniFilename != '\0') {
			SyncDockIdsFromLiveNodes();
			ImGui::SaveIniSettingsToDisk(io.IniFilename);
			std::error_code ec;
			const bool exists = std::filesystem::is_regular_file(io.IniFilename, ec);
			AIM_CORE_INFO_TAG("ImGui", "Saved editor layout to {} ({})",
				io.IniFilename,
				exists ? "ok" : "WRITE FAILED — path not writable?");
		}

		ImGuiImplWebGPU::Shutdown();
		ImGui_ImplGlfw_Shutdown();
		PackageImGuiBridge::Clear();
		ImGui::DestroyContext();
		m_IniFilePath.clear();

		m_IsInitialized = false;
	}

	void ImGuiContextLayer::OnPreRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}

		// Consume pending Reset / Load-preset request BEFORE NewFrame.
		// The previous frame's EndFrame has already torn down per-
		// frame dock state, so ClearIniSettings + LoadIniSettings here
		// gets to populate the dock context cleanly; the panels that
		// Begin() later in this frame then pick up their new DockId
		// on first use and attach into the freshly-built tree.
		if (!s_PendingLayoutReloadPath.empty()) {
			const std::string path = std::move(s_PendingLayoutReloadPath);
			s_PendingLayoutReloadPath.clear();
			ImGui::ClearIniSettings();
			ImGui::LoadIniSettingsFromDisk(path.c_str());
			// LoadIniSettingsFromDisk silently flips WantSaveIniSettings
			// off; suppress the next periodic-save trigger so it doesn't
			// immediately re-save the freshly-loaded state (which would
			// also reset our save-timer baseline to "right now," masking
			// real subsequent dirty events).
			ImGui::GetIO().WantSaveIniSettings = false;
			m_LastSaveTime = std::chrono::steady_clock::now();
			AIM_CORE_INFO_TAG("ImGui",
				"Applied deferred layout reload from '{}'.", path);
		}

		ImGuiImplWebGPU::NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiContextLayer::OnPostRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
		ImGui::Render();
		// viewId is vestigial on WebGPU (preserved in the API signature
		// for ABI stability). ImGuiImplWebGPU::RenderDrawData uses
		// whatever framebuffer RenderApi::BindFramebuffer last bound.
		ImGuiImplWebGPU::RenderDrawData(ImGui::GetDrawData(), /*viewId*/ 0xFFFFu);

		// Belt-and-suspenders settings flush — runs after ImGui::Render
		// has finalised the frame's settings state, so any dock split,
		// window close, or layout mutation issued during this frame is
		// guaranteed visible to SaveIniSettingsToDisk.
		FlushSettingsIfDirtyOrPeriodic();
	}

	void ImGuiContextLayer::FlushSettingsIfDirtyOrPeriodic() {
		// 5 second cap between forced saves. Tight enough that a hard
		// quit (process kill, BSOD) loses at most a few seconds of
		// layout work; slow enough that the editor isn't writing the
		// same bytes hundreds of times per session under steady-state
		// (no layout edits happening).
		constexpr float k_PeriodicSaveSeconds = 5.0f;

		if (!m_IsInitialized) return;
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') return;

		const auto now = std::chrono::steady_clock::now();
		const float secondsSinceSave = std::chrono::duration<float>(
			now - m_LastSaveTime).count();

		// `WantSaveIniSettings` flips true when ImGui has a dirty
		// settings state that its internal auto-save couldn't flush
		// for some reason (e.g. a late-frame mutation past the timer
		// expiry). The periodic save catches the rest — anything
		// that ImGui's MarkSettingsDirty path missed because the
		// caller didn't trip a known-dirty hook.
		const bool shouldSave = io.WantSaveIniSettings
			|| secondsSinceSave >= k_PeriodicSaveSeconds;
		if (!shouldSave) return;

		SyncDockIdsFromLiveNodes();
		ImGui::SaveIniSettingsToDisk(io.IniFilename);
		io.WantSaveIniSettings = false;
		m_LastSaveTime = now;
	}

	void ImGuiContextLayer::OnEvent(Application& app, AxiomEvent& event) {
		(void)app;
		if (!m_IsInitialized) return;
		// Save on focus loss (Alt-Tab away, click into a different
		// app). The user typically iterates layout → switch to docs /
		// IDE / Photoshop → back, and a session that crashes or is
		// killed in that "background" interval would lose the most
		// recent layout work without this. Triggered via the event
		// system (registered as a Layer::OnEvent) so we don't have to
		// poll glfwGetWindowAttrib(GLFW_FOCUSED) every frame.
		// Also save on the X-button close: the WindowCloseEvent runs
		// before the layer stack tears down, but a layout mutation
		// made within the past tick (lazy-save still ticking) would
		// otherwise wait for OnDetach. Saving here closes that gap if
		// any subsequent shutdown step throws or aborts.
		const EventType type = event.GetEventType();
		if (type == EventType::WindowLostFocus || type == EventType::WindowClose) {
			ImGuiIO& io = ImGui::GetIO();
			if (io.IniFilename != nullptr && *io.IniFilename != '\0') {
				SyncDockIdsFromLiveNodes();
				ImGui::SaveIniSettingsToDisk(io.IniFilename);
				io.WantSaveIniSettings = false;
				m_LastSaveTime = std::chrono::steady_clock::now();
			}
		}
	}

	bool ImGuiContextLayer::IsValidLayoutPresetName(const std::string& name) {
		if (name.empty() || name.size() > 64) return false;
		if (name == "." || name == "..") return false;
		// Reject every char that's illegal in a Windows filename OR
		// would let the name escape its directory. We're stricter than
		// strictly necessary on POSIX so a preset authored on Windows
		// keeps the same name verbatim if the user ever moves their
		// %LOCALAPPDATA% snapshot to a Linux build.
		for (char c : name) {
			if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
				c == '"' || c == '<' || c == '>' || c == '|') {
				return false;
			}
			// Reject control chars; they're never typed deliberately and
			// produce filenames that can't be displayed in the menu.
			if (static_cast<unsigned char>(c) < 0x20) return false;
		}
		// Trailing dot / space → Windows silently strips on save, then
		// the next ListLayoutPresets() won't find the file the user
		// just "saved". Easier to refuse up front.
		if (name.back() == ' ' || name.back() == '.') return false;
		return true;
	}

	std::vector<std::string> ImGuiContextLayer::ListLayoutPresets() {
		std::vector<std::string> result;
		const std::filesystem::path dir = GetLayoutPresetsDirectory();
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) {
			return result;
		}
		for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
			const std::filesystem::directory_entry& entry = *it;
			std::error_code ec2;
			if (!entry.is_regular_file(ec2) || ec2) continue;
			const std::filesystem::path& p = entry.path();
			if (p.extension() != ".ini") continue;
			result.push_back(p.stem().string());
		}
		std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
			// Case-insensitive sort so "MyLayout" and "mylayout" land
			// next to each other in the menu rather than splitting by
			// ASCII codepoint.
			return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
				[](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
		});
		return result;
	}

	bool ImGuiContextLayer::SaveLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			AIM_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path dir = GetLayoutPresetsDirectory();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			AIM_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: failed to create presets directory '{}': {}",
				dir.string(), ec.message());
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		ImGui::SaveIniSettingsToDisk(presetPath.string().c_str());
		ec.clear();
		const bool exists = std::filesystem::is_regular_file(presetPath, ec);
		if (!exists) {
			AIM_CORE_WARN_TAG("ImGui",
				"SaveLayoutPreset: write failed (path not writable?): '{}'",
				presetPath.string());
			return false;
		}
		AIM_CORE_INFO_TAG("ImGui", "Saved layout preset '{}' → {}",
			name, presetPath.string());
		return true;
	}

	bool ImGuiContextLayer::LoadLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			AIM_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		std::error_code ec;
		if (!std::filesystem::is_regular_file(presetPath, ec)) {
			AIM_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: preset file not found at '{}'.",
				presetPath.string());
			return false;
		}
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') {
			AIM_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: no IniFilename set; ignoring.");
			return false;
		}
		// Persist the preset content to the user's imgui.ini path
		// immediately (so a hard quit between this call and the
		// deferred reload doesn't lose the choice), then queue the
		// in-context reload for the start of the next frame. See the
		// long comment on s_PendingLayoutReloadPath for the timing
		// reason — calling ClearIniSettings + LoadIniSettingsFromDisk
		// mid-frame leaves dockable windows floating.
		std::filesystem::copy_file(presetPath, io.IniFilename,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			AIM_CORE_WARN_TAG("ImGui",
				"LoadLayoutPreset: failed to copy preset to user ini: {}",
				ec.message());
			return false;
		}
		s_PendingLayoutReloadPath = io.IniFilename;
		AIM_CORE_INFO_TAG("ImGui",
			"Queued layout preset '{}' for reload from {}",
			name, presetPath.string());
		return true;
	}

	bool ImGuiContextLayer::DeleteLayoutPreset(const std::string& name) {
		if (!IsValidLayoutPresetName(name)) {
			AIM_CORE_WARN_TAG("ImGui",
				"DeleteLayoutPreset: rejected invalid preset name '{}'.", name);
			return false;
		}
		const std::filesystem::path presetPath = GetLayoutPresetPath(name);
		std::error_code ec;
		const bool removed = std::filesystem::remove(presetPath, ec) && !ec;
		if (!removed) {
			AIM_CORE_WARN_TAG("ImGui",
				"DeleteLayoutPreset: remove failed for '{}': {}",
				presetPath.string(), ec ? ec.message() : "file did not exist");
			return false;
		}
		AIM_CORE_INFO_TAG("ImGui", "Deleted layout preset '{}' ({})",
			name, presetPath.string());
		return true;
	}

	void ImGuiContextLayer::ResetLayoutToBundledDefault() {
		ImGuiIO& io = ImGui::GetIO();
		if (io.IniFilename == nullptr || *io.IniFilename == '\0') {
			AIM_CORE_WARN_TAG("ImGui",
				"Reset Layout requested but no IniFilename is set; ignoring.");
			return;
		}

		std::filesystem::path defaultIniFile = FindDefaultEditorIniFile();
		if (defaultIniFile.empty()) {
			AIM_CORE_WARN_TAG("ImGui",
				"Reset Layout: bundled default imgui.ini not found in any "
				"candidate path. Layout left unchanged.");
			return;
		}

		// Copy bundled → user ini eagerly so the new layout survives a
		// crash between this call and the deferred reload, then queue
		// the in-context reload. See s_PendingLayoutReloadPath for why
		// mid-frame ClearIniSettings + LoadIniSettingsFromDisk produced
		// the "everything floats" symptom.
		std::error_code ec;
		std::filesystem::copy_file(defaultIniFile, io.IniFilename,
			std::filesystem::copy_options::overwrite_existing, ec);
		if (ec) {
			AIM_CORE_WARN_TAG("ImGui",
				"Reset Layout: failed to copy bundled default to user ini: {}",
				ec.message());
			return;
		}
		s_PendingLayoutReloadPath = io.IniFilename;

		AIM_CORE_INFO_TAG("ImGui",
			"Queued reset of editor layout from bundled default '{}' → '{}'",
			defaultIniFile.string(),
			io.IniFilename);
	}

	void ImGuiContextLayer::ApplyAxiomTheme() {
		ImGuiStyle& style = ImGui::GetStyle();

		// ── Sizing & Rounding ───────────────────────────────────────
		style.WindowPadding     = ImVec2(10, 10);
		style.FramePadding      = ImVec2(6, 4);
		style.CellPadding       = ImVec2(4, 3);
		style.ItemSpacing       = ImVec2(8, 5);
		style.ItemInnerSpacing  = ImVec2(5, 4);
		style.IndentSpacing     = 16.0f;
		style.ScrollbarSize     = 13.0f;
		style.GrabMinSize       = 8.0f;

		style.WindowRounding    = 4.0f;
		style.ChildRounding     = 3.0f;
		style.FrameRounding     = 3.0f;
		style.PopupRounding     = 3.0f;
		style.ScrollbarRounding = 6.0f;
		style.GrabRounding      = 2.0f;
		style.TabRounding       = 3.0f;

		style.WindowBorderSize  = 1.0f;
		style.FrameBorderSize   = 0.0f;
		style.PopupBorderSize   = 1.0f;
		style.TabBorderSize     = 0.0f;

		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.SeparatorTextBorderSize  = 2.0f;

		// ── Colors ──────────────────────────────────────────────────
		ImVec4* c = style.Colors;

		const ImVec4 bg        = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
		const ImVec4 bgChild   = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
		const ImVec4 bgPopup   = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		const ImVec4 surface   = ImVec4(0.18f, 0.18f, 0.21f, 1.00f);
		const ImVec4 surfaceHi = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);
		const ImVec4 surfaceAct= ImVec4(0.28f, 0.28f, 0.33f, 1.00f);
		const ImVec4 border    = ImVec4(0.24f, 0.24f, 0.28f, 0.65f);
		const ImVec4 accent    = ImVec4(0.33f, 0.53f, 0.84f, 1.00f);
		const ImVec4 text      = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
		const ImVec4 textDim   = ImVec4(0.50f, 0.50f, 0.54f, 1.00f);

		c[ImGuiCol_WindowBg]             = bg;
		c[ImGuiCol_ChildBg]              = bgChild;
		c[ImGuiCol_PopupBg]              = bgPopup;

		c[ImGuiCol_Border]               = border;
		c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Text]                 = text;
		c[ImGuiCol_TextDisabled]         = textDim;
		c[ImGuiCol_TextSelectedBg]       = ImVec4(accent.x, accent.y, accent.z, 0.35f);

		c[ImGuiCol_FrameBg]              = surface;
		c[ImGuiCol_FrameBgHovered]       = surfaceHi;
		c[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);

		c[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
		c[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
		c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.10f, 0.10f, 0.12f, 0.75f);

		c[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);

		c[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.10f, 0.12f, 0.53f);
		c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
		c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
		c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.42f, 0.48f, 1.00f);

		c[ImGuiCol_Button]               = surface;
		c[ImGuiCol_ButtonHovered]        = surfaceHi;
		c[ImGuiCol_ButtonActive]         = surfaceAct;

		c[ImGuiCol_Header]               = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
		c[ImGuiCol_HeaderHovered]        = surfaceHi;
		c[ImGuiCol_HeaderActive]         = surfaceAct;

		c[ImGuiCol_Separator]            = border;
		c[ImGuiCol_SeparatorHovered]     = ImVec4(0.36f, 0.36f, 0.42f, 1.00f);
		c[ImGuiCol_SeparatorActive]      = ImVec4(0.44f, 0.44f, 0.50f, 1.00f);

		c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripHovered]    = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ResizeGripActive]     = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_Tab]                  = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		c[ImGuiCol_TabHovered]           = surfaceHi;
		c[ImGuiCol_TabSelected]          = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
		c[ImGuiCol_TabSelectedOverline]  = accent;
		c[ImGuiCol_TabDimmed]            = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
		c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.17f, 0.17f, 0.20f, 1.00f);

		c[ImGuiCol_DockingPreview]       = ImVec4(accent.x, accent.y, accent.z, 0.40f);
		c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

		c[ImGuiCol_CheckMark]            = accent;
		c[ImGuiCol_SliderGrab]           = ImVec4(0.40f, 0.40f, 0.46f, 1.00f);
		c[ImGuiCol_SliderGrabActive]     = ImVec4(0.50f, 0.50f, 0.56f, 1.00f);

		c[ImGuiCol_DragDropTarget]       = ImVec4(accent.x, accent.y, accent.z, 0.70f);

		c[ImGuiCol_NavCursor]            = accent;

		c[ImGuiCol_TableHeaderBg]        = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
		c[ImGuiCol_TableBorderStrong]    = border;
		c[ImGuiCol_TableBorderLight]     = ImVec4(border.x, border.y, border.z, 0.40f);
		c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.03f);

		c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.55f);
	}

}
