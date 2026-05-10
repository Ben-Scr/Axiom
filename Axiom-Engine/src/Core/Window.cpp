#include "pch.hpp"
#include <bgfx/bgfx.h>
#include "Window.hpp"
#include "Application.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/OpenGL.hpp"
#include "Graphics/RenderApi.hpp"
#include "Events/WindowEvents.hpp"
#include "Events/KeyEvents.hpp"
#include "Events/MouseEvents.hpp"
#include "Scripting/ScriptEngine.hpp"
#include <Utils/StringHelper.hpp>

#ifdef AIM_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace Axiom {
	Window* Window::s_ActiveWindow = nullptr;
	bool Window::s_IsVsync = true;
	std::unique_ptr<Viewport> Window::s_MainViewport = nullptr;
	Window::UIRegion Window::s_UIRegion{};
	bool Window::s_IsInitialized = false;
	const GLFWvidmode* Window::k_Videomode = nullptr;

	Window::Window(int width, int height, const std::string& title)
	{
		Create(WindowSpecification{ width , height, title });
	}

	Window::Window(const WindowSpecification& props)
	{
		Create(props);
	}

	void Window::RefreshCallback(GLFWwindow* window) {
		(void)window;
		Application* app = Application::GetInstance();
		if (!app) return;
		app->RenderOnceForRefresh();
	}

	void Window::Initialize() {
		if (s_IsInitialized) {
			AIM_CORE_WARN_TAG("Window", "GLFW is already initialized");
			return;
		}

		int code = glfwInit();

		AIM_ASSERT(code == GLFW_TRUE, AxiomErrorCode::Undefined, "GLFW library couldn't initialize, error code " + StringHelper::WrapWith(std::to_string(code), '\''));

		// Headless / CI / detached-session: glfwGetPrimaryMonitor can return null.
		GLFWmonitor* mainMonitor = GetMainMonitor();
		if (!mainMonitor) {
			AIM_CORE_ERROR_TAG("Window", "No primary monitor available");
			k_Videomode = nullptr;
			glfwTerminate();
			s_IsInitialized = false;
			return;
		}
		k_Videomode = glfwGetVideoMode(mainMonitor);
		AIM_ASSERT(k_Videomode != nullptr, AxiomErrorCode::Undefined, "Failed to query monitor video mode");

		s_IsInitialized = true;
	}

	void Window::Shutdown() {
		if (!s_IsInitialized) {
			return;
		}

		s_MainViewport.reset();
		s_ActiveWindow = nullptr;
		k_Videomode = nullptr;
		// Reset across reloads — last user-set vsync state would otherwise leak.
		s_IsVsync = true;
		s_IsInitialized = false;
		glfwTerminate();
	}

	void Window::IconifyCallback(GLFWwindow* /*window*/, int iconified) {
		// Authoritative source for the engine's "minimized" state. The framebuffer-resize-
		// to-(0,0) path that used to set this flag doesn't fire on every platform; iconify
		// is the explicit signal. Window is friend-declared in Application so this member
		// callback can reach Application::SetWindowMinimized (private accessor).
		Application::SetWindowMinimized(static_cast<bool>(iconified));
	}

	void Window::FocusCallback(GLFWwindow* window, int focused) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		Application::SetWindowFocused(static_cast<bool>(focused));

		if ((bool)focused) {
			ScriptEngine::RaiseFocusChanged(true);
			if (win && win->m_EventCallback) {
				WindowFocusEvent e;
				win->m_EventCallback(e);
			}
		}
		else {
			ScriptEngine::RaiseFocusChanged(false);
			if (win && win->m_EventCallback) {
				WindowLostFocusEvent e;
				win->m_EventCallback(e);
			}
		}
	}

	void Window::CloseCallback(GLFWwindow* window) {
		// Fires for the title-bar X click AND Alt+F4 (Windows posts
		// the same WM_CLOSE for both). Forward to the engine's event
		// system; the WindowCloseEvent handler in
		// Application::DispatchEvent calls RequestQuit and resets
		// glfwSetWindowShouldClose so the main loop continues for at
		// least one more frame, giving layers a chance to intercept
		// (the editor's save-before-quit dialog).
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			WindowCloseEvent e;
			win->m_EventCallback(e);
		}
	}

	void Window::Create(const WindowSpecification& props) {
		AIM_ASSERT(s_IsInitialized, AxiomErrorCode::NotInitialized, "The Window isn't initialized");

		// bgfx owns the GPU context (D3D11 / Vulkan / Metal / its own GL
		// loader, picked at bgfx::init time). Telling GLFW to skip the
		// OpenGL context creation is mandatory — otherwise GLFW would
		// pick a GL context for us that bgfx then can't talk to.
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_SAMPLES, 8);


		s_MainViewport = std::make_unique<Viewport>(
			props.Fullscreen ? k_Videomode->width : props.Width,
			props.Fullscreen ? k_Videomode->height : props.Height
		);

		AIM_INFO_TAG("Window",
			"Create: spec={}x{}, fullscreen={}, videomode={}x{} -> viewport={}x{}",
			props.Width, props.Height, props.Fullscreen,
			k_Videomode ? k_Videomode->width : 0,
			k_Videomode ? k_Videomode->height : 0,
			s_MainViewport->GetWidth(), s_MainViewport->GetHeight());

		m_GLFWwindow = glfwCreateWindow(s_MainViewport->GetWidth(), s_MainViewport->GetHeight(), props.Title.c_str(), nullptr, nullptr);
		AIM_ASSERT(m_GLFWwindow, AxiomErrorCode::Undefined, "Failed to create window!");

		SetDecorated(props.Decorated);
		SetResizeable(props.Resizeable);

		if (props.Fullscreen)
			SetFullScreen(true);
		else
			CenterWindow();

		// Log the post-fullscreen / post-center size so a divergence
		// between viewport (the size we *want*) and GLFW's reported
		// framebuffer (the size we *got*) is visible immediately. This
		// surfaces the failure mode where SetFullScreen silently bails
		// out (e.g. monitor query returns nullptr) and leaves the window
		// at GLFW's default 480x270 — which is what showed up as the
		// "editor only renders into the top-left corner" bug.
		{
			int fbW = 0, fbH = 0;
			glfwGetFramebufferSize(m_GLFWwindow, &fbW, &fbH);
			AIM_INFO_TAG("Window",
				"After fullscreen/center: GLFW framebuffer={}x{}", fbW, fbH);
		}

		// No GL context to make current — bgfx handles the device.
		glfwSetWindowUserPointer(m_GLFWwindow, this);

		glfwSetKeyCallback(m_GLFWwindow, SetKeyCallback);
		glfwSetCharCallback(m_GLFWwindow, SetCharCallback);
		glfwSetMouseButtonCallback(m_GLFWwindow, SetMouseButtonCallback);
		glfwSetCursorPosCallback(m_GLFWwindow, SetCursorPositionCallback);
		glfwSetScrollCallback(m_GLFWwindow, SetScrollCallback);
		glfwSetWindowFocusCallback(m_GLFWwindow, FocusCallback);

		glfwSetFramebufferSizeCallback(m_GLFWwindow, SetWindowResizedCallback);
		glfwSetWindowIconifyCallback(m_GLFWwindow, &Window::IconifyCallback);

		glfwSetDropCallback(m_GLFWwindow, SetDropCallback);
		glfwSetWindowRefreshCallback(m_GLFWwindow, RefreshCallback);
		// Required so layers can intercept the window-close attempt
		// (Alt+F4 + title-bar X button both go through here on
		// Windows). The handler in Application::DispatchEvent already
		// calls RequestQuit + resets shouldClose so the main loop
		// stays alive long enough for the editor's OnPreRender
		// intercept to pop the save-before-quit dialog. Without the
		// registration, GLFW's default WM_CLOSE handler just sets
		// shouldClose=true and the loop exits with no warning.
		glfwSetWindowCloseCallback(m_GLFWwindow, &Window::CloseCallback);

		// Vsync is handled by bgfx via BGFX_RESET_VSYNC at bgfx::init time.
		SyncViewportFromFramebuffer();

		s_ActiveWindow = this;
	}
	void Window::Destroy() {
		if (!m_GLFWwindow) {
			return;
		}

		// m_Cursor aliases either m_DefaultCursor or m_UICursor (or is
		// null when the OS default is in use). Free the underlying
		// owners; clearing m_Cursor avoids a double-destroy of the
		// alias.
		m_Cursor = nullptr;
		if (m_DefaultCursor) {
			glfwDestroyCursor(m_DefaultCursor);
			m_DefaultCursor = nullptr;
		}
		if (m_UICursor) {
			glfwDestroyCursor(m_UICursor);
			m_UICursor = nullptr;
		}
		glfwSetWindowUserPointer(m_GLFWwindow, nullptr);
		m_EventCallback = {};
		glfwDestroyWindow(m_GLFWwindow);
		m_GLFWwindow = nullptr;
		if (s_ActiveWindow == this) {
			s_ActiveWindow = nullptr;
		}
	}

	void Window::SwapBuffers() const {
		// Presenting the rendered frame is `bgfx::frame()` — it advances
		// bgfx's internal frame index, flushes the render-thread command
		// buffer (or executes inline when single-threaded), and presents
		// the swap-chain. GLFW's glfwSwapBuffers needs a current GL
		// context, which doesn't exist (GLFW was created with
		// GLFW_NO_API).
		bgfx::frame();
	}

	void Window::CenterWindow() {
		Vec2Int screenCenter = GetScreenCenter();
		int windowWidth, windowHeight;
		glfwGetWindowSize(m_GLFWwindow, &windowWidth, &windowHeight);
		int posX = screenCenter.x - windowWidth / 2;
		int posY = screenCenter.y - windowHeight / 2;
		glfwSetWindowPos(m_GLFWwindow, posX, posY);
	}
	void Window::Focus() {
		glfwFocusWindow(m_GLFWwindow);
	}
	void Window::MaximizeWindow(bool reset) {
		if (reset && IsMaximized()) {
			glfwRestoreWindow(m_GLFWwindow);
		}
		else {
			glfwMaximizeWindow(m_GLFWwindow);
		}

		SyncViewportFromFramebuffer();
		UpdateViewport();
	}
	void Window::MinimizeWindow() {
		glfwIconifyWindow(m_GLFWwindow);
	}
	void Window::RestoreWindow() {
		glfwRestoreWindow(m_GLFWwindow);
		SyncViewportFromFramebuffer();
		UpdateViewport();
	}

	void Window::SetKeyCallback(GLFWwindow* window, int key, int, int action, int mods) {
		(void)mods;
		if (key == GLFW_KEY_UNKNOWN) {
			return;
		}

		Application* app = Application::GetInstance();
		if (!app) return;

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));

		switch (action) {
		case GLFW_PRESS:
			app->m_Input.OnKeyDown(key);
			ScriptEngine::RaiseKeyDown(key);
			if (win && win->m_EventCallback) {
				KeyPressedEvent e(key, false);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_RELEASE:
			app->m_Input.OnKeyUp(key);
			ScriptEngine::RaiseKeyUp(key);
			if (win && win->m_EventCallback) {
				KeyReleasedEvent e(key);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_REPEAT:
			app->m_Input.OnKeyDown(key);
			if (win && win->m_EventCallback) {
				KeyPressedEvent e(key, true);
				win->m_EventCallback(e);
			}
			break;
		default:
			break;
		}
	}

	void Window::SetCharCallback(GLFWwindow* /*window*/, unsigned int codepoint) {
		// Drives UI input fields. GLFW only fires char-callback for printable
		// codepoints (after IME composition) — control keys (Backspace, Enter,
		// arrow keys) come through the regular key callback, not here.
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnChar(static_cast<uint32_t>(codepoint));
	}

	void Window::SetMouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
		(void)mods;
		Application* app = Application::GetInstance();
		if (!app) return;

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));

		switch (action) {
		case GLFW_PRESS:
			app->m_Input.OnMouseDown(button);
			ScriptEngine::RaiseMouseDown(button);
			if (win && win->m_EventCallback) {
				MouseButtonPressedEvent e(button);
				win->m_EventCallback(e);
			}
			break;
		case GLFW_RELEASE:
			app->m_Input.OnMouseUp(button);
			ScriptEngine::RaiseMouseUp(button);
			if (win && win->m_EventCallback) {
				MouseButtonReleasedEvent e(button);
				win->m_EventCallback(e);
			}
			break;
		default:
			break;
		}
	}

	void Window::SetCursorPositionCallback(GLFWwindow* window, double xPos, double yPos) {
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnMouseMove(xPos, yPos);
		ScriptEngine::RaiseMouseMove(static_cast<float>(xPos), static_cast<float>(yPos));

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			MouseMovedEvent e(static_cast<float>(xPos), static_cast<float>(yPos));
			win->m_EventCallback(e);
		}
	}

	void Window::SetScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
		Application* app = Application::GetInstance();
		if (!app) return;
		app->m_Input.OnScroll(static_cast<float>(yoffset));
		ScriptEngine::RaiseMouseScroll(static_cast<float>(yoffset));

		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (win && win->m_EventCallback) {
			MouseScrolledEvent e(static_cast<float>(xoffset), static_cast<float>(yoffset));
			win->m_EventCallback(e);
		}
	}

	void Window::SetFullScreen(bool enabled) {
		bool isFullScreen = IsFullScreen();

		if ((isFullScreen && enabled) || (!isFullScreen && !enabled))
			return;

		if (enabled) {
			m_RestoreSize = GetSize();
			m_RestorePos = GetPosition();
		}

		glfwSetWindowMonitor(
			m_GLFWwindow,
			enabled ? GetMainMonitor() : nullptr,
			enabled ? 0 : m_RestorePos.x,
			enabled ? 0 : m_RestorePos.y,
			enabled ? k_Videomode->width : m_RestoreSize.x,
			enabled ? k_Videomode->height : m_RestoreSize.y,
			k_Videomode->refreshRate
		);
		SyncViewportFromFramebuffer();
		UpdateViewport();

		if (!enabled) {
			SetSize(m_RestoreSize);
			SetPosition(m_RestorePos);
		}
	}

	void Window::SetDecorated(bool enabled) {
		glfwSetWindowAttrib(m_GLFWwindow, GLFW_DECORATED, enabled ? GLFW_TRUE : GLFW_FALSE);
	}
	void Window::SetVisible(bool enabled) {
		if (enabled)
			glfwShowWindow(m_GLFWwindow);
		else
			glfwHideWindow(m_GLFWwindow);
	}

	void Window::SetResizeable(bool enabled) {
		glfwSetWindowAttrib(m_GLFWwindow, GLFW_RESIZABLE, enabled ? GLFW_TRUE : GLFW_FALSE);
	}

	void Window::SetCursorLocked(bool enabled) {
		glfwSetInputMode(m_GLFWwindow, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
	}
	void Window::SetCursorHidden(bool enabled) {
		glfwSetInputMode(m_GLFWwindow, GLFW_CURSOR, enabled ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
	}
	namespace {
		// Build a GLFWcursor from a Texture2D. Returns nullptr on any
		// failure with a warning logged. The caller owns the returned
		// cursor and must free it via glfwDestroyCursor.
		GLFWcursor* MakeCursorFromTexture(const Texture2D* tex2D) {
			if (!tex2D) return nullptr;
			std::unique_ptr<ImageData> imgData = tex2D->GetImageData();
			if (!imgData || !imgData->Pixels) {
				AIM_WARN_TAG("Window", "Cannot set cursor image from invalid texture data");
				return nullptr;
			}
			GLFWimage img;
			img.width = imgData->Width;
			img.height = imgData->Height;
			img.pixels = imgData->Pixels;
			return glfwCreateCursor(&img, 0, 0);
		}
	}

	void Window::SetCursorImage(const Texture2D* tex2D) {
		// Stage as the new "default" cursor. If the cursor isn't currently
		// over UI we apply it immediately; otherwise the swap waits for
		// SetCursorOverUI(false). Passing nullptr clears the slot back
		// to the OS default.
		GLFWcursor* fresh = tex2D ? MakeCursorFromTexture(tex2D) : nullptr;
		if (tex2D && !fresh) return;

		const bool wasUsingDefault = (m_Cursor == m_DefaultCursor);
		if (m_DefaultCursor) {
			glfwDestroyCursor(m_DefaultCursor);
		}
		m_DefaultCursor = fresh;

		if (wasUsingDefault) {
			m_Cursor = m_DefaultCursor;
			glfwSetCursor(m_GLFWwindow, m_DefaultCursor);
		}
	}

	void Window::SetUICursorImage(const Texture2D* tex2D) {
		GLFWcursor* fresh = tex2D ? MakeCursorFromTexture(tex2D) : nullptr;
		if (tex2D && !fresh) return;

		const bool wasUsingUI = (m_Cursor == m_UICursor);
		if (m_UICursor) {
			glfwDestroyCursor(m_UICursor);
		}
		m_UICursor = fresh;

		if (wasUsingUI) {
			m_Cursor = m_UICursor;
			glfwSetCursor(m_GLFWwindow, m_UICursor);
		}
	}

	void Window::SetCursorOverUI(bool overUI) {
		if (overUI == m_CursorOverUI) return;
		m_CursorOverUI = overUI;

		// Resolve which slot to apply. UI hovering only switches when the
		// project supplied a UI cursor — otherwise we keep the default
		// active so an unset UI cursor doesn't fall back to the OS arrow
		// every time the user mouses over a button.
		GLFWcursor* desired = overUI && m_UICursor ? m_UICursor : m_DefaultCursor;
		if (desired == m_Cursor) return;
		m_Cursor = desired;
		glfwSetCursor(m_GLFWwindow, desired);
	}

	void Window::SetWindowIcon(const Texture2D* tex2D) {
		AIM_ASSERT(tex2D, AxiomErrorCode::NullReference, "Texture is null");
		std::unique_ptr<ImageData> imgData(tex2D->GetImageData());
		AIM_ASSERT(imgData, AxiomErrorCode::NullReference, "Image data is null");


		const int w = imgData->Width;
		const int h = imgData->Height;

		AIM_ASSERT(w > 0 && h > 0, AxiomErrorCode::InvalidValue, "Icon size must be > 0");
		AIM_ASSERT(imgData->Pixels != nullptr, AxiomErrorCode::NullReference, "Icon pixels null");

		imgData->FlipVerticalRGBA();

		GLFWimage img;
		img.width = imgData->Width;
		img.height = imgData->Height;
		img.pixels = imgData->Pixels;

		glfwSetWindowIcon(m_GLFWwindow, 1, &img);
	}

	void Window::SetWindowIconFromResource() {
#ifdef AIM_PLATFORM_WINDOWS
		HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
		if (!hIcon) {
			AIM_WARN_TAG("Window", "No embedded icon resource found.");
			return;
		}

		ICONINFO iconInfo{};
		if (!GetIconInfo(hIcon, &iconInfo)) {
			DestroyIcon(hIcon);
			AIM_WARN_TAG("Window", "Failed to get icon info.");
			return;
		}

		BITMAP bm{};
		GetObject(iconInfo.hbmColor, sizeof(bm), &bm);

		int w = bm.bmWidth;
		int h = bm.bmHeight;

		std::vector<unsigned char> pixels(w * h * 4);

		BITMAPINFOHEADER bi{};
		bi.biSize = sizeof(bi);
		bi.biWidth = w;
		bi.biHeight = -h; // top-down
		bi.biPlanes = 1;
		bi.biBitCount = 32;
		bi.biCompression = BI_RGB;

		HDC hdc = GetDC(NULL);
		GetDIBits(hdc, iconInfo.hbmColor, 0, h, pixels.data(),
			reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
		ReleaseDC(NULL, hdc);

		// Windows gives BGRA, GLFW expects RGBA
		for (int i = 0; i < w * h * 4; i += 4)
			std::swap(pixels[i], pixels[i + 2]);

		DeleteObject(iconInfo.hbmColor);
		DeleteObject(iconInfo.hbmMask);
		DestroyIcon(hIcon);

		GLFWimage img;
		img.width = w;
		img.height = h;
		img.pixels = pixels.data();

		glfwSetWindowIcon(m_GLFWwindow, 1, &img);
#else
		AIM_WARN_TAG("Window", "SetWindowIconFromResource is only supported on Windows.");
#endif
	}

	void Window::SetWindowResizedCallback(GLFWwindow* window, int width, int height) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (!win) return;

		win->SyncViewportFromFramebuffer();
		win->UpdateViewport();

		if (win->m_EventCallback) {
			WindowResizeEvent e(width, height);
			win->m_EventCallback(e);
		}
	}

	Vec2 Window::GetCursorPosition() const {
		double x, y;
		glfwGetCursorPos(m_GLFWwindow, &x, &y);
		return Vec2(x, y);
	}
	std::string Window::GetClipboardString() const {
		const char* clipboard = glfwGetClipboardString(m_GLFWwindow);
		return clipboard ? std::string(clipboard) : std::string{};
	}
	Vec2Int Window::GetScreenCenter() const {
		return Vec2Int(k_Videomode->width / 2, k_Videomode->height / 2);
	}
	Vec2 Window::GetWindowCenter() const {
		Vec2Int size = GetSize();
		return Vec2(size.x / 2.f, size.y / 2.f);
	}
	GLFWmonitor* Window::GetWindowMonitor() const { return glfwGetWindowMonitor(m_GLFWwindow); }
	GLFWmonitor* Window::GetMainMonitor() { return glfwGetPrimaryMonitor(); }

	bool Window::IsMaximized() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_MAXIMIZED);
	}
	bool Window::IsMinimized() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_ICONIFIED);
	}
	bool Window::IsFullScreen()const {
		return GetWindowMonitor() != nullptr;
	}
	bool Window::IsVisible() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_VISIBLE) == GLFW_TRUE;
	}
	bool Window::IsDecorated()const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_DECORATED);
	}
	bool Window::IsResizeable() const {
		return glfwGetWindowAttrib(m_GLFWwindow, GLFW_RESIZABLE);
	}


	void Window::SetDropCallback(GLFWwindow* window, int count, const char** paths) {
		Window* win = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
		if (!win || !win->m_EventCallback || count <= 0) return;

		std::vector<std::string> filePaths;
		filePaths.reserve(count);
		for (int i = 0; i < count; i++) {
			filePaths.emplace_back(paths[i]);
		}

		FileDropEvent e(std::move(filePaths));
		win->m_EventCallback(e);
	}

	void Window::UpdateViewport() {
		if (!RenderApi::IsInitialized() || !s_MainViewport) {
			return;
		}
		const int w = s_MainViewport->GetWidth();
		const int h = s_MainViewport->GetHeight();
		// Drives BOTH the swap-chain reset (bgfx::reset on the GLFW
		// framebuffer's new size) AND the default view's rect. Without
		// the explicit OnWindowResize call, SetViewport-only would miss
		// the reset whenever it was called with an FBO currently bound
		// — the editor's FBO render keeps view 1+ bound between scene
		// passes, so a window resize landing mid-frame would set the
		// per-FBO viewport but leave the swap chain at bgfx::init's
		// resolution (visible as "editor renders only into the top-
		// left corner of the OS window").
		RenderApi::OnWindowResize(w, h);
		RenderApi::SetViewport(0, 0, w, h);
	}

	void Window::SyncViewportFromFramebuffer() {
		if (!m_GLFWwindow) {
			return;
		}

		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(m_GLFWwindow, &width, &height);

		if (!s_MainViewport) {
			s_MainViewport = std::make_unique<Viewport>(width, height);
			return;
		}

		s_MainViewport->SetWidth(width);
		s_MainViewport->SetHeight(height);
	}
}
