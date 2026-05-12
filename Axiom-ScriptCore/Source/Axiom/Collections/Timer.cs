using System.Diagnostics;

namespace Axiom.Collections;

public struct PerformanceTimer
{
    private long _start;
    private long _elapsed;
    private bool _running;

    public bool IsRunning => _running;

    public double ElapsedSeconds =>
        ElapsedRawTicks / (double)Stopwatch.Frequency;

    public float ElapsedSecondsF =>
        (float)ElapsedSeconds;

    public long ElapsedRawTicks =>
        _running
            ? _elapsed + Stopwatch.GetTimestamp() - _start
            : _elapsed;

    public void Start()
    {
        if (_running)
            return;

        _start = Stopwatch.GetTimestamp();
        _running = true;
    }

    public void Stop()
    {
        if (!_running)
            return;

        _elapsed += Stopwatch.GetTimestamp() - _start;
        _running = false;
    }

    public void Restart()
    {
        _elapsed = 0;
        _start = Stopwatch.GetTimestamp();
        _running = true;
    }

    public void Reset()
    {
        _start = 0;
        _elapsed = 0;
        _running = false;
    }

    public static PerformanceTimer StartNew()
    {
        PerformanceTimer sw = default;
        sw.Start();
        return sw;
    }
}

