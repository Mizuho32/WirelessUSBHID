namespace VirtualHidClient;

public enum MouseButton
{
    Left,
    Right,
    Middle,
    Back,
    Forward,
}

[Flags]
public enum HidModifier : byte
{
    LeftCtrl = 1 << 0,
    LeftShift = 1 << 1,
    LeftAlt = 1 << 2,
    LeftGui = 1 << 3,
    RightCtrl = 1 << 4,
    RightShift = 1 << 5,
    RightAlt = 1 << 6,
    RightGui = 1 << 7,
}

/// <summary>OSごとのHID注入バックエンド。Windows=SendInput、Linuxは未実装スタブ。</summary>
public interface IHidInjector : IDisposable
{
    void MouseMove(short dx, short dy);
    void MouseButton(MouseButton button, bool down);
    void MouseWheel(sbyte delta);
    void MousePan(sbyte delta);
    void KeyEvent(byte hidUsage, bool down);
    void ModifierEvent(HidModifier modifier, bool down);
    void ConsumerEvent(ushort usageId, bool down);
}
