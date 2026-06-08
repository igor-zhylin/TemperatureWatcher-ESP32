# SensorDataViewer

Blazor Web App (.NET 10) для импорта CSV с датчиков ESP32 weather station и
просмотра графиков по всем метрикам.

## Что делает

- Загрузка CSV файла (формат эндпоинта `/api/export` устройства):
  `num,time,temperature_c,pressure_hpa,pressure_mmhg,altitude_m`
- Парсинг с обработкой ошибок (битые строки пропускаются с предупреждением).
- Графики (Chart.js) по каждой метрике: температура, давление (hPa и mmHg), высота.
- Сводка (кол-во записей, период, мин/макс по каждой метрике) и таблица данных.

## Запуск

```bash
cd SensorDataViewer
dotnet run
```

Открыть адрес из вывода (по умолчанию http://localhost:5022).
Файл `sample-data.csv` — пример для проверки импорта.

## Структура

- `Models/SensorReading.cs` — модель строки и список метрик.
- `Services/CsvImporter.cs` — парсер CSV (поддержка кавычек, инвариантная культура).
- `Components/Pages/Home.razor` — страница импорта и графиков.
- `wwwroot/js/charts.js` — обёртка Chart.js для JS interop.
