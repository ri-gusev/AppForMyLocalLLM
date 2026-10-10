# LocalAgent

Консольный чат на C++17 для Windows с локальной моделью `qwen3:8b`
через Ollama (`localhost:11434/api/chat`). Введите текст и нажмите Enter.
После ответа можно отправить следующую реплику. Выход — Ctrl+C
или конец входного потока. Пустые строки пропускаются; ошибка запроса
не завершает чат.

История успешных сообщений и ответов передаётся модели при каждом запросе
и хранится в памяти до завершения программы. Длина разговора ограничена
контекстным окном модели; автоматического сокращения истории пока нет.

## Сборка и запуск

Нужны CMake 3.20+, MinGW-w64 (g++ и mingw32-make в PATH), запущенный Ollama
и установленная модель `qwen3:8b`.

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug
.\build\local_agent.exe
```

## Структура

- `src/main.cpp` — консольный ввод и вывод.
- `include/ollama_client.h`, `src/ollama_client.cpp` — HTTP-клиент Ollama.
- `third_party/nlohmann/json.hpp` — JSON-библиотека версии 3.11.3 (MIT; лицензия внутри заголовка).
- `CMakeLists.txt`, `CMakePresets.json` — сборка.

HTTP работает через системный Windows WinHTTP. Зависимости при сборке
не скачиваются. Консоль использует UTF-8.
