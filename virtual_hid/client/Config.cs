namespace VirtualHidClient;

/// <summary>
/// 設定はサーバーURLのみの最小構成。認証トークンが要る場合はURLのクエリに
/// 含める(例: wss://host/?token=xxxx)。 mds/virtual_hid/overview.md の
/// 「Virtual HIDはサーバーホストのみ設定項目の超シンプル構成」を踏襲。
/// </summary>
public sealed class Config
{
    public required Uri ServerUri { get; init; }

    public static Config? Parse(string[] args)
    {
        string? server = null;

        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--server":
                    if (i + 1 >= args.Length)
                    {
                        Console.Error.WriteLine("--server にはURLを指定してください");
                        return null;
                    }
                    server = args[++i];
                    break;
                default:
                    Console.Error.WriteLine($"不明な引数: {args[i]}");
                    return null;
            }
        }

        if (server is null)
        {
            PrintUsage();
            return null;
        }

        if (!Uri.TryCreate(server, UriKind.Absolute, out var uri) ||
            (uri.Scheme != "ws" && uri.Scheme != "wss"))
        {
            Console.Error.WriteLine($"--server はws://またはwss://のURLで指定してください: {server}");
            return null;
        }

        return new Config { ServerUri = uri };
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine("使い方: VirtualHidClient --server <ws(s)://host[:port][/path][?token=...]>");
        Console.Error.WriteLine("例:     VirtualHidClient --server wss://vhid.example.com/?token=SECRET");
    }
}
