using System.Threading.Tasks;
using Godot;
using Networked;

public partial class Backpack : Node
{
    public bool IsFull() => false;
}

public partial class Shop : Node
{
    public Variant Buy(StringName item) => default;
}

public sealed class Stock
{
    public Task<bool> Take(StringName item) => Task.FromResult(true);
}

public sealed class Scoreboard
{
    public void Rename(long from, long to) { }
}

public static class SampleStands
{
    public static Shop shop;
    public static Stock stock;
    public const string BansUrl = "https://api.mygame.com/bans/";
    public static HttpRequest http;
    public static Scoreboard scoreboard;
    public static Control loadingScreen;
    public static Node bombs;
    public static NetwIdToken idToken;

    public static void Steer(double delta) { }

    public static Node MakeBomb(Vector2 pos) => null;

    public static bool MayRemove(long peerId) => true;
}
