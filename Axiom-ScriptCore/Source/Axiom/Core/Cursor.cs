
namespace Axiom;

public enum CursorMode
{
    /// <summary>Cursor is visible and behaves normally.</summary>
    Default = 0,
    /// <summary>Cursor is locked to the center of the window.</summary>
    Locked = 1,
    /// <summary>Cursor is hidden.</summary>
    Hidden = 3,
    /// <summary>Cursor is hidden and is locked to the center of the window.</summary>
    HiddenLocked = 2,
}

public static class Cursor
{
    public static CursorMode Mode
    {
        get => throw new System.NotImplementedException();
        set => throw new System.NotImplementedException();
    }

    /// <summary>The current texture of the cursor</summary>
    public static Texture Texture
    {
        get => throw new System.NotImplementedException();
        set => throw new System.NotImplementedException();
    }
}

