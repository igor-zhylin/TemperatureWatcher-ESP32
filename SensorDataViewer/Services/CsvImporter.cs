using System.Globalization;
using System.Runtime.CompilerServices;
using SensorDataViewer.Models;

namespace SensorDataViewer.Services;

public sealed class CsvImportResult
{
    public List<SensorReading> Readings { get; } = [];
    public List<string> Warnings { get; } = [];
}

/// <summary>
/// Parses the CSV produced by the ESP32 weather station's /api/export endpoint.
/// Header: num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m
/// </summary>
public sealed class CsvImporter
{
    /// <summary>Buffers the whole stream into a result. Convenient for local files.</summary>
    public async Task<CsvImportResult> ImportAsync(Stream stream, CancellationToken ct = default)
    {
        var result = new CsvImportResult();
        await foreach (var reading in ParseAsync(stream, result.Warnings, ct))
            result.Readings.Add(reading);

        if (result.Readings.Count == 0)
            result.Warnings.Add("No valid data rows were found in the file.");

        return result;
    }

    /// <summary>
    /// Streams readings as they are parsed, so callers can update the UI while a slow
    /// source (e.g. the ESP32 over Wi-Fi) is still sending. Warnings accumulate into <paramref name="warnings"/>.
    /// </summary>
    public async IAsyncEnumerable<SensorReading> ParseAsync(
        Stream stream, List<string> warnings, [EnumeratorCancellation] CancellationToken ct = default)
    {
        using var reader = new StreamReader(stream);

        string? line = await reader.ReadLineAsync(ct);
        int lineNo = 1;

        // Skip an optional header row (detected by a non-numeric first field).
        if (line is not null && !int.TryParse(SplitCsv(line).FirstOrDefault(), out _))
        {
            line = await reader.ReadLineAsync(ct);
            lineNo++;
        }

        while (line is not null)
        {
            ct.ThrowIfCancellationRequested();
            if (!string.IsNullOrWhiteSpace(line))
            {
                var reading = TryParseRow(line, lineNo, warnings);
                if (reading is not null)
                    yield return reading;
            }
            line = await reader.ReadLineAsync(ct);
            lineNo++;
        }
    }

    private static SensorReading? TryParseRow(string line, int lineNo, List<string> warnings)
    {
        var fields = SplitCsv(line);
        if (fields.Length < 6)
        {
            warnings.Add($"Line {lineNo}: expected 6 columns, got {fields.Length} — skipped.");
            return null;
        }

        var ci = CultureInfo.InvariantCulture;
        try
        {
            return new SensorReading
            {
                Num = int.TryParse(fields[0], out var n) ? n : lineNo,
                Time = ParseTime(fields[1]),
                TemperatureC = double.Parse(fields[2], ci),
                PressureHpa = double.Parse(fields[3], ci),
                PressureMmHg = double.Parse(fields[4], ci),
                AltitudeM = double.Parse(fields[5], ci),
            };
        }
        catch (FormatException)
        {
            warnings.Add($"Line {lineNo}: could not parse numeric values — skipped.");
            return null;
        }
    }

    private static DateTime? ParseTime(string raw)
    {
        raw = raw.Trim().Trim('"');
        if (string.IsNullOrEmpty(raw)) return null;
        return DateTime.TryParse(raw, CultureInfo.InvariantCulture, DateTimeStyles.None, out var dt)
            ? dt
            : null;
    }

    /// <summary>Minimal CSV field splitter handling double-quoted fields.</summary>
    private static string[] SplitCsv(string line)
    {
        var fields = new List<string>();
        var field = new System.Text.StringBuilder();
        bool inQuotes = false;

        for (int i = 0; i < line.Length; i++)
        {
            char c = line[i];
            if (inQuotes)
            {
                if (c == '"')
                {
                    if (i + 1 < line.Length && line[i + 1] == '"') { field.Append('"'); i++; }
                    else inQuotes = false;
                }
                else field.Append(c);
            }
            else if (c == '"') inQuotes = true;
            else if (c == ',') { fields.Add(field.ToString()); field.Clear(); }
            else field.Append(c);
        }
        fields.Add(field.ToString());
        return fields.Select(f => f.Trim()).ToArray();
    }
}
