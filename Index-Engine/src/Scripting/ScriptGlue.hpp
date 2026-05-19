#pragma once
#include <cstdint>

namespace Index {

	/// Layout must match C# NativeBindingsStruct exactly (Sequential, blittable).
	struct NativeBindings
	{
		// ── Application ──────────────────────────────────────────────
		float  (*Application_GetDeltaTime)();
		float  (*Application_GetElapsedTime)();
		int    (*Application_GetScreenWidth)();
		int    (*Application_GetScreenHeight)();
		float  (*Application_GetTargetFrameRate)();
		void   (*Application_SetTargetFrameRate)(float fps);
		void   (*Application_Quit)();
		float  (*Application_GetFixedDeltaTime)();
		float  (*Application_GetUnscaledDeltaTime)();
		float  (*Application_GetFixedUnscaledDeltaTime)();
		float  (*Application_GetTimeScale)();
		void   (*Application_SetTimeScale)(float scale);
		int    (*Application_IsEditor)(); // 1 = host is editor, 0 = standalone runtime
		int    (*Application_GetClipboardStringBuffer)(char* outBuffer, int capacity);
		void   (*Application_SetClipboardString)(const char* value);
		int    (*Application_GetVsyncEnabled)();
		void   (*Application_SetVsyncEnabled)(int enabled);

		// ── Window ───────────────────────────────────────────────────
		// Direct accessors for the engine's OS window (title bar, size,
		// position, fullscreen state, focus). The C# `Index.Window` static
		// class wraps these. Strings cross via the two-call buffer
		// pattern; bools are int 1/0; Vector2Int comes back via two
		// out-int pointers (same shape as Input_GetMousePosition's float
		// pair) and goes in as two ints.
		int    (*Window_GetWidth)();
		int    (*Window_GetHeight)();
		int    (*Window_GetTitleBuffer)(char* outBuffer, int capacity);
		void   (*Window_SetTitle)(const char* title);
		void   (*Window_Minimize)();
		void   (*Window_Maximize)();
		int    (*Window_IsMaximized)();
		int    (*Window_IsFullScreen)();
		void   (*Window_SetFullScreen)(int enabled);
		void   (*Window_GetPosition)(int* outX, int* outY);
		void   (*Window_SetPosition)(int x, int y);
		void   (*Window_Focus)();
		// Returns the primary monitor's video-mode dimensions. Used by
		// `Window.ScreenCenter` to compute the desktop centre; mirrors
		// `Window::GetScreenCenter()`'s engine-side data source.
		void   (*Window_GetScreenSize)(int* outWidth, int* outHeight);

		// ── Engine ───────────────────────────────────────────────────
		// Build identity + GPU caps. Two-call buffer pattern for strings:
		// pass (null, 0) to learn required size, then a sized buffer.
		int    (*Engine_GetVersionBuffer)(char* outBuffer, int capacity);
		// "Index <version> (<platform> <config>)" — matches IDX_VERSION_LONG.
		int    (*Engine_GetVersionLongBuffer)(char* outBuffer, int capacity);
		// 0 = Debug (editor preview), 1 = Development, 2 = Release.
		int    (*Engine_GetBuildConfiguration)();
		int    (*Engine_GetPlatformBuffer)(char* outBuffer, int capacity);
		int    (*Engine_GetGraphicsApiBuffer)(char* outBuffer, int capacity);
		int    (*Engine_GetGpuVendorBuffer)(char* outBuffer, int capacity);
		int    (*Engine_GetGpuRendererBuffer)(char* outBuffer, int capacity);

		// ── Time ─────────────────────────────────────────────────────
		int    (*Time_GetFrameCount)();
		float  (*Time_GetTimeSinceStartup)();
		float  (*Time_GetRealtimeSinceStartup)();

		// ── Log ──────────────────────────────────────────────────────
		void   (*Log_Trace)(const char* message);
		void   (*Log_Info)(const char* message);
		void   (*Log_Warn)(const char* message);
		void   (*Log_Error)(const char* message);

		// ── Input ────────────────────────────────────────────────────
		int    (*Input_GetKey)(int keyCode);
		int    (*Input_GetKeyDown)(int keyCode);
		int    (*Input_GetKeyUp)(int keyCode);
		int    (*Input_GetAnyKey)();
		int    (*Input_GetMouseButton)(int button);
		int    (*Input_GetMouseButtonDown)(int button);
		int    (*Input_GetMouseButtonUp)(int button);
		void   (*Input_GetMousePosition)(float* outX, float* outY);
		void   (*Input_GetAxis)(float* outX, float* outY);
		void   (*Input_GetMouseDelta)(float* outX, float* outY);
		float  (*Input_GetScrollWheelDelta)();

		// ── Entity ───────────────────────────────────────────────────
		int      (*Entity_IsValid)(uint64_t entityID);
		uint64_t (*Entity_FindByName)(const char* name);
		void     (*Entity_Destroy)(uint64_t entityID);
		uint64_t (*Entity_Create)(const char* name);
		uint64_t (*Entity_Clone)(uint64_t sourceEntityID);
		uint64_t (*Entity_InstantiatePrefab)(uint64_t prefabGuid);
		int      (*Entity_GetOrigin)(uint64_t entityID);
		uint64_t (*Entity_GetRuntimeID)(uint64_t entityID);
		uint64_t (*Entity_GetSceneGUID)(uint64_t entityID);
		uint64_t (*Entity_GetPrefabGUID)(uint64_t entityID);
		int      (*Entity_HasComponent)(uint64_t entityID, const char* componentName);
		int      (*Entity_AddComponent)(uint64_t entityID, const char* componentName);
		int      (*Entity_RemoveComponent)(uint64_t entityID, const char* componentName);
		// Script-specific routes (EntityScript subclasses). Mirror the
		// component triplet but target ScriptComponent.Scripts +
		// ScriptSystem instead of the ECS pool. Class name is the C#
		// short type name (e.g. "TutorialScript").
		int      (*Entity_AddScript)(uint64_t entityID, const char* className);
		int      (*Entity_HasScript)(uint64_t entityID, const char* className);
		int      (*Entity_RemoveScript)(uint64_t entityID, const char* className);
		// Delayed destruction — push onto Scene::m_PendingDestroys.
		// Forwards to Scene::DestroyEntity(handle, delay).
		void     (*Entity_DestroyDelayed)(uint64_t entityID, float delay);
		int      (*Entity_GetManagedComponentFieldsBuffer)(uint64_t entityID, const char* componentName, char* outBuffer, int capacity);
		int      (*Entity_GetIsStatic)(uint64_t entityID);
		void     (*Entity_SetIsStatic)(uint64_t entityID, int isStatic);
		int      (*Entity_GetIsEnabled)(uint64_t entityID);
		int      (*Entity_GetIsEnabledInHierarchy)(uint64_t entityID);
		void     (*Entity_SetIsEnabled)(uint64_t entityID, int isEnabled);

		// ── NameComponent ────────────────────────────────────────────
		int         (*NameComponent_GetNameBuffer)(uint64_t entityID, char* outBuffer, int capacity);
		void        (*NameComponent_SetName)(uint64_t entityID, const char* name);

		// ── Transform2D ──────────────────────────────────────────────
		void  (*Transform2D_GetPosition)(uint64_t entityID, float* outX, float* outY);
		void  (*Transform2D_SetPosition)(uint64_t entityID, float x, float y);
		float (*Transform2D_GetRotation)(uint64_t entityID);
		void  (*Transform2D_SetRotation)(uint64_t entityID, float rotation);
		void  (*Transform2D_GetScale)(uint64_t entityID, float* outX, float* outY);
		void  (*Transform2D_SetScale)(uint64_t entityID, float x, float y);
		uint64_t (*Transform2D_GetEntity)(uint64_t entityID);
		void  (*Transform2D_GetLocalPosition)(uint64_t entityID, float* outX, float* outY);
		void  (*Transform2D_SetLocalPosition)(uint64_t entityID, float x, float y);
		float (*Transform2D_GetLocalRotation)(uint64_t entityID);
		void  (*Transform2D_SetLocalRotation)(uint64_t entityID, float rotation);
		void  (*Transform2D_GetLocalScale)(uint64_t entityID, float* outX, float* outY);
		void  (*Transform2D_SetLocalScale)(uint64_t entityID, float x, float y);
		uint64_t (*Transform2D_GetParent)(uint64_t entityID);
		int   (*Transform2D_SetParent)(uint64_t entityID, uint64_t parentEntityID);
		int   (*Transform2D_GetChildCount)(uint64_t entityID);
		uint64_t (*Transform2D_GetChildAt)(uint64_t entityID, int index);
		int   (*Transform2D_GetChildren)(uint64_t entityID, uint64_t* outIDs, int maxOut);

		// ── SpriteRenderer ───────────────────────────────────────────
		void (*SpriteRenderer_GetColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*SpriteRenderer_SetColor)(uint64_t entityID, float r, float g, float b, float a);
		uint64_t (*SpriteRenderer_GetTexture)(uint64_t entityID);
		void (*SpriteRenderer_SetTexture)(uint64_t entityID, uint64_t assetId);
		int  (*SpriteRenderer_GetSortingOrder)(uint64_t entityID);
		void (*SpriteRenderer_SetSortingOrder)(uint64_t entityID, int order);
		int  (*SpriteRenderer_GetSortingLayer)(uint64_t entityID);
		void (*SpriteRenderer_SetSortingLayer)(uint64_t entityID, int layer);

		// ── TextRenderer ─────────────────────────────────────────────
		int         (*TextRenderer_GetTextBuffer)(uint64_t entityID, char* outBuffer, int capacity);
		void        (*TextRenderer_SetText)(uint64_t entityID, const char* text);
		uint64_t    (*TextRenderer_GetFont)(uint64_t entityID);
		void        (*TextRenderer_SetFont)(uint64_t entityID, uint64_t assetId);
		float       (*TextRenderer_GetFontSize)(uint64_t entityID);
		void        (*TextRenderer_SetFontSize)(uint64_t entityID, float size);
		void        (*TextRenderer_GetColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void        (*TextRenderer_SetColor)(uint64_t entityID, float r, float g, float b, float a);
		float       (*TextRenderer_GetLetterSpacing)(uint64_t entityID);
		void        (*TextRenderer_SetLetterSpacing)(uint64_t entityID, float spacing);
		int         (*TextRenderer_GetHAlign)(uint64_t entityID);
		void        (*TextRenderer_SetHAlign)(uint64_t entityID, int alignment);
		int         (*TextRenderer_GetWrapMode)(uint64_t entityID);
		void        (*TextRenderer_SetWrapMode)(uint64_t entityID, int mode);
		// WrapWidth slots removed — wrap area is now derived from
		// the host RectTransform2D's width minus Margin.
		int         (*TextRenderer_GetSortingOrder)(uint64_t entityID);
		void        (*TextRenderer_SetSortingOrder)(uint64_t entityID, int order);
		int         (*TextRenderer_GetSortingLayer)(uint64_t entityID);
		void        (*TextRenderer_SetSortingLayer)(uint64_t entityID, int layer);

		// ── Camera2D ─────────────────────────────────────────────────
		float (*Camera2D_GetOrthographicSize)(uint64_t entityID);
		void  (*Camera2D_SetOrthographicSize)(uint64_t entityID, float size);
		float (*Camera2D_GetZoom)(uint64_t entityID);
		void  (*Camera2D_SetZoom)(uint64_t entityID, float zoom);
		void  (*Camera2D_GetClearColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void  (*Camera2D_SetClearColor)(uint64_t entityID, float r, float g, float b, float a);
		void  (*Camera2D_ScreenToWorld)(uint64_t entityID, float sx, float sy, float* outX, float* outY);
		float (*Camera2D_GetViewportWidth)(uint64_t entityID);
		float (*Camera2D_GetViewportHeight)(uint64_t entityID);
		// Returns the entity ID of the active scene's main camera (0 if none).
		// Backs C# `Camera2DComponent.Main`.
		uint64_t (*Camera2D_GetMainEntity)();

		// ── Rigidbody2D ──────────────────────────────────────────────
		void  (*Rigidbody2D_ApplyForce)(uint64_t entityID, float forceX, float forceY, int wake);
		void  (*Rigidbody2D_ApplyImpulse)(uint64_t entityID, float impulseX, float impulseY, int wake);
		void  (*Rigidbody2D_GetLinearVelocity)(uint64_t entityID, float* outX, float* outY);
		void  (*Rigidbody2D_SetLinearVelocity)(uint64_t entityID, float x, float y);
		float (*Rigidbody2D_GetAngularVelocity)(uint64_t entityID);
		void  (*Rigidbody2D_SetAngularVelocity)(uint64_t entityID, float velocity);
		int   (*Rigidbody2D_GetBodyType)(uint64_t entityID);
		void  (*Rigidbody2D_SetBodyType)(uint64_t entityID, int type);
		float (*Rigidbody2D_GetGravityScale)(uint64_t entityID);
		void  (*Rigidbody2D_SetGravityScale)(uint64_t entityID, float scale);
		float (*Rigidbody2D_GetMass)(uint64_t entityID);
		void  (*Rigidbody2D_SetMass)(uint64_t entityID, float mass);

		// ── BoxCollider2D ────────────────────────────────────────────
		void  (*BoxCollider2D_GetScale)(uint64_t entityID, float* outX, float* outY);
		void  (*BoxCollider2D_GetCenter)(uint64_t entityID, float* outX, float* outY);
		void  (*BoxCollider2D_SetEnabled)(uint64_t entityID, int enabled);

		// ── CircleCollider2D ─────────────────────────────────────────
		float (*CircleCollider2D_GetRadius)(uint64_t entityID);
		void  (*CircleCollider2D_SetRadius)(uint64_t entityID, float radius);
		void  (*CircleCollider2D_GetCenter)(uint64_t entityID, float* outX, float* outY);
		void  (*CircleCollider2D_SetCenter)(uint64_t entityID, float x, float y);
		void  (*CircleCollider2D_SetEnabled)(uint64_t entityID, int enabled);

		// ── PolygonCollider2D ────────────────────────────────────────
		// Vertex count, world-space vertex copy-out, and a custom hull setter
		// (mirrors Physics2D_OverlapPolygon's `points + count` interleaved-floats convention).
		int   (*PolygonCollider2D_GetVertexCount)(uint64_t entityID);
		// Copies up to maxOut interleaved floats (x,y pairs) of world-space
		// vertices into outPoints; returns the number of vertices actually written.
		int   (*PolygonCollider2D_GetWorldPoints)(uint64_t entityID, float* outPoints, int maxOut);
		// `points` is interleaved x,y; pointCount must be in [3, B2_MAX_POLYGON_VERTICES (8)].
		void  (*PolygonCollider2D_SetPoints)(uint64_t entityID, const float* points, int pointCount);
		void  (*PolygonCollider2D_SetSides)(uint64_t entityID, int sides);
		void  (*PolygonCollider2D_GetCenter)(uint64_t entityID, float* outX, float* outY);
		void  (*PolygonCollider2D_SetCenter)(uint64_t entityID, float x, float y);
		void  (*PolygonCollider2D_GetSize)(uint64_t entityID, float* outX, float* outY);
		void  (*PolygonCollider2D_SetSize)(uint64_t entityID, float x, float y);
		void  (*PolygonCollider2D_SetEnabled)(uint64_t entityID, int enabled);

		// ── AudioSource ──────────────────────────────────────────────
		void  (*AudioSource_Play)(uint64_t entityID);
		void  (*AudioSource_Pause)(uint64_t entityID);
		void  (*AudioSource_Stop)(uint64_t entityID);
		void  (*AudioSource_Resume)(uint64_t entityID);
		float (*AudioSource_GetVolume)(uint64_t entityID);
		void  (*AudioSource_SetVolume)(uint64_t entityID, float volume);
		float (*AudioSource_GetPitch)(uint64_t entityID);
		void  (*AudioSource_SetPitch)(uint64_t entityID, float pitch);
		int   (*AudioSource_GetLoop)(uint64_t entityID);
		void  (*AudioSource_SetLoop)(uint64_t entityID, int loop);
		int   (*AudioSource_IsPlaying)(uint64_t entityID);
		int   (*AudioSource_IsPaused)(uint64_t entityID);
		// Audio asset reference: backs C# `AudioSourceComponent.Audio`.
		// Get returns the asset UUID currently assigned (0 when none).
		// Set assigns by UUID and refreshes the live AudioHandle via AudioManager.
		uint64_t (*AudioSource_GetAudio)(uint64_t entityID);
		void     (*AudioSource_SetAudio)(uint64_t entityID, uint64_t assetId);

		// ── Axiom-Physics ─────────────────────────────────────────────
		int   (*FastBody2D_GetBodyType)(uint64_t entityID);
		void  (*FastBody2D_SetBodyType)(uint64_t entityID, int type);
		float (*FastBody2D_GetMass)(uint64_t entityID);
		void  (*FastBody2D_SetMass)(uint64_t entityID, float mass);
		int   (*FastBody2D_GetUseGravity)(uint64_t entityID);
		void  (*FastBody2D_SetUseGravity)(uint64_t entityID, int enabled);
		void  (*FastBody2D_GetVelocity)(uint64_t entityID, float* outX, float* outY);
		void  (*FastBody2D_SetVelocity)(uint64_t entityID, float x, float y);
		void  (*FastBoxCollider2D_GetHalfExtents)(uint64_t entityID, float* outX, float* outY);
		void  (*FastBoxCollider2D_SetHalfExtents)(uint64_t entityID, float x, float y);
		float (*FastCircleCollider2D_GetRadius)(uint64_t entityID);
		void  (*FastCircleCollider2D_SetRadius)(uint64_t entityID, float radius);

		// ── Scene ────────────────────────────────────────────────────
		int         (*Scene_GetActiveSceneNameBuffer)(char* outBuffer, int capacity);
		int         (*Scene_GetEntityCount)();
		int         (*Scene_GetEntityCountByName)(const char* sceneName);
		int         (*Scene_LoadAdditive)(const char* sceneName);
		int         (*Scene_Load)(const char* sceneName);
		void        (*Scene_Unload)(const char* sceneName);
		int         (*Scene_SetActive)(const char* sceneName);
		int         (*Scene_Reload)(const char* sceneName);
		int         (*Scene_SetGameSystemEnabled)(const char* sceneName, const char* className, int enabled);
		int         (*Scene_IsGameSystemEnabled)(const char* sceneName, const char* className);
		void        (*Scene_SetGlobalSystemEnabled)(const char* className, int enabled);
		int         (*Scene_DoesSceneExist)(const char* sceneName);
		int         (*Scene_GetLoadedCount)();
		int         (*Scene_GetLoadedSceneNameAtBuffer)(int index, char* outBuffer, int capacity);
		int         (*Scene_GetEntityNameByUUIDBuffer)(uint64_t uuid, char* outBuffer, int capacity);
		int         (*Scene_QueryEntities)(const char* componentNames, uint64_t* outEntityIDs, int maxOut);
		int         (*Scene_QueryEntitiesFiltered)(const char* withComponents, const char* withoutComponents, const char* mustHaveComponents, int enableFilter, uint64_t* outEntityIDs, int maxOut);
		int         (*Scene_QueryEntitiesInScene)(const char* sceneName, const char* componentNames, uint64_t* outEntityIDs, int maxOut);
		int         (*Scene_QueryEntitiesFilteredInScene)(const char* sceneName, const char* withComponents, const char* withoutComponents, const char* mustHaveComponents, int enableFilter, uint64_t* outEntityIDs, int maxOut);
		int         (*Asset_IsValid)(uint64_t assetId);
		uint64_t    (*Asset_GetOrCreateUUIDFromPath)(const char* path);
		int         (*Asset_GetPathBuffer)(uint64_t assetId, char* outBuffer, int capacity);
		int         (*Asset_GetDisplayNameBuffer)(uint64_t assetId, char* outBuffer, int capacity);
		int         (*Asset_GetKind)(uint64_t assetId);
		int         (*Asset_FindAllBuffer)(const char* pathPrefix, int kind, char* outBuffer, int capacity);
		int         (*Texture_LoadAsset)(uint64_t assetId);
		int         (*Texture_GetWidth)(uint64_t assetId);
		int         (*Texture_GetHeight)(uint64_t assetId);
		// Resolve a built-in DefaultTexture enum value to its
		// AssetRegistry GUID so `new Texture(assetId)` round-trips.
		// Returns 0 for out-of-range or pre-init queries.
		uint64_t    (*Texture_GetDefaultAssetUUID)(uint8_t which);
		int         (*Audio_LoadAsset)(uint64_t assetId);
		void        (*Audio_PlayOneShotAsset)(uint64_t assetId, float volume);
		int         (*Font_LoadAsset)(uint64_t assetId);

		// ── ParticleSystem2D ─────────────────────────────────────────
		void  (*ParticleSystem2D_Play)(uint64_t entityID);
		void  (*ParticleSystem2D_Pause)(uint64_t entityID);
		void  (*ParticleSystem2D_Stop)(uint64_t entityID);
		int   (*ParticleSystem2D_IsPlaying)(uint64_t entityID);
		int   (*ParticleSystem2D_GetPlayOnAwake)(uint64_t entityID);
		void  (*ParticleSystem2D_SetPlayOnAwake)(uint64_t entityID, int enabled);
		void  (*ParticleSystem2D_GetColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void  (*ParticleSystem2D_SetColor)(uint64_t entityID, float r, float g, float b, float a);
		float (*ParticleSystem2D_GetLifeTime)(uint64_t entityID);
		void  (*ParticleSystem2D_SetLifeTime)(uint64_t entityID, float lifetime);
		float (*ParticleSystem2D_GetSpeed)(uint64_t entityID);
		void  (*ParticleSystem2D_SetSpeed)(uint64_t entityID, float speed);
		float (*ParticleSystem2D_GetScale)(uint64_t entityID);
		void  (*ParticleSystem2D_SetScale)(uint64_t entityID, float scale);
		int   (*ParticleSystem2D_GetEmitOverTime)(uint64_t entityID);
		void  (*ParticleSystem2D_SetEmitOverTime)(uint64_t entityID, int rate);
		void  (*ParticleSystem2D_Emit)(uint64_t entityID, int count);

		// ── Gizmos ───────────────────────────────────────────────────
		void (*Gizmo_DrawLine)(float x1, float y1, float x2, float y2);
		void (*Gizmo_DrawSquare)(float cx, float cy, float sx, float sy, float degrees);
		void (*Gizmo_DrawCircle)(float cx, float cy, float radius, int segments);
		void (*Gizmo_SetColor)(float r, float g, float b, float a);
		void (*Gizmo_GetColor)(float* r, float* g, float* b, float* a);
		float (*Gizmo_GetLineWidth)();
		void (*Gizmo_SetLineWidth)(float width);

		// ── Physics2D ────────────────────────────────────────────────
		int (*Physics2D_Raycast)(float originX, float originY, float dirX, float dirY, float distance,
		                         uint64_t* hitEntityID, float* hitX, float* hitY, float* hitNormalX, float* hitNormalY, float* hitDistance);
		int (*Physics2D_OverlapCircle)(float originX, float originY, float radius, int mode, uint64_t* entityID);
		int (*Physics2D_OverlapBox)(float originX, float originY, float halfX, float halfY, float degrees, int mode, uint64_t* entityID);
		int (*Physics2D_OverlapPolygon)(float originX, float originY, const float* points, int pointCount, int mode, uint64_t* entityID);
		int (*Physics2D_OverlapCircleAll)(float originX, float originY, float radius, uint64_t* outEntityIDs, int maxOut);
		int (*Physics2D_OverlapBoxAll)(float originX, float originY, float halfX, float halfY, float degrees, uint64_t* outEntityIDs, int maxOut);
		int (*Physics2D_OverlapPolygonAll)(float originX, float originY, const float* points, int pointCount, uint64_t* outEntityIDs, int maxOut);
		int (*Physics2D_ContainsPoint)(float originX, float originY, int mode, uint64_t* entityID);
		int (*Physics2D_ContainsPointAll)(float originX, float originY, uint64_t* outEntityIDs, int maxOut);

		// ── UI: RectTransform2D ──────────────────────────────────────
		void (*RectTransform_GetAnchorMin)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetAnchorMin)(uint64_t entityID, float x, float y);
		void (*RectTransform_GetAnchorMax)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetAnchorMax)(uint64_t entityID, float x, float y);
		void (*RectTransform_GetPivot)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetPivot)(uint64_t entityID, float x, float y);
		void (*RectTransform_GetAnchoredPosition)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetAnchoredPosition)(uint64_t entityID, float x, float y);
		void (*RectTransform_GetSizeDelta)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetSizeDelta)(uint64_t entityID, float x, float y);
		float (*RectTransform_GetRotation)(uint64_t entityID);
		void (*RectTransform_SetRotation)(uint64_t entityID, float rotation);
		void (*RectTransform_GetScale)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetScale)(uint64_t entityID, float x, float y);
		float (*RectTransform_GetLocalRotation)(uint64_t entityID);
		void (*RectTransform_SetLocalRotation)(uint64_t entityID, float rotation);
		void (*RectTransform_GetLocalScale)(uint64_t entityID, float* outX, float* outY);
		void (*RectTransform_SetLocalScale)(uint64_t entityID, float x, float y);
		void (*RectTransform_GetResolvedSize)(uint64_t entityID, float* outW, float* outH);

		// ── UI: Image ────────────────────────────────────────────────
		void (*Image_GetColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Image_SetColor)(uint64_t entityID, float r, float g, float b, float a);
		uint64_t (*Image_GetTexture)(uint64_t entityID);
		void (*Image_SetTexture)(uint64_t entityID, uint64_t assetId);
		int  (*Image_GetSortingOrder)(uint64_t entityID);
		void (*Image_SetSortingOrder)(uint64_t entityID, int order);
		int  (*Image_GetSortingLayer)(uint64_t entityID);
		void (*Image_SetSortingLayer)(uint64_t entityID, int layer);

		// ── UI: Interactable ─────────────────────────────────────────
		int (*Interactable_GetInteractable)(uint64_t entityID);
		void (*Interactable_SetInteractable)(uint64_t entityID, int value);
		int (*Interactable_GetIsHovered)(uint64_t entityID);
		int (*Interactable_GetIsClicked)(uint64_t entityID);
		int (*Interactable_GetIsPressed)(uint64_t entityID);
		int (*Interactable_GetIsMouseDown)(uint64_t entityID);
		int (*Interactable_GetIsMouseUp)(uint64_t entityID);
		// Optional focus / selection navigation. Focusable defaults to
		// false so existing scenes are not silently included in Tab
		// order; IsFocused is normally driven by UIFocusSystem but the
		// setter is honoured for one frame so script-driven focus
		// (e.g. "open this menu and put focus on the first button")
		// works without a separate API.
		int (*Interactable_GetFocusable)(uint64_t entityID);
		void (*Interactable_SetFocusable)(uint64_t entityID, int value);
		int (*Interactable_GetIsFocused)(uint64_t entityID);
		void (*Interactable_SetIsFocused)(uint64_t entityID, int value);

		// ── UI: Button ───────────────────────────────────────────────
		void (*Button_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Button_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Button_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Button_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Button_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Button_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Button_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Button_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Button_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Button_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);

		void (*Toggle_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Toggle_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Slider_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Slider_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*InputField_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*InputField_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── UI: TransitionMode + sprite slots ────────────────────────
		// TransitionMode is the enum that picks ColorTint / SpriteSwap /
		// None for each widget. Sprite slots are UUIDs (0 == unset).
		// One slot per state mirrors the *Color slots above.
		int (*Button_GetTransitionMode)(uint64_t entityID);
		void (*Button_SetTransitionMode)(uint64_t entityID, int mode);
		int (*Toggle_GetTransitionMode)(uint64_t entityID);
		void (*Toggle_SetTransitionMode)(uint64_t entityID, int mode);
		int (*Slider_GetTransitionMode)(uint64_t entityID);
		void (*Slider_SetTransitionMode)(uint64_t entityID, int mode);
		int (*InputField_GetTransitionMode)(uint64_t entityID);
		void (*InputField_SetTransitionMode)(uint64_t entityID, int mode);
		int (*Dropdown_GetTransitionMode)(uint64_t entityID);
		void (*Dropdown_SetTransitionMode)(uint64_t entityID, int mode);

		uint64_t (*Button_GetNormalSprite)(uint64_t entityID);
		void     (*Button_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Button_GetHoveredSprite)(uint64_t entityID);
		void     (*Button_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Button_GetPressedSprite)(uint64_t entityID);
		void     (*Button_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Button_GetDisabledSprite)(uint64_t entityID);
		void     (*Button_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Button_GetFocusedSprite)(uint64_t entityID);
		void     (*Button_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		uint64_t (*Toggle_GetNormalSprite)(uint64_t entityID);
		void     (*Toggle_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Toggle_GetHoveredSprite)(uint64_t entityID);
		void     (*Toggle_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Toggle_GetPressedSprite)(uint64_t entityID);
		void     (*Toggle_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Toggle_GetDisabledSprite)(uint64_t entityID);
		void     (*Toggle_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Toggle_GetFocusedSprite)(uint64_t entityID);
		void     (*Toggle_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		uint64_t (*Slider_GetNormalSprite)(uint64_t entityID);
		void     (*Slider_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Slider_GetHoveredSprite)(uint64_t entityID);
		void     (*Slider_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Slider_GetPressedSprite)(uint64_t entityID);
		void     (*Slider_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Slider_GetDisabledSprite)(uint64_t entityID);
		void     (*Slider_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Slider_GetFocusedSprite)(uint64_t entityID);
		void     (*Slider_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		uint64_t (*InputField_GetNormalSprite)(uint64_t entityID);
		void     (*InputField_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*InputField_GetHoveredSprite)(uint64_t entityID);
		void     (*InputField_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*InputField_GetPressedSprite)(uint64_t entityID);
		void     (*InputField_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*InputField_GetDisabledSprite)(uint64_t entityID);
		void     (*InputField_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*InputField_GetFocusedSprite)(uint64_t entityID);
		void     (*InputField_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		uint64_t (*Dropdown_GetNormalSprite)(uint64_t entityID);
		void     (*Dropdown_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Dropdown_GetHoveredSprite)(uint64_t entityID);
		void     (*Dropdown_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Dropdown_GetPressedSprite)(uint64_t entityID);
		void     (*Dropdown_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Dropdown_GetDisabledSprite)(uint64_t entityID);
		void     (*Dropdown_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Dropdown_GetFocusedSprite)(uint64_t entityID);
		void     (*Dropdown_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		// ── UI: IsReadOnly + entity-ref + popup-option colors ────────
		int  (*Toggle_GetIsReadOnly)(uint64_t entityID);
		void (*Toggle_SetIsReadOnly)(uint64_t entityID, int value);
		int  (*Slider_GetIsReadOnly)(uint64_t entityID);
		void (*Slider_SetIsReadOnly)(uint64_t entityID, int value);
		int  (*Dropdown_GetIsReadOnly)(uint64_t entityID);
		void (*Dropdown_SetIsReadOnly)(uint64_t entityID, int value);

		uint64_t (*Button_GetTargetGraphic)(uint64_t entityID);
		void     (*Button_SetTargetGraphic)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*Slider_GetFillEntity)(uint64_t entityID);
		void     (*Slider_SetFillEntity)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*Slider_GetHandleEntity)(uint64_t entityID);
		void     (*Slider_SetHandleEntity)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*Slider_GetBackgroundEntity)(uint64_t entityID);
		void     (*Slider_SetBackgroundEntity)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*Toggle_GetCheckmarkEntity)(uint64_t entityID);
		void     (*Toggle_SetCheckmarkEntity)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*InputField_GetTextEntity)(uint64_t entityID);
		void     (*InputField_SetTextEntity)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*Dropdown_GetLabelEntity)(uint64_t entityID);
		void     (*Dropdown_SetLabelEntity)(uint64_t entityID, uint64_t refUuid);

		void (*Dropdown_GetOptionNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetOptionNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetOptionHoverColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetOptionHoverColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetOptionPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetOptionPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetOptionSelectedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetOptionSelectedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetPopupBackgroundColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetPopupBackgroundColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetOptionTextColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetOptionTextColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── UI: Slider ───────────────────────────────────────────────
		float (*Slider_GetValue)(uint64_t entityID);
		void (*Slider_SetValue)(uint64_t entityID, float value);
		float (*Slider_GetMinValue)(uint64_t entityID);
		void (*Slider_SetMinValue)(uint64_t entityID, float value);
		float (*Slider_GetMaxValue)(uint64_t entityID);
		void (*Slider_SetMaxValue)(uint64_t entityID, float value);
		int (*Slider_GetWholeNumbers)(uint64_t entityID);
		void (*Slider_SetWholeNumbers)(uint64_t entityID, int value);
		int (*Slider_GetValueChangedThisFrame)(uint64_t entityID);
		void (*Slider_MarkValueObserved)(uint64_t entityID);
		void (*Slider_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Slider_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Slider_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Slider_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Slider_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Slider_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Slider_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Slider_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── UI: Toggle ───────────────────────────────────────────────
		int (*Toggle_GetIsOn)(uint64_t entityID);
		void (*Toggle_SetIsOn)(uint64_t entityID, int value);
		int (*Toggle_GetValueChangedThisFrame)(uint64_t entityID);
		void (*Toggle_MarkIsOnObserved)(uint64_t entityID);
		void (*Toggle_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Toggle_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Toggle_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Toggle_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Toggle_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Toggle_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Toggle_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Toggle_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── UI: InputField ───────────────────────────────────────────
		int (*InputField_GetTextBuffer)(uint64_t entityID, char* outBuffer, int capacity);
		void (*InputField_SetText)(uint64_t entityID, const char* text);
		int (*InputField_GetPlaceholderTextBuffer)(uint64_t entityID, char* outBuffer, int capacity);
		void (*InputField_SetPlaceholderText)(uint64_t entityID, const char* text);
		int (*InputField_GetIsFocused)(uint64_t entityID);
		void (*InputField_SetIsFocused)(uint64_t entityID, int value);
		int (*InputField_GetSubmittedThisFrame)(uint64_t entityID);
		int (*InputField_GetCharacterLimit)(uint64_t entityID);
		void (*InputField_SetCharacterLimit)(uint64_t entityID, int value);
		void (*InputField_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*InputField_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*InputField_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*InputField_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*InputField_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*InputField_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*InputField_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*InputField_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── UI: Dropdown ─────────────────────────────────────────────
		int (*Dropdown_GetSelectedIndex)(uint64_t entityID);
		void (*Dropdown_SetSelectedIndex)(uint64_t entityID, int value);
		int (*Dropdown_GetIsOpen)(uint64_t entityID);
		void (*Dropdown_SetIsOpen)(uint64_t entityID, int value);
		int (*Dropdown_GetSelectionChangedThisFrame)(uint64_t entityID);
		void (*Dropdown_MarkSelectedIndexObserved)(uint64_t entityID);
		int (*Dropdown_GetOptionCount)(uint64_t entityID);
		int (*Dropdown_GetOptionBuffer)(uint64_t entityID, int index, char* outBuffer, int capacity);
		void (*Dropdown_SetOption)(uint64_t entityID, int index, const char* text);
		void (*Dropdown_AddOption)(uint64_t entityID, const char* text);
		void (*Dropdown_RemoveOption)(uint64_t entityID, int index);
		void (*Dropdown_ClearOptions)(uint64_t entityID);
		void (*Dropdown_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void (*Dropdown_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void (*Dropdown_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);

		// ── ECS ref-API (appended for binary compat) ────────────────
		// Direct pointer into the component's EnTT storage slot for `entityID`,
		// or nullptr when the entity / component is missing. The C# side casts
		// the void* to a blittable struct that mirrors the C++ component's
		// layout and reads/writes fields with no per-property P/Invoke. Pointer
		// is valid only until the next structural change to the same component
		// pool, so callers refetch rather than cache across frames.
		void* (*Entity_GetComponentPtr)(uint64_t entityID, const char* componentName);

		// sizeof(T) of the underlying C++ component, 0 when the name doesn't
		// resolve or the type is empty/tag. C# checks its mirror struct's
		// sizeof against this at script-engine init and refuses to load the
		// user assembly on mismatch — catches silent layout drift before it
		// corrupts memory.
		int (*Entity_GetComponentSize)(const char* componentName);

		// Opens a view across the named pools and fills outPointers with
		// one row per matching entity. Each row contains poolCount slots:
		// the first `writeCount` are raw pointers into the write pools (in
		// the order names appear in `writeNames`), followed by `roCount`
		// pointers into the readonly pools (in `readonlyNames` order). C#
		// derives poolCount itself from the type parameters and indexes
		// outPointers as `outPointers[row * poolCount + col]`.
		//
		// Returns the actual matched row count. If the return is larger than
		// `maxRows`, only the first `maxRows` rows were written and the
		// caller should resize and retry (same pattern as Scene_QueryEntities).
		// Pointers are invalidated by any structural change to the same
		// component pool — scripts must NOT add/remove/destroy entities
		// inside a query iteration.
		int (*Scene_OpenQueryView)(
			const char* sceneName,
			const char* writeNames,
			const char* readonlyNames,
			const char* mustHaveNames,
			const char* withoutNames,
			int enableFilter,
			void** outPointers,
			int maxRows);

		// Core API appended after the ref-API block to preserve existing field order.
		int      (*Application_GetRunInBackground)();
		void     (*Application_SetRunInBackground)(int enabled);
		void     (*Window_Restore)();
		int      (*Cursor_GetMode)();
		void     (*Cursor_SetMode)(int mode);
		uint64_t (*Cursor_GetTexture)();
		void     (*Cursor_SetTexture)(uint64_t assetId);

		// ── EntityCommandBuffer (appended for binary compat) ────────────
		// Resolve a component's serializedName / displayName to its stable
		// u32 typeId assigned by ComponentRegistry::Register. Called once
		// per component type at managed AppDomain load, the result is
		// cached in ComponentTypes<T>.NativeId, and every ECB command on
		// the hot path references components purely by this u32. Returns
		// 0 when the name is unknown (matching the "unregistered"
		// sentinel of ComponentRegistry::GetByTypeId).
		uint32_t (*Component_GetTypeId)(const char* componentName);

		// Replay an entire ECB payload against the active scene in one
		// P/Invoke. `buffer` is the byte-packed command stream produced
		// by the managed NativeEntityCommandBuffer recorder; `length`
		// bounds its size. On success, the runtime ID of each created
		// entity is written into `outRuntimeIds` (which must hold at
		// least `entityCount` ulong slots; managed code derives that
		// count from its own recorder state). Returns the number of
		// entities actually created, or a negative error code:
		//   -1 = invalid header / truncated buffer
		//   -2 = no active scene
		//   -3 = output buffer too small
		// Wire format documented in EntityCommandBufferWire.hpp.
		int (*Ecb_Playback)(const uint8_t* buffer, int length,
			uint64_t* outRuntimeIds, int maxOut);

		// ── JobSystem (appended for binary compat) ──────────────────────
		// Cross-runtime job dispatch — managed schedules feed into the
		// same native work-stealing pool that physics / particles use, so
		// the CLR ThreadPool no longer fights the engine pool for cores.
		//
		// Handles are monotonically-numbered uint64_t IDs (0 = invalid).
		// The native side keeps a small map from id → JobHandle + context
		// + releaseContext callback. Release calls releaseContext(context)
		// then erases the entry — managed dispatch passes a
		// `&FreeGCHandle` style callback that owns the per-job
		// GCHandle-wrapped box.

		// Enqueue a single work item. `work(context)` runs on a native
		// worker exactly once. `releaseContext(context)` fires on Release
		// (after Wait completes) so the managed side can free the
		// associated GCHandle. Returns 0 if the pool is shut down or the
		// map can't accept another entry (extremely unlikely in practice).
		uint64_t (*JobSystem_Enqueue)(
			void (*work)(void* context),
			void* context,
			void (*releaseContext)(void* context));

		// Partitioned dispatch. The native side fans out [begin, end) into
		// chunks of `batchSize` (or auto when 0) and invokes
		// `work(context, lo, hi)` once per chunk concurrently. Returns a
		// single handle that waits on all chunks. `releaseContext` fires
		// once on Release (NOT per chunk) — chunks share the GCHandle.
		uint64_t (*JobSystem_ParallelFor)(
			int begin, int end, int batchSize,
			void (*work)(void* context, int lo, int hi),
			void* context,
			void (*releaseContext)(void* context));

		// Block the calling thread until the handle completes. Drains the
		// queue while waiting (work-stealing) so a job that spawns
		// sub-jobs and waits on them cannot deadlock. Idempotent.
		void (*JobSystem_Wait)(uint64_t handle);

		// Non-blocking completion query. Returns 1 when the handle's work
		// is done, 0 otherwise. A zero / unknown handle returns 1
		// ("nothing to do" reads as done).
		int (*JobSystem_IsComplete)(uint64_t handle);

		// Free the handle's slot in the native side's map. Calls the
		// stored releaseContext(context) before erasing. Must be called
		// exactly once per Enqueue / ParallelFor return value, after
		// either Wait completes or IsComplete returned 1.
		void (*JobSystem_Release)(uint64_t handle);

		int (*JobSystem_GetWorkerCount)();
		int (*JobSystem_GetCallerWorkerIndex)(); // -1 when not on a worker

		// ── UI: Scrollbar / ScrollRect / Mask / CircularSlider /
		//       Layout Groups / ContentSizeFitter / WidthConstraint
		//       (appended for binary compat — order MUST match the C#
		//       NativeBindingsStruct mirror and the assignments at the
		//       end of ScriptBindings::PopulateNativeBindings.) ──

		// ── UI: Scrollbar ─────────────────────────────────────────────
		float    (*Scrollbar_GetValue)(uint64_t entityID);
		void     (*Scrollbar_SetValue)(uint64_t entityID, float value);
		float    (*Scrollbar_GetSize)(uint64_t entityID);
		void     (*Scrollbar_SetSize)(uint64_t entityID, float value);
		int      (*Scrollbar_GetNumberOfSteps)(uint64_t entityID);
		void     (*Scrollbar_SetNumberOfSteps)(uint64_t entityID, int value);
		int      (*Scrollbar_GetDirection)(uint64_t entityID);
		void     (*Scrollbar_SetDirection)(uint64_t entityID, int value);
		int      (*Scrollbar_GetIsReadOnly)(uint64_t entityID);
		void     (*Scrollbar_SetIsReadOnly)(uint64_t entityID, int value);
		uint64_t (*Scrollbar_GetHandleEntity)(uint64_t entityID);
		void     (*Scrollbar_SetHandleEntity)(uint64_t entityID, uint64_t refUuid);
		int      (*Scrollbar_GetValueChangedThisFrame)(uint64_t entityID);
		void     (*Scrollbar_MarkValueObserved)(uint64_t entityID);
		void     (*Scrollbar_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*Scrollbar_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*Scrollbar_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*Scrollbar_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*Scrollbar_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*Scrollbar_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*Scrollbar_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*Scrollbar_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*Scrollbar_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*Scrollbar_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);
		int      (*Scrollbar_GetTransitionMode)(uint64_t entityID);
		void     (*Scrollbar_SetTransitionMode)(uint64_t entityID, int mode);
		uint64_t (*Scrollbar_GetNormalSprite)(uint64_t entityID);
		void     (*Scrollbar_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Scrollbar_GetHoveredSprite)(uint64_t entityID);
		void     (*Scrollbar_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Scrollbar_GetPressedSprite)(uint64_t entityID);
		void     (*Scrollbar_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Scrollbar_GetDisabledSprite)(uint64_t entityID);
		void     (*Scrollbar_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*Scrollbar_GetFocusedSprite)(uint64_t entityID);
		void     (*Scrollbar_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		// ── UI: ScrollRect ────────────────────────────────────────────
		uint64_t (*ScrollRect_GetContent)(uint64_t entityID);
		void     (*ScrollRect_SetContent)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*ScrollRect_GetViewport)(uint64_t entityID);
		void     (*ScrollRect_SetViewport)(uint64_t entityID, uint64_t refUuid);
		int      (*ScrollRect_GetHorizontal)(uint64_t entityID);
		void     (*ScrollRect_SetHorizontal)(uint64_t entityID, int value);
		int      (*ScrollRect_GetVertical)(uint64_t entityID);
		void     (*ScrollRect_SetVertical)(uint64_t entityID, int value);
		int      (*ScrollRect_GetMovementType)(uint64_t entityID);
		void     (*ScrollRect_SetMovementType)(uint64_t entityID, int value);
		float    (*ScrollRect_GetElasticity)(uint64_t entityID);
		void     (*ScrollRect_SetElasticity)(uint64_t entityID, float value);
		int      (*ScrollRect_GetInertia)(uint64_t entityID);
		void     (*ScrollRect_SetInertia)(uint64_t entityID, int value);
		float    (*ScrollRect_GetDecelerationRate)(uint64_t entityID);
		void     (*ScrollRect_SetDecelerationRate)(uint64_t entityID, float value);
		float    (*ScrollRect_GetScrollSensitivity)(uint64_t entityID);
		void     (*ScrollRect_SetScrollSensitivity)(uint64_t entityID, float value);
		uint64_t (*ScrollRect_GetHorizontalScrollbar)(uint64_t entityID);
		void     (*ScrollRect_SetHorizontalScrollbar)(uint64_t entityID, uint64_t refUuid);
		uint64_t (*ScrollRect_GetVerticalScrollbar)(uint64_t entityID);
		void     (*ScrollRect_SetVerticalScrollbar)(uint64_t entityID, uint64_t refUuid);
		int      (*ScrollRect_GetHorizontalScrollbarVisibility)(uint64_t entityID);
		void     (*ScrollRect_SetHorizontalScrollbarVisibility)(uint64_t entityID, int value);
		int      (*ScrollRect_GetVerticalScrollbarVisibility)(uint64_t entityID);
		void     (*ScrollRect_SetVerticalScrollbarVisibility)(uint64_t entityID, int value);
		float    (*ScrollRect_GetHorizontalScrollbarSpacing)(uint64_t entityID);
		void     (*ScrollRect_SetHorizontalScrollbarSpacing)(uint64_t entityID, float value);
		float    (*ScrollRect_GetVerticalScrollbarSpacing)(uint64_t entityID);
		void     (*ScrollRect_SetVerticalScrollbarSpacing)(uint64_t entityID, float value);
		void     (*ScrollRect_GetNormalizedPosition)(uint64_t entityID, float* outX, float* outY);
		void     (*ScrollRect_SetNormalizedPosition)(uint64_t entityID, float x, float y);
		int      (*ScrollRect_GetValueChangedThisFrame)(uint64_t entityID);
		void     (*ScrollRect_MarkValueObserved)(uint64_t entityID);

		// ── UI: Mask ──────────────────────────────────────────────────
		int  (*Mask_GetShowMaskGraphic)(uint64_t entityID);
		void (*Mask_SetShowMaskGraphic)(uint64_t entityID, int value);

		// ── UI: CircularSlider ────────────────────────────────────────
		float    (*CircularSlider_GetValue)(uint64_t entityID);
		void     (*CircularSlider_SetValue)(uint64_t entityID, float value);
		float    (*CircularSlider_GetMinValue)(uint64_t entityID);
		void     (*CircularSlider_SetMinValue)(uint64_t entityID, float value);
		float    (*CircularSlider_GetMaxValue)(uint64_t entityID);
		void     (*CircularSlider_SetMaxValue)(uint64_t entityID, float value);
		int      (*CircularSlider_GetWholeNumbers)(uint64_t entityID);
		void     (*CircularSlider_SetWholeNumbers)(uint64_t entityID, int value);
		int      (*CircularSlider_GetIsReadOnly)(uint64_t entityID);
		void     (*CircularSlider_SetIsReadOnly)(uint64_t entityID, int value);
		float    (*CircularSlider_GetStartAngleDegrees)(uint64_t entityID);
		void     (*CircularSlider_SetStartAngleDegrees)(uint64_t entityID, float value);
		float    (*CircularSlider_GetSweepDegrees)(uint64_t entityID);
		void     (*CircularSlider_SetSweepDegrees)(uint64_t entityID, float value);
		int      (*CircularSlider_GetClockwise)(uint64_t entityID);
		void     (*CircularSlider_SetClockwise)(uint64_t entityID, int value);
		float    (*CircularSlider_GetRingThickness)(uint64_t entityID);
		void     (*CircularSlider_SetRingThickness)(uint64_t entityID, float value);
		int      (*CircularSlider_GetRingSegments)(uint64_t entityID);
		void     (*CircularSlider_SetRingSegments)(uint64_t entityID, int value);
		void     (*CircularSlider_GetBackgroundColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetBackgroundColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*CircularSlider_GetFillColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetFillColor)(uint64_t entityID, float r, float g, float b, float a);
		uint64_t (*CircularSlider_GetHandleEntity)(uint64_t entityID);
		void     (*CircularSlider_SetHandleEntity)(uint64_t entityID, uint64_t refUuid);
		int      (*CircularSlider_GetValueChangedThisFrame)(uint64_t entityID);
		void     (*CircularSlider_MarkValueObserved)(uint64_t entityID);
		void     (*CircularSlider_GetNormalColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetNormalColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*CircularSlider_GetHoveredColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetHoveredColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*CircularSlider_GetPressedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetPressedColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*CircularSlider_GetDisabledColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetDisabledColor)(uint64_t entityID, float r, float g, float b, float a);
		void     (*CircularSlider_GetFocusedColor)(uint64_t entityID, float* r, float* g, float* b, float* a);
		void     (*CircularSlider_SetFocusedColor)(uint64_t entityID, float r, float g, float b, float a);
		int      (*CircularSlider_GetTransitionMode)(uint64_t entityID);
		void     (*CircularSlider_SetTransitionMode)(uint64_t entityID, int mode);
		uint64_t (*CircularSlider_GetNormalSprite)(uint64_t entityID);
		void     (*CircularSlider_SetNormalSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*CircularSlider_GetHoveredSprite)(uint64_t entityID);
		void     (*CircularSlider_SetHoveredSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*CircularSlider_GetPressedSprite)(uint64_t entityID);
		void     (*CircularSlider_SetPressedSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*CircularSlider_GetDisabledSprite)(uint64_t entityID);
		void     (*CircularSlider_SetDisabledSprite)(uint64_t entityID, uint64_t uuid);
		uint64_t (*CircularSlider_GetFocusedSprite)(uint64_t entityID);
		void     (*CircularSlider_SetFocusedSprite)(uint64_t entityID, uint64_t uuid);

		// ── UI: HorizontalLayoutGroup ─────────────────────────────────
		float (*HorizontalLayoutGroup_GetPaddingLeft)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetPaddingLeft)(uint64_t entityID, float value);
		float (*HorizontalLayoutGroup_GetPaddingRight)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetPaddingRight)(uint64_t entityID, float value);
		float (*HorizontalLayoutGroup_GetPaddingTop)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetPaddingTop)(uint64_t entityID, float value);
		float (*HorizontalLayoutGroup_GetPaddingBottom)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetPaddingBottom)(uint64_t entityID, float value);
		float (*HorizontalLayoutGroup_GetSpacing)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetSpacing)(uint64_t entityID, float value);
		int   (*HorizontalLayoutGroup_GetChildAlignment)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetChildAlignment)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetReverseArrangement)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetReverseArrangement)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetControlChildWidth)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetControlChildWidth)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetControlChildHeight)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetControlChildHeight)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetUseChildScaleWidth)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetUseChildScaleWidth)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetUseChildScaleHeight)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetUseChildScaleHeight)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetChildForceExpandWidth)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetChildForceExpandWidth)(uint64_t entityID, int value);
		int   (*HorizontalLayoutGroup_GetChildForceExpandHeight)(uint64_t entityID);
		void  (*HorizontalLayoutGroup_SetChildForceExpandHeight)(uint64_t entityID, int value);

		// ── UI: VerticalLayoutGroup ───────────────────────────────────
		float (*VerticalLayoutGroup_GetPaddingLeft)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetPaddingLeft)(uint64_t entityID, float value);
		float (*VerticalLayoutGroup_GetPaddingRight)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetPaddingRight)(uint64_t entityID, float value);
		float (*VerticalLayoutGroup_GetPaddingTop)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetPaddingTop)(uint64_t entityID, float value);
		float (*VerticalLayoutGroup_GetPaddingBottom)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetPaddingBottom)(uint64_t entityID, float value);
		float (*VerticalLayoutGroup_GetSpacing)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetSpacing)(uint64_t entityID, float value);
		int   (*VerticalLayoutGroup_GetChildAlignment)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetChildAlignment)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetReverseArrangement)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetReverseArrangement)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetControlChildWidth)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetControlChildWidth)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetControlChildHeight)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetControlChildHeight)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetUseChildScaleWidth)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetUseChildScaleWidth)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetUseChildScaleHeight)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetUseChildScaleHeight)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetChildForceExpandWidth)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetChildForceExpandWidth)(uint64_t entityID, int value);
		int   (*VerticalLayoutGroup_GetChildForceExpandHeight)(uint64_t entityID);
		void  (*VerticalLayoutGroup_SetChildForceExpandHeight)(uint64_t entityID, int value);

		// ── UI: GridLayoutGroup ───────────────────────────────────────
		float (*GridLayoutGroup_GetPaddingLeft)(uint64_t entityID);
		void  (*GridLayoutGroup_SetPaddingLeft)(uint64_t entityID, float value);
		float (*GridLayoutGroup_GetPaddingRight)(uint64_t entityID);
		void  (*GridLayoutGroup_SetPaddingRight)(uint64_t entityID, float value);
		float (*GridLayoutGroup_GetPaddingTop)(uint64_t entityID);
		void  (*GridLayoutGroup_SetPaddingTop)(uint64_t entityID, float value);
		float (*GridLayoutGroup_GetPaddingBottom)(uint64_t entityID);
		void  (*GridLayoutGroup_SetPaddingBottom)(uint64_t entityID, float value);
		void  (*GridLayoutGroup_GetCellSize)(uint64_t entityID, float* outX, float* outY);
		void  (*GridLayoutGroup_SetCellSize)(uint64_t entityID, float x, float y);
		void  (*GridLayoutGroup_GetSpacing)(uint64_t entityID, float* outX, float* outY);
		void  (*GridLayoutGroup_SetSpacing)(uint64_t entityID, float x, float y);
		int   (*GridLayoutGroup_GetStartCorner)(uint64_t entityID);
		void  (*GridLayoutGroup_SetStartCorner)(uint64_t entityID, int value);
		int   (*GridLayoutGroup_GetStartAxis)(uint64_t entityID);
		void  (*GridLayoutGroup_SetStartAxis)(uint64_t entityID, int value);
		int   (*GridLayoutGroup_GetChildAlignment)(uint64_t entityID);
		void  (*GridLayoutGroup_SetChildAlignment)(uint64_t entityID, int value);
		int   (*GridLayoutGroup_GetConstraint)(uint64_t entityID);
		void  (*GridLayoutGroup_SetConstraint)(uint64_t entityID, int value);
		int   (*GridLayoutGroup_GetConstraintCount)(uint64_t entityID);
		void  (*GridLayoutGroup_SetConstraintCount)(uint64_t entityID, int value);
		int   (*GridLayoutGroup_GetReverse)(uint64_t entityID);
		void  (*GridLayoutGroup_SetReverse)(uint64_t entityID, int value);

		// ── UI: ContentSizeFitter ─────────────────────────────────────
		int   (*ContentSizeFitter_GetHorizontalFit)(uint64_t entityID);
		void  (*ContentSizeFitter_SetHorizontalFit)(uint64_t entityID, int value);
		int   (*ContentSizeFitter_GetVerticalFit)(uint64_t entityID);
		void  (*ContentSizeFitter_SetVerticalFit)(uint64_t entityID, int value);
		float (*ContentSizeFitter_GetPaddingLeft)(uint64_t entityID);
		void  (*ContentSizeFitter_SetPaddingLeft)(uint64_t entityID, float value);
		float (*ContentSizeFitter_GetPaddingRight)(uint64_t entityID);
		void  (*ContentSizeFitter_SetPaddingRight)(uint64_t entityID, float value);
		float (*ContentSizeFitter_GetPaddingTop)(uint64_t entityID);
		void  (*ContentSizeFitter_SetPaddingTop)(uint64_t entityID, float value);
		float (*ContentSizeFitter_GetPaddingBottom)(uint64_t entityID);
		void  (*ContentSizeFitter_SetPaddingBottom)(uint64_t entityID, float value);

		// ── UI: WidthConstraint ───────────────────────────────────────
		float (*WidthConstraint_GetMinWidth)(uint64_t entityID);
		void  (*WidthConstraint_SetMinWidth)(uint64_t entityID, float value);
		float (*WidthConstraint_GetMaxWidth)(uint64_t entityID);
		void  (*WidthConstraint_SetMaxWidth)(uint64_t entityID, float value);

		// ── CPU / JobSystem tuning (appended for binary compat) ──
		// Returns std::thread::hardware_concurrency() with a floor of 4
		// when the OS can't report a value. Exposed to scripts as
		// `Index.Application.ProcessorCount` so games can size their
		// own thread budgets against the host machine.
		int (*Application_GetProcessorCount)();

		// Resize the engine job pool. `workerCount <= 0` selects the
		// auto-resolved default; positive values pass through verbatim
		// (clamped to [1, 32]). Returns the resolved worker count after
		// the resize. Drains queued work and joins existing workers, so
		// safe to call at any time but may briefly stall the caller.
		int (*JobSystem_Reconfigure)(int workerCount);

		// ── SpriteRenderer filter (appended for binary compat) ──
		// Mirrors the inspector setter — writes the component's FilterMode
		// AND calls Texture2D::SetFilter on the bound texture so the
		// sampler is regenerated. Filter ints map to enum class Filter
		// {Point=0, Bilinear=1, Trilinear=2, Anisotropic=3}.
		int  (*SpriteRenderer_GetFilter)(uint64_t entityID);
		void (*SpriteRenderer_SetFilter)(uint64_t entityID, int filter);

		// ── Dynamic component registration (appended for binary compat) ──
		// Called from the C# host (DynamicComponentRegistrar) at user-
		// assembly load. Reflects over every struct annotated
		// [NativeComponent(..., Generate = true)] and registers it with
		// ComponentRegistry via this entry point — no codegen, no engine
		// rebuild. Returns the assigned stable typeIdU32, or 0 on failure
		// (duplicate name, zero size, etc.). Category: 0 = Component, 1 = Tag.
		uint32_t (*Component_RegisterDynamic)(
			const char* displayName,
			const char* serializedName,
			const char* subcategory,
			uint32_t category,
			uint32_t size,
			uint32_t alignment);

		// Drops every dynamic registration. Called from UnloadUserAssembly
		// BEFORE the ALC tears down so captured lambdas don't outlive the
		// storage they reference. Idempotent.
		void (*Component_UnregisterAllDynamic)();
	};

	/// Layout must match C# ManagedCallbacksStruct exactly.
	struct ManagedCallbacks
	{
		int32_t (*CreateScriptInstance)(const char* className, uint64_t entityID);
		void    (*DestroyScriptInstance)(int32_t handle);
		void    (*InvokeStart)(int32_t handle);
		void    (*InvokeUpdate)(int32_t handle);
		void    (*InvokeOnDestroy)(int32_t handle);
		void    (*InvokeOnEnable)(int32_t handle);
		void    (*InvokeOnDisable)(int32_t handle);
		void    (*InvokeCollisionEnter2D)(int32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		void    (*InvokeCollisionStay2D)(int32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		void    (*InvokeCollisionExit2D)(int32_t handle, uint64_t selfEntityID, uint64_t otherEntityID, uint64_t entityAID, uint64_t entityBID, float contactPointX, float contactPointY);
		int     (*ClassExists)(const char* className);
		int     (*LoadUserAssembly)(const char* path);
		void    (*UnloadUserAssembly)();
		const char* (*GetScriptFields)(int32_t handle);
		void    (*SetScriptField)(int32_t handle, const char* fieldName, const char* value);
		const char* (*GetClassFieldDefs)(const char* className);
		void    (*RaiseApplicationStart)();
		void    (*RaiseApplicationPaused)();
		void    (*RaiseApplicationQuit)();
		void    (*RaiseFocusChanged)(int focused);
		void    (*RaiseKeyDown)(int key);
		void    (*RaiseKeyUp)(int key);
		void    (*RaiseMouseDown)(int button);
		void    (*RaiseMouseUp)(int button);
		void    (*RaiseMouseScroll)(float delta);
		void    (*RaiseMouseMove)(float x, float y);
		void    (*RaiseBeforeSceneLoaded)(const char* sceneName);
		void    (*RaiseSceneLoaded)(const char* sceneName);
		void    (*RaiseBeforeSceneUnloaded)(const char* sceneName);
		void    (*RaiseSceneUnloaded)(const char* sceneName);
		int32_t (*CreateGameSystemInstance)(const char* className, const char* sceneName);
		void    (*DestroyGameSystemInstance)(int32_t handle);
		void    (*InvokeGameSystemStart)(int32_t handle);
		void    (*InvokeGameSystemUpdate)(int32_t handle);
		void    (*InvokeGameSystemEnable)(int32_t handle);
		void    (*InvokeGameSystemDisable)(int32_t handle);
		void    (*InvokeGameSystemDestroy)(int32_t handle);
		int     (*GameSystemClassExists)(const char* className);
		int32_t (*CreateGlobalSystemInstance)(const char* className);
		void    (*DestroyGlobalSystemInstance)(int32_t handle);
		void    (*InvokeGlobalSystemInitialize)(int32_t handle);
		void    (*InvokeGlobalSystemUpdate)(int32_t handle);
		void    (*InvokeGlobalSystemEnable)(int32_t handle);
		void    (*InvokeGlobalSystemDisable)(int32_t handle);
		int     (*GlobalSystemClassExists)(const char* className);

		// ── New lifecycle slots (appended for binary compat) ──
		void    (*InvokeAwake)(int32_t handle);
		void    (*InvokeFixedUpdate)(int32_t handle);
		void    (*InvokeGameSystemAwake)(int32_t handle);
		void    (*InvokeGameSystemFixedUpdate)(int32_t handle);
		void    (*InvokeGlobalSystemFixedUpdate)(int32_t handle);

		// ── GameSystem field reflection (appended for binary compat) ──
		const char* (*GetGameSystemFields)(int32_t handle);
		void        (*SetGameSystemField)(int32_t handle, const char* fieldName, const char* value);

		// ── UI event dispatch (appended for binary compat) ──
		// Called by native UIEventSystem once per frame after writing
		// transient UI state flags. The managed handler iterates UI
		// components and fans out to static UI events.
		void        (*RaiseUiEventDispatch)();

		// ── Coroutine pump (appended for binary compat) ──
		// Drains the IndexSynchronizationContext queue and ticks
		// pending EntityScript coroutine awaiters. PumpCoroutinesUpdate
		// is called once at the top of ScriptSystem::Update with the
		// frame's scaled delta time; PumpCoroutinesFixedUpdate is
		// called at the top of ScriptSystem::FixedUpdate.
		void        (*PumpCoroutinesUpdate)(float deltaTime);
		void        (*PumpCoroutinesFixedUpdate)();

		// ── Inspector event bindings (appended for binary compat) ──
		// Powers Unity-style "On Click ()" lists wired from the editor.
		// GetInvokableMethodsBuffer returns a JSON string array of every
		// invokable method declared on `className`'s user subclass — the
		// inspector's method-name combo populates from this. Each entry
		// is encoded as "<methodName>:<argKind>" where argKind is the
		// numeric value of `InspectorEventArgKind` describing the
		// method's first (and only) parameter type, or 0 for void.
		// Methods with more than one parameter or unsupported types
		// are filtered out at enumeration time.
		// InvokeScriptMethodByName invokes the named method on the
		// live instance addressed by `handle` and passes the typed
		// `argValue` parsed against `argKind`; returns 1 on success,
		// 0 when the method isn't on the class so the native side can
		// log-once. argValue may be null when argKind == 0 (Void).
		int         (*GetInvokableMethodsBuffer)(const char* className, char* outBuffer, int capacity);
		int         (*InvokeScriptMethodByName)(int32_t handle, const char* methodName,
			uint8_t argKind, const char* argValue);

		// ── Window events (appended for binary compat) ──
		// Native Window::SetWindowResizedCallback routes the resize event
		// through Application's dispatcher; the dispatcher fires this so
		// `Index.Window.OnResize` subscribers run on the same frame the
		// GLFW callback delivered the new framebuffer size.
		void        (*RaiseWindowResize)();
		void        (*RaiseEnterChar)(uint32_t codepoint);
	};

} // namespace Index
