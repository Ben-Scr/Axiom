#include "ImGuiContextLayer.hpp"

#include "Core/Application.hpp"
#include "Core/Assert.hpp"
#include "Core/Window.hpp"
#include "Packages/PackageImGuiBridge.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#if defined(AIM_RHI_BGFX)
// Backend now lives inside Axiom-Engine.dll (Axiom-Engine/src/Gui/ImGuiImplBgfx.{hpp,cpp}).
// We previously static-linked the .cpp into each consumer .exe but bgfx's renderer
// state is process-global static — having two copies of bgfx (one in engine.dll,
// one in the .exe) meant the launcher saw `RendererType::Noop` even after
// engine.dll's bgfx::init brought up D3D11. The header is reached via the engine
// include path; the four entry points are exported through AXIOM_API.
#include "Gui/ImGuiImplBgfx.hpp"
#else
#include <backends/imgui_impl_opengl3.h>
#endif

#include <algorithm>

namespace Axiom {

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

		// Engine.dll has its own static-linked copy of ImGui under --rhi=bgfx
		// (so it can host ImGuiImplBgfx alongside bgfx::init). Publish our
		// context + allocators here so engine.dll's ImGui state can sync to
		// our context on every backend entry point. See Axiom-Engine/src/
		// Gui/ImGuiImplBgfx.cpp `SyncImGuiContextFromBridge`.
		{
			ImGuiMemAllocFunc allocFn = nullptr;
			ImGuiMemFreeFunc  freeFn  = nullptr;
			void*             userData = nullptr;
			ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &userData);
			PackageImGuiBridge::Publish(
				reinterpret_cast<void*>(ImGui::GetCurrentContext()),
				reinterpret_cast<void*>(allocFn),
				reinterpret_cast<void*>(freeFn),
				userData);
		}

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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

#if defined(AIM_RHI_BGFX)
		AIM_VERIFY(ImGui_ImplGlfw_InitForOther(glfwWindow, true),
			"Failed to init glfw for imgui (bgfx backend)!");
		AIM_VERIFY(ImGuiImplBgfx::Init(),
			"Failed to init bgfx imgui backend (shader binaries missing?)");
#else
		AIM_VERIFY(ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true), "Failed to init glfw for imgui!");
		AIM_VERIFY(ImGui_ImplOpenGL3_Init("#version 330 core"), "Failed to init openGL3 for imgui!");
#endif

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

#if defined(AIM_RHI_BGFX)
		ImGuiImplBgfx::Shutdown();
#else
		ImGui_ImplOpenGL3_Shutdown();
#endif
		ImGui_ImplGlfw_Shutdown();
		PackageImGuiBridge::Clear();
		ImGui::DestroyContext();

		m_IsInitialized = false;
	}

	void ImGuiContextLayer::OnPreRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
#if defined(AIM_RHI_BGFX)
		ImGuiImplBgfx::NewFrame();
#else
		ImGui_ImplOpenGL3_NewFrame();
#endif
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiContextLayer::OnPostRender(Application& app) {
		(void)app;
		if (!m_IsInitialized) {
			return;
		}
		ImGui::Render();
#if defined(AIM_RHI_BGFX)
		// View 255: ImGui overlays everything else (bgfx flushes views in
		// numeric order; the launcher only uses view 0 for its window
		// clear so 255 is comfortably above any other submission).
		ImGuiImplBgfx::RenderDrawData(ImGui::GetDrawData(), 255);
#else
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
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
