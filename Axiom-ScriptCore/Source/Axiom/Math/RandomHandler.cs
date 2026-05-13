
namespace Axiom;

public static class RandomHandler
{
    private static Random random = new Random();

    public static void SetSeed(ulong seed) => random.SetSeed(seed);
    public static void RemoveSeed() => random.RemoveSeed();
    public static bool NextBool() => random.NextBool();
    public static byte NextByte() => random.NextByte();
    public static byte NextByte(byte max) => random.NextByte(max);
    public static byte NextByte(byte min, byte max) => random.NextByte(min, max);

    public static int NextInt(int min, int max) => random.NextInt(min, max);
    public static int NextInt(int max) => random.NextInt(max);

    public static float NextFloat(float min, float max) => random.NextFloat(min, max);
    public static float NextFloat(float max) => random.NextFloat(max);

    public static double NextDouble(double min, double max) => random.NextDouble(min, max);
    public static double NextDouble(double max) => random.NextDouble(max);

    public static string NextString(int length = 10, string? charset = null) => random.NextString(length, charset);

    public static T Next<T>(T min, T max) where T : IComparable<T> => random.Next(min, max);
    public static T Next<T>(T max) where T : IComparable<T> => random.Next(max);
    public static T Next<T>() where T : IComparable<T> => random.Next<T>();
}
