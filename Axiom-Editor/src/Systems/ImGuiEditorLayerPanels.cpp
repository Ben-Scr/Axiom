#include <pch.hpp>
#include "Systems/ImGuiEditorLayer.hpp"

#include <imgui.h>

#include "Assets/AssetRegistry.hpp"
#include "Core/Application.hpp"
#include "Core/Window.hpp"
#include "Graphics/ImageData.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureManager.hpp"
#include "Gui/EditorIcons.hpp"
#include "Gui/ImGuiUtils.hpp"
#include "Inspector/ReferencePicker.hpp"
#include "Packages/GitHubSource.hpp"
#include "Packages/NuGetSource.hpp"
#include "Project/AxiomProject.hpp"
#include "Project/ProjectManager.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneManager.hpp"
#include "Scripting/ScriptDiscovery.hpp"
#include "Scripting/ScriptEngine.hpp"
#include "Scripting/ScriptSystem.hpp"
#include "Serialization/Directory.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SceneSerializer.hpp"
#include "Utils/Process.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <unordered_set>

#ifdef AIM_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Axiom {
	namespace {
		// Hook the just-submitted ImGui item (e.g. a Browse... button) as a
		// drop target accepting an Asset Browser file payload. The callback
		// runs once on a successful drop with the dropped item's absolute
		// path. `extWhitelist` filters by extension (lower-cased, dot-
		// included, e.g. {".png", ".jpg"}); pass an empty span to accept any
		// file. Returns true when a valid drop was applied. Used by every
		// Browse... button in the Project Settings panel so the user can
		// drag-drop assets onto the field as an alternative to the picker.
		bool BrowseAcceptImageDrop(std::span<const std::string_view> extWhitelist,
			const std::function<void(const std::string&)>& onDrop)
		{
			if (!ImGui::BeginDragDropTarget()) return false;
			bool applied = false;
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
				if (payload->Data && payload->DataSize > 0) {
					std::string dropped(static_cast<const char*>(payload->Data),
						static_cast<std::size_t>(payload->DataSize));
					if (!dropped.empty() && dropped.back() == '\0') dropped.pop_back();
					if (!dropped.empty()) {
						bool extOk = extWhitelist.empty();
						if (!extOk) {
							std::string ext = std::filesystem::path(dropped).extension().string();
							std::transform(ext.begin(), ext.end(), ext.begin(),
								[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							for (std::string_view allowed : extWhitelist) {
								if (ext == allowed) { extOk = true; break; }
							}
						}
						if (extOk) {
							onDrop(dropped);
							applied = true;
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
			return applied;
		}

		// Convert a dropped (or picked) absolute asset path to the
		// project-relative form the project file persists. Mirrors the
		// post-picker logic each settings slot already runs: prefer
		// `Assets/foo.png` when the file lives under the project's
		// Assets dir; otherwise fall back to the bare filename so the
		// stored path stays portable across project relocations.
		std::string ToProjectRelativeAssetPath(const std::filesystem::path& absPath,
			const std::string& assetsDirectory)
		{
			std::filesystem::path assetsDir(assetsDirectory);
			if (absPath.string().find(assetsDir.string()) == 0) {
				return std::filesystem::relative(absPath, assetsDir.parent_path()).string();
			}
			return absPath.filename().string();
		}

		// Shared image-extension whitelist for every texture-typed
		// Browse... drop site (App Icon, Splash Image, Cursors). Matches
		// the AssetKind::Texture filter the picker uses.
		constexpr std::string_view k_ImageExts[] = {
			".png", ".jpg", ".jpeg", ".bmp"
		};

		bool NeedsCopy(const std::filesystem::path& src, const std::filesystem::path& dest) {
			if (!std::filesystem::exists(dest)) return true;
			try {
				return std::filesystem::last_write_time(src) > std::filesystem::last_write_time(dest);
			}
			catch (...) {
				return true;
			}
		}

		int CopyDirIncremental(const std::filesystem::path& srcDir, const std::filesystem::path& destDir) {
			int copied = 0;
			std::filesystem::create_directories(destDir);
			for (auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
				auto rel = std::filesystem::relative(entry.path(), srcDir);
				auto dest = destDir / rel;
				try {
					if (entry.is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(entry.path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		// Variant that skips any file or directory whose path-relative-to-srcDir
		// has a leading segment matching one of `excludedSegments`. Used to keep
		// editor-only assets (AxiomAssets/Textures/Editor) out of shipped builds.
		// Match is case-insensitive on Windows so the Editor folder excludes
		// regardless of how the user cased it on disk.
		int CopyDirIncrementalExcluding(const std::filesystem::path& srcDir,
			const std::filesystem::path& destDir,
			const std::vector<std::filesystem::path>& excludedRelativePaths)
		{
			int copied = 0;
			std::filesystem::create_directories(destDir);

			auto isExcluded = [&](const std::filesystem::path& rel) {
				const std::string relStr = rel.generic_string();
				for (const auto& excluded : excludedRelativePaths) {
					const std::string excStr = excluded.generic_string();
					// Match the excluded path itself or anything underneath it.
#ifdef AIM_PLATFORM_WINDOWS
					if (relStr.size() >= excStr.size()
						&& _strnicmp(relStr.c_str(), excStr.c_str(), excStr.size()) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#else
					if (relStr.size() >= excStr.size()
						&& relStr.compare(0, excStr.size(), excStr) == 0
						&& (relStr.size() == excStr.size() || relStr[excStr.size()] == '/'))
					{
						return true;
					}
#endif
				}
				return false;
			};

			for (auto it = std::filesystem::recursive_directory_iterator(srcDir);
				it != std::filesystem::recursive_directory_iterator(); ++it)
			{
				auto rel = std::filesystem::relative(it->path(), srcDir);
				if (isExcluded(rel)) {
					if (it->is_directory()) {
						it.disable_recursion_pending();
					}
					continue;
				}
				auto dest = destDir / rel;
				try {
					if (it->is_directory()) {
						std::filesystem::create_directories(dest);
					}
					else if (NeedsCopy(it->path(), dest)) {
						std::filesystem::create_directories(dest.parent_path());
						std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing);
						copied++;
					}
				}
				catch (...) {
				}
			}
			return copied;
		}

		std::string GetRuntimeExecutableFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "Axiom-Runtime.exe";
#else
			return "Axiom-Runtime";
#endif
		}

#ifdef AIM_PLATFORM_WINDOWS
		// Build a single-image .ico blob containing a 32-bit BMP DIB from a
		// PNG/JPEG/etc. that the engine's Texture2D class can decode. We
		// reuse Texture2D so we don't have to pull stb_image into the editor
		// (the engine DLL doesn't dllexport stb's symbols). The texture
		// round-trips through GL — it's a one-shot per build, so the cost
		// is irrelevant.
		//
		// Layout produced (matches what Win32's RT_ICON / RT_GROUP_ICON
		// loaders expect):
		//   [IconDir (6 bytes)]
		//   [IcoEntry (16 bytes)]
		//   [BITMAPINFOHEADER (40 bytes)] biHeight = 2*H (XOR + AND mask)
		//   [BGRA pixels, bottom-up] W * H * 4 bytes
		//   [AND mask, 1bpp, 4-byte aligned rows] all zero (alpha-channel cutout)
		std::string BuildIcoBytesFromImage(const std::filesystem::path& imagePath,
			std::vector<std::uint8_t>& outIcoBytes)
		{
#pragma pack(push, 1)
			struct IconDirHdr {
				std::uint16_t Reserved;
				std::uint16_t Type;
				std::uint16_t Count;
			};
			struct IcoEntryHdr {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint32_t ImageOffset;
			};
			struct BitmapInfoHdr {
				std::uint32_t biSize;
				std::int32_t  biWidth;
				std::int32_t  biHeight;
				std::uint16_t biPlanes;
				std::uint16_t biBitCount;
				std::uint32_t biCompression;
				std::uint32_t biSizeImage;
				std::int32_t  biXPelsPerMeter;
				std::int32_t  biYPelsPerMeter;
				std::uint32_t biClrUsed;
				std::uint32_t biClrImportant;
			};
#pragma pack(pop)

			// flipVertical=false so we read pixels in the PNG's natural
			// top-down order; we'll flip + BGRA-swizzle on the way into the
			// DIB. generateMipmaps=false because we only need level 0.
			Texture2D tex(imagePath.string().c_str(), Filter::Point,
				Wrap::Clamp, Wrap::Clamp,
				/*generateMipmaps*/ false,
				/*srgb*/ false,
				/*flipVertical*/ false);
			if (!tex.IsValid()) {
				return "failed to decode image";
			}

			std::unique_ptr<ImageData> img = tex.GetImageData();
			if (!img) {
				return "failed to read decoded pixels";
			}

			const int width = img->Width;
			const int height = img->Height;
			if (width <= 0 || height <= 0) {
				return "image has zero size";
			}
			// Win32 ICO entries store width/height in 8-bit fields with 0
			// meaning "256 or larger". We refuse > 256 — Explorer scales the
			// largest entry it finds anyway, and accidentally writing a 257
			// would silently truncate to 1 in the on-disk entry.
			if (width > 256 || height > 256) {
				return "image exceeds 256x256 (use a smaller icon source)";
			}

			const std::uint32_t pixelBytes =
				static_cast<std::uint32_t>(width) *
				static_cast<std::uint32_t>(height) * 4u;
			// AND mask: 1 bit per pixel, each row padded to a 4-byte boundary.
			const std::uint32_t maskRowBytes =
				((static_cast<std::uint32_t>(width) + 31u) / 32u) * 4u;
			const std::uint32_t maskBytes = maskRowBytes * static_cast<std::uint32_t>(height);
			const std::uint32_t dibBytes = sizeof(BitmapInfoHdr) + pixelBytes + maskBytes;

			outIcoBytes.assign(sizeof(IconDirHdr) + sizeof(IcoEntryHdr) + dibBytes, 0);

			IconDirHdr dir{};
			dir.Reserved = 0;
			dir.Type = 1;
			dir.Count = 1;
			std::memcpy(outIcoBytes.data(), &dir, sizeof(dir));

			IcoEntryHdr entry{};
			entry.Width = static_cast<std::uint8_t>(width == 256 ? 0 : width);
			entry.Height = static_cast<std::uint8_t>(height == 256 ? 0 : height);
			entry.ColorCount = 0;
			entry.Reserved = 0;
			entry.Planes = 1;
			entry.BitCount = 32;
			entry.BytesInRes = dibBytes;
			entry.ImageOffset = sizeof(IconDirHdr) + sizeof(IcoEntryHdr);
			std::memcpy(outIcoBytes.data() + sizeof(IconDirHdr), &entry, sizeof(entry));

			BitmapInfoHdr bih{};
			bih.biSize = sizeof(BitmapInfoHdr);
			bih.biWidth = width;
			// 2*H is the ICO convention: top half is the XOR (color) bitmap,
			// bottom half is the AND mask. biHeight stays positive — the DIB
			// is bottom-up.
			bih.biHeight = height * 2;
			bih.biPlanes = 1;
			bih.biBitCount = 32;
			bih.biCompression = 0; // BI_RGB
			bih.biSizeImage = pixelBytes + maskBytes;
			std::uint8_t* dibStart = outIcoBytes.data() + entry.ImageOffset;
			std::memcpy(dibStart, &bih, sizeof(bih));

			// Swizzle RGBA top-down -> BGRA bottom-up. ImageData's pixels are
			// laid out row by row with row 0 at the top; the DIB needs row 0
			// at the bottom.
			const std::uint8_t* srcPixels = img->Pixels;
			std::uint8_t* dstPixels = dibStart + sizeof(BitmapInfoHdr);
			for (int y = 0; y < height; ++y) {
				const std::uint8_t* srcRow = srcPixels + (height - 1 - y) * width * 4;
				std::uint8_t* dstRow = dstPixels + y * width * 4;
				for (int x = 0; x < width; ++x) {
					dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B
					dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
					dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R
					dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
				}
			}
			// AND mask region is already zero-initialised (assign() above).
			// All-zero AND mask means "use the alpha channel for transparency",
			// which is what we want for 32-bit icons.

			return std::string{};
		}

		// Read an existing .ico file straight into a byte vector. Used when
		// the user supplied a real .ico (no decode/re-encode needed).
		std::string ReadIcoFileBytes(const std::filesystem::path& icoPath,
			std::vector<std::uint8_t>& outIcoBytes)
		{
			std::ifstream in(icoPath, std::ios::binary);
			if (!in) return "open .ico failed";
			outIcoBytes.assign((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			return std::string{};
		}

		// Identifier of an existing RT_ICON / RT_GROUP_ICON resource. Win32
		// names a resource either by a numeric ordinal (the high 16 bits of
		// the LPSTR are zero) or by a string. We capture both forms so we
		// can hand the original LPSTR-encoded name back to UpdateResource.
		struct ExistingResName {
			bool IsNumeric = false;
			WORD NumericId = 0;
			std::string StringName;
		};

		// Enumerate every resource of `resType` that the .exe at `exePath`
		// already carries. Used so EmbedIconIntoExecutable can DELETE the
		// runtime's pristine icon (whatever name `Axiom-Runtime/icon.rc`
		// happened to compile it under) before adding our own — leaving a
		// stale RT_GROUP_ICON alongside ours risks Explorer picking the
		// wrong one. Returns empty vector and sets error string on failure.
		std::vector<ExistingResName> EnumerateExistingResources(
			const std::string& exePath, WORD resType, std::string& outError)
		{
			std::vector<ExistingResName> out;
			HMODULE hMod = ::LoadLibraryExA(exePath.c_str(), nullptr,
				LOAD_LIBRARY_AS_DATAFILE);
			if (!hMod) {
				const DWORD err = ::GetLastError();
				outError = "LoadLibraryEx failed (error " +
					std::to_string(err) + ")";
				return out;
			}
			struct Ctx { std::vector<ExistingResName>* List; };
			Ctx ctx{ &out };
			auto cb = [](HMODULE, LPCSTR, LPSTR lpName, LONG_PTR lParam) -> BOOL {
				auto* c = reinterpret_cast<Ctx*>(lParam);
				ExistingResName r{};
				if ((reinterpret_cast<ULONG_PTR>(lpName) >> 16) == 0) {
					r.IsNumeric = true;
					r.NumericId = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(lpName) & 0xFFFF);
				}
				else {
					r.IsNumeric = false;
					r.StringName = lpName;
				}
				c->List->push_back(std::move(r));
				return TRUE;
			};
			if (!::EnumResourceNamesA(hMod, MAKEINTRESOURCEA(resType),
				cb, reinterpret_cast<LONG_PTR>(&ctx)))
			{
				const DWORD err = ::GetLastError();
				// ERROR_RESOURCE_TYPE_NOT_FOUND (1813) just means the .exe
				// has no resources of this type — return an empty list.
				if (err != ERROR_RESOURCE_TYPE_NOT_FOUND
					&& err != ERROR_RESOURCE_DATA_NOT_FOUND)
				{
					outError = "EnumResourceNames failed (error " +
						std::to_string(err) + ")";
				}
			}
			::FreeLibrary(hMod);
			return out;
		}

		// Replace the RT_GROUP_ICON / RT_ICON resources of `exePath` with the
		// icons in `icoBytes` (the contents of a .ico — either read from disk
		// or built from a PNG by BuildIcoBytesFromImage). Returns an empty
		// string on success or a human-readable error description on failure.
		// The runtime exe is compiled with `1 ICON "icon.ico"`
		// (Axiom-Runtime/icon.rc), so we overwrite group ID 1 — Explorer /
		// taskbar / Alt+Tab pick the new icon up immediately because we use
		// the same resource name. Pre-existing RT_GROUP_ICON / RT_ICON
		// resources under any OTHER name (e.g. older `IDI_ICON1` builds where
		// rc.exe with no `#define` compiled the symbol as a STRING-named
		// resource) are deleted as part of the same update so Explorer can't
		// fall through to a stale group and render the default Axiom icon.
		// Wraps EmbedIconIntoExecutableOnce with a small retry loop.
		// Error 1359 (ERROR_INTERNAL_ERROR) and the kernel-level "another
		// process has the file open" failures from BeginUpdateResource
		// are nearly always transient: the runtime exe was copy_file'd
		// moments ago and Windows / antivirus / Explorer haven't fully
		// released their handles yet. Sleep + retry a handful of times
		// before giving up so a Build that races AV doesn't randomly
		// fail to update the taskbar icon. (The function is defined
		// later in this file; declaration here keeps the wrapper next
		// to the public entry point users will call.)
		std::string EmbedIconIntoExecutableOnce(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes);

		std::string EmbedIconIntoExecutable(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes)
		{
			constexpr int k_MaxAttempts = 6;
			constexpr int k_BaseDelayMs = 150;
			std::string lastErr;
			for (int attempt = 0; attempt < k_MaxAttempts; ++attempt) {
				lastErr = EmbedIconIntoExecutableOnce(exePath, icoBytes);
				if (lastErr.empty()) return {};
				// Only retry on the well-known transient failure modes;
				// other errors (malformed .ico, ico-out-of-bounds, etc.)
				// won't fix themselves with a delay.
				const bool transient = lastErr.find("error 1359") != std::string::npos
					|| lastErr.find("error 32") != std::string::npos       // ERROR_SHARING_VIOLATION
					|| lastErr.find("error 33") != std::string::npos       // ERROR_LOCK_VIOLATION
					|| lastErr.find("BeginUpdateResource") != std::string::npos;
				if (!transient) return lastErr;
				::Sleep(static_cast<DWORD>(k_BaseDelayMs * (attempt + 1)));
			}
			return lastErr.empty() ? std::string("icon embed retry exhausted") : lastErr;
		}

		std::string EmbedIconIntoExecutableOnce(const std::string& exePath,
			const std::vector<std::uint8_t>& icoBytes)
		{
#pragma pack(push, 1)
			struct IconDir {
				std::uint16_t Reserved;
				std::uint16_t Type;
				std::uint16_t Count;
			};
			struct IcoEntry {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint32_t ImageOffset;
			};
			// The wire-format RT_GROUP_ICON entry differs from the on-disk
			// .ico entry: the trailing field is a 16-bit resource ID rather
			// than a 32-bit byte offset, so the per-entry size is 14 bytes
			// instead of 16. Pack to avoid trailing padding the loader would
			// reject.
			struct GroupEntry {
				std::uint8_t Width;
				std::uint8_t Height;
				std::uint8_t ColorCount;
				std::uint8_t Reserved;
				std::uint16_t Planes;
				std::uint16_t BitCount;
				std::uint32_t BytesInRes;
				std::uint16_t Id;
			};
#pragma pack(pop)

			if (icoBytes.size() < sizeof(IconDir)) return "ico too small";

			IconDir dir{};
			std::memcpy(&dir, icoBytes.data(), sizeof(dir));
			if (dir.Type != 1 || dir.Count == 0) return "not a .ico";

			const std::size_t entriesEnd = sizeof(IconDir)
				+ static_cast<std::size_t>(dir.Count) * sizeof(IcoEntry);
			if (icoBytes.size() < entriesEnd) return "ico header truncated";

			std::vector<IcoEntry> entries(dir.Count);
			std::memcpy(entries.data(), icoBytes.data() + sizeof(IconDir),
				entries.size() * sizeof(IcoEntry));

			// Snapshot pre-existing icon resource names BEFORE opening the
			// update handle (LoadLibraryEx + UpdateResource on the same
			// file have to be sequential). Whatever names rc.exe compiled
			// `Axiom-Runtime/icon.rc` under, we delete them in the update
			// pass below — without that, Explorer is free to pick a stale
			// group instead of the one we add at numeric ID 1.
			std::string enumErr;
			std::vector<ExistingResName> existingGroups =
				EnumerateExistingResources(exePath, 14 /*RT_GROUP_ICON*/, enumErr);
			if (!enumErr.empty()) return enumErr;
			std::vector<ExistingResName> existingIcons =
				EnumerateExistingResources(exePath, 3 /*RT_ICON*/, enumErr);
			if (!enumErr.empty()) return enumErr;

			HANDLE hUpdate = ::BeginUpdateResourceA(exePath.c_str(), FALSE);
			if (!hUpdate) {
				return "BeginUpdateResource failed (error " +
					std::to_string(::GetLastError()) + ")";
			}

			// Delete every pre-existing RT_GROUP_ICON / RT_ICON. Win32 spec:
			// passing lpData=NULL, cbData=0 to UpdateResource removes that
			// resource from the in-progress update. Failing to delete one
			// (returns FALSE) is non-fatal — log nothing and keep going so a
			// single oddity can't sabotage the entire embed.
			auto deleteRes = [&](WORD resType, const ExistingResName& r) {
				LPSTR name = r.IsNumeric
					? MAKEINTRESOURCEA(r.NumericId)
					: const_cast<LPSTR>(r.StringName.c_str());
				::UpdateResourceA(hUpdate, MAKEINTRESOURCEA(resType), name,
					MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
					nullptr, 0);
			};
			for (const auto& r : existingGroups) deleteRes(14, r);
			for (const auto& r : existingIcons)  deleteRes(3, r);

			// Write each RT_ICON. We pick fresh IDs (101..N) — the runtime's
			// icon.rc compiled into RT_ICON IDs that we don't read back, so
			// using a high base avoids any overlap with the existing entries
			// we're about to abandon. The RT_GROUP_ICON we write next ties
			// these IDs together for the Win32 loader.
			constexpr WORD k_BaseIconId = 101;
			std::vector<GroupEntry> groupEntries(dir.Count);
			for (std::size_t i = 0; i < entries.size(); ++i) {
				const IcoEntry& e = entries[i];
				if (e.ImageOffset + e.BytesInRes > icoBytes.size()) {
					::EndUpdateResourceA(hUpdate, TRUE);
					return "ico entry out of bounds";
				}
				const WORD iconId = static_cast<WORD>(k_BaseIconId + i);
				// Bypass RT_ICON / RT_GROUP_ICON macros — those expand
				// through MAKEINTRESOURCE which is the WIDE (LPWSTR) form
				// when UNICODE is defined (the default for VS projects),
				// triggering a type mismatch against UpdateResourceA's
				// LPCSTR. Use MAKEINTRESOURCEA directly with the documented
				// numeric IDs (RT_ICON=3, RT_GROUP_ICON=14).
				if (!::UpdateResourceA(hUpdate,
					MAKEINTRESOURCEA(3),
					MAKEINTRESOURCEA(iconId),
					MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
					const_cast<std::uint8_t*>(icoBytes.data()) + e.ImageOffset,
					e.BytesInRes))
				{
					const DWORD err = ::GetLastError();
					::EndUpdateResourceA(hUpdate, TRUE);
					return "UpdateResource(RT_ICON) failed (error " +
						std::to_string(err) + ")";
				}
				groupEntries[i] = GroupEntry{
					e.Width, e.Height, e.ColorCount, e.Reserved,
					e.Planes, e.BitCount, e.BytesInRes, iconId,
				};
			}

			std::vector<std::uint8_t> groupBlob(sizeof(IconDir)
				+ groupEntries.size() * sizeof(GroupEntry));
			std::memcpy(groupBlob.data(), &dir, sizeof(IconDir));
			std::memcpy(groupBlob.data() + sizeof(IconDir),
				groupEntries.data(),
				groupEntries.size() * sizeof(GroupEntry));

			// Group ID 1 matches Axiom-Runtime/icon.rc — overwriting at the
			// same ID swaps Explorer's icon for the new one without leaving
			// the old group alongside. RT_GROUP_ICON's numeric ID is 14.
			if (!::UpdateResourceA(hUpdate,
				MAKEINTRESOURCEA(14),
				MAKEINTRESOURCEA(1),
				MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
				groupBlob.data(),
				static_cast<DWORD>(groupBlob.size())))
			{
				const DWORD err = ::GetLastError();
				::EndUpdateResourceA(hUpdate, TRUE);
				return "UpdateResource(RT_GROUP_ICON) failed (error " +
					std::to_string(err) + ")";
			}

			if (!::EndUpdateResourceA(hUpdate, FALSE)) {
				return "EndUpdateResource failed (error " +
					std::to_string(::GetLastError()) + ")";
			}
			return std::string{};
		}
#endif

		std::string GetEngineRuntimeFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "Axiom-Engine.dll";
#elif defined(__APPLE__)
			return "libAxiom-Engine.dylib";
#else
			return "libAxiom-Engine.so";
#endif
		}

		std::string GetNetHostRuntimeFilename() {
#if defined(AIM_PLATFORM_WINDOWS)
			return "nethost.dll";
#else
			return "libnethost.so";
#endif
		}

		// Filename of a premake `kind "SharedLib"` target on the current platform.
		// Windows: <Name>.dll, Linux: lib<Name>.so, macOS: lib<Name>.dylib.
		std::string SharedLibraryFilename(std::string_view projectName) {
#if defined(AIM_PLATFORM_WINDOWS)
			return std::string(projectName) + ".dll";
#elif defined(__APPLE__)
			return "lib" + std::string(projectName) + ".dylib";
#else
			return "lib" + std::string(projectName) + ".so";
#endif
		}

		std::string NormalizePreviewTexturePath(const std::filesystem::path& path) {
			std::error_code ec;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
			if (ec) {
				ec.clear();
				normalized = std::filesystem::absolute(path, ec);
				if (ec) {
					normalized = path.lexically_normal();
				}
			}

			return normalized.lexically_normal().make_preferred().string();
		}

		bool IsLogEntryVisible(Log::Level level, bool showInfo, bool showWarn, bool showError) {
			if (level <= Log::Level::Info) return showInfo;
			if (level == Log::Level::Warn) return showWarn;
			return showError;
		}

		const char* GetLogLevelPrefix(Log::Level level) {
			if (level <= Log::Level::Info) return "[Info] ";
			if (level == Log::Level::Warn) return "[Warn] ";
			return "[Error] ";
		}

		ImVec4 GetLogLevelColor(Log::Level level) {
			if (level <= Log::Level::Info) return ImVec4(0.86f, 0.88f, 0.92f, 1.0f);
			if (level == Log::Level::Warn) return ImVec4(1.0f, 0.78f, 0.22f, 1.0f);
			return ImVec4(1.0f, 0.32f, 0.32f, 1.0f);
		}
	}

	void ImGuiEditorLayer::RenderLogPanel() {
		DrainPendingLogEntries();
		ImGui::Begin("Log");

		if (ImGui::Button("Clear")) {
			ClearLogEntries();
		}

		int infoCount = 0, warnCount = 0, errorCount = 0;
		for (const auto& entry : m_LogEntries) {
			if (entry.Level <= Log::Level::Info) infoCount++;
			else if (entry.Level == Log::Level::Warn) warnCount++;
			else errorCount++;
		}

		ImGui::SameLine();
		ImGui::TextDisabled("|");
		ImGui::SameLine();

		{
			unsigned int infoIcon = EditorIcons::Get("info", 16);
			if (infoIcon) {
				ImVec4 tint = m_ShowLogInfo ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
				if (ImGui::ImageButton("##FilterInfo",
					static_cast<ImTextureID>(static_cast<intptr_t>(infoIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogInfo = !m_ShowLogInfo;
				}
			}
			else if (ImGui::SmallButton(m_ShowLogInfo ? "[I]" : "( )")) {
				m_ShowLogInfo = !m_ShowLogInfo;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Info (%d)", infoCount);
		}

		ImGui::SameLine();

		{
			unsigned int warnIcon = EditorIcons::Get("warning", 16);
			ImVec4 tint = m_ShowLogWarn ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (warnIcon) {
				if (ImGui::ImageButton("##FilterWarn",
					static_cast<ImTextureID>(static_cast<intptr_t>(warnIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogWarn ? "W" : "(W)")) {
					m_ShowLogWarn = !m_ShowLogWarn;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warnings (%d)", warnCount);
		}

		ImGui::SameLine();

		{
			unsigned int errIcon = EditorIcons::Get("error", 16);
			ImVec4 tint = m_ShowLogError ? ImVec4(1, 1, 1, 1) : ImVec4(0.4f, 0.4f, 0.4f, 0.5f);
			if (errIcon) {
				if (ImGui::ImageButton("##FilterError",
					static_cast<ImTextureID>(static_cast<intptr_t>(errIcon)),
					ImVec2(14, 14), ImVec2(0, 1), ImVec2(1, 0), ImVec4(0, 0, 0, 0), tint)) {
					m_ShowLogError = !m_ShowLogError;
				}
			}
			else {
				ImGui::PushStyleColor(ImGuiCol_Text, tint);
				if (ImGui::SmallButton(m_ShowLogError ? "E" : "(E)")) {
					m_ShowLogError = !m_ShowLogError;
				}
				ImGui::PopStyleColor();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Errors (%d)", errorCount);
		}

		ImGui::Separator();

		ImGui::BeginChild("##LogEntries", ImVec2(-1.0f, -1.0f), true,
			ImGuiWindowFlags_HorizontalScrollbar);

		const bool wasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
		for (const auto& entry : m_LogEntries) {
			if (!IsLogEntryVisible(entry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
				continue;
			}

			const std::string line = std::string(GetLogLevelPrefix(entry.Level)) + entry.Message;
			ImGui::PushStyleColor(ImGuiCol_Text, GetLogLevelColor(entry.Level));
			ImGui::TextWrapped("%s", line.c_str());
			ImGui::PopStyleColor();
		}

		if (wasAtBottom) {
			ImGui::SetScrollHereY(1.0f);
		}

		if (ImGui::BeginPopupContextWindow("##LogTextCtx")) {
			if (ImGui::MenuItem("Copy All Visible")) {
				std::string all;
				for (const auto& visibleEntry : m_LogEntries) {
					if (!IsLogEntryVisible(visibleEntry.Level, m_ShowLogInfo, m_ShowLogWarn, m_ShowLogError)) {
						continue;
					}
					all += std::string(GetLogLevelPrefix(visibleEntry.Level)) + visibleEntry.Message + "\n";
				}
				ImGui::SetClipboardText(all.c_str());
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderProjectPanel() {
		if (!m_AssetBrowserInitialized) {
			std::string assetsRoot;
			AxiomProject* project = ProjectManager::GetCurrentProject();
			if (project) assetsRoot = project->AssetsDirectory;
			else assetsRoot = Path::Combine(Path::ExecutableDir(), "Assets");

			if (!Directory::Exists(assetsRoot)) {
				Directory::Create(assetsRoot);
			}

			m_AssetBrowser.Initialize(assetsRoot);
			m_AssetBrowserInitialized = true;
		}

		m_AssetBrowser.Render();
		if (m_AssetBrowser.TakeSelectionActivated() && !m_AssetBrowser.GetSelectedPath().empty()) {
			ClearEntitySelection();
		}
	}

	void ImGuiEditorLayer::ReportBuildProgress(float progress, std::string_view stage) {
		// Atomics for the bar (read every frame), mutex for the
		// human-readable string (read once per frame). Splitting the
		// state lets the per-frame UI poll be lock-free for the bar
		// and only acquire the lock to copy the latest stage label.
		m_BuildProgress.store(std::clamp(progress, 0.0f, 1.0f), std::memory_order_relaxed);
		{
			std::lock_guard<std::mutex> lk(m_BuildProgressMutex);
			m_BuildStage.assign(stage);
		}
	}

	void ImGuiEditorLayer::ExecuteBuildAsync() {
		m_BuildProgress.store(0.0f, std::memory_order_relaxed);
		m_BuildSucceeded.store(true, std::memory_order_relaxed);
		ReportBuildProgress(0.0f, "Starting build");

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			AIM_ERROR_TAG("Build", "No project loaded.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "No project loaded");
			return;
		}

		// Persist the current scene + project on the UI thread before
		// the worker starts. SceneSerializer / project Save touch
		// state owned by the UI side (active scene, LastOpenedScene)
		// and aren't thread-safe to call from the worker. Doing it
		// here mirrors the original synchronous flow.
		Scene* active = SceneManager::Get().GetActiveScene();
		if (active && active->IsDirty()) {
			std::string scenePath = project->GetSceneFilePath(active->GetName());
			SceneSerializer::SaveToFile(*active, scenePath);
			project->LastOpenedScene = active->GetName();
			project->Save();
			AIM_INFO_TAG("Build", "Saved current scene.");
		}

		// Kick off the actual build on a worker thread. The future
		// becomes "ready" when ExecuteBuild returns; the UI polls
		// it once per frame in the editor's chrome update.
		m_BuildFuture = std::async(std::launch::async, [this]() {
			ExecuteBuild();
		});
	}

	void ImGuiEditorLayer::ExecuteBuild() {
		m_BuildStartTime = std::chrono::steady_clock::now();

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			AIM_ERROR_TAG("Build", "No project loaded.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			return;
		}

		m_BuildOutputDir = std::string(m_BuildOutputDirBuffer);
		if (m_BuildOutputDir.empty()) {
			AIM_ERROR_TAG("Build", "No output directory specified.");
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "No output directory");
			return;
		}

		AIM_INFO_TAG("Build", "Starting build for '{}'...", project->Name);
		ReportBuildProgress(0.05f, "Preparing build");

		auto exeDir = std::filesystem::path(Path::ExecutableDir());
		auto outDir = std::filesystem::path(m_BuildOutputDir);
		const std::string buildConfiguration = AxiomProject::GetActiveBuildConfiguration();

		if (std::filesystem::exists(project->CsprojPath)) {
			ReportBuildProgress(0.10f, "Compiling C# scripts");
			AIM_INFO_TAG("Build", "Compiling C# scripts...");
			Process::Result buildResult = Process::Run({
				"dotnet",
				"build",
				project->CsprojPath,
				"-c", buildConfiguration,
				"--nologo",
				"-v", "q",
				"-p:DefineConstants=" + AxiomProject::BuildManagedDefineConstants("AXIOM_BUILD")
			});
			if (!buildResult.Succeeded()) {
				AIM_ERROR_TAG("Build", "C# script compilation failed (exit code {}).", buildResult.ExitCode);
				if (!buildResult.Output.empty()) {
					AIM_ERROR_TAG("Build", "{}", buildResult.Output);
				}
				m_BuildSucceeded.store(false, std::memory_order_relaxed);
				ReportBuildProgress(1.0f, "C# compilation failed");
				return;
			}
			AIM_INFO_TAG("Build", "C# scripts compiled.");
			ReportBuildProgress(0.30f, "C# scripts compiled");
		}

		// Rebuild native scripts with the project's chosen build profile so
		// the shipped DLL has AXIOM_BUILD_RELEASE / AXIOM_BUILD_DEVELOPMENT
		// matching the C# side. Editor hot-reload always builds DEVELOPMENT;
		// here we override via -DAXIOM_BUILD_PROFILE so a Release build
		// actually strips dev-only #ifdef'd code from native scripts.
		// Skipped when the project has no CMakeLists (no native scripts).
		const std::filesystem::path nativeProjectDir(project->NativeScriptsDir);
		const std::filesystem::path nativeCMakeLists = nativeProjectDir / "CMakeLists.txt";
		if (std::filesystem::exists(nativeCMakeLists)) {
			ReportBuildProgress(0.35f, "Compiling native scripts");
			AIM_INFO_TAG("Build", "Compiling native scripts ({} profile)...",
				AxiomProject::BuildProfileToString(project->ActiveBuildProfile));
			const std::string profileCmakeArg = std::string("-DAXIOM_BUILD_PROFILE=")
				+ ((project->ActiveBuildProfile == AxiomProject::BuildProfile::Release) ? "RELEASE" : "DEVELOPMENT");
			const std::filesystem::path nativeBuildDir = nativeProjectDir / "build";
			// Configure (overrides editor's cached AXIOM_BUILD_PROFILE so the
			// next-frame editor hot-reload also picks up the explicit value).
			Process::Result cfg = Process::Run({
				"cmake",
				"-B", nativeBuildDir.string(),
				"-S", nativeProjectDir.string(),
				"-DCMAKE_BUILD_TYPE=" + buildConfiguration,
				profileCmakeArg
			}, nativeProjectDir.string());
			if (!cfg.Succeeded()) {
				AIM_WARN_TAG("Build",
					"Native script CMake configure failed (exit code {}); shipping the editor's last-built native DLL.",
					cfg.ExitCode);
			} else {
				Process::Result bld = Process::Run({
					"cmake", "--build", nativeBuildDir.string(),
					"--config", buildConfiguration
				}, nativeProjectDir.string());
				if (!bld.Succeeded()) {
					AIM_WARN_TAG("Build",
						"Native script build failed (exit code {}); shipping the editor's last-built native DLL.",
						bld.ExitCode);
					if (!bld.Output.empty()) AIM_WARN_TAG("Build", "{}", bld.Output);
				} else {
					AIM_INFO_TAG("Build", "Native scripts compiled.");
				}
			}
		}

		try {
			std::filesystem::create_directories(outDir);
		}
		catch (const std::exception& e) {
			AIM_ERROR_TAG("Build", "Failed to create output directory: {}", e.what());
			m_BuildSucceeded.store(false, std::memory_order_relaxed);
			ReportBuildProgress(1.0f, "Output directory creation failed");
			return;
		}
		ReportBuildProgress(0.55f, "Copying runtime binaries");

		auto copyFile = [&](const std::filesystem::path& src, const std::filesystem::path& dest, const std::string& name) {
			try {
				if (!std::filesystem::exists(src)) {
					AIM_WARN_TAG("Build", "{} not found at {}", name, src.string());
					return;
				}
				auto canonical = std::filesystem::canonical(src);
				if (NeedsCopy(canonical, dest)) {
					std::filesystem::create_directories(dest.parent_path());
					std::filesystem::copy_file(canonical, dest, std::filesystem::copy_options::overwrite_existing);
					AIM_INFO_TAG("Build", "Copied {}", name);
				}
			}
			catch (const std::exception& e) {
				AIM_WARN_TAG("Build", "Failed to copy {}: {}", name, e.what());
			}
		};

		const std::filesystem::path runtimeOutputDirectory = exeDir / ".." / "Axiom-Runtime";
		const std::string runtimeExecutableFilename = GetRuntimeExecutableFilename();
		const std::string outputExecutableStem = project->ExecutableName.empty()
			? project->Name : project->ExecutableName;
		copyFile(runtimeOutputDirectory / runtimeExecutableFilename,
			outDir / (outputExecutableStem + std::filesystem::path(runtimeExecutableFilename).extension().string()),
			"runtime executable");
		copyFile(runtimeOutputDirectory / GetEngineRuntimeFilename(),
			outDir / GetEngineRuntimeFilename(),
			GetEngineRuntimeFilename());

		// GLFW and Glad are SharedLibs (premake5.lua: one shared copy across
		// engine.dll + runtime.exe). Tracy is also shared, but only when the
		// engine was built without --no-profiler. The runtime postbuild stages
		// all of them next to Axiom-Runtime.exe, so we copy from there.
		for (std::string_view depName : { std::string_view{"GLFW"}, std::string_view{"Glad"} }) {
			const std::string filename = SharedLibraryFilename(depName);
			copyFile(runtimeOutputDirectory / filename, outDir / filename, filename);
		}
		{
			const std::string tracyFilename = SharedLibraryFilename("Tracy");
			const std::filesystem::path tracySource = runtimeOutputDirectory / tracyFilename;
			if (std::filesystem::exists(tracySource)) {
				copyFile(tracySource, outDir / tracyFilename, tracyFilename);
			}
		}

		auto scriptCoreDir = exeDir / ".." / "Axiom-ScriptCore";
		copyFile(scriptCoreDir / "Axiom-ScriptCore.dll", outDir / "Axiom-ScriptCore.dll", "Axiom-ScriptCore.dll");
		copyFile(scriptCoreDir / "Axiom-ScriptCore.runtimeconfig.json", outDir / "Axiom-ScriptCore.runtimeconfig.json", "ScriptCore config");
		copyFile(scriptCoreDir / "Axiom-ScriptCore.deps.json", outDir / "Axiom-ScriptCore.deps.json", "ScriptCore deps");
		{
			const std::string netHostFilename = GetNetHostRuntimeFilename();
			const std::filesystem::path netHostSource = exeDir / netHostFilename;
			if (std::filesystem::exists(netHostSource)) {
				copyFile(netHostSource, outDir / netHostFilename, netHostFilename);
			}
		}

		try {
			std::filesystem::copy_file(project->ProjectFilePath, outDir / "axiom-project.json",
				std::filesystem::copy_options::overwrite_existing);
		}
		catch (const std::exception& e) {
			AIM_WARN_TAG("Build", "Failed to copy axiom-project.json: {}", e.what());
		}

		ReportBuildProgress(0.75f, "Copying project assets");
		if (std::filesystem::exists(project->AssetsDirectory)) {
			int updatedFiles = CopyDirIncremental(project->AssetsDirectory, outDir / "Assets");
			AIM_INFO_TAG("Build", "Assets: {} file(s) updated", updatedFiles);
		}

		{
			std::string axiomAssetsSrc;
			if (std::filesystem::exists(project->AxiomAssetsDirectory)) {
				axiomAssetsSrc = project->AxiomAssetsDirectory;
			}
			else {
				axiomAssetsSrc = Path::ResolveAxiomAssets("");
			}

			if (!axiomAssetsSrc.empty() && std::filesystem::exists(axiomAssetsSrc)) {
				// Editor-only assets (icons, gizmo art, file-type previews)
				// have no business in a shipped runtime — skip them. The
				// excluded path is relative to AxiomAssets/, so the
				// recursive copy walks past Textures/Editor entirely.
				const std::vector<std::filesystem::path> excluded{
					std::filesystem::path("Textures") / "Editor",
				};
				int updatedFiles = CopyDirIncrementalExcluding(
					axiomAssetsSrc, outDir / "AxiomAssets", excluded);
				AIM_INFO_TAG("Build", "AxiomAssets: {} file(s) updated", updatedFiles);
			}
			else {
				AIM_WARN_TAG("Build", "AxiomAssets not found - build may be incomplete");
			}
		}

		{
			auto userBinDir = std::filesystem::path(project->GetUserAssemblyOutputPath()).parent_path();
			if (std::filesystem::exists(userBinDir)) {
				auto destBinDir = outDir / "bin" / buildConfiguration;
				int copied = 0;
				for (const auto& entry : std::filesystem::directory_iterator(userBinDir)) {
					if (!entry.is_regular_file()) continue;
					auto ext = entry.path().extension().string();
					if (ext == ".dll" || ext == ".json") {
						copyFile(entry.path(), destBinDir / entry.path().filename(), entry.path().filename().string());
						copied++;
					}
				}
				AIM_INFO_TAG("Build", "User assemblies: {} file(s) copied", copied);
			}
		}

		{
			std::string nativeDll = project->GetNativeDllPath();
			if (std::filesystem::exists(nativeDll)) {
				const std::filesystem::path nativeLibraryPath(nativeDll);
				copyFile(nativeDll,
					outDir / "NativeScripts" / "build" / buildConfiguration / nativeLibraryPath.filename(),
					"native script DLL");
			}
		}

		// Native package DLLs.
		//
		// `PackageHost::LoadAll` (engine side) scans `<exeDir>/Packages/` for
		// folders matching `Pkg.<Name>.Native/Pkg.<Name>.Native.dll` and
		// LoadLibrary-loads each one — that's how packages with a native
		// layer (engine_core / standalone_cpp / pinvoke_dll) register their
		// components, systems, and P/Invoke surface in the runtime.
		//
		// The user's csproj reference already pulls the C# `Pkg.<Name>.dll`
		// into the build via MSBuild's standard copy. The native sibling has
		// no such automatic copy, so without this block a project that uses
		// e.g. `Axiom.Tilemap2D` shipped with `Pkg.Axiom.Tilemap2D.dll`
		// alone — DllImport resolves into a process where no native module
		// exists, components never register, `GetComponent<Tilemap2D>()`
		// returns null, and the user's first `tilemap.SetTile(...)` NREs.
		//
		// Source layout (dev): the editor exe lives at
		//   <axiomBin>/<config>/Axiom-Editor/Axiom-Editor.exe
		// and each native package is a sibling folder:
		//   <axiomBin>/<config>/Pkg.<Name>.Native/Pkg.<Name>.Native.dll
		// We mirror that into the build's distribution layout under
		// `<buildOutput>/Packages/`, which matches the second probe
		// PackageHost::LoadInternal walks (`exeDir / "Packages"`).
		{
			const std::filesystem::path packageSearchRoot = exeDir / "..";
			int copiedPackages = 0;
			for (const std::string& packageName : project->Packages) {
				if (packageName.empty()) continue;
				const std::string nativeFolderName = "Pkg." + packageName + ".Native";
				const std::string nativeFileName = nativeFolderName + ".dll";
				const std::filesystem::path src = packageSearchRoot / nativeFolderName / nativeFileName;
				const std::filesystem::path dst = outDir / "Packages" / nativeFolderName / nativeFileName;
				if (!std::filesystem::exists(src)) {
					// Package may be managed-only (csharp layer with no
					// engine_core/standalone_cpp). That's a valid manifest, so
					// surface as INFO rather than WARN.
					AIM_INFO_TAG("Build",
						"Package '{}' has no native DLL at '{}' — skipping (managed-only?).",
						packageName, src.string());
					continue;
				}
				copyFile(src, dst, nativeFileName);
				copiedPackages++;

				// Best-effort PDB copy so a native crash inside a shipped
				// package surfaces with line numbers when the user keeps
				// debug symbols around.
				const std::filesystem::path pdbSrc = packageSearchRoot / nativeFolderName /
					(nativeFolderName + ".pdb");
				if (std::filesystem::exists(pdbSrc)) {
					copyFile(pdbSrc, dst.parent_path() / pdbSrc.filename(), pdbSrc.filename().string());
				}
			}
			if (copiedPackages > 0) {
				AIM_INFO_TAG("Build", "Native packages: {} DLL(s) staged under Packages/.", copiedPackages);
			}
		}

#ifdef AIM_PLATFORM_WINDOWS
		// Embed a custom icon resource into the copied runtime executable.
		// Without this, the shipped .exe always shows the default Axiom
		// runtime icon (compiled into Axiom-Runtime via icon.rc) regardless
		// of what AppIconPath points to — the project setting only ever
		// affected the in-app window icon. We update RT_GROUP_ICON / RT_ICON
		// directly via the Win32 resource APIs so the Explorer / taskbar /
		// Alt+Tab icon all match what the user picked. PNG / JPEG / BMP /
		// any format Texture2D can decode is converted to a 32-bit BMP-DIB
		// .ico in-memory before embedding, so the user's .png AppIconPath
		// works without re-exporting to .ico.
		if (!project->AppIconPath.empty()) {
			std::filesystem::path iconPath(project->AppIconPath);
			if (!iconPath.is_absolute()) {
				iconPath = std::filesystem::path(project->RootDirectory) / project->AppIconPath;
			}
			const std::string iconExt = iconPath.has_extension()
				? iconPath.extension().string() : std::string{};
			std::string iconExtLower = iconExt;
			std::transform(iconExtLower.begin(), iconExtLower.end(),
				iconExtLower.begin(), [](unsigned char c) { return std::tolower(c); });
			const std::filesystem::path embedTargetExe = outDir
				/ (outputExecutableStem + std::filesystem::path(runtimeExecutableFilename).extension().string());
			if (!std::filesystem::exists(iconPath)) {
				AIM_WARN_TAG("Build", "AppIcon not found at {}", iconPath.string());
			}
			else if (!std::filesystem::exists(embedTargetExe)) {
				AIM_WARN_TAG("Build", "Cannot embed icon — built executable not found at {}",
					embedTargetExe.string());
			}
			else {
				std::vector<std::uint8_t> icoBytes;
				std::string buildErr;
				if (iconExtLower == ".ico") {
					buildErr = ReadIcoFileBytes(iconPath, icoBytes);
				}
				else {
					buildErr = BuildIcoBytesFromImage(iconPath, icoBytes);
				}
				if (!buildErr.empty()) {
					AIM_WARN_TAG("Build", "Failed to prepare icon ({}): {}",
						iconPath.filename().string(), buildErr);
				}
				else {
					const std::string err = EmbedIconIntoExecutable(
						embedTargetExe.string(), icoBytes);
					if (err.empty()) {
						AIM_INFO_TAG("Build", "Embedded icon: {}", iconPath.filename().string());
					}
					else {
						AIM_WARN_TAG("Build", "Failed to embed icon: {}", err);
					}
				}
			}
		}
#endif

		ReportBuildProgress(0.95f, "Finalizing");
		float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_BuildStartTime).count();
		AIM_INFO_TAG("Build", "Build completed in {:.2f}s -> {}", elapsed, m_BuildOutputDir);
		ReportBuildProgress(1.0f, "Build complete");

#ifdef AIM_PLATFORM_WINDOWS
		// ShellExecute opens an Explorer window — touches OS UI state
		// that's safer to do from the editor's UI thread than the
		// build worker. Skipped here; the explorer popup happens on
		// the UI thread once the future resolves.
#endif
	}

	void ImGuiEditorLayer::RenderBuildPanel() {
		if (!m_ShowBuildPanel) return;

		ImGui::Begin("Build", &m_ShowBuildPanel);

		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (project) {
			// Sync m_BuildSceneList with disk every frame so newly-imported
			// scenes auto-appear and deleted ones drop out, while preserving
			// any manual drag-drop reordering the user has done within the
			// panel. AssetRegistry caches the scan, so this is cheap when
			// nothing has changed since last sync.
			AssetRegistry::Sync();
			std::vector<std::string> diskScenes;
			diskScenes.reserve(m_BuildSceneList.size() + 4);
			for (const AssetRegistry::Record& record : AssetRegistry::FindAll(AssetKind::Scene)) {
				diskScenes.push_back(std::filesystem::path(record.Path).stem().string());
			}

			// Drop entries whose .scene file no longer exists.
			m_BuildSceneList.erase(
				std::remove_if(m_BuildSceneList.begin(), m_BuildSceneList.end(),
					[&](const std::string& s) {
						return std::find(diskScenes.begin(), diskScenes.end(), s) == diskScenes.end();
					}),
				m_BuildSceneList.end());

			// Append entries the user has not seen yet (new on disk).
			for (const std::string& stem : diskScenes) {
				if (std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), stem) == m_BuildSceneList.end()) {
					m_BuildSceneList.push_back(stem);
				}
			}

			// Pin StartupScene to position [0] so the [Startup] tag and
			// the runtime's first-loaded scene stay in sync with the
			// project file's StartupScene field.
			if (!project->StartupScene.empty()) {
				auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), project->StartupScene);
				if (it != m_BuildSceneList.end() && it != m_BuildSceneList.begin()) {
					std::string startupScene = *it;
					m_BuildSceneList.erase(it);
					m_BuildSceneList.insert(m_BuildSceneList.begin(), startupScene);
				}
			}

			if (ImGui::CollapsingHeader("Scene List", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (m_BuildSceneList.empty()) {
					ImGui::TextDisabled("No scenes found in Assets/");
				}
				else {
					for (int i = 0; i < static_cast<int>(m_BuildSceneList.size()); i++) {
						ImGui::PushID(i);
						bool isStartup = (i == 0);

						if (isStartup) ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "[Startup]");
						else ImGui::TextDisabled("[%d]", i);

						ImGui::SameLine();
						const std::string sceneItemId = std::to_string(i);
						ImGuiUtils::SelectableEllipsis(m_BuildSceneList[i], sceneItemId.c_str());

						if (ImGui::BeginDragDropSource()) {
							ImGui::SetDragDropPayload("SCENE_LIST_ITEM", &i, sizeof(int));
							ImGui::Text("Move: %s", m_BuildSceneList[i].c_str());
							ImGui::EndDragDropSource();
						}
						if (ImGui::BeginDragDropTarget()) {
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_LIST_ITEM")) {
								int srcIndex = *static_cast<const int*>(payload->Data);
								if (srcIndex != i) {
									std::string moved = m_BuildSceneList[srcIndex];
									m_BuildSceneList.erase(m_BuildSceneList.begin() + srcIndex);
									int insertAt = (srcIndex < i) ? i - 1 : i;
									m_BuildSceneList.insert(m_BuildSceneList.begin() + insertAt, moved);
									if (!m_BuildSceneList.empty()) {
										project->StartupScene = m_BuildSceneList[0];
										project->Save();
									}
								}
							}
							ImGui::EndDragDropTarget();
						}

						ImGui::PopID();
					}
				}

				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_BROWSER_ITEM")) {
						std::string droppedPath(static_cast<const char*>(payload->Data));
						if (std::filesystem::path(droppedPath).extension() == ".scene") {
							std::string sceneName = std::filesystem::path(droppedPath).stem().string();
							auto it = std::find(m_BuildSceneList.begin(), m_BuildSceneList.end(), sceneName);
							if (it == m_BuildSceneList.end()) {
								m_BuildSceneList.push_back(sceneName);
							}
						}
					}
					ImGui::EndDragDropTarget();
				}

				ImGui::Unindent(8);
			}

			ImGui::Spacing();

			if (ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Indent(8);
				if (ImGui::Button("Open Player Settings")) {
					m_ShowPlayerSettings = true;
				}

				ImGui::Spacing();

				// Build Profile dropdown. Drives the AXIOM_BUILD_DEVELOPMENT
				// vs AXIOM_BUILD_RELEASE compile-time defines passed to BOTH
				// C# and native scripts at build time. Switching the profile
				// also flips the runtime overlay defaults (stats + logs)
				// since release ships shouldn't expose dev diagnostics by
				// default — the user can re-enable explicitly via Player
				// Settings after the auto-flip.
				ImGui::TextUnformatted("Build Profile:");
				ImGui::SetNextItemWidth(-1);
				const char* profileItems[] = { "Development", "Release" };
				int profileIdx = (project->ActiveBuildProfile == AxiomProject::BuildProfile::Release) ? 1 : 0;
				if (ImGui::Combo("##BuildProfile", &profileIdx, profileItems, IM_ARRAYSIZE(profileItems))) {
					const auto newProfile = (profileIdx == 1)
						? AxiomProject::BuildProfile::Release
						: AxiomProject::BuildProfile::Development;
					if (newProfile != project->ActiveBuildProfile) {
						project->ActiveBuildProfile = newProfile;
						// Auto-flip overlay defaults to match the new profile.
						// Release = both off (ship-clean). Development = both on.
						const bool isDev = (newProfile == AxiomProject::BuildProfile::Development);
						project->ShowRuntimeStats = isDev;
						project->ShowRuntimeLogs  = isDev;
						project->Save();
					}
				}

				ImGui::Spacing();

				// Custom defines list. Symbols here are baked into both the
				// C# .csproj's <DefineConstants> and the native scripts'
				// CMakeLists target_compile_definitions on next compile.
				// Names only (no `=value`) — Unity-style scripting symbols.
				ImGui::TextUnformatted("Custom Defines:");
				// Reserve room for the "Add" button on the right so the
				// input field doesn't push it off-panel — previously the
				// input was width=-1 which left the button visually clipped
				// to a few pixels and made the click target unusable.
				const float addBtnWidth = ImGui::CalcTextSize("Add").x
					+ ImGui::GetStyle().FramePadding.x * 2.0f;
				const float availForInput = ImGui::GetContentRegionAvail().x
					- addBtnWidth - ImGui::GetStyle().ItemInnerSpacing.x;
				ImGui::SetNextItemWidth(availForInput);
				const bool entryEnter = ImGui::InputTextWithHint("##NewCustomDefine",
					"Add a symbol (e.g. STEAM_BUILD)…",
					m_CustomDefineEntryBuffer, sizeof(m_CustomDefineEntryBuffer),
					ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				bool addClicked = ImGui::Button("Add##CustomDefine") || entryEnter;
				const bool entryReady = m_CustomDefineEntryBuffer[0] != '\0';
				if (addClicked && entryReady) {
					std::string newDef(m_CustomDefineEntryBuffer);
					// Trim whitespace + reject duplicates / empty strings.
					while (!newDef.empty() && std::isspace(static_cast<unsigned char>(newDef.back()))) newDef.pop_back();
					std::size_t firstNonSpace = 0;
					while (firstNonSpace < newDef.size()
						&& std::isspace(static_cast<unsigned char>(newDef[firstNonSpace]))) ++firstNonSpace;
					newDef.erase(0, firstNonSpace);
					if (!newDef.empty()
						&& std::find(project->CustomDefines.begin(), project->CustomDefines.end(), newDef) == project->CustomDefines.end()) {
						project->CustomDefines.push_back(std::move(newDef));
						project->Save();
					}
					m_CustomDefineEntryBuffer[0] = '\0';
				}

				if (project->CustomDefines.empty()) {
					ImGui::TextDisabled("(no custom defines)");
				} else {
					int removeIdx = -1;
					for (int i = 0; i < static_cast<int>(project->CustomDefines.size()); ++i) {
						ImGui::PushID(i);
						ImGui::Bullet();
						ImGui::TextUnformatted(project->CustomDefines[i].c_str());
						ImGui::SameLine();
						if (ImGui::SmallButton("x")) removeIdx = i;
						ImGui::PopID();
					}
					if (removeIdx >= 0) {
						project->CustomDefines.erase(project->CustomDefines.begin() + removeIdx);
						project->Save();
					}
				}

				ImGui::Spacing();
				ImGui::Text("Output Directory:");
				ImGui::SetNextItemWidth(-1);
				ImGui::InputText("##BuildOutputDir", m_BuildOutputDirBuffer, sizeof(m_BuildOutputDirBuffer));
				ImGui::Unindent(8);
			}
		}

		ImGui::Spacing();

		bool canBuild = project && !Application::GetIsPlaying() && m_BuildState == 0;
		if (!canBuild) ImGui::BeginDisabled();

		float halfWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (ImGui::Button("Build", ImVec2(halfWidth, 0))) {
			m_BuildState = 1;
			m_BuildAndPlay = false;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}
		ImGui::SameLine();
		if (ImGui::Button("Build and Play", ImVec2(-1, 0))) {
			m_BuildState = 1;
			m_BuildAndPlay = true;
			m_BuildStartTime = std::chrono::steady_clock::now();
		}

		if (!canBuild) ImGui::EndDisabled();
		ImGui::End();
	}

	void ImGuiEditorLayer::RenderPlayerSettingsPanel() {
		if (!m_ShowPlayerSettings) return;

		ImGui::Begin("Project Settings", &m_ShowPlayerSettings);
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("No project loaded");
			ImGui::End();
			return;
		}

		bool changed = false;
		if (ImGui::CollapsingHeader("Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::InputInt("Width", &project->BuildWidth);
			changed |= ImGui::InputInt("Height", &project->BuildHeight);
			if (project->BuildWidth < 320) project->BuildWidth = 320;
			if (project->BuildHeight < 240) project->BuildHeight = 240;
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Window", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Fullscreen", &project->BuildFullscreen);
			changed |= ImGui::Checkbox("Resizable", &project->BuildResizable);
			ImGui::Unindent(8);
		}

		// UI scaling — Canvas-Scaler-style. Reference resolution defines
		// the "pixel unit" the UI was authored in; the layout system
		// multiplies SizeDelta / AnchoredPosition / padding by
		// (curResolution / refResolution) so the same scene renders
		// proportionally on any window size. Defaults are pre-seeded to
		// match the build resolution so a freshly-created project's
		// Game View previews 1:1 unless the user opts in.
		if (ImGui::CollapsingHeader("UI Scaling", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::InputInt("Reference Width##UIRef", &project->UIReferenceWidth);
			changed |= ImGui::InputInt("Reference Height##UIRef", &project->UIReferenceHeight);
			if (project->UIReferenceWidth  < 1) project->UIReferenceWidth  = 1;
			if (project->UIReferenceHeight < 1) project->UIReferenceHeight = 1;
			changed |= ImGui::SliderFloat("Match Width / Height", &project->UIScaleMatch, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"0 = scale UI by window WIDTH only.\n"
					"1 = scale UI by window HEIGHT only.\n"
					"0.5 = balanced (geometric mean).\n"
					"\n"
					"Move toward 1 if your UI hugs the top/bottom edges,\n"
					"toward 0 if it hugs the left/right edges.");
			}
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Show runtime stats overlay (F6)", &project->ShowRuntimeStats);
			changed |= ImGui::Checkbox("Show runtime log overlay (F7)", &project->ShowRuntimeLogs);
			ImGui::Unindent(8);
		}

		// Asset Browser + Auto-Save moved to the Project Settings panel
		// (editor-side preferences). Keeping the Player Settings panel
		// focused on runtime / shipped-game knobs so users don't have
		// to scan past editor-experience toggles to find what ships in
		// the build.

		if (ImGui::CollapsingHeader("Executable", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::TextUnformatted("Output Name:");
			char exeBuf[256];
			std::string current = project->ExecutableName.empty() ? project->Name : project->ExecutableName;
			std::snprintf(exeBuf, sizeof(exeBuf), "%s", current.c_str());
			ImGui::SetNextItemWidth(-1);
			if (ImGui::InputText("##ExecutableName", exeBuf, sizeof(exeBuf))) {
				const std::string newName(exeBuf);
				project->ExecutableName = (newName == project->Name) ? "" : newName;
				changed = true;
			}
			ImGui::TextDisabled("Default: project name (\"%s\"). The platform extension is appended automatically.",
				project->Name.c_str());
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Splash Screen", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			constexpr const char* k_SplashPickerKey = "ProjectSettings.SplashImage";

			if (auto pending = ReferencePicker::ConsumeSelection(k_SplashPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project->SplashScreen.ImagePath.clear();
					changed = true;
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						std::filesystem::path absFs(absPath);
						std::filesystem::path assetsDir(project->AssetsDirectory);
						if (absFs.string().find(assetsDir.string()) == 0) {
							project->SplashScreen.ImagePath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
						}
						else {
							project->SplashScreen.ImagePath = absFs.filename().string();
						}
						changed = true;
					}
				}
			}

			changed |= ImGui::Checkbox("Enabled##Splash", &project->SplashScreen.Enabled);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("When off, the runtime loads the startup scene immediately with no splash.");
			}

			if (project->SplashScreen.Enabled) {
				ImGui::Spacing();
				changed |= ImGui::SliderFloat("Duration (s)##Splash",
					&project->SplashScreen.DurationSeconds, 0.5f, 10.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade In (s)##Splash",
					&project->SplashScreen.FadeInSeconds, 0.0f, 3.0f, "%.2f");
				changed |= ImGui::SliderFloat("Fade Out (s)##Splash",
					&project->SplashScreen.FadeOutSeconds, 0.0f, 3.0f, "%.2f");

				ImGui::Spacing();
				float bg[3] = {
					project->SplashScreen.BackgroundR,
					project->SplashScreen.BackgroundG,
					project->SplashScreen.BackgroundB,
				};
				if (ImGui::ColorEdit3("Background##Splash", bg)) {
					project->SplashScreen.BackgroundR = bg[0];
					project->SplashScreen.BackgroundG = bg[1];
					project->SplashScreen.BackgroundB = bg[2];
					changed = true;
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Image (optional, replaces default Axiom logo):");
				if (project->SplashScreen.ImagePath.empty()) {
					// Render the engine default logo as a placeholder so
					// the user sees what'll ship. Same source the runtime
					// splash falls back to (RuntimeSplashLayer's
					// ResolveDefaultLogoPath uses
					// `<AxiomAssets>/Textures/icon.png`).
					const std::string root = Path::ResolveAxiomAssets("Textures");
					std::string defaultLogoPath;
					if (!root.empty()) {
						const std::filesystem::path c = std::filesystem::path(root) / "icon.png";
						if (std::filesystem::exists(c)) defaultLogoPath = c.string();
					}
					if (!defaultLogoPath.empty()) {
						TextureHandle h = TextureManager::LoadTexture(defaultLogoPath);
						Texture2D* tex = TextureManager::GetTexture(h);
						if (tex && tex->IsValid()) {
							ImGui::Image(
								static_cast<ImTextureID>(static_cast<intptr_t>(tex->GetHandle())),
								ImVec2(48, 48), ImVec2(0, 1), ImVec2(1, 0));
							ImGui::SameLine();
						}
					}
					ImGui::TextDisabled("(default Axiom logo)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(project->SplashScreen.ImagePath);
				}
				if (ImGui::Button("Browse...##SplashImage")) {
					ReferencePicker::OpenForFieldKey(k_SplashPickerKey, "Select Splash Image",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project->SplashScreen.ImagePath =
						ToProjectRelativeAssetPath(dropped, project->AssetsDirectory);
					changed = true;
				})) {}
				if (!project->SplashScreen.ImagePath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear##SplashImage")) {
						project->SplashScreen.ImagePath.clear();
						changed = true;
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Background Image (optional, painted full-screen behind logo):");
				constexpr const char* k_SplashBgPickerKey = "ProjectSettings.SplashBackground";

				// Apply a pending background-image picker selection — same
				// path-normalisation flow as the foreground image so the
				// stored value stays project-relative.
				if (auto pending = ReferencePicker::ConsumeSelection(k_SplashBgPickerKey); pending) {
					const std::string raw = *pending;
					uint64_t pickedId = 0;
					try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
					if (pickedId == 0) {
						project->SplashScreen.BackgroundImagePath.clear();
						changed = true;
					}
					else {
						std::string absPath = AssetRegistry::ResolvePath(pickedId);
						if (!absPath.empty()) {
							std::filesystem::path absFs(absPath);
							std::filesystem::path assetsDir(project->AssetsDirectory);
							if (absFs.string().find(assetsDir.string()) == 0) {
								project->SplashScreen.BackgroundImagePath =
									std::filesystem::relative(absFs, assetsDir.parent_path()).string();
							}
							else {
								project->SplashScreen.BackgroundImagePath = absFs.filename().string();
							}
							changed = true;
						}
					}
				}

				if (project->SplashScreen.BackgroundImagePath.empty()) {
					ImGui::TextDisabled("(no background image — solid colour fill above)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(project->SplashScreen.BackgroundImagePath);
				}
				if (ImGui::Button("Browse...##SplashBgImage")) {
					ReferencePicker::OpenForFieldKey(k_SplashBgPickerKey, "Select Splash Background",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project->SplashScreen.BackgroundImagePath =
						ToProjectRelativeAssetPath(dropped, project->AssetsDirectory);
					changed = true;
				})) {}
				if (!project->SplashScreen.BackgroundImagePath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear##SplashBgImage")) {
						project->SplashScreen.BackgroundImagePath.clear();
						changed = true;
					}
				}

				ImGui::Spacing();
				ImGui::TextUnformatted("Custom Text (optional, replaces version + platform line):");
				char textBuf[256];
				std::snprintf(textBuf, sizeof(textBuf), "%s", project->SplashScreen.CustomText.c_str());
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputTextWithHint("##SplashText", "Leave empty for engine default",
					textBuf, sizeof(textBuf))) {
					project->SplashScreen.CustomText = textBuf;
					changed = true;
				}

				// Show preview — replays the splash sequence (fade-in,
				// hold, fade-out) over the editor's main viewport so the
				// user can sanity-check colours / images / text without
				// running a full build. The actual layer push lives on
				// ImGuiEditorLayer (we only set the request flag here);
				// see RequestSplashPreview in the header.
				ImGui::Spacing();
				if (ImGui::Button("Show Preview")) {
					m_SplashPreviewRequest = true;
				}
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"Replay the splash screen using the current\n"
						"settings, drawn on top of the editor for the\n"
						"FadeIn + Duration + FadeOut window.");
				}
			}
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("App Icon", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			constexpr const char* k_AppIconPickerKey = "ProjectSettings.AppIcon";

			// AppIconPath is stored as a project-relative path (e.g.
			// "Assets/icon.png") so the project file stays portable, but
			// TextureManager::ResolveTexturePath only searches CWD,
			// engine AxiomAssets, and <exe>/Assets/Textures — none of
			// which match a path-relative-to-project-root once the editor
			// is launched from somewhere other than the project dir.
			// Prepending project->RootDirectory turns it into a path the
			// `File::Exists(rawPath)` first branch of ResolveTexturePath
			// accepts.
			auto resolveProjectRelative = [&](const std::string& rel) -> std::string {
				std::filesystem::path p(rel);
				if (p.is_absolute()) return rel;
				return (std::filesystem::path(project->RootDirectory) / p).string();
			};

			// Engine-shipped default icon — `<axiomBin>/AxiomAssets/Textures/icon.png`.
			// Same payload the runtime's splash falls back to (see
			// `RuntimeSplashLayer::ResolveDefaultLogoPath`) and what gets
			// embedded into the runtime exe's RT_GROUP_ICON resource at
			// compile time. Presented as a placeholder thumbnail when the
			// project hasn't picked its own icon, so the user sees
			// exactly what their build would ship without any
			// configuration.
			auto resolveDefaultEngineIcon = []() -> std::string {
				const std::string root = Path::ResolveAxiomAssets("Textures");
				if (root.empty()) return {};
				const std::filesystem::path candidate = std::filesystem::path(root) / "icon.png";
				if (!std::filesystem::exists(candidate)) return {};
				return candidate.string();
			};

			// Apply a pending picker selection from a previous frame.
			//
			// AppIconPath is the SHIPPED game's window icon (and what the
			// build pipeline embeds into the runtime .exe). It must not be
			// applied to the editor's own window — the editor keeps its own
			// embedded RT_ICON regardless of which project is open. The 64x64
			// thumbnail rendered below is the in-panel preview; we don't
			// need to push the texture into glfwSetWindowIcon to show what
			// the user picked.
			if (auto pending = ReferencePicker::ConsumeSelection(k_AppIconPickerKey); pending) {
				const std::string raw = *pending;
				uint64_t pickedId = 0;
				try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
				if (pickedId == 0) {
					project->AppIconPath.clear();
					changed = true;
				}
				else {
					std::string absPath = AssetRegistry::ResolvePath(pickedId);
					if (!absPath.empty()) {
						std::filesystem::path absFs(absPath);
						std::filesystem::path assetsDir(project->AssetsDirectory);
						if (absFs.string().find(assetsDir.string()) == 0) {
							project->AppIconPath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
						}
						else {
							project->AppIconPath = absFs.filename().string();
						}
						changed = true;
					}
				}
			}

			if (!project->AppIconPath.empty()) {
				TextureHandle iconHandle = TextureManager::LoadTexture(resolveProjectRelative(project->AppIconPath));
				Texture2D* iconTex = TextureManager::GetTexture(iconHandle);
				if (iconTex && iconTex->IsValid()) {
					ImGui::Image(
						static_cast<ImTextureID>(static_cast<intptr_t>(iconTex->GetHandle())),
						ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
					ImGui::SameLine();
				}
				else {
					ImGui::TextDisabled("Failed to load:");
					ImGuiUtils::TextDisabledEllipsis(project->AppIconPath);
				}

				ImGui::BeginGroup();
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project->AppIconPath =
						ToProjectRelativeAssetPath(dropped, project->AssetsDirectory);
					changed = true;
				})) {}
				ImGui::SameLine();
				if (ImGui::Button("Clear##AppIcon")) {
					project->AppIconPath.clear();
					changed = true;
				}
				ImGui::EndGroup();

				if (iconTex && iconTex->IsValid()) {
					ImGuiUtils::TextDisabledEllipsis(project->AppIconPath);
				}
			}
			else {
				// Render the engine default icon as the placeholder thumbnail
				// so the user sees what'll ship by default. Falls through to
				// the disabled "No icon set" text only if AxiomAssets isn't
				// installed (development environment without `Setup.bat`).
				const std::string defaultIconPath = resolveDefaultEngineIcon();
				if (!defaultIconPath.empty()) {
					TextureHandle defIconHandle = TextureManager::LoadTexture(defaultIconPath);
					Texture2D* defIconTex = TextureManager::GetTexture(defIconHandle);
					if (defIconTex && defIconTex->IsValid()) {
						ImGui::Image(
							static_cast<ImTextureID>(static_cast<intptr_t>(defIconTex->GetHandle())),
							ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
						ImGui::SameLine();
					}
				}

				ImGui::BeginGroup();
				if (defaultIconPath.empty()) {
					ImGui::TextDisabled("No icon set");
				}
				else {
					ImGui::TextDisabled("(engine default — icon.png)");
				}
				if (ImGui::Button("Browse...##AppIcon")) {
					ReferencePicker::OpenForFieldKey(k_AppIconPickerKey, "Select App Icon",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				// Drop target lives on the Browse button itself — keeps
				// the affordance consistent with every other Browse... in
				// this panel (Splash Image, Cursors). Previously the drop
				// target wrapped the entire App Icon section's last item
				// (the disabled-text hint), which made the drop hit-area
				// inconsistent with the rest of the panel and confused
				// users about WHERE to drop. Note: NOT calling
				// SetWindowIcon — the AppIconPath is the SHIPPED game's
				// icon, not the editor's window icon.
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					project->AppIconPath =
						ToProjectRelativeAssetPath(dropped, project->AssetsDirectory);
					changed = true;
				})) {}
				ImGui::SameLine();
				ImGui::TextDisabled("or drag an image onto the Browse button");
				ImGui::EndGroup();
			}

			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Cursors", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);

			auto resolveProjectRelativeCursor = [&](const std::string& rel) -> std::string {
				std::filesystem::path p(rel);
				if (p.is_absolute()) return rel;
				return (std::filesystem::path(project->RootDirectory) / p).string();
			};

			// Apply a project-relative path to the live Window cursor so
			// the change is visible immediately rather than requiring a
			// restart. Empty path clears the slot back to the OS default.
			auto applyCursorLive = [&](const std::string& path, void (Window::*setter)(const Texture2D*))
			{
				Window* win = Application::GetInstance() ? Application::GetInstance()->GetWindow() : nullptr;
				if (!win) return;
				if (path.empty()) {
					(win->*setter)(nullptr);
					return;
				}
				const std::string abs = resolveProjectRelativeCursor(path);
				if (!std::filesystem::exists(abs)) {
					AIM_WARN_TAG("Editor", "Cursor image not found: {}", abs);
					return;
				}
				TextureHandle h = TextureManager::LoadTexture(abs);
				if (Texture2D* tex = TextureManager::GetTexture(h); tex && tex->IsValid()) {
					(win->*setter)(tex);
				}
			};

			// Renders the picker / drag-drop / clear UI for one cursor
			// slot. `slotName` is the persisted project field; the lambda
			// rewrites it on selection. The "live apply" callback pushes
			// the new cursor into the Window so the user sees the swap
			// without re-launching.
			auto renderCursorSlot = [&](const char* label,
				const char* pickerKey, std::string& slotPath,
				void (Window::*liveSetter)(const Texture2D*))
			{
				ImGui::PushID(label);
				ImGui::TextUnformatted(label);

				if (auto pending = ReferencePicker::ConsumeSelection(pickerKey); pending) {
					const std::string raw = *pending;
					uint64_t pickedId = 0;
					try { pickedId = std::stoull(raw); } catch (...) { pickedId = 0; }
					if (pickedId == 0) {
						slotPath.clear();
						changed = true;
						applyCursorLive("", liveSetter);
					}
					else {
						std::string absPath = AssetRegistry::ResolvePath(pickedId);
						if (!absPath.empty()) {
							std::filesystem::path absFs(absPath);
							std::filesystem::path assetsDir(project->AssetsDirectory);
							if (absFs.string().find(assetsDir.string()) == 0) {
								slotPath = std::filesystem::relative(absFs, assetsDir.parent_path()).string();
							}
							else {
								slotPath = absFs.filename().string();
							}
							changed = true;
							applyCursorLive(slotPath, liveSetter);
						}
					}
				}

				if (slotPath.empty()) {
					ImGui::TextDisabled("(OS default)");
				}
				else {
					ImGuiUtils::TextDisabledEllipsis(slotPath);
				}

				if (ImGui::Button("Browse...")) {
					ReferencePicker::OpenForFieldKey(pickerKey, "Select Cursor Image",
						ReferencePicker::CollectAssetsByKind(AssetKind::Texture),
						ReferencePicker::Style::Thumbnails);
				}
				if (BrowseAcceptImageDrop(k_ImageExts, [&](const std::string& dropped) {
					slotPath = ToProjectRelativeAssetPath(dropped, project->AssetsDirectory);
					changed = true;
					applyCursorLive(slotPath, liveSetter);
				})) {}
				if (!slotPath.empty()) {
					ImGui::SameLine();
					if (ImGui::Button("Clear")) {
						slotPath.clear();
						changed = true;
						applyCursorLive("", liveSetter);
					}
				}
				ImGui::PopID();
			};

			renderCursorSlot("Default cursor", "ProjectSettings.DefaultCursor",
				project->CursorImagePath, &Window::SetCursorImage);
			ImGui::Spacing();
			renderCursorSlot("UI hover cursor", "ProjectSettings.UICursor",
				project->UIInteractableCursorImagePath, &Window::SetUICursorImage);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Shown while the cursor sits over an Axiom UI element\n"
					"with an Interactable component (Button / Slider / etc.).\n"
					"Falls back to the default cursor when unset.");
			}

			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Global Systems", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##GlobalSystemSearch", "Search global systems...",
				m_GlobalSystemSearchBuffer, sizeof(m_GlobalSystemSearchBuffer));

			std::string filter(m_GlobalSystemSearchBuffer);
			std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

			std::vector<EditorScriptDiscovery::ScriptEntry> scriptEntries;
			EditorScriptDiscovery::CollectProjectScriptEntries(scriptEntries);

			std::unordered_set<std::string> discoveredGlobalSystems;
			for (const auto& scriptEntry : scriptEntries) {
				if (!scriptEntry.IsGlobalSystem) {
					continue;
				}

				discoveredGlobalSystems.insert(scriptEntry.ClassName);
				if (!filter.empty()) {
					std::string lowerClassName = EditorScriptDiscovery::ToLowerCopy(scriptEntry.ClassName);
					std::string lowerPath = EditorScriptDiscovery::ToLowerCopy(scriptEntry.Path.string());
					if (lowerClassName.find(filter) == std::string::npos
						&& lowerPath.find(filter) == std::string::npos) {
						continue;
					}
				}

				auto it = std::find_if(project->GlobalSystems.begin(), project->GlobalSystems.end(),
					[&](const AxiomProject::GlobalSystemRegistration& registration) {
						return registration.ClassName == scriptEntry.ClassName;
					});
				bool active = it != project->GlobalSystems.end() ? it->Active : false;
				if (ImGui::Checkbox(scriptEntry.ClassName.c_str(), &active)) {
					if (it == project->GlobalSystems.end()) {
						project->GlobalSystems.push_back({ scriptEntry.ClassName, active });
					}
					else {
						it->Active = active;
					}
					changed = true;
				}
			}

			for (auto& registration : project->GlobalSystems) {
				if (discoveredGlobalSystems.contains(registration.ClassName)) {
					continue;
				}
				bool active = registration.Active;
				std::string label = registration.ClassName + " (missing)";
				if (ImGui::Checkbox(label.c_str(), &active)) {
					registration.Active = active;
					changed = true;
				}
			}

			ImGui::Unindent(8);
		}

		if (changed) {
			project->Save();
			std::vector<std::string> activeGlobalSystems;
			for (const auto& registration : project->GlobalSystems) {
				if (registration.Active && !registration.ClassName.empty()) {
					activeGlobalSystems.push_back(registration.ClassName);
				}
			}
			ScriptEngine::ShutdownGlobalSystems();
			ScriptEngine::InitializeGlobalSystems(activeGlobalSystems);
		}

		// Render any reference picker popup opened from this panel
		// (App Icon, Splash image). Without this, OpenForFieldKey
		// stages a request that never gets a render call and the popup
		// never appears for fields hosted outside the inspector.
		ReferencePicker::RenderPopup();

		ImGui::End();
	}

	// Project Settings — editor-side preferences scoped to this project.
	// Distinct from Player Settings (which configures the shipped game's
	// runtime). Anything here only affects the editor's UX while the
	// project is open: how the asset browser renders names, when scenes
	// auto-save, etc. Adding new editor-experience toggles? They belong
	// in this panel, not Player Settings.
	void ImGuiEditorLayer::RenderProjectSettingsPanel() {
		if (!m_ShowProjectSettings) return;

		ImGui::Begin("Project Settings", &m_ShowProjectSettings);
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			ImGui::TextDisabled("No project loaded");
			ImGui::End();
			return;
		}

		bool changed = false;

		if (ImGui::CollapsingHeader("Asset Browser", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Show file extensions", &project->ShowFileExtensions);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"When off (default), asset names render without their\n"
					"extension (\"MyScene\" instead of \"MyScene.scene\").\n"
					"Renaming pre-fills only the stem and re-appends the\n"
					"original extension on commit. When on, extensions are\n"
					"shown and editable verbatim.");
			}
			ImGui::Unindent(8);
		}

		if (ImGui::CollapsingHeader("Auto-Save", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Indent(8);
			changed |= ImGui::Checkbox("Auto-save scenes", &project->AutoSaveScenes);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Periodically save the active scene while editing.\n"
					"Skipped during Play mode (Play-mode edits are discarded\n"
					"on Stop, so saving them would clobber the pre-Play snapshot).");
			}
			if (project->AutoSaveScenes) {
				ImGui::SetNextItemWidth(160.0f);
				changed |= ImGui::InputFloat("Interval (seconds)", &project->AutoSaveIntervalSeconds, 5.0f, 30.0f, "%.0f");
				if (project->AutoSaveIntervalSeconds < 5.0f) {
					project->AutoSaveIntervalSeconds = 5.0f;
				}
			}
			ImGui::Unindent(8);
		}

		if (changed) {
			project->Save();
		}

		ImGui::End();
	}

	// Splash preview — replays the runtime's splash timeline as a
	// foreground overlay over the editor. Triggered by the
	// "Show Preview" button in Player Settings → Splash Screen.
	// Self-completes after FadeIn + Duration + FadeOut seconds.
	// Path resolution mirrors RuntimeSplashLayer's logic so the user
	// sees exactly what the build will ship.
	void ImGuiEditorLayer::TickSplashPreview() {
		AxiomProject* project = ProjectManager::GetCurrentProject();
		if (!project) {
			m_SplashPreviewRequest = false;
			m_SplashPreviewActive = false;
			return;
		}

		if (m_SplashPreviewRequest) {
			m_SplashPreviewRequest = false;
			m_SplashPreviewActive = true;
			m_SplashPreviewElapsed = 0.0f;

			// Resolve foreground + background paths the same way the
			// runtime splash does so the preview matches the shipped
			// game's behaviour. AppIconPath / SplashScreen.ImagePath
			// are stored project-relative; prepend RootDirectory.
			auto resolveProjectRelative = [&](const std::string& rel) -> std::string {
				if (rel.empty()) return {};
				std::filesystem::path p(rel);
				if (p.is_absolute() && std::filesystem::exists(p)) return p.string();
				std::filesystem::path projectRel = std::filesystem::path(project->RootDirectory) / p;
				if (std::filesystem::exists(projectRel)) return projectRel.string();
				std::filesystem::path assetsRel = std::filesystem::path(project->AssetsDirectory) / p.filename();
				if (std::filesystem::exists(assetsRel)) return assetsRel.string();
				return rel;
			};

			// Logo
			std::string logoPath = resolveProjectRelative(project->SplashScreen.ImagePath);
			if (logoPath.empty()) {
				const std::string axiomTexRoot = Path::ResolveAxiomAssets("Textures");
				if (!axiomTexRoot.empty()) {
					std::filesystem::path candidate = std::filesystem::path(axiomTexRoot) / "Axiom64.png";
					if (std::filesystem::exists(candidate)) logoPath = candidate.string();
				}
			}
			m_SplashPreviewLogo = logoPath.empty()
				? TextureHandle::Invalid()
				: TextureManager::LoadTexture(logoPath);

			// Background
			const std::string bgPath = resolveProjectRelative(project->SplashScreen.BackgroundImagePath);
			m_SplashPreviewBackground = bgPath.empty()
				? TextureHandle::Invalid()
				: TextureManager::LoadTexture(bgPath);

			// Keep both textures alive across PurgeUnreferenced for
			// the preview lifetime; release in the completion path
			// below.
			if (m_SplashPreviewTexRefToken == 0) {
				m_SplashPreviewTexRefToken = TextureManager::AddReferenceProvider(
					[this](const TextureManager::ReferenceEmitter& emit) {
						if (m_SplashPreviewLogo.IsValid()) emit(m_SplashPreviewLogo);
						if (m_SplashPreviewBackground.IsValid()) emit(m_SplashPreviewBackground);
					});
			}
		}

		if (!m_SplashPreviewActive) return;

		auto* app = Application::GetInstance();
		const float dt = app ? app->GetTime().GetDeltaTimeUnscaled() : 0.0f;
		m_SplashPreviewElapsed += dt;

		const float fadeIn = std::max(0.0f, project->SplashScreen.FadeInSeconds);
		const float hold   = std::max(0.0f, project->SplashScreen.DurationSeconds);
		const float fadeOut = std::max(0.0f, project->SplashScreen.FadeOutSeconds);
		const float total = fadeIn + hold + fadeOut;
		if (m_SplashPreviewElapsed >= total) {
			m_SplashPreviewActive = false;
			if (m_SplashPreviewTexRefToken != 0) {
				TextureManager::RemoveReferenceProvider(m_SplashPreviewTexRefToken);
				m_SplashPreviewTexRefToken = 0;
			}
			m_SplashPreviewLogo = TextureHandle::Invalid();
			m_SplashPreviewBackground = TextureHandle::Invalid();
			return;
		}

		float alpha = 1.0f;
		if (m_SplashPreviewElapsed < fadeIn && fadeIn > 0.0f) {
			alpha = m_SplashPreviewElapsed / fadeIn;
		}
		else if (m_SplashPreviewElapsed > fadeIn + hold && fadeOut > 0.0f) {
			alpha = 1.0f - (m_SplashPreviewElapsed - fadeIn - hold) / fadeOut;
		}
		alpha = std::clamp(alpha, 0.0f, 1.0f);

		// Draw onto the OS-window foreground draw list so the preview
		// floats above every dock / panel including the toolbar. Using
		// the foreground list also dodges the InvisibleButton/scissor
		// nesting issues the dockspace adds — the preview can't be
		// docked or focused.
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImDrawList* draw = ImGui::GetForegroundDrawList(vp);
		const ImVec2 wMin = vp->WorkPos;
		const ImVec2 wMax(wMin.x + vp->WorkSize.x, wMin.y + vp->WorkSize.y);
		const float width = vp->WorkSize.x;
		const float height = vp->WorkSize.y;

		auto packColor = [](float r, float g, float b, float a) {
			r = std::clamp(r, 0.0f, 1.0f);
			g = std::clamp(g, 0.0f, 1.0f);
			b = std::clamp(b, 0.0f, 1.0f);
			a = std::clamp(a, 0.0f, 1.0f);
			return IM_COL32(int(r * 255), int(g * 255), int(b * 255), int(a * 255));
		};

		draw->AddRectFilled(wMin, wMax, packColor(
			project->SplashScreen.BackgroundR,
			project->SplashScreen.BackgroundG,
			project->SplashScreen.BackgroundB,
			alpha));

		Texture2D* bg = TextureManager::GetTexture(m_SplashPreviewBackground);
		if (bg && bg->IsValid()) {
			const float bgW = static_cast<float>(bg->GetWidth());
			const float bgH = static_cast<float>(bg->GetHeight());
			if (bgW > 0.0f && bgH > 0.0f) {
				const float canvasAspect = width / height;
				const float bgAspect = bgW / bgH;
				float drawW = width, drawH = height;
				if (bgAspect > canvasAspect) drawW = drawH * bgAspect;
				else                          drawH = drawW / bgAspect;
				const ImVec2 bgMin(wMin.x + (width  - drawW) * 0.5f,
				                   wMin.y + (height - drawH) * 0.5f);
				const ImVec2 bgMax(bgMin.x + drawW, bgMin.y + drawH);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(bg->GetHandle())),
					bgMin, bgMax,
					ImVec2(0, 1), ImVec2(1, 0),
					packColor(1.0f, 1.0f, 1.0f, alpha));
			}
		}

		const float centerX = wMin.x + width * 0.5f;
		const float centerY = wMin.y + height * 0.5f;

		Texture2D* logo = TextureManager::GetTexture(m_SplashPreviewLogo);
		if (logo && logo->IsValid()) {
			const float maxLogoSide = std::min(width, height) * 0.35f;
			float logoW = static_cast<float>(logo->GetWidth());
			float logoH = static_cast<float>(logo->GetHeight());
			if (logoW > 0 && logoH > 0) {
				const float scale = std::min(maxLogoSide / logoW, maxLogoSide / logoH);
				logoW *= scale; logoH *= scale;
				const ImVec2 imgMin(centerX - logoW * 0.5f, centerY - logoH * 0.65f);
				const ImVec2 imgMax(imgMin.x + logoW, imgMin.y + logoH);
				draw->AddImage(
					static_cast<ImTextureID>(static_cast<intptr_t>(logo->GetHandle())),
					imgMin, imgMax,
					ImVec2(0, 1), ImVec2(1, 0),
					packColor(1.0f, 1.0f, 1.0f, alpha));
			}
		}

		const std::string subtitle = project->SplashScreen.CustomText.empty()
			? std::string("Axiom ") + AIM_VERSION
			: project->SplashScreen.CustomText;
		if (!subtitle.empty()) {
			const ImVec2 textSize = ImGui::CalcTextSize(subtitle.c_str());
			const ImVec2 textPos(centerX - textSize.x * 0.5f,
				centerY + std::min(width, height) * 0.18f);
			draw->AddText(textPos, packColor(1.0f, 1.0f, 1.0f, alpha * 0.85f), subtitle.c_str());
		}
	}

	void ImGuiEditorLayer::RenderAssetInspector() {
		const std::string& selectedPath = m_AssetBrowser.GetSelectedPath();
		if (selectedPath.empty()) {
			// Selection cleared. If the prefab inspector held a dirty prefab,
			// the save/discard prompt is driven from the dispatch above; here
			// just close cleanly.
			if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
				m_PrefabInspector.Close();
				m_PrefabInspectorPath.clear();
			}
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::filesystem::path path(selectedPath);
		if (!std::filesystem::exists(path)) {
			ImGui::TextDisabled("No entity or asset selected");
			return;
		}

		std::string name = path.filename().string();
		std::string ext = path.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// `.prefab` selection routes through PrefabInspector. Selection change
		// while a dirty prefab is open opens a save/discard modal — the new
		// selection is queued in m_PendingPrefabSwitchPath until the user picks.
		if (ext == ".prefab") {
			if (m_PrefabInspector.IsOpen() && m_PrefabInspectorPath != selectedPath) {
				if (m_PrefabInspector.HasUnsavedChanges()) {
					m_PendingPrefabSwitchPath = selectedPath;
					m_ShowPrefabSavePrompt = true;
				}
				else {
					m_PrefabInspector.Open(selectedPath);
					m_PrefabInspectorPath = selectedPath;
				}
			}
			else if (!m_PrefabInspector.IsOpen()) {
				m_PrefabInspector.Open(selectedPath);
				m_PrefabInspectorPath = selectedPath;
			}
			m_PrefabInspector.Render();
			return;
		}

		// Non-prefab asset: clean up any open prefab inspector (no prompt — user
		// already navigated away; if they had unsaved work the prompt UX would
		// have caught it during the .prefab→.prefab switch above).
		if (m_PrefabInspector.IsOpen() && !m_PrefabInspector.HasUnsavedChanges()) {
			m_PrefabInspector.Close();
			m_PrefabInspectorPath.clear();
		}

		ImGui::TextDisabled("Asset:");
		ImGui::SameLine();
		ImGuiUtils::TextEllipsis(name);
		ImGui::Separator();

		ImGui::TextDisabled("Path:");
		ImGui::SameLine();
		ImGuiUtils::TextDisabledEllipsis(selectedPath);

		try {
			auto fileSize = std::filesystem::file_size(path);
			if (fileSize >= 1024 * 1024) ImGui::TextDisabled("Size: %.2f MB", fileSize / (1024.0f * 1024.0f));
			else if (fileSize >= 1024) ImGui::TextDisabled("Size: %.1f KB", fileSize / 1024.0f);
			else ImGui::TextDisabled("Size: %llu bytes", fileSize);
		}
		catch (...) {
		}

		ImGui::TextDisabled("Type: %s", ext.c_str());
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
			ImGui::Spacing();
			const Texture2D* tex = GetPreviewTexture(path);
			if (tex && tex->IsValid()) {
				ImGuiUtils::DrawTexturePreview(tex->GetHandle(), tex->GetWidth(), tex->GetHeight(), 128.0f);
				ImGui::Text("%.0f x %.0f", tex->GetWidth(), tex->GetHeight());
			}
		}
	}

	const Texture2D* ImGuiEditorLayer::GetPreviewTexture(const std::filesystem::path& path) {
		const std::string canonicalPath = NormalizePreviewTexturePath(path);
		if (canonicalPath.empty()) {
			return nullptr;
		}

		if (const auto it = m_PreviewTextureLookup.find(canonicalPath); it != m_PreviewTextureLookup.end()) {
			PreviewTextureEntry& entry = m_PreviewTextureCache[it->second];
			entry.LastTouchTick = ++m_PreviewTextureTick;
			return entry.Texture.get();
		}

		// TODO(perf): Synchronous decode on UI thread — large textures cause first-select hitches.
		// Convert to async-task pattern (see axiom-add-editor-panel skill).
		auto texture = std::make_unique<Texture2D>(canonicalPath.c_str(), Filter::Point, Wrap::Clamp, Wrap::Clamp);
		if (!texture->IsValid()) {
			return nullptr;
		}

		PreviewTextureEntry entry;
		entry.CanonicalPath = canonicalPath;
		entry.Texture = std::move(texture);
		entry.LastTouchTick = ++m_PreviewTextureTick;

		const size_t index = m_PreviewTextureCache.size();
		m_PreviewTextureLookup[canonicalPath] = index;
		m_PreviewTextureCache.push_back(std::move(entry));
		TrimPreviewTextureCache();
		return m_PreviewTextureCache.back().Texture.get();
	}

	void ImGuiEditorLayer::TrimPreviewTextureCache() {
		while (m_PreviewTextureCache.size() > kMaxPreviewTextures) {
			auto victimIt = std::min_element(
				m_PreviewTextureCache.begin(),
				m_PreviewTextureCache.end(),
				[](const PreviewTextureEntry& a, const PreviewTextureEntry& b) {
					return a.LastTouchTick < b.LastTouchTick;
				});
			if (victimIt == m_PreviewTextureCache.end()) {
				break;
			}

			m_PreviewTextureCache.erase(victimIt);
			m_PreviewTextureLookup.clear();
			for (size_t i = 0; i < m_PreviewTextureCache.size(); ++i) {
				m_PreviewTextureLookup[m_PreviewTextureCache[i].CanonicalPath] = i;
			}
		}
	}

	void ImGuiEditorLayer::ClearPreviewTextureCache() {
		m_PreviewTextureLookup.clear();
		m_PreviewTextureCache.clear();
		m_PreviewTextureTick = 0;
	}

	void ImGuiEditorLayer::RenderPackageManagerPanel() {
		if (!m_ShowPackageManager) return;

		if (!m_PackageManagerInitialized) {
			m_PackageManager.Initialize();

			if (m_PackageManager.IsReady()) {
				m_PackageManager.AddSource(std::make_unique<NuGetSource>(m_PackageManager.GetToolPath()));
				m_PackageManager.AddSource(std::make_unique<GitHubSource>(
					m_PackageManager.GetToolPath(),
					"https://raw.githubusercontent.com/Ben-Scr/axiom-packages/main/index.json",
					"Engine Packages"));
			}

			m_PackageManagerPanel.Initialize(&m_PackageManager);
			m_PackageManagerInitialized = true;
		}

		// Pre-seed a sensible first-open size and minimum constraints so a
		// freshly-docked panel isn't squashed to its title bar — without
		// this, ImGui's dock layout could collapse the panel's content
		// area to 0 px and the user only sees the tab strip.
		ImGui::SetNextWindowSize(ImVec2(880, 540), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(420, 320), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::Begin("Package Manager", &m_ShowPackageManager);
		m_PackageManagerPanel.Render();
		ImGui::End();
	}

} // namespace Axiom
