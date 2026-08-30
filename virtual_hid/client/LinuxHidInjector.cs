namespace VirtualHidClient;

/// <summary>
/// 未実装スタブ。実装する場合は /dev/uinput への ioctl P/Invoke を想定
/// (mds/virtual_hid/overview.md 参照)。
/// </summary>
public sealed class LinuxHidInjector : IHidInjector
{
    public LinuxHidInjector()
    {
        throw new NotImplementedException("Linux virtual HID (uinput) is not implemented yet.");
    }

    public void MouseMove(short dx, short dy) => throw new NotImplementedException();
    public void MouseButton(MouseButton button, bool down) => throw new NotImplementedException();
    public void MouseWheel(sbyte delta) => throw new NotImplementedException();
    public void MousePan(sbyte delta) => throw new NotImplementedException();
    public void KeyEvent(byte hidUsage, bool down) => throw new NotImplementedException();
    public void ModifierEvent(HidModifier modifier, bool down) => throw new NotImplementedException();
    public void ConsumerEvent(ushort usageId, bool down) => throw new NotImplementedException();
    public void Dispose() { }
}
