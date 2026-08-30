namespace VirtualHidClient;

/// <summary>
/// UDPレポートは「今押されている状態」のスナップショット(USB HID Bootレポートと同じ)。
/// SendInput/uinputはdown/upのエッジイベントを要求するため、前回状態との差分を取って
/// 変化したキー/ボタンだけをdown/upイベントとして注入する。
/// </summary>
public sealed class PacketDispatcher
{
    private readonly IHidInjector _injector;
    private readonly HashSet<byte> _pressedKeys = new();
    private byte _modifiers;
    private byte _mouseButtons;
    private ushort _consumerUsage;

    public PacketDispatcher(IHidInjector injector)
    {
        _injector = injector;
    }

    public void Dispatch(in ParsedPacket packet)
    {
        switch (packet.Type)
        {
            case PacketType.Mouse:
                DispatchMouse(packet.Mouse);
                break;
            case PacketType.Keyboard:
                DispatchKeyboard(packet.Keyboard);
                break;
            case PacketType.Consumer:
                DispatchConsumer(packet.Consumer);
                break;
        }
    }

    private void DispatchMouse(in MousePacket m)
    {
        _injector.MouseMove(m.Dx, m.Dy);
        _injector.MouseWheel(m.Wheel);
        _injector.MousePan(m.Pan);

        DiffButtons(_mouseButtons, m.Buttons);
        _mouseButtons = m.Buttons;
    }

    private void DiffButtons(byte previous, byte current)
    {
        DiffBit(previous, current, 1 << 0, MouseButton.Left);
        DiffBit(previous, current, 1 << 1, MouseButton.Right);
        DiffBit(previous, current, 1 << 2, MouseButton.Middle);
        DiffBit(previous, current, 1 << 3, MouseButton.Back);
        DiffBit(previous, current, 1 << 4, MouseButton.Forward);
    }

    private void DiffBit(byte previous, byte current, int bit, MouseButton button)
    {
        var was = (previous & bit) != 0;
        var isNow = (current & bit) != 0;
        if (was == isNow) return;
        _injector.MouseButton(button, isNow);
    }

    private void DispatchKeyboard(in KeyboardPacket k)
    {
        foreach (HidModifier mod in Enum.GetValues<HidModifier>())
        {
            var was = (_modifiers & (byte)mod) != 0;
            var isNow = (k.Modifiers & (byte)mod) != 0;
            if (was != isNow) _injector.ModifierEvent(mod, isNow);
        }
        _modifiers = k.Modifiers;

        var current = new HashSet<byte>(k.Keycodes.Where(b => b != 0));

        foreach (var released in _pressedKeys.Except(current))
        {
            _injector.KeyEvent(released, down: false);
        }
        foreach (var pressed in current.Except(_pressedKeys))
        {
            _injector.KeyEvent(pressed, down: true);
        }

        _pressedKeys.Clear();
        foreach (var key in current) _pressedKeys.Add(key);
    }

    private void DispatchConsumer(in ConsumerPacket c)
    {
        if (_consumerUsage == c.UsageId) return;

        if (_consumerUsage != 0) _injector.ConsumerEvent(_consumerUsage, down: false);
        if (c.UsageId != 0) _injector.ConsumerEvent(c.UsageId, down: true);

        _consumerUsage = c.UsageId;
    }
}
