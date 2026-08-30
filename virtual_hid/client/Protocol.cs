using System.Buffers.Binary;

namespace VirtualHidClient;

/// <summary>
/// virtual_hid/PROTOCOL.md 記載の16byte固定フォーマット。
/// esp32-kvm-ip/server/protocol.py とバイト配置は同一だが、独立実装。
/// </summary>
public enum PacketType : byte
{
    Mouse = 0x01,
    Keyboard = 0x02,
    Consumer = 0x03,
}

public readonly struct MousePacket
{
    public required byte Buttons { get; init; }
    public required short Dx { get; init; }
    public required short Dy { get; init; }
    public required sbyte Wheel { get; init; }
    public required sbyte Pan { get; init; }
}

public readonly struct KeyboardPacket
{
    public required byte Modifiers { get; init; }
    /// <summary>USB HID Usage ID、最大6個。未使用スロットは0x00。</summary>
    public required byte[] Keycodes { get; init; }
}

public readonly struct ConsumerPacket
{
    public required ushort UsageId { get; init; }
}

public readonly struct ParsedPacket
{
    public required PacketType Type { get; init; }
    public MousePacket Mouse { get; init; }
    public KeyboardPacket Keyboard { get; init; }
    public ConsumerPacket Consumer { get; init; }
}

public static class PacketParser
{
    private const ushort Magic = 0xCAFE;
    public const int PacketSize = 16;

    public static bool TryParse(ReadOnlySpan<byte> data, out ParsedPacket packet)
    {
        packet = default;
        if (data.Length != PacketSize) return false;
        if (BinaryPrimitives.ReadUInt16LittleEndian(data) != Magic) return false;

        var typeByte = data[6];
        if (!Enum.IsDefined(typeof(PacketType), typeByte)) return false;
        var type = (PacketType)typeByte;
        var payload = data[8..16];

        switch (type)
        {
            case PacketType.Mouse:
                packet = new ParsedPacket
                {
                    Type = type,
                    Mouse = new MousePacket
                    {
                        Buttons = payload[0],
                        Dx = BinaryPrimitives.ReadInt16LittleEndian(payload[1..3]),
                        Dy = BinaryPrimitives.ReadInt16LittleEndian(payload[3..5]),
                        Wheel = unchecked((sbyte)payload[5]),
                        Pan = unchecked((sbyte)payload[6]),
                    },
                };
                return true;

            case PacketType.Keyboard:
                packet = new ParsedPacket
                {
                    Type = type,
                    Keyboard = new KeyboardPacket
                    {
                        Modifiers = payload[0],
                        Keycodes = payload[2..8].ToArray(),
                    },
                };
                return true;

            case PacketType.Consumer:
                packet = new ParsedPacket
                {
                    Type = type,
                    Consumer = new ConsumerPacket
                    {
                        UsageId = BinaryPrimitives.ReadUInt16LittleEndian(payload[0..2]),
                    },
                };
                return true;

            default:
                return false;
        }
    }
}
