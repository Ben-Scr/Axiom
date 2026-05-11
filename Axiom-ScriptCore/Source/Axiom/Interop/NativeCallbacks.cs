using System.Runtime.InteropServices;

namespace Axiom.Interop;
/// <summary>
/// Layout must match the C++ NativeBindings struct exactly (Sequential, blittable).
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeBindingsStruct
{
    // ── Application ──────────────────────────────────────────────
    public delegate* unmanaged<float> Application_GetDeltaTime;
    public delegate* unmanaged<float> Application_GetElapsedTime;
    public delegate* unmanaged<int> Application_GetScreenWidth;
    public delegate* unmanaged<int> Application_GetScreenHeight;
    public delegate* unmanaged<float> Application_GetTargetFrameRate;
    public delegate* unmanaged<float, void> Application_SetTargetFrameRate;
    public delegate* unmanaged<void> Application_Quit;
    public delegate* unmanaged<float> Application_GetFixedDeltaTime;
    public delegate* unmanaged<float> Application_GetUnscaledDeltaTime;
    public delegate* unmanaged<float> Application_GetFixedUnscaledDeltaTime;
    public delegate* unmanaged<float> Application_GetTimeScale;
    public delegate* unmanaged<float, void> Application_SetTimeScale;
    public delegate* unmanaged<int> Application_IsEditor; // 1 = host is editor, 0 = standalone runtime
    public delegate* unmanaged<byte*, int, int> Application_GetClipboardStringBuffer;
    public delegate* unmanaged<byte*, void> Application_SetClipboardString;
    public delegate* unmanaged<int> Application_GetVsyncEnabled;
    public delegate* unmanaged<int, void> Application_SetVsyncEnabled;

    // ── Window ───────────────────────────────────────────────────
    // Layout must match the Window_* block in ScriptGlue.hpp exactly.
    public delegate* unmanaged<int> Window_GetWidth;
    public delegate* unmanaged<int> Window_GetHeight;
    public delegate* unmanaged<byte*, int, int> Window_GetTitleBuffer;
    public delegate* unmanaged<byte*, void> Window_SetTitle;
    public delegate* unmanaged<void> Window_Minimize;
    public delegate* unmanaged<void> Window_Maximize;
    public delegate* unmanaged<int> Window_IsMaximized;
    public delegate* unmanaged<int> Window_IsFullScreen;
    public delegate* unmanaged<int, void> Window_SetFullScreen;
    public delegate* unmanaged<int*, int*, void> Window_GetPosition;
    public delegate* unmanaged<int, int, void> Window_SetPosition;
    public delegate* unmanaged<void> Window_Focus;
    public delegate* unmanaged<int*, int*, void> Window_GetScreenSize;

    // ── Engine ───────────────────────────────────────────────────
    public delegate* unmanaged<byte*, int, int> Engine_GetVersionBuffer;
    public delegate* unmanaged<byte*, int, int> Engine_GetVersionLongBuffer;
    public delegate* unmanaged<int> Engine_GetBuildConfiguration; // 0=Debug, 1=Development, 2=Release
    public delegate* unmanaged<byte*, int, int> Engine_GetPlatformBuffer;
    public delegate* unmanaged<byte*, int, int> Engine_GetGraphicsApiBuffer;
    public delegate* unmanaged<byte*, int, int> Engine_GetGpuVendorBuffer;
    public delegate* unmanaged<byte*, int, int> Engine_GetGpuRendererBuffer;

    // ── Time ─────────────────────────────────────────────────────
    public delegate* unmanaged<int> Time_GetFrameCount;
    public delegate* unmanaged<float> Time_GetTimeSinceStartup;
    public delegate* unmanaged<float> Time_GetRealtimeSinceStartup;

    // ── Log ──────────────────────────────────────────────────────
    public delegate* unmanaged<byte*, void> Log_Trace;
    public delegate* unmanaged<byte*, void> Log_Info;
    public delegate* unmanaged<byte*, void> Log_Warn;
    public delegate* unmanaged<byte*, void> Log_Error;

    // ── Input ────────────────────────────────────────────────────
    public delegate* unmanaged<int, int> Input_GetKey;
    public delegate* unmanaged<int, int> Input_GetKeyDown;
    public delegate* unmanaged<int, int> Input_GetKeyUp;
    public delegate* unmanaged<int, int> Input_GetMouseButton;
    public delegate* unmanaged<int, int> Input_GetMouseButtonDown;
    public delegate* unmanaged<int, int> Input_GetMouseButtonUp;
    public delegate* unmanaged<float*, float*, void> Input_GetMousePosition;
    public delegate* unmanaged<float*, float*, void> Input_GetAxis;
    public delegate* unmanaged<float*, float*, void> Input_GetMouseDelta;
    public delegate* unmanaged<float> Input_GetScrollWheelDelta;

    // ── Entity ───────────────────────────────────────────────────
    public delegate* unmanaged<ulong, int> Entity_IsValid;
    public delegate* unmanaged<byte*, ulong> Entity_FindByName;
    public delegate* unmanaged<ulong, void> Entity_Destroy;
    public delegate* unmanaged<byte*, ulong> Entity_Create;
    public delegate* unmanaged<ulong, ulong> Entity_Clone;
    public delegate* unmanaged<ulong, ulong> Entity_InstantiatePrefab;
    public delegate* unmanaged<ulong, int> Entity_GetOrigin;
    public delegate* unmanaged<ulong, ulong> Entity_GetRuntimeID;
    public delegate* unmanaged<ulong, ulong> Entity_GetSceneGUID;
    public delegate* unmanaged<ulong, ulong> Entity_GetPrefabGUID;
    public delegate* unmanaged<ulong, byte*, int> Entity_HasComponent;
    public delegate* unmanaged<ulong, byte*, int> Entity_AddComponent;
    public delegate* unmanaged<ulong, byte*, int> Entity_RemoveComponent;
    public delegate* unmanaged<ulong, byte*, byte*, int, int> Entity_GetManagedComponentFieldsBuffer;
    public delegate* unmanaged<ulong, int> Entity_GetIsStatic;
    public delegate* unmanaged<ulong, int, void> Entity_SetIsStatic;
    public delegate* unmanaged<ulong, int> Entity_GetIsEnabled;
    public delegate* unmanaged<ulong, int> Entity_GetIsEnabledInHierarchy;
    public delegate* unmanaged<ulong, int, void> Entity_SetIsEnabled;

    // ── NameComponent ────────────────────────────────────────────
    public delegate* unmanaged<ulong, byte*, int, int> NameComponent_GetNameBuffer;
    public delegate* unmanaged<ulong, byte*, void> NameComponent_SetName;

    // ── Transform2D ──────────────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, void> Transform2D_GetPosition;
    public delegate* unmanaged<ulong, float, float, void> Transform2D_SetPosition;
    public delegate* unmanaged<ulong, float> Transform2D_GetRotation;
    public delegate* unmanaged<ulong, float, void> Transform2D_SetRotation;
    public delegate* unmanaged<ulong, float*, float*, void> Transform2D_GetScale;
    public delegate* unmanaged<ulong, float, float, void> Transform2D_SetScale;
    public delegate* unmanaged<ulong, ulong> Transform2D_GetEntity;
    public delegate* unmanaged<ulong, float*, float*, void> Transform2D_GetLocalPosition;
    public delegate* unmanaged<ulong, float, float, void> Transform2D_SetLocalPosition;
    public delegate* unmanaged<ulong, float> Transform2D_GetLocalRotation;
    public delegate* unmanaged<ulong, float, void> Transform2D_SetLocalRotation;
    public delegate* unmanaged<ulong, float*, float*, void> Transform2D_GetLocalScale;
    public delegate* unmanaged<ulong, float, float, void> Transform2D_SetLocalScale;
    public delegate* unmanaged<ulong, ulong> Transform2D_GetParent;
    public delegate* unmanaged<ulong, ulong, int> Transform2D_SetParent;
    public delegate* unmanaged<ulong, int> Transform2D_GetChildCount;
    public delegate* unmanaged<ulong, int, ulong> Transform2D_GetChildAt;
    public delegate* unmanaged<ulong, ulong*, int, int> Transform2D_GetChildren;

    // ── SpriteRenderer ───────────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> SpriteRenderer_GetColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> SpriteRenderer_SetColor;
    public delegate* unmanaged<ulong, ulong> SpriteRenderer_GetTexture;
    public delegate* unmanaged<ulong, ulong, void> SpriteRenderer_SetTexture;
    public delegate* unmanaged<ulong, int> SpriteRenderer_GetSortingOrder;
    public delegate* unmanaged<ulong, int, void> SpriteRenderer_SetSortingOrder;
    public delegate* unmanaged<ulong, int> SpriteRenderer_GetSortingLayer;
    public delegate* unmanaged<ulong, int, void> SpriteRenderer_SetSortingLayer;

    // ── TextRenderer ─────────────────────────────────────────────
    public delegate* unmanaged<ulong, byte*, int, int> TextRenderer_GetTextBuffer;
    public delegate* unmanaged<ulong, byte*, void> TextRenderer_SetText;
    public delegate* unmanaged<ulong, ulong> TextRenderer_GetFont;
    public delegate* unmanaged<ulong, ulong, void> TextRenderer_SetFont;
    public delegate* unmanaged<ulong, float> TextRenderer_GetFontSize;
    public delegate* unmanaged<ulong, float, void> TextRenderer_SetFontSize;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> TextRenderer_GetColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> TextRenderer_SetColor;
    public delegate* unmanaged<ulong, float> TextRenderer_GetLetterSpacing;
    public delegate* unmanaged<ulong, float, void> TextRenderer_SetLetterSpacing;
    public delegate* unmanaged<ulong, int> TextRenderer_GetHAlign;
    public delegate* unmanaged<ulong, int, void> TextRenderer_SetHAlign;
    public delegate* unmanaged<ulong, int> TextRenderer_GetWrapMode;
    public delegate* unmanaged<ulong, int, void> TextRenderer_SetWrapMode;
    // WrapWidth slots removed — wrap area now comes from the host
    // RectTransform2D's width minus the TextRenderer's Margin (.x + .z).
    public delegate* unmanaged<ulong, int> TextRenderer_GetSortingOrder;
    public delegate* unmanaged<ulong, int, void> TextRenderer_SetSortingOrder;
    public delegate* unmanaged<ulong, int> TextRenderer_GetSortingLayer;
    public delegate* unmanaged<ulong, int, void> TextRenderer_SetSortingLayer;

    // ── Camera2D ─────────────────────────────────────────────────
    public delegate* unmanaged<ulong, float> Camera2D_GetOrthographicSize;
    public delegate* unmanaged<ulong, float, void> Camera2D_SetOrthographicSize;
    public delegate* unmanaged<ulong, float> Camera2D_GetZoom;
    public delegate* unmanaged<ulong, float, void> Camera2D_SetZoom;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Camera2D_GetClearColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Camera2D_SetClearColor;
    public delegate* unmanaged<ulong, float, float, float*, float*, void> Camera2D_ScreenToWorld;
    public delegate* unmanaged<ulong, float> Camera2D_GetViewportWidth;
    public delegate* unmanaged<ulong, float> Camera2D_GetViewportHeight;
    public delegate* unmanaged<ulong> Camera2D_GetMainEntity;

    // ── Rigidbody2D ──────────────────────────────────────────────
    public delegate* unmanaged<ulong, float, float, int, void> Rigidbody2D_ApplyForce;
    public delegate* unmanaged<ulong, float, float, int, void> Rigidbody2D_ApplyImpulse;
    public delegate* unmanaged<ulong, float*, float*, void> Rigidbody2D_GetLinearVelocity;
    public delegate* unmanaged<ulong, float, float, void> Rigidbody2D_SetLinearVelocity;
    public delegate* unmanaged<ulong, float> Rigidbody2D_GetAngularVelocity;
    public delegate* unmanaged<ulong, float, void> Rigidbody2D_SetAngularVelocity;
    public delegate* unmanaged<ulong, int> Rigidbody2D_GetBodyType;
    public delegate* unmanaged<ulong, int, void> Rigidbody2D_SetBodyType;
    public delegate* unmanaged<ulong, float> Rigidbody2D_GetGravityScale;
    public delegate* unmanaged<ulong, float, void> Rigidbody2D_SetGravityScale;
    public delegate* unmanaged<ulong, float> Rigidbody2D_GetMass;
    public delegate* unmanaged<ulong, float, void> Rigidbody2D_SetMass;

    // ── BoxCollider2D ────────────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, void> BoxCollider2D_GetScale;
    public delegate* unmanaged<ulong, float*, float*, void> BoxCollider2D_GetCenter;
    public delegate* unmanaged<ulong, int, void> BoxCollider2D_SetEnabled;

    // ── CircleCollider2D ─────────────────────────────────────────
    public delegate* unmanaged<ulong, float> CircleCollider2D_GetRadius;
    public delegate* unmanaged<ulong, float, void> CircleCollider2D_SetRadius;
    public delegate* unmanaged<ulong, float*, float*, void> CircleCollider2D_GetCenter;
    public delegate* unmanaged<ulong, float, float, void> CircleCollider2D_SetCenter;
    public delegate* unmanaged<ulong, int, void> CircleCollider2D_SetEnabled;

    // ── PolygonCollider2D ────────────────────────────────────────
    public delegate* unmanaged<ulong, int> PolygonCollider2D_GetVertexCount;
    public delegate* unmanaged<ulong, float*, int, int> PolygonCollider2D_GetWorldPoints;
    public delegate* unmanaged<ulong, float*, int, void> PolygonCollider2D_SetPoints;
    public delegate* unmanaged<ulong, int, void> PolygonCollider2D_SetSides;
    public delegate* unmanaged<ulong, float*, float*, void> PolygonCollider2D_GetCenter;
    public delegate* unmanaged<ulong, float, float, void> PolygonCollider2D_SetCenter;
    public delegate* unmanaged<ulong, float*, float*, void> PolygonCollider2D_GetSize;
    public delegate* unmanaged<ulong, float, float, void> PolygonCollider2D_SetSize;
    public delegate* unmanaged<ulong, int, void> PolygonCollider2D_SetEnabled;

    // ── AudioSource ──────────────────────────────────────────────
    public delegate* unmanaged<ulong, void> AudioSource_Play;
    public delegate* unmanaged<ulong, void> AudioSource_Pause;
    public delegate* unmanaged<ulong, void> AudioSource_Stop;
    public delegate* unmanaged<ulong, void> AudioSource_Resume;
    public delegate* unmanaged<ulong, float> AudioSource_GetVolume;
    public delegate* unmanaged<ulong, float, void> AudioSource_SetVolume;
    public delegate* unmanaged<ulong, float> AudioSource_GetPitch;
    public delegate* unmanaged<ulong, float, void> AudioSource_SetPitch;
    public delegate* unmanaged<ulong, int> AudioSource_GetLoop;
    public delegate* unmanaged<ulong, int, void> AudioSource_SetLoop;
    public delegate* unmanaged<ulong, int> AudioSource_IsPlaying;
    public delegate* unmanaged<ulong, int> AudioSource_IsPaused;
    public delegate* unmanaged<ulong, ulong> AudioSource_GetAudio;
    public delegate* unmanaged<ulong, ulong, void> AudioSource_SetAudio;

    // ── Axiom-Physics ─────────────────────────────────────────────
    public delegate* unmanaged<ulong, int> FastBody2D_GetBodyType;
    public delegate* unmanaged<ulong, int, void> FastBody2D_SetBodyType;
    public delegate* unmanaged<ulong, float> FastBody2D_GetMass;
    public delegate* unmanaged<ulong, float, void> FastBody2D_SetMass;
    public delegate* unmanaged<ulong, int> FastBody2D_GetUseGravity;
    public delegate* unmanaged<ulong, int, void> FastBody2D_SetUseGravity;
    public delegate* unmanaged<ulong, float*, float*, void> FastBody2D_GetVelocity;
    public delegate* unmanaged<ulong, float, float, void> FastBody2D_SetVelocity;
    public delegate* unmanaged<ulong, float*, float*, void> FastBoxCollider2D_GetHalfExtents;
    public delegate* unmanaged<ulong, float, float, void> FastBoxCollider2D_SetHalfExtents;
    public delegate* unmanaged<ulong, float> FastCircleCollider2D_GetRadius;
    public delegate* unmanaged<ulong, float, void> FastCircleCollider2D_SetRadius;

    // ── Scene Query ──────────────────────────────────────────────
    public delegate* unmanaged<byte*, int, int> Scene_GetActiveSceneNameBuffer;
    public delegate* unmanaged<int> Scene_GetEntityCount;
    public delegate* unmanaged<byte*, int> Scene_GetEntityCountByName;
    public delegate* unmanaged<byte*, int> Scene_LoadAdditive;
    public delegate* unmanaged<byte*, int> Scene_Load;
    public delegate* unmanaged<byte*, void> Scene_Unload;
    public delegate* unmanaged<byte*, int> Scene_SetActive;
    public delegate* unmanaged<byte*, int> Scene_Reload;
    public delegate* unmanaged<byte*, byte*, int, int> Scene_SetGameSystemEnabled;
    public delegate* unmanaged<byte*, int, void> Scene_SetGlobalSystemEnabled;
    public delegate* unmanaged<byte*, int> Scene_DoesSceneExist;
    public delegate* unmanaged<int> Scene_GetLoadedCount;
    public delegate* unmanaged<int, byte*, int, int> Scene_GetLoadedSceneNameAtBuffer;
    public delegate* unmanaged<ulong, byte*, int, int> Scene_GetEntityNameByUUIDBuffer;
    public delegate* unmanaged<byte*, ulong*, int, int> Scene_QueryEntities;
    public delegate* unmanaged<byte*, byte*, byte*, int, ulong*, int, int> Scene_QueryEntitiesFiltered;
    public delegate* unmanaged<byte*, byte*, ulong*, int, int> Scene_QueryEntitiesInScene;
    public delegate* unmanaged<byte*, byte*, byte*, byte*, int, ulong*, int, int> Scene_QueryEntitiesFilteredInScene;
    public delegate* unmanaged<ulong, int> Asset_IsValid;
    public delegate* unmanaged<byte*, ulong> Asset_GetOrCreateUUIDFromPath;
    public delegate* unmanaged<ulong, byte*, int, int> Asset_GetPathBuffer;
    public delegate* unmanaged<ulong, byte*, int, int> Asset_GetDisplayNameBuffer;
    public delegate* unmanaged<ulong, int> Asset_GetKind;
    public delegate* unmanaged<byte*, int, byte*, int, int> Asset_FindAllBuffer;
    public delegate* unmanaged<ulong, int> Texture_LoadAsset;
    public delegate* unmanaged<ulong, int> Texture_GetWidth;
    public delegate* unmanaged<ulong, int> Texture_GetHeight;
    public delegate* unmanaged<byte, ulong> Texture_GetDefaultAssetUUID;
    public delegate* unmanaged<ulong, int> Audio_LoadAsset;
    public delegate* unmanaged<ulong, float, void> Audio_PlayOneShotAsset;
    public delegate* unmanaged<ulong, int> Font_LoadAsset;

    // ── ParticleSystem2D ─────────────────────────────────────────
    public delegate* unmanaged<ulong, void> ParticleSystem2D_Play;
    public delegate* unmanaged<ulong, void> ParticleSystem2D_Pause;
    public delegate* unmanaged<ulong, void> ParticleSystem2D_Stop;
    public delegate* unmanaged<ulong, int> ParticleSystem2D_IsPlaying;
    public delegate* unmanaged<ulong, int> ParticleSystem2D_GetPlayOnAwake;
    public delegate* unmanaged<ulong, int, void> ParticleSystem2D_SetPlayOnAwake;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> ParticleSystem2D_GetColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> ParticleSystem2D_SetColor;
    public delegate* unmanaged<ulong, float> ParticleSystem2D_GetLifeTime;
    public delegate* unmanaged<ulong, float, void> ParticleSystem2D_SetLifeTime;
    public delegate* unmanaged<ulong, float> ParticleSystem2D_GetSpeed;
    public delegate* unmanaged<ulong, float, void> ParticleSystem2D_SetSpeed;
    public delegate* unmanaged<ulong, float> ParticleSystem2D_GetScale;
    public delegate* unmanaged<ulong, float, void> ParticleSystem2D_SetScale;
    public delegate* unmanaged<ulong, int> ParticleSystem2D_GetEmitOverTime;
    public delegate* unmanaged<ulong, int, void> ParticleSystem2D_SetEmitOverTime;
    public delegate* unmanaged<ulong, int, void> ParticleSystem2D_Emit;

    // ── Gizmos ───────────────────────────────────────────────────
    public delegate* unmanaged<float, float, float, float, void> Gizmo_DrawLine;
    public delegate* unmanaged<float, float, float, float, float, void> Gizmo_DrawSquare;
    public delegate* unmanaged<float, float, float, int, void> Gizmo_DrawCircle;
    public delegate* unmanaged<float, float, float, float, void> Gizmo_SetColor;
    public delegate* unmanaged<float*, float*, float*, float*, void> Gizmo_GetColor;
    public delegate* unmanaged<float> Gizmo_GetLineWidth;
    public delegate* unmanaged<float, void> Gizmo_SetLineWidth;

    // ── Physics2D ────────────────────────────────────────────────
    public delegate* unmanaged<float, float, float, float, float, ulong*, float*, float*, float*, float*, float*, int> Physics2D_Raycast;
    public delegate* unmanaged<float, float, float, int, ulong*, int> Physics2D_OverlapCircle;
    public delegate* unmanaged<float, float, float, float, float, int, ulong*, int> Physics2D_OverlapBox;
    public delegate* unmanaged<float, float, float*, int, int, ulong*, int> Physics2D_OverlapPolygon;
    public delegate* unmanaged<float, float, float, ulong*, int, int> Physics2D_OverlapCircleAll;
    public delegate* unmanaged<float, float, float, float, float, ulong*, int, int> Physics2D_OverlapBoxAll;
    public delegate* unmanaged<float, float, float*, int, ulong*, int, int> Physics2D_OverlapPolygonAll;
    public delegate* unmanaged<float, float, int, ulong*, int> Physics2D_ContainsPoint;
    public delegate* unmanaged<float, float, ulong*, int, int> Physics2D_ContainsPointAll;

    // ── UI: RectTransform2D ──────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetAnchorMin;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetAnchorMin;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetAnchorMax;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetAnchorMax;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetPivot;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetPivot;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetAnchoredPosition;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetAnchoredPosition;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetSizeDelta;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetSizeDelta;
    public delegate* unmanaged<ulong, float> RectTransform_GetRotation;
    public delegate* unmanaged<ulong, float, void> RectTransform_SetRotation;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetScale;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetScale;
    public delegate* unmanaged<ulong, float> RectTransform_GetLocalRotation;
    public delegate* unmanaged<ulong, float, void> RectTransform_SetLocalRotation;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetLocalScale;
    public delegate* unmanaged<ulong, float, float, void> RectTransform_SetLocalScale;
    public delegate* unmanaged<ulong, float*, float*, void> RectTransform_GetResolvedSize;

    // ── UI: Image ────────────────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Image_GetColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Image_SetColor;
    public delegate* unmanaged<ulong, ulong> Image_GetTexture;
    public delegate* unmanaged<ulong, ulong, void> Image_SetTexture;
    public delegate* unmanaged<ulong, int> Image_GetSortingOrder;
    public delegate* unmanaged<ulong, int, void> Image_SetSortingOrder;
    public delegate* unmanaged<ulong, int> Image_GetSortingLayer;
    public delegate* unmanaged<ulong, int, void> Image_SetSortingLayer;

    // ── UI: Interactable ─────────────────────────────────────────
    public delegate* unmanaged<ulong, int> Interactable_GetInteractable;
    public delegate* unmanaged<ulong, int, void> Interactable_SetInteractable;
    public delegate* unmanaged<ulong, int> Interactable_GetIsHovered;
    public delegate* unmanaged<ulong, int> Interactable_GetIsClicked;
    public delegate* unmanaged<ulong, int> Interactable_GetIsPressed;
    public delegate* unmanaged<ulong, int> Interactable_GetIsMouseDown;
    public delegate* unmanaged<ulong, int> Interactable_GetIsMouseUp;
    // Optional focus / selection navigation. Order must match
    // ScriptGlue.hpp's NativeBindings struct exactly.
    public delegate* unmanaged<ulong, int> Interactable_GetFocusable;
    public delegate* unmanaged<ulong, int, void> Interactable_SetFocusable;
    public delegate* unmanaged<ulong, int> Interactable_GetIsFocused;
    public delegate* unmanaged<ulong, int, void> Interactable_SetIsFocused;

    // ── UI: Button ───────────────────────────────────────────────
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Button_GetNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Button_SetNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Button_GetHoveredColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Button_SetHoveredColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Button_GetPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Button_SetPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Button_GetDisabledColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Button_SetDisabledColor;
    // Per-widget FocusedColor accessors are grouped together below
    // (still inside Button's logical block in C#) because the C++
    // side declares them in one block right after Button_SetDisabledColor
    // and the layout has to match.
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Button_GetFocusedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Button_SetFocusedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Toggle_GetFocusedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Toggle_SetFocusedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Slider_GetFocusedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Slider_SetFocusedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> InputField_GetFocusedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> InputField_SetFocusedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetFocusedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetFocusedColor;

    // ── UI: TransitionMode + per-state sprite UUIDs ──────────────
    // Order must match ScriptGlue.hpp's NativeBindings struct exactly.
    public delegate* unmanaged<ulong, int> Button_GetTransitionMode;
    public delegate* unmanaged<ulong, int, void> Button_SetTransitionMode;
    public delegate* unmanaged<ulong, int> Toggle_GetTransitionMode;
    public delegate* unmanaged<ulong, int, void> Toggle_SetTransitionMode;
    public delegate* unmanaged<ulong, int> Slider_GetTransitionMode;
    public delegate* unmanaged<ulong, int, void> Slider_SetTransitionMode;
    public delegate* unmanaged<ulong, int> InputField_GetTransitionMode;
    public delegate* unmanaged<ulong, int, void> InputField_SetTransitionMode;
    public delegate* unmanaged<ulong, int> Dropdown_GetTransitionMode;
    public delegate* unmanaged<ulong, int, void> Dropdown_SetTransitionMode;

    public delegate* unmanaged<ulong, ulong> Button_GetNormalSprite;
    public delegate* unmanaged<ulong, ulong, void> Button_SetNormalSprite;
    public delegate* unmanaged<ulong, ulong> Button_GetHoveredSprite;
    public delegate* unmanaged<ulong, ulong, void> Button_SetHoveredSprite;
    public delegate* unmanaged<ulong, ulong> Button_GetPressedSprite;
    public delegate* unmanaged<ulong, ulong, void> Button_SetPressedSprite;
    public delegate* unmanaged<ulong, ulong> Button_GetDisabledSprite;
    public delegate* unmanaged<ulong, ulong, void> Button_SetDisabledSprite;
    public delegate* unmanaged<ulong, ulong> Button_GetFocusedSprite;
    public delegate* unmanaged<ulong, ulong, void> Button_SetFocusedSprite;

    public delegate* unmanaged<ulong, ulong> Toggle_GetNormalSprite;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetNormalSprite;
    public delegate* unmanaged<ulong, ulong> Toggle_GetHoveredSprite;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetHoveredSprite;
    public delegate* unmanaged<ulong, ulong> Toggle_GetPressedSprite;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetPressedSprite;
    public delegate* unmanaged<ulong, ulong> Toggle_GetDisabledSprite;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetDisabledSprite;
    public delegate* unmanaged<ulong, ulong> Toggle_GetFocusedSprite;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetFocusedSprite;

    public delegate* unmanaged<ulong, ulong> Slider_GetNormalSprite;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetNormalSprite;
    public delegate* unmanaged<ulong, ulong> Slider_GetHoveredSprite;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetHoveredSprite;
    public delegate* unmanaged<ulong, ulong> Slider_GetPressedSprite;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetPressedSprite;
    public delegate* unmanaged<ulong, ulong> Slider_GetDisabledSprite;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetDisabledSprite;
    public delegate* unmanaged<ulong, ulong> Slider_GetFocusedSprite;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetFocusedSprite;

    public delegate* unmanaged<ulong, ulong> InputField_GetNormalSprite;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetNormalSprite;
    public delegate* unmanaged<ulong, ulong> InputField_GetHoveredSprite;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetHoveredSprite;
    public delegate* unmanaged<ulong, ulong> InputField_GetPressedSprite;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetPressedSprite;
    public delegate* unmanaged<ulong, ulong> InputField_GetDisabledSprite;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetDisabledSprite;
    public delegate* unmanaged<ulong, ulong> InputField_GetFocusedSprite;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetFocusedSprite;

    public delegate* unmanaged<ulong, ulong> Dropdown_GetNormalSprite;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetNormalSprite;
    public delegate* unmanaged<ulong, ulong> Dropdown_GetHoveredSprite;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetHoveredSprite;
    public delegate* unmanaged<ulong, ulong> Dropdown_GetPressedSprite;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetPressedSprite;
    public delegate* unmanaged<ulong, ulong> Dropdown_GetDisabledSprite;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetDisabledSprite;
    public delegate* unmanaged<ulong, ulong> Dropdown_GetFocusedSprite;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetFocusedSprite;

    // ── UI: IsReadOnly + entity-ref + popup-option colors ────────
    // Order must match ScriptGlue.hpp's NativeBindings struct.
    public delegate* unmanaged<ulong, int> Toggle_GetIsReadOnly;
    public delegate* unmanaged<ulong, int, void> Toggle_SetIsReadOnly;
    public delegate* unmanaged<ulong, int> Slider_GetIsReadOnly;
    public delegate* unmanaged<ulong, int, void> Slider_SetIsReadOnly;
    public delegate* unmanaged<ulong, int> Dropdown_GetIsReadOnly;
    public delegate* unmanaged<ulong, int, void> Dropdown_SetIsReadOnly;

    public delegate* unmanaged<ulong, ulong> Button_GetTargetGraphic;
    public delegate* unmanaged<ulong, ulong, void> Button_SetTargetGraphic;
    public delegate* unmanaged<ulong, ulong> Slider_GetFillEntity;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetFillEntity;
    public delegate* unmanaged<ulong, ulong> Slider_GetHandleEntity;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetHandleEntity;
    public delegate* unmanaged<ulong, ulong> Slider_GetBackgroundEntity;
    public delegate* unmanaged<ulong, ulong, void> Slider_SetBackgroundEntity;
    public delegate* unmanaged<ulong, ulong> Toggle_GetCheckmarkEntity;
    public delegate* unmanaged<ulong, ulong, void> Toggle_SetCheckmarkEntity;
    public delegate* unmanaged<ulong, ulong> InputField_GetTextEntity;
    public delegate* unmanaged<ulong, ulong, void> InputField_SetTextEntity;
    public delegate* unmanaged<ulong, ulong> Dropdown_GetLabelEntity;
    public delegate* unmanaged<ulong, ulong, void> Dropdown_SetLabelEntity;

    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetOptionNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetOptionNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetOptionHoverColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetOptionHoverColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetOptionPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetOptionPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetOptionSelectedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetOptionSelectedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetPopupBackgroundColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetPopupBackgroundColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetOptionTextColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetOptionTextColor;

    // ── UI: Slider ───────────────────────────────────────────────
    public delegate* unmanaged<ulong, float> Slider_GetValue;
    public delegate* unmanaged<ulong, float, void> Slider_SetValue;
    public delegate* unmanaged<ulong, float> Slider_GetMinValue;
    public delegate* unmanaged<ulong, float, void> Slider_SetMinValue;
    public delegate* unmanaged<ulong, float> Slider_GetMaxValue;
    public delegate* unmanaged<ulong, float, void> Slider_SetMaxValue;
    public delegate* unmanaged<ulong, int> Slider_GetWholeNumbers;
    public delegate* unmanaged<ulong, int, void> Slider_SetWholeNumbers;
    public delegate* unmanaged<ulong, int> Slider_GetValueChangedThisFrame;
    public delegate* unmanaged<ulong, void> Slider_MarkValueObserved;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Slider_GetNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Slider_SetNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Slider_GetHoveredColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Slider_SetHoveredColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Slider_GetPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Slider_SetPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Slider_GetDisabledColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Slider_SetDisabledColor;

    // ── UI: Toggle ───────────────────────────────────────────────
    public delegate* unmanaged<ulong, int> Toggle_GetIsOn;
    public delegate* unmanaged<ulong, int, void> Toggle_SetIsOn;
    public delegate* unmanaged<ulong, int> Toggle_GetValueChangedThisFrame;
    public delegate* unmanaged<ulong, void> Toggle_MarkIsOnObserved;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Toggle_GetNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Toggle_SetNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Toggle_GetHoveredColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Toggle_SetHoveredColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Toggle_GetPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Toggle_SetPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Toggle_GetDisabledColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Toggle_SetDisabledColor;

    // ── UI: InputField ───────────────────────────────────────────
    public delegate* unmanaged<ulong, byte*, int, int> InputField_GetTextBuffer;
    public delegate* unmanaged<ulong, byte*, void> InputField_SetText;
    public delegate* unmanaged<ulong, byte*, int, int> InputField_GetPlaceholderTextBuffer;
    public delegate* unmanaged<ulong, byte*, void> InputField_SetPlaceholderText;
    public delegate* unmanaged<ulong, int> InputField_GetIsFocused;
    public delegate* unmanaged<ulong, int, void> InputField_SetIsFocused;
    public delegate* unmanaged<ulong, int> InputField_GetSubmittedThisFrame;
    public delegate* unmanaged<ulong, int> InputField_GetCharacterLimit;
    public delegate* unmanaged<ulong, int, void> InputField_SetCharacterLimit;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> InputField_GetNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> InputField_SetNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> InputField_GetHoveredColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> InputField_SetHoveredColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> InputField_GetPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> InputField_SetPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> InputField_GetDisabledColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> InputField_SetDisabledColor;

    // ── UI: Dropdown ─────────────────────────────────────────────
    public delegate* unmanaged<ulong, int> Dropdown_GetSelectedIndex;
    public delegate* unmanaged<ulong, int, void> Dropdown_SetSelectedIndex;
    public delegate* unmanaged<ulong, int> Dropdown_GetIsOpen;
    public delegate* unmanaged<ulong, int, void> Dropdown_SetIsOpen;
    public delegate* unmanaged<ulong, int> Dropdown_GetSelectionChangedThisFrame;
    public delegate* unmanaged<ulong, void> Dropdown_MarkSelectedIndexObserved;
    public delegate* unmanaged<ulong, int> Dropdown_GetOptionCount;
    public delegate* unmanaged<ulong, int, byte*, int, int> Dropdown_GetOptionBuffer;
    public delegate* unmanaged<ulong, int, byte*, void> Dropdown_SetOption;
    public delegate* unmanaged<ulong, byte*, void> Dropdown_AddOption;
    public delegate* unmanaged<ulong, int, void> Dropdown_RemoveOption;
    public delegate* unmanaged<ulong, void> Dropdown_ClearOptions;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetNormalColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetNormalColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetHoveredColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetHoveredColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetPressedColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetPressedColor;
    public delegate* unmanaged<ulong, float*, float*, float*, float*, void> Dropdown_GetDisabledColor;
    public delegate* unmanaged<ulong, float, float, float, float, void> Dropdown_SetDisabledColor;

    // ── ECS ref-API (appended for binary compat with ScriptGlue.hpp) ──
    public delegate* unmanaged<ulong, byte*, void*> Entity_GetComponentPtr;
    public delegate* unmanaged<byte*, int> Entity_GetComponentSize;
    public delegate* unmanaged<byte*, byte*, byte*, byte*, byte*, int, void**, int, int> Scene_OpenQueryView;
}

internal static unsafe class NativeCallbacks
{
    internal static NativeBindingsStruct Bindings;

    internal static void SetFrom(NativeBindingsStruct* native)
    {
        Bindings = *native;
    }
}
