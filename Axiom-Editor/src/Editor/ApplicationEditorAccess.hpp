#pragma once

#include "Core/Application.hpp"

namespace Axiom {
	class ApplicationEditorAccess {
	public:
		static void SetPlaymodePaused(bool paused)
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_IsGameplayPaused = paused;
			}
		}

		static bool IsPlaymodePaused()
		{
			return Application::s_Instance ? Application::s_Instance->m_IsGameplayPaused : false;
		}

		static void SetGameInputEnabled(bool enabled)
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_IsScriptInputEnabled = enabled;
			}
		}

		static bool IsGameInputEnabled()
		{
			return Application::s_Instance ? Application::s_Instance->m_IsScriptInputEnabled : true;
		}

		// Set / consume the "stop play mode" flag that scripted
		// Application.Quit() calls raise inside the editor. Quit in a
		// shipped runtime closes the process; in the editor it must
		// instead stop play mode (and leave the editor open). The editor
		// polls this flag every frame and routes it through its normal
		// stop-play path; consuming clears the flag so the editor
		// doesn't re-stop on the next frame.
		static bool ConsumeQuitStopPlayRequest()
		{
			if (!Application::s_Instance) return false;
			const bool requested = Application::s_Instance->m_EditorStopPlayRequested;
			Application::s_Instance->m_EditorStopPlayRequested = false;
			return requested;
		}

		static void RequestEditorStopPlay()
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_EditorStopPlayRequested = true;
			}
		}

		// Resets Time.TimeSinceStartup / Time.RealtimeSinceStartup to zero. Called
		// at editor play-mode entry so every play session starts at t=0; built games
		// already get this for free at the end of Application::Initialize.
		static void MarkGameStart()
		{
			if (Application::s_Instance) {
				Application::s_Instance->m_Time.MarkGameStart();
			}
		}

	private:
		ApplicationEditorAccess() = delete;
	};
}
