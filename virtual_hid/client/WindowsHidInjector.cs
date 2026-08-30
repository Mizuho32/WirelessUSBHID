using System.Runtime.InteropServices;

namespace VirtualHidClient;

/// <summary>
/// user32.dll!SendInput によるOS入力キュー注入。
/// ドライバ・管理者権限不要(送信先が昇格ウィンドウの場合を除く)。
/// Raw Input/DirectInputを直接見るアプリ(主にゲーム)には合成入力として
/// 無視され得る点に注意 (mds/virtual_hid/overview.md 参照)。
/// </summary>
public sealed class WindowsHidInjector : IHidInjector
{
    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    private struct INPUT
    {
        public uint type;
        public InputUnion u;
    }

    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;

    private const uint MouseEventFMove = 0x0001;
    private const uint MouseEventFLeftDown = 0x0002;
    private const uint MouseEventFLeftUp = 0x0004;
    private const uint MouseEventFRightDown = 0x0008;
    private const uint MouseEventFRightUp = 0x0010;
    private const uint MouseEventFMiddleDown = 0x0020;
    private const uint MouseEventFMiddleUp = 0x0040;
    private const uint MouseEventFXDown = 0x0080;
    private const uint MouseEventFXUp = 0x0100;
    private const uint MouseEventFWheel = 0x0800;
    private const uint MouseEventFHWheel = 0x1000;

    private const uint XButton1 = 0x0001;
    private const uint XButton2 = 0x0002;

    private const uint KeyEventFKeyUp = 0x0002;

    private const int WheelDelta = 120;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    public void MouseMove(short dx, short dy)
    {
        if (dx == 0 && dy == 0) return;
        Send(Mouse(MouseEventFMove, dx: dx, dy: dy));
    }

    public void MouseButton(MouseButton button, bool down)
    {
        var (flags, data) = button switch
        {
            VirtualHidClient.MouseButton.Left => (down ? MouseEventFLeftDown : MouseEventFLeftUp, 0u),
            VirtualHidClient.MouseButton.Right => (down ? MouseEventFRightDown : MouseEventFRightUp, 0u),
            VirtualHidClient.MouseButton.Middle => (down ? MouseEventFMiddleDown : MouseEventFMiddleUp, 0u),
            VirtualHidClient.MouseButton.Back => (down ? MouseEventFXDown : MouseEventFXUp, XButton1),
            VirtualHidClient.MouseButton.Forward => (down ? MouseEventFXDown : MouseEventFXUp, XButton2),
            _ => throw new ArgumentOutOfRangeException(nameof(button)),
        };
        Send(Mouse(flags, mouseData: data));
    }

    public void MouseWheel(sbyte delta)
    {
        if (delta == 0) return;
        Send(Mouse(MouseEventFWheel, mouseData: unchecked((uint)(delta * WheelDelta))));
    }

    public void MousePan(sbyte delta)
    {
        if (delta == 0) return;
        Send(Mouse(MouseEventFHWheel, mouseData: unchecked((uint)(delta * WheelDelta))));
    }

    public void KeyEvent(byte hidUsage, bool down)
    {
        if (HidKeycodeMap.TryGetVirtualKey(hidUsage, out var vk)) SendKey(vk, down);
    }

    public void ModifierEvent(HidModifier modifier, bool down) => SendKey(HidKeycodeMap.ModifierVirtualKey(modifier), down);

    public void ConsumerEvent(ushort usageId, bool down)
    {
        if (HidKeycodeMap.TryGetConsumerVirtualKey(usageId, out var vk)) SendKey(vk, down);
    }

    private static INPUT Mouse(uint flags, int dx = 0, int dy = 0, uint mouseData = 0) => new()
    {
        type = InputMouse,
        u = new InputUnion { mi = new MOUSEINPUT { dx = dx, dy = dy, dwFlags = flags, mouseData = mouseData } },
    };

    private void SendKey(ushort vk, bool down) => Send(new INPUT
    {
        type = InputKeyboard,
        u = new InputUnion { ki = new KEYBDINPUT { wVk = vk, dwFlags = down ? 0u : KeyEventFKeyUp } },
    });

    private static void Send(INPUT input)
    {
        var inputs = new[] { input };
        if (SendInput(1, inputs, Marshal.SizeOf<INPUT>()) == 0)
        {
            Console.Error.WriteLine($"[WinHid] SendInput failed, GetLastError={Marshal.GetLastWin32Error()}");
        }
    }

    public void Dispose()
    {
    }
}
