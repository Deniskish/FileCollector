# FileCollector

Потокобезопасный сборщик файлов из чанков, приходящих в произвольном порядке
и с пересечениями. Header-only, C++20, без зависимостей (GTest — только для тестов).

## Сборка и тесты

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# с ThreadSanitizer / AddressSanitizer (GCC/Clang)
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DFC_SANITIZER=thread
```

GoogleTest берётся из системы (`find_package`), иначе скачивается через FetchContent.

## Интерфейс

| Метод | Назначение |
|---|---|
| `CollectFile(id, size)` | Регистрирует файл, выделяет буфер один раз |
| `OnNewChunk(id, pos, chunk&&)` | Добавляет чанк; до `CollectFile` чанки буферизуются |
| `TryTakeFile(id)` | Полный файл перемещением, без копирования; `nullopt`, если не готов |
| `WaitAndTakeFile(id, timeout)` | То же с ожиданием завершения сборки |
| `ReadRange(id, pos, span)` | Частичное чтение уже полученного диапазона в буфер вызывающего |
| `GetReceivedRanges(id)` / `GetProgress(id)` | Что уже получено |

## Возможные улучшения

- Шардирование таблицы файлов при очень большом числе новых id.
- Отображение больших файлов в память (`mmap`) вместо `std::vector`.
- `ReadRange` сейчас копирует под мьютексом файла; при необходимости
  буфер можно держать в `shared_ptr` и копировать вне блокировки.
