#include "pch.hpp"
#include "Editor/ExternalEditor.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Json.hpp"
#include "Serialization/Path.hpp"
#include "Project/ProjectManager.hpp"
#include "Project/IndexProject.hpp"

#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#ifdef IDX_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <oleauto.h>
#endif

namespace Index {

#ifdef IDX_PLATFORM_WINDOWS
	namespace {
		// M29: tracker for the editor-launcher worker threads spawned in
		// OpenFile (one short-lived `CreateProcessW`, one Sleep(4000) +
		// secondary CreateProcessW for the cold-start VS path). Tracked
		// instead of detached so editor shutdown can join them.
		// FULL JOIN is potentially blocking on the cold-start VS thread
		// for up to ~4s — see PARTIAL note in the audit.
		std::mutex s_LaunchMutex;
		std::vector<std::thread> s_LaunchThreads;
	}

	static void TrackLaunchThread(std::thread t) {
		std::scoped_lock lock(s_LaunchMutex);
		// Drop already-finished threads so the vector doesn't grow without
		// bound during a long editor session.
		s_LaunchThreads.erase(
			std::remove_if(s_LaunchThreads.begin(), s_LaunchThreads.end(),
				[](std::thread& th) { return !th.joinable(); }),
			s_LaunchThreads.end());
		s_LaunchThreads.push_back(std::move(t));
	}
#endif

	void ExternalEditor::JoinPendingLaunchThreads() {
#ifdef IDX_PLATFORM_WINDOWS
		std::vector<std::thread> drained;
		{
			std::scoped_lock lock(s_LaunchMutex);
			drained.swap(s_LaunchThreads);
		}
		for (auto& t : drained) {
			if (t.joinable()) t.join();
		}
#endif
	}

#ifdef IDX_PLATFORM_WINDOWS
	// Convert UTF-8 → UTF-16 for Windows wide-API calls. The previous byte-wise
	// `std::wstring(narrow.begin(), narrow.end())` produced one wchar_t per byte,
	// corrupting any path containing multi-byte UTF-8 (Unicode user folders,
	// project names with accented characters, etc.). The wide path then failed
	// to launch the external editor on any non-ASCII install.
	static std::wstring Utf8ToWide(const std::string& utf8) {
		if (utf8.empty()) return {};
		const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
			static_cast<int>(utf8.size()), nullptr, 0);
		if (needed <= 0) return {};
		std::wstring out(static_cast<size_t>(needed), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
			out.data(), needed);
		return out;
	}
#endif

	std::vector<ExternalEditorInfo> ExternalEditor::s_Editors;
	int ExternalEditor::s_SelectedIndex = 0;
	bool ExternalEditor::s_Detected = false;

	bool ExternalEditor::IsScriptExtension(const std::string& ext) {
		return ext == ".cs" || ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp"
			|| ext == ".lua" || ext == ".py" || ext == ".js" || ext == ".ts"
			|| ext == ".json" || ext == ".xml" || ext == ".yaml" || ext == ".yml"
			|| ext == ".txt" || ext == ".md" || ext == ".cfg" || ext == ".ini"
			|| ext == ".glsl" || ext == ".hlsl" || ext == ".shader";
	}

	void ExternalEditor::TryDetect(ExternalEditorType type, const std::string& displayName,
		const std::vector<std::string>& candidates) {
		for (const auto& path : candidates) {
			if (std::filesystem::exists(path)) {
				ExternalEditorInfo info;
				info.Type = type;
				info.DisplayName = displayName;
				info.ExecutablePath = path;
				info.Available = true;
				s_Editors.push_back(std::move(info));
				return;
			}
		}
	}

	void ExternalEditor::DetectEditors() {
		if (s_Detected) return;
		s_Detected = true;
		s_Editors.clear();

#ifdef IDX_PLATFORM_WINDOWS
		// Visual Studio 2022 (all editions)
		TryDetect(ExternalEditorType::VisualStudio, "Visual Studio 2022", {
			"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\devenv.exe",
			"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\Common7\\IDE\\devenv.exe",
			"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\IDE\\devenv.exe",
		});

		// VS Code
		{
			std::string localAppData;
			char* env = nullptr;
			size_t len = 0;
			if (_dupenv_s(&env, &len, "LOCALAPPDATA") == 0 && env) {
				localAppData = env;
				free(env);
			}

			std::vector<std::string> vscodePaths = {
				localAppData + "\\Programs\\Microsoft VS Code\\Code.exe",
				"C:\\Program Files\\Microsoft VS Code\\Code.exe",
			};

			// Also check PATH
			char pathBuf[MAX_PATH]{};
			DWORD found = SearchPathA(nullptr, "code.cmd", nullptr, MAX_PATH, pathBuf, nullptr);
			if (found > 0)
				vscodePaths.insert(vscodePaths.begin(), std::string(pathBuf));

			TryDetect(ExternalEditorType::VSCode, "Visual Studio Code", vscodePaths);
		}

		// JetBrains Rider
		{
			std::string localAppData;
			char* env = nullptr;
			size_t len = 0;
			if (_dupenv_s(&env, &len, "LOCALAPPDATA") == 0 && env) {
				localAppData = env;
				free(env);
			}

			std::vector<std::string> riderPaths;
			// Toolbox installations
			auto toolboxDir = std::filesystem::path(localAppData) / "JetBrains" / "Toolbox" / "apps" / "Rider";
			if (std::filesystem::exists(toolboxDir)) {
				for (const auto& entry : std::filesystem::directory_iterator(toolboxDir)) {
					if (!entry.is_directory()) continue;
					auto bin = entry.path() / "bin" / "rider64.exe";
					if (std::filesystem::exists(bin))
						riderPaths.push_back(bin.string());
				}
			}

			// Standard install
			riderPaths.push_back("C:\\Program Files\\JetBrains\\JetBrains Rider\\bin\\rider64.exe");

			if (!riderPaths.empty())
				TryDetect(ExternalEditorType::Rider, "JetBrains Rider", riderPaths);
		}
#endif

		if (s_Editors.empty()) {
			IDX_CORE_WARN_TAG("ExternalEditor", "No code editors detected on this system");
		}
		else {
			IDX_CORE_INFO_TAG("ExternalEditor", "Detected {} code editor(s):", s_Editors.size());
			for (size_t i = 0; i < s_Editors.size(); i++)
				IDX_CORE_INFO_TAG("ExternalEditor", "  [{}] {} ({})", i, s_Editors[i].DisplayName, s_Editors[i].ExecutablePath);
		}

		LoadPreferences();
	}

	static std::string GetPreferencesPath() {
		return Path::Combine(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData), "Index", "editor-preferences.json");
	}

	void ExternalEditor::SavePreferences() {
		std::string path = GetPreferencesPath();
		auto dir = std::filesystem::path(path).parent_path();
		if (!std::filesystem::exists(dir))
			std::filesystem::create_directories(dir);

		const auto& selected = (s_SelectedIndex >= 0 && s_SelectedIndex < static_cast<int>(s_Editors.size()))
			? s_Editors[s_SelectedIndex].DisplayName : "";

		Json::Value root = Json::Value::MakeObject();
		root.AddMember("selectedEditor", selected);
		(void)File::WriteAllText(path, Json::Stringify(root, true));
	}

	void ExternalEditor::LoadPreferences() {
		std::string path = GetPreferencesPath();
		if (!std::filesystem::exists(path)) return;

		Json::Value root;
		std::string parseError;
		if (!Json::TryParse(File::ReadAllText(path), root, &parseError) || !root.IsObject()) {
			IDX_CORE_WARN_TAG("ExternalEditor", "Failed to parse editor preferences '{}': {}", path, parseError);
			return;
		}

		const Json::Value* selectedEditorValue = root.FindMember("selectedEditor");
		if (!selectedEditorValue) {
			return;
		}
		std::string savedName = selectedEditorValue->AsStringOr();

		for (int i = 0; i < static_cast<int>(s_Editors.size()); i++) {
			if (s_Editors[i].DisplayName == savedName) {
				s_SelectedIndex = i;
				return;
			}
		}
	}

	void ExternalEditor::SetSelectedIndex(int index) {
		s_SelectedIndex = index;
		SavePreferences();
	}

	const ExternalEditorInfo& ExternalEditor::GetSelected() {
		DetectEditors();
		if (s_SelectedIndex >= 0 && s_SelectedIndex < static_cast<int>(s_Editors.size()))
			return s_Editors[s_SelectedIndex];

		static ExternalEditorInfo fallback;
		return fallback;
	}

	std::string ExternalEditor::ResolveProjectContext(const std::string& filePath, const std::string& ext) {
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (!project) return "";

		// C# scripts → project .sln
		if (ext == ".cs") {
			if (std::filesystem::exists(project->SlnPath))
				return std::filesystem::canonical(project->SlnPath).string();
		}

		// C++ native scripts → NativeScripts .sln
		if (ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp") {
			auto nativeSln = std::filesystem::path(project->NativeScriptsDir)
				/ "build" / (project->Name + "-NativeScripts.sln");
			if (std::filesystem::exists(nativeSln))
				return std::filesystem::canonical(nativeSln).string();
		}

		// Fallback: project .sln if available
		if (std::filesystem::exists(project->SlnPath))
			return std::filesystem::canonical(project->SlnPath).string();

		return "";
	}

#ifdef IDX_PLATFORM_WINDOWS
	// Check if Visual Studio specifically has a window open containing the search string.
	// Requires BOTH the search title AND "Visual Studio" to be in the window title,
	// to avoid false positives from the Index editor or other windows.
	[[maybe_unused]] static bool IsVisualStudioOpen(const std::string& searchTitle) {
		if (searchTitle.empty()) return false;
		std::wstring wSearch = Utf8ToWide(searchTitle);
		std::wstring wVS = L"Visual Studio";
		bool found = false;

		struct Ctx { bool* found; std::wstring* search; std::wstring* vs; };
		Ctx ctx = { &found, &wSearch, &wVS };

		EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
			auto* c = reinterpret_cast<Ctx*>(lParam);
			wchar_t title[512]{};
			GetWindowTextW(hwnd, title, 512);
			if (wcsstr(title, c->search->c_str()) != nullptr &&
				wcsstr(title, c->vs->c_str()) != nullptr) {
				*c->found = true;
				return FALSE;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&ctx));
		return found;
	}

	// Late-bound IDispatch helper. We deliberately do NOT #import the EnvDTE
	// type library to avoid pulling a generated .tlh into the build; instead
	// we resolve names with GetIDsOfNames + Invoke. DISPPARAMS::rgvarg expects
	// arguments in REVERSE order — the caller fills `args` accordingly.
	static HRESULT InvokeByName(IDispatch* disp, const wchar_t* name, WORD flags,
		VARIANT* args, UINT cArgs, VARIANT* result)
	{
		if (!disp || !name) return E_POINTER;
		LPOLESTR n = const_cast<LPOLESTR>(name);
		DISPID dispid = 0;
		HRESULT hr = disp->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &dispid);
		if (FAILED(hr)) return hr;
		DISPPARAMS params{};
		params.rgvarg = args;
		params.cArgs = cArgs;
		if (flags == DISPATCH_PROPERTYPUT) {
			static DISPID putDispid = DISPID_PROPERTYPUT;
			params.rgdispidNamedArgs = &putDispid;
			params.cNamedArgs = 1;
		}
		return disp->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags,
			&params, result, nullptr, nullptr);
	}

	// Walk the Running Object Table for !VisualStudio.DTE.<ver>:<pid> monikers
	// and return the AddRef'd IDispatch* of the instance whose loaded solution
	// matches `slnPath` (canonical, case-insensitive). Returns nullptr if no
	// instance matches — caller then takes the cold-launch path.
	//
	// On match, also writes the devenv process ID to *outPid (0 if the moniker
	// suffix couldn't be parsed). The caller uses this PID with
	// AllowSetForegroundWindow so the matched VS instance can pull itself to
	// the foreground — without that grant, Windows' foreground-lock policy
	// silently demotes DTE.MainWindow.Activate to a taskbar flash.
	//
	// This replaces the window-title substring match in IsVisualStudioOpen,
	// which could not distinguish between multiple running devenv instances.
	static IDispatch* FindVSDTEForSolution(const std::wstring& slnPath, DWORD* outPid) {
		if (outPid) *outPid = 0;
		if (slnPath.empty()) return nullptr;

		std::error_code ec;
		std::wstring canonTarget = std::filesystem::weakly_canonical(slnPath, ec).wstring();
		if (canonTarget.empty()) canonTarget = slnPath;

		IRunningObjectTable* rot = nullptr;
		if (FAILED(GetRunningObjectTable(0, &rot)) || !rot) return nullptr;

		IBindCtx* bindCtx = nullptr;
		if (FAILED(CreateBindCtx(0, &bindCtx)) || !bindCtx) {
			rot->Release();
			return nullptr;
		}

		IEnumMoniker* enumMoniker = nullptr;
		if (FAILED(rot->EnumRunning(&enumMoniker)) || !enumMoniker) {
			bindCtx->Release();
			rot->Release();
			return nullptr;
		}

		IDispatch* matched = nullptr;
		IMoniker* moniker = nullptr;
		while (!matched && enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
			LPOLESTR displayName = nullptr;
			if (SUCCEEDED(moniker->GetDisplayName(bindCtx, nullptr, &displayName)) && displayName) {
				std::wstring name(displayName);
				CoTaskMemFree(displayName);

				if (name.rfind(L"!VisualStudio.DTE.", 0) == 0) {
					IUnknown* unk = nullptr;
					if (SUCCEEDED(rot->GetObject(moniker, &unk)) && unk) {
						IDispatch* dte = nullptr;
						if (SUCCEEDED(unk->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dte))) && dte) {
							VARIANT solVar; VariantInit(&solVar);
							HRESULT hr = InvokeByName(dte, L"Solution", DISPATCH_PROPERTYGET, nullptr, 0, &solVar);
							if (SUCCEEDED(hr) && solVar.vt == VT_DISPATCH && solVar.pdispVal) {
								VARIANT nameVar; VariantInit(&nameVar);
								hr = InvokeByName(solVar.pdispVal, L"FullName", DISPATCH_PROPERTYGET, nullptr, 0, &nameVar);
								if (SUCCEEDED(hr) && nameVar.vt == VT_BSTR && nameVar.bstrVal) {
									std::wstring full(nameVar.bstrVal, SysStringLen(nameVar.bstrVal));
									if (!full.empty()) {
										std::error_code ec2;
										std::wstring canonFull = std::filesystem::weakly_canonical(full, ec2).wstring();
										if (canonFull.empty()) canonFull = full;
										if (_wcsicmp(canonFull.c_str(), canonTarget.c_str()) == 0) {
											matched = dte;   // transfer ownership (already AddRef'd by QI)
											dte = nullptr;
											if (outPid) {
												// Moniker name is "!VisualStudio.DTE.<ver>:<pid>".
												// Parse the PID suffix; leave 0 on parse failure.
												auto colon = name.find_last_of(L':');
												if (colon != std::wstring::npos && colon + 1 < name.size()) {
													wchar_t* end = nullptr;
													unsigned long pid = wcstoul(name.c_str() + colon + 1, &end, 10);
													if (end && *end == L'\0') *outPid = static_cast<DWORD>(pid);
												}
											}
										}
									}
								}
								VariantClear(&nameVar);
							}
							VariantClear(&solVar);
							if (dte) dte->Release();
						}
						unk->Release();
					}
				}
			}
			moniker->Release();
			moniker = nullptr;
		}

		enumMoniker->Release();
		bindCtx->Release();
		rot->Release();
		return matched;
	}

	// Open `file` in the specific running VS instance represented by `dte`,
	// optionally jumping to `line`. Retries on RPC_E_CALL_REJECTED so the
	// call survives a VS busy with solution load. Returns true on success;
	// caller falls back to a cold launch if false.
	static bool OpenFileViaDTE(IDispatch* dte, const std::wstring& file, int line) {
		if (!dte) return false;
		// vsViewKindPrimary — opens whichever editor is registered as primary
		// for the file extension (avoids forcing the XML/code view).
		const wchar_t* kViewKindPrimary = L"{00000000-0000-0000-0000-000000000000}";

		for (int attempt = 0; attempt < 5; ++attempt) {
			VARIANT itemOpsVar; VariantInit(&itemOpsVar);
			HRESULT hr = InvokeByName(dte, L"ItemOperations", DISPATCH_PROPERTYGET,
				nullptr, 0, &itemOpsVar);
			if (hr == RPC_E_CALL_REJECTED) {
				VariantClear(&itemOpsVar);
				Sleep(500);
				continue;
			}
			if (FAILED(hr) || itemOpsVar.vt != VT_DISPATCH || !itemOpsVar.pdispVal) {
				VariantClear(&itemOpsVar);
				return false;
			}

			BSTR fileBstr = SysAllocStringLen(file.c_str(), static_cast<UINT>(file.size()));
			BSTR viewBstr = SysAllocString(kViewKindPrimary);

			// DISPPARAMS::rgvarg is in REVERSE order:
			// args[0] = 2nd parameter (ViewKind), args[1] = 1st parameter (File).
			VARIANT args[2];
			VariantInit(&args[0]); args[0].vt = VT_BSTR; args[0].bstrVal = viewBstr;
			VariantInit(&args[1]); args[1].vt = VT_BSTR; args[1].bstrVal = fileBstr;

			VARIANT openResult; VariantInit(&openResult);
			hr = InvokeByName(itemOpsVar.pdispVal, L"OpenFile", DISPATCH_METHOD,
				args, 2, &openResult);

			SysFreeString(fileBstr);
			SysFreeString(viewBstr);
			VariantClear(&openResult);
			VariantClear(&itemOpsVar);

			if (hr == RPC_E_CALL_REJECTED) {
				Sleep(500);
				continue;
			}
			if (FAILED(hr)) return false;

			// Bring the matched VS window to the foreground so the user sees
			// the file land. DTE.MainWindow.Activate calls SetForegroundWindow
			// inside devenv, which Windows' foreground-lock policy blocks
			// unless we (the current foreground process) already granted
			// devenv that right via AllowSetForegroundWindow(pid) — see the
			// VS launcher in OpenFile. Belt-and-suspenders: also pull the
			// HWND from MainWindow.HWnd and call SetForegroundWindow /
			// BringWindowToTop ourselves, restoring from minimised first.
			VARIANT mainWinVar; VariantInit(&mainWinVar);
			if (SUCCEEDED(InvokeByName(dte, L"MainWindow", DISPATCH_PROPERTYGET,
				nullptr, 0, &mainWinVar)) && mainWinVar.vt == VT_DISPATCH && mainWinVar.pdispVal)
			{
				VARIANT trueVar; VariantInit(&trueVar);
				trueVar.vt = VT_BOOL; trueVar.boolVal = VARIANT_TRUE;
				InvokeByName(mainWinVar.pdispVal, L"Visible", DISPATCH_PROPERTYPUT,
					&trueVar, 1, nullptr);
				VARIANT actResult; VariantInit(&actResult);
				InvokeByName(mainWinVar.pdispVal, L"Activate", DISPATCH_METHOD,
					nullptr, 0, &actResult);
				VariantClear(&actResult);

				VARIANT hwndVar; VariantInit(&hwndVar);
				if (SUCCEEDED(InvokeByName(mainWinVar.pdispVal, L"HWnd", DISPATCH_PROPERTYGET,
					nullptr, 0, &hwndVar)))
				{
					LONG_PTR raw = 0;
					if (hwndVar.vt == VT_I4)      raw = hwndVar.lVal;
					else if (hwndVar.vt == VT_I8) raw = static_cast<LONG_PTR>(hwndVar.llVal);
					HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(raw));
					if (hwnd && IsWindow(hwnd)) {
						if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
						BringWindowToTop(hwnd);
						SetForegroundWindow(hwnd);
					}
				}
				VariantClear(&hwndVar);
			}
			VariantClear(&mainWinVar);

			// Jump to line via ActiveDocument.Selection.GotoLine(line, Select=false)
			if (line > 0) {
				VARIANT docVar; VariantInit(&docVar);
				if (SUCCEEDED(InvokeByName(dte, L"ActiveDocument", DISPATCH_PROPERTYGET,
					nullptr, 0, &docVar)) && docVar.vt == VT_DISPATCH && docVar.pdispVal)
				{
					VARIANT selVar; VariantInit(&selVar);
					if (SUCCEEDED(InvokeByName(docVar.pdispVal, L"Selection", DISPATCH_PROPERTYGET,
						nullptr, 0, &selVar)) && selVar.vt == VT_DISPATCH && selVar.pdispVal)
					{
						VARIANT gotoArgs[2];
						VariantInit(&gotoArgs[0]); gotoArgs[0].vt = VT_BOOL; gotoArgs[0].boolVal = VARIANT_FALSE;
						VariantInit(&gotoArgs[1]); gotoArgs[1].vt = VT_I4;   gotoArgs[1].lVal    = line;
						VARIANT gotoResult; VariantInit(&gotoResult);
						InvokeByName(selVar.pdispVal, L"GotoLine", DISPATCH_METHOD,
							gotoArgs, 2, &gotoResult);
						VariantClear(&gotoResult);
					}
					VariantClear(&selVar);
				}
				VariantClear(&docVar);
			}

			return true;
		}
		return false;
	}
#endif

	void ExternalEditor::OpenFile(const std::string& filePath, int line) {
		DetectEditors();

		if (s_Editors.empty()) {
			IDX_CORE_WARN_TAG("ExternalEditor", "No code editor available, falling back to OS default");
#ifdef IDX_PLATFORM_WINDOWS
			std::wstring wpath = Utf8ToWide(filePath);
			ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
			return;
		}

		const auto& editor = GetSelected();
		std::string absPath = std::filesystem::absolute(filePath).string();

		std::string ext = std::filesystem::path(filePath).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// Resolve .sln path for VS/Rider, folder path for VS Code
		std::string slnPath = ResolveProjectContext(filePath, ext);
		std::string projectFolder;
		IndexProject* project = ProjectManager::GetCurrentProject();
		if (project) projectFolder = project->RootDirectory;

		IDX_CORE_INFO_TAG("ExternalEditor", "Opening '{}' with {} (sln: {})",
			absPath, editor.DisplayName, slnPath.empty() ? "none" : slnPath);

#ifdef IDX_PLATFORM_WINDOWS
		std::string fullCmd;

		switch (editor.Type) {
		case ExternalEditorType::VSCode: {
			// VS Code needs the FOLDER (not .sln) as primary argument for IntelliSense.
			// code "projectFolder" --reuse-window --goto "file:line:col"
			// If already open, --reuse-window makes VS Code focus the existing window.
			fullCmd = "\"" + editor.ExecutablePath + "\"";
			if (!projectFolder.empty())
				fullCmd += " \"" + projectFolder + "\"";
			fullCmd += " --reuse-window";
			if (line > 0)
				fullCmd += " --goto \"" + absPath + ":" + std::to_string(line) + ":1\"";
			else
				fullCmd += " --goto \"" + absPath + ":1:1\"";
			break;
		}

		case ExternalEditorType::VisualStudio: {
			// VS2022 approach (instance-targeted via COM Running Object Table):
			//
			//   1. Enumerate running devenv instances via the ROT and find the
			//      one whose loaded Solution.FullName matches our .sln path.
			//      If found → call DTE.ItemOperations.OpenFile on THAT instance.
			//      This handles both "this project already open" and the
			//      "multiple VS instances running" cases without ambiguity.
			//
			//   2. If no running VS has our .sln loaded, cold-launch devenv
			//      with the .sln AND the file as trailing arguments in a SINGLE
			//      CreateProcessW call. The new instance loads the solution and
			//      opens the file inside itself — no Sleep, no /edit follow-up
			//      (which is the bug that routed files to the wrong VS instance
			//      when an unrelated devenv was already running).
			//
			//   3. Last-resort fallback when there is no .sln context: plain
			//      `devenv /edit "file"`. Behaviour unchanged from before.
			std::string devenvPath = editor.ExecutablePath;
			std::string sln = slnPath;
			std::string file = absPath;
			int lineNum = line;

			// All COM work (ROT enumeration + DTE.OpenFile) runs inside the
			// tracked launcher thread so the main editor thread never blocks
			// on a busy/loading VS. JoinPendingLaunchThreads still drains us
			// on shutdown.
			std::thread vsLauncher([devenvPath, sln, file, lineNum]() {
				HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				const bool needUninit = SUCCEEDED(comHr);

				std::wstring wsln = Utf8ToWide(sln);
				std::wstring wfile = Utf8ToWide(file);

				DWORD matchedPid = 0;
				IDispatch* matchedDTE = sln.empty()
					? nullptr
					: FindVSDTEForSolution(wsln, &matchedPid);
				bool handled = false;
				if (matchedDTE) {
					IDX_CORE_INFO_TAG("ExternalEditor",
						"Matched running VS instance (pid={}) for sln '{}' — opening file via DTE",
						matchedPid, sln);
					// Grant devenv the right to take foreground once. Index-Editor
					// is the foreground process (user just double-clicked here),
					// so the grant is accepted. Without it, the file opens but
					// VS stays behind our window. Best-effort: ignore the BOOL
					// return — failure just means the user has to alt-tab.
					if (matchedPid != 0) AllowSetForegroundWindow(matchedPid);
					handled = OpenFileViaDTE(matchedDTE, wfile, lineNum);
					if (!handled) {
						IDX_CORE_WARN_TAG("ExternalEditor",
							"DTE.OpenFile failed on matched instance, falling back to cold launch");
					}
					matchedDTE->Release();
				}

				if (!handled) {
					std::string cmd;
					if (!sln.empty()) {
						// Combined launch: one devenv invocation loads the
						// solution AND opens the file inside the SAME new
						// instance. This is what fixes the case-3 bug.
						cmd = "\"" + devenvPath + "\" \"" + sln + "\" \"" + file + "\"";
						if (lineNum > 0) {
							cmd += " /command \"Edit.GoTo " + std::to_string(lineNum) + "\"";
						}
						IDX_CORE_INFO_TAG("ExternalEditor",
							"No running VS matched sln '{}' — cold launching devenv with sln + file",
							sln);
					}
					else {
						cmd = "\"" + devenvPath + "\" /edit \"" + file + "\"";
						IDX_CORE_WARN_TAG("ExternalEditor",
							"No .sln context — falling back to devenv /edit '{}'", file);
					}

					std::wstring wcmd = Utf8ToWide(cmd);
					std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
					buf.push_back(L'\0');

					STARTUPINFOW si{}; si.cb = sizeof(si);
					PROCESS_INFORMATION pi{};
					if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
						FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
					{
						CloseHandle(pi.hProcess);
						CloseHandle(pi.hThread);
					}
				}

				if (needUninit) CoUninitialize();
			});
			TrackLaunchThread(std::move(vsLauncher));

			// Return early — the thread handles everything (matching the
			// original cold-start path's contract).
			return;
		}

		case ExternalEditorType::Rider: {
			// rider "Project.sln" "file.cs"
			// .sln is the PROJECT-LOAD argument (loads Solution Explorer + IntelliSense).
			// Second argument is the file to navigate to.
			// Rider auto-reuses if same .sln is already open.
			fullCmd = "\"" + editor.ExecutablePath + "\"";
			if (!slnPath.empty())
				fullCmd += " \"" + slnPath + "\"";
			if (line > 0)
				fullCmd += " --line " + std::to_string(line);
			fullCmd += " \"" + absPath + "\"";
			break;
		}

		default: {
			fullCmd = "\"" + editor.ExecutablePath + "\" \"" + absPath + "\"";
			break;
		}
		}

		// Launch async so we don't block the engine editor.
		// M29: tracked instead of detached so editor shutdown can join.
		std::thread launcher([fullCmd]() {
			std::wstring wcmd = Utf8ToWide(fullCmd);
			std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
			buf.push_back(L'\0');

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};

			if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
				FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
			{
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}
		});
		TrackLaunchThread(std::move(launcher));
#endif
	}

}
