namespace SensorDataViewer.Models;

/// <summary>
/// One row from the device CSV export
/// (num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m).
/// </summary>
public sealed class SensorReading
{
    public int Num { get; init; }
    public DateTime? Time { get; init; }
    public double TemperatureC { get; init; }
    public double PressureHpa { get; init; }
    public double PressureMmHg { get; init; }
    public double AltitudeM { get; init; }
}

/// <summary>Describes one plottable metric so the UI can render a chart per column.</summary>
public sealed record Metric(string Key, string Label, string Unit, string Color, Func<SensorReading, double> Selector);

public static class Metrics
{
    public static readonly IReadOnlyList<Metric> All =
    [
        new("temperature_c", "Temperature", "°C", "#e74c3c", r => r.TemperatureC),
        new("pressure_hpa", "Pressure", "hPa", "#3498db", r => r.PressureHpa),
        new("pressure_mmhg", "Pressure", "mmHg", "#9b59b6", r => r.PressureMmHg),
        new("altitude_m", "Altitude", "m", "#27ae60", r => r.AltitudeM),
    ];
}
