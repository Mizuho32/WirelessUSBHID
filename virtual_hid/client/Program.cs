using System.Net.WebSockets;
using System.Runtime.InteropServices;

namespace VirtualHidClient;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var config = Config.Parse(args);
        if (config is null) return 1;

        using IHidInjector injector = RuntimeInformation.IsOSPlatform(OSPlatform.Windows)
            ? new WindowsHidInjector()
            : new LinuxHidInjector();

        var dispatcher = new PacketDispatcher(injector);

        Console.WriteLine($"[VHID] target: {config.ServerUri}");

        while (true)
        {
            try
            {
                await RunOnce(config.ServerUri, dispatcher);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[VHID] connection error: {ex.Message}");
            }

            Console.WriteLine("[VHID] reconnecting in 3s...");
            await Task.Delay(TimeSpan.FromSeconds(3));
        }
    }

    private static async Task RunOnce(Uri serverUri, PacketDispatcher dispatcher)
    {
        using var ws = new ClientWebSocket();
        using var cts = new CancellationTokenSource();

        await ws.ConnectAsync(serverUri, cts.Token);
        Console.WriteLine("[VHID] connected");

        var buffer = new byte[PacketParser.PacketSize];

        while (ws.State == WebSocketState.Open)
        {
            var received = 0;
            WebSocketReceiveResult result;
            do
            {
                var segment = new ArraySegment<byte>(buffer, received, buffer.Length - received);
                result = await ws.ReceiveAsync(segment, cts.Token);
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    Console.WriteLine("[VHID] server closed connection");
                    return;
                }
                received += result.Count;
            } while (!result.EndOfMessage && received < buffer.Length);

            if (!result.EndOfMessage || received != PacketParser.PacketSize)
            {
                // サイズ不一致は破損/未知フォーマットとして無視する
                continue;
            }

            if (PacketParser.TryParse(buffer.AsSpan(0, received), out var packet))
            {
                dispatcher.Dispatch(packet);
            }
        }
    }
}
