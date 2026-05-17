using System;

namespace Index;
/// <summary>
/// 2D Perlin gradient noise. Provides per-point sampling primitives only —
/// callers iterate their own grid/area to build a noise map.
/// </summary>
public sealed class PerlinNoise2D
{
    private const int TableSize = 256;
    private const int TableMask = TableSize - 1;

    private readonly int[] perm = new int[TableSize * 2];
    private int seed;

    // Output scale: 2D Perlin's theoretical range is roughly [-√(1/2), √(1/2)].
    // Multiplying by √2 normalizes Sample() to ~[-1, 1].
    private static readonly float OutputScale = Mathf.Sqrt(2.0f);

    // ── Construction ─────────────────────────────────────────────
    public PerlinNoise2D()
    {
        int s = (int)(DateTime.Now.Ticks ^ Guid.NewGuid().GetHashCode());
        SetSeed(s);
    }

    public PerlinNoise2D(int seed)
    {
        SetSeed(seed);
    }

    // ── Seed ─────────────────────────────────────────────────────
    public void SetSeed(int seed)
    {
        this.seed = seed;
        BuildPermutationTable(seed);
    }

    public int GetSeed() => seed;

    private void BuildPermutationTable(int seed)
    {
        for (int i = 0; i < TableSize; i++)
            perm[i] = i;

        // Local xorshift PRNG seeded from `seed`; kept independent of Index.Random
        // so this class has no cross-coupling.
        uint state = (uint)seed;
        if (state == 0) state = 0x9E3779B1u;

        for (int i = TableSize - 1; i > 0; i--)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;

            int j = (int)(state % (uint)(i + 1));
            (perm[i], perm[j]) = (perm[j], perm[i]);
        }

        // Duplicate to avoid index wrap on lookup.
        for (int i = 0; i < TableSize; i++)
            perm[TableSize + i] = perm[i];
    }

    // ── Sampling ─────────────────────────────────────────────────
    /// <summary>Gradient noise at (x, y), output in approximately [-1, 1].</summary>
    public float Sample(float x, float y)
    {
        int xi = (int)Mathf.Floor(x) & TableMask;
        int yi = (int)Mathf.Floor(y) & TableMask;

        float xf = x - Mathf.Floor(x);
        float yf = y - Mathf.Floor(y);

        float u = Fade(xf);
        float v = Fade(yf);

        int aa = perm[perm[xi]     + yi];
        int ab = perm[perm[xi]     + yi + 1];
        int ba = perm[perm[xi + 1] + yi];
        int bb = perm[perm[xi + 1] + yi + 1];

        float x1 = Mathf.LerpUnclamped(Grad(aa, xf,         yf),         Grad(ba, xf - 1.0f, yf),         u);
        float x2 = Mathf.LerpUnclamped(Grad(ab, xf,         yf - 1.0f),  Grad(bb, xf - 1.0f, yf - 1.0f),  u);

        return Mathf.LerpUnclamped(x1, x2, v) * OutputScale;
    }

    /// <summary>Same as <see cref="Sample"/> remapped to [0, 1].</summary>
    public float Sample01(float x, float y) => Mathf.Clamp01(Sample(x, y) * 0.5f + 0.5f);

    /// <summary>
    /// Fractal Brownian Motion: sum of <paramref name="octaves"/> Perlin octaves.
    /// Each octave multiplies frequency by <paramref name="lacunarity"/> and
    /// amplitude by <paramref name="persistence"/>. Result is normalized to ~[-1, 1].
    /// </summary>
    public float SampleFractal(float x, float y, int octaves, float persistence, float lacunarity)
    {
        if (octaves <= 0) throw new ArgumentOutOfRangeException(nameof(octaves));
        if (lacunarity <= 0.0f) throw new ArgumentOutOfRangeException(nameof(lacunarity));

        float amplitude = 1.0f;
        float frequency = 1.0f;
        float sum = 0.0f;
        float maxAmplitude = 0.0f;

        for (int i = 0; i < octaves; i++)
        {
            sum += Sample(x * frequency, y * frequency) * amplitude;
            maxAmplitude += amplitude;
            amplitude *= persistence;
            frequency *= lacunarity;
        }

        return maxAmplitude > 0.0f ? sum / maxAmplitude : 0.0f;
    }

    /// <summary>Same as <see cref="SampleFractal"/> remapped to [0, 1].</summary>
    public float SampleFractal01(float x, float y, int octaves, float persistence, float lacunarity)
        => Mathf.Clamp01(SampleFractal(x, y, octaves, persistence, lacunarity) * 0.5f + 0.5f);

    // ── Internals ────────────────────────────────────────────────
    private static float Fade(float t) => t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);

    private static float Grad(int hash, float x, float y)
    {
        // 8-direction 2D gradient table selected by the low 3 bits of the hash.
        switch (hash & 7)
        {
            case 0: return  x + y;
            case 1: return -x + y;
            case 2: return  x - y;
            case 3: return -x - y;
            case 4: return  x;
            case 5: return -x;
            case 6: return  y;
            default: return -y;
        }
    }
}
