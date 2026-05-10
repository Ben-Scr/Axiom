#include "RuntimeSplashLayer.hpp"
#include "RuntimeImGuiHost.hpp"

#include "Collections/Viewport.hpp"
#include "Core/Application.hpp"
#include "Core/Log.hpp"
#include "Core/Time.hpp"
#include "Core/Version.hpp"
#include "Core/Window.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <filesystem>
#include <imgui.h>
#include <string>

namespace Axiom {

	namespace {
		// Solid 0xAARRGGBB encoded for ImGui draw list.
		ImU32 PackColor(float r, float g, float b, float a) {
			r = std::clamp(r, 0.0f, 1.0f);
			g = std::clamp(g, 0.0f, 1.0f);
			b = std::clamp(b, 0.0f, 1.0f);
			a = std::clamp(a, 0.0f, 1.0f);
			return IM_COL32(
				static_cast<int>(r * 255.0f),
				static_cast<int>(g * 255.0f),
				static_cast<int>(b * 255.0f),
				static_cast<int>(a * 255.0f));
		}

		std::string DefaultBuildLine() {
			std::string profile;
#if defined(AXIOM_BUILD_RELEASE)
			profile = "Release";
#elif defined(AXIOM_BUILD_DEVELOPMENT)
			profile = "Development";
#else
			profile = "Development";
#endif
			std::string platform;
#if defined(AIM_PLATFORM_WINDOWS)
			platform = "Windows";
#elif defined(__APPLE__)
			platform = "macOS";
#else
			platform = "Linux";
#endif
			// Latin-1 supplement covers '·' (U+00B7) — the engine's text
			// renderer atlas now bakes 32-126 + 160-255. ImGui's default
			// font here renders the Latin-1 set since codepoints 160-255
			// fall inside its baked range too. Higher codepoints like
			// '•' (U+2022) still need the SDF/on-demand path.
			return std::string("Axiom ") + AIM_VERSION + "  ·  " + platform + "  ·  " + profile;
		}

		std::string ResolveDefaultLogoPath() {
			// The canonical engine logo lives at the root of
			// AxiomAssets/Textures as `icon.png` — same payload the
			// editor uses for the unset-AppIcon placeholder thumbnail,
			// so a project with no SplashScreen.ImagePath ships with
			// the same default look as its build's .exe icon.
			// `Axiom64.png` is kept as a back-compat fallback for older
			// engine installs that still carry the historical name.
			std::string root = Path::ResolveAxiomAssets("Textures");
			if (root.empty()) return {};
			std::filesystem::path candidate = std::filesystem::path(root) / "icon.png";
			if (std::filesystem::exists(candidate)) return candidate.string();
			candidate = std::filesystem::path(root) / "Axiom64.png";
			if (std::filesystem::exists(candidate)) return candidate.string();
			return {};
		}

		std::string ResolveCustomLogoPath(const std::string& projectRelative) {
			if (projectRelative.empty()) return {};
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (!project) return projectRelative;

			std::filesystem::path absolute(projectRelative);
			if (absolute.is_absolute() && std::filesystem::exists(absolute)) {
				return absolute.string();
			}
			// AppIconPath / SplashScreen.ImagePath are stored relative to the
			// project root (Assets/foo.png style). Try that first, then fall
			// back to the cwd next to the executable.
			std::filesystem::path projectRel = std::filesystem::path(project->RootDirectory) / projectRelative;
			if (std::filesystem::exists(projectRel)) return projectRel.string();

			// Last-resort fallback for builds where the runtime exe sits in
			// a different cwd than its asset payload. Try the explicit
			// AssetsDirectory the project records (the editor copies the
			// user's Assets tree alongside the runtime, but Process::Run
			// starts the game with cwd at the user's Documents/Desktop on
			// double-click — `projectRelative` typed as "Assets/foo.png"
			// wouldn't otherwise resolve). Without this, ResolveTexturePath
			// inside LoadTexture sees a relative path that can't be found
			// and returns an invalid handle, which is the symptom that
			// surfaces as "TextureHandle index 65535" log spam.
			std::filesystem::path assetsRel = std::filesystem::path(project->AssetsDirectory)
				/ std::filesystem::path(projectRelative).filename();
			if (std::filesystem::exists(assetsRel)) return assetsRel.string();

			return projectRelative;
		}
	}

	void RuntimeSplashLayer::OnAttach(Application& app) {
		m_ImGuiAcquired = RuntimeImGuiHost::Acquire(app.GetWindow());

		// Tell the Application this splash is live so Initialize() will
		// defer the InitializeStartupScenes call until OnDetach below
		// fires. Without this signal Application would load scenes
		// synchronously during Init and the splash would render on top
		// of an already-loaded scene instead of *before* it loads.
		Application::SignalSplashAttached();

		// Opt our (lazy-loaded) m_Logo into TextureManager::PurgeUnreferenced
		// so the first scene load doesn't reap the splash texture out from
		// under us. The provider is queried each Purge call, so it's fine
		// that m_Logo is still default-invalid here.
		m_TextureRefToken = TextureManager::AddReferenceProvider(
			[this](const TextureManager::ReferenceEmitter& emit) {
				if (m_Logo.IsValid()) emit(m_Logo);
				if (m_Background.IsValid()) emit(m_Background);
			});

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			m_FadeIn = std::max(0.0f, project->SplashScreen.FadeInSeconds);
			m_Hold = std::max(0.0f, project->SplashScreen.DurationSeconds);
			m_FadeOut = std::max(0.0f, project->SplashScreen.FadeOutSeconds);
			m_BackgroundR = project->SplashScreen.BackgroundR;
			m_BackgroundG = project->SplashScreen.BackgroundG;
			m_BackgroundB = project->SplashScreen.BackgroundB;
			m_Subtitle = project->SplashScreen.CustomText.empty()
				? DefaultBuildLine() : project->SplashScreen.CustomText;
		}
		else {
			m_Subtitle = DefaultBuildLine();
		}
	}

	void RuntimeSplashLayer::OnDetach(Application&) {
		if (m_TextureRefToken != 0) {
			TextureManager::RemoveReferenceProvider(m_TextureRefToken);
			m_TextureRefToken = 0;
		}
		if (m_ImGuiAcquired) {
			RuntimeImGuiHost::Release();
			m_ImGuiAcquired = false;
		}

		// Splash is fully gone — the main loop's TickDeferredStartupScenes
		// will pick up this signal next frame and finally load the
		// startup scene. Sequenced last so any teardown above completes
		// before the (potentially seconds-long) scene-load stutter.
		Application::SignalSplashDetached();
	}

	void RuntimeSplashLayer::OnUpdate(Application& app, float /*dt*/) {
		if (m_RequestPop) return;
		// Use unscaled dt — splash timing should not respond to Time.timeScale.
		m_Elapsed += app.GetTime().GetDeltaTimeUnscaled();

		const float total = m_FadeIn + m_Hold + m_FadeOut;
		if (m_Elapsed >= total) {
			m_RequestPop = true;
			// Self-pop at the end of the timeline. PopOverlay defers the
			// actual erase to dispatch-end (we're inside the main loop's
			// OnUpdate dispatch right now), but runs OnDetach
			// immediately — which fires SignalSplashDetached and lets
			// Application::TickDeferredStartupScenes drain the deferred
			// startup-scene load on the very next BeginFrame step.
			// Without this call, the splash would just go silent
			// (return-early on every Update / PreRender) and the scene
			// would never load.
			app.PopOverlay(this);
		}
	}

	void RuntimeSplashLayer::OnPreRender(Application& app) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		if (m_RequestPop) return;

		// Lazy-load the logo on the first render frame so OpenGL context
		// is guaranteed to exist (TextureManager::LoadTexture creates a GL
		// texture on first call).
		if (!m_LogoLoadAttempted) {
			m_LogoLoadAttempted = true;
			std::string logoPath;
			bool customRequested = false;
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project && !project->SplashScreen.ImagePath.empty()) {
				customRequested = true;
				logoPath = ResolveCustomLogoPath(project->SplashScreen.ImagePath);
			}
			if (logoPath.empty()) {
				logoPath = ResolveDefaultLogoPath();
			}
			if (!logoPath.empty()) {
				m_Logo = TextureManager::LoadTexture(logoPath);
			}
			// If a custom splash image was authored but the load didn't
			// produce a valid handle, surface why exactly once. Without
			// this the only diagnostic was an `[OutOfRange] TextureHandle
			// index 65535` line per render frame from GetTexture, which
			// pointed at the read site rather than the load failure that
			// caused it. Logging the requested path + the resolved path
			// (or the missing-asset cause) tells the user which file the
			// build expects and where to put it.
			if (customRequested && !TextureManager::IsValid(m_Logo)) {
				AIM_CORE_WARN_TAG("RuntimeSplash",
					"Custom splash image '{}' failed to load (resolved: '{}'); falling back to no logo.",
					project->SplashScreen.ImagePath,
					logoPath.empty() ? std::string("<unresolved>") : logoPath);
			}

			// Background image — same path resolution as the logo, but
			// optional. If the user didn't author one we leave m_Background
			// invalid and fall back to the solid Background{R,G,B} fill
			// already drawn below.
			if (project && !project->SplashScreen.BackgroundImagePath.empty()) {
				const std::string bgPath = ResolveCustomLogoPath(project->SplashScreen.BackgroundImagePath);
				if (!bgPath.empty()) {
					m_Background = TextureManager::LoadTexture(bgPath);
				}
				if (!TextureManager::IsValid(m_Background)) {
					AIM_CORE_WARN_TAG("RuntimeSplash",
						"Splash background image '{}' failed to load (resolved: '{}'); using solid colour fallback.",
						project->SplashScreen.BackgroundImagePath,
						bgPath.empty() ? std::string("<unresolved>") : bgPath);
				}
			}
		}

		RuntimeImGuiHost::BeginFrame();

		int width = 0, height = 0;
		if (Window* window = app.GetWindow()) {
			if (Viewport* vp = Window::GetMainViewport()) {
				width = vp->GetWidth();
				height = vp->GetHeight();
			}
			else {
				width = window->GetWidth();
				height = window->GetHeight();
			}
		}
		if (width <= 0 || height <= 0) return;

		// Fade timeline. Alpha curves linearly from 0→1 over FadeIn,
		// stays 1 during Hold, then 1→0 over FadeOut. The background
		// fades on top so the previous frame doesn't bleed through.
		float alpha = 1.0f;
		if (m_Elapsed < m_FadeIn && m_FadeIn > 0.0f) {
			alpha = m_Elapsed / m_FadeIn;
		}
		else if (m_Elapsed > m_FadeIn + m_Hold && m_FadeOut > 0.0f) {
			alpha = 1.0f - (m_Elapsed - m_FadeIn - m_Hold) / m_FadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoFocusOnAppearing;
		ImGui::Begin("##AxiomSplash", nullptr, flags);

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 wMin = ImGui::GetWindowPos();
		const ImVec2 wMax = ImVec2(wMin.x + width, wMin.y + height);
		// Solid colour painted FIRST so it acts as the fallback when no
		// background image was authored, AND as the underlay if the
		// image fails to load or carries transparency. The optional
		// background image is drawn on top with the same fade alpha so
		// it animates with the rest of the splash. Letterboxing /
		// pillarboxing is handled implicitly: scale the image so its
		// shorter side fills the canvas (cover style), centred — so the
		// background bleeds beyond the canvas edges instead of leaving
		// solid bars when aspect ratios differ.
		draw->AddRectFilled(wMin, wMax, PackColor(m_BackgroundR, m_BackgroundG, m_BackgroundB, alpha));

		Texture2D* background = TextureManager::GetTexture(m_Background);
		if (background && background->IsValid()) {
			const float bgW = static_cast<float>(background->GetWidth());
			const float bgH = static_cast<float>(background->GetHeight());
			if (bgW > 0.0f && bgH > 0.0f) {
				const float canvasAspect = static_cast<float>(width) / static_cast<float>(height);
				const float bgAspect = bgW / bgH;
				float drawW = static_cast<float>(width);
				float drawH = static_cast<float>(height);
				if (bgAspect > canvasAspect) {
					// Image is wider than canvas — fit to height, overflow horizontally.
					drawW = drawH * bgAspect;
				}
				else {
					// Image is taller — fit to width, overflow vertically.
					drawH = drawW / bgAspect;
				}
				const ImVec2 bgMin(wMin.x + (width  - drawW) * 0.5f,
				                   wMin.y + (height - drawH) * 0.5f);
				const ImVec2 bgMax(bgMin.x + drawW, bgMin.y + drawH);
				const ImU32 bgTint = PackColor(1.0f, 1.0f, 1.0f, alpha);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(background->GetHandle())),
					bgMin, bgMax,
					ImVec2(0, 1), ImVec2(1, 0),
					bgTint);
			}
		}

		const float centerX = wMin.x + width * 0.5f;
		const float centerY = wMin.y + height * 0.5f;

		Texture2D* logo = TextureManager::GetTexture(m_Logo);
		if (logo && logo->IsValid()) {
			const float maxLogoSide = std::min(width, height) * 0.35f;
			float logoW = static_cast<float>(logo->GetWidth());
			float logoH = static_cast<float>(logo->GetHeight());
			if (logoW > 0 && logoH > 0) {
				const float scale = std::min(maxLogoSide / logoW, maxLogoSide / logoH);
				logoW *= scale;
				logoH *= scale;
				const ImVec2 imgMin(centerX - logoW * 0.5f, centerY - logoH * 0.65f);
				const ImVec2 imgMax(imgMin.x + logoW, imgMin.y + logoH);
				const ImU32 tint = PackColor(1.0f, 1.0f, 1.0f, alpha);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(logo->GetHandle())),
					imgMin, imgMax,
					ImVec2(0, 1), ImVec2(1, 0),
					tint);
			}
		}

		// Subtitle line — engine version + platform + build profile, OR
		// the project's customText override.
		if (!m_Subtitle.empty()) {
			const ImVec2 textSize = ImGui::CalcTextSize(m_Subtitle.c_str());
			const ImVec2 textPos(centerX - textSize.x * 0.5f,
				centerY + std::min(width, height) * 0.18f);
			draw->AddText(textPos, PackColor(1.0f, 1.0f, 1.0f, alpha * 0.85f), m_Subtitle.c_str());
		}

		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	void RuntimeSplashLayer::OnPostRender(Application&) {
		if (!m_ImGuiAcquired || !RuntimeImGuiHost::IsInitialized()) return;
		RuntimeImGuiHost::EndFrame();
	}

} // namespace Axiom
