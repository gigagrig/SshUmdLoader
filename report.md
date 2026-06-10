# Отчет о проделанной работе

## Шаг 2. Контракты

- Добавлен минимальный CMake-каркас с interface target `sshumdloader_contracts`.
- Добавлены доменные модели в `include/SshUmdLoader/Domain.h`.
- Добавлены порты Clean Architecture в `include/SshUmdLoader/Ports.h`.
- Добавлены объявления use case и фасада приложения в `include/SshUmdLoader/UseCases.h`.
- Добавлен пример композиции зависимостей через `ApplicationDependencies` и `ComposeApplication`.
- Добавлены `.gitignore` и `.hgignore` для раздельных debug/release build-директорий.

Реализация инфраструктуры и бизнес-алгоритмов пока намеренно не добавлялась: текущий инкремент фиксирует контракты и точки Dependency Injection для следующего шага.

Проверка:

- `cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`
- `cmake --build build-debug -j $(nproc)`
- `g++ -std=c++14 -Iinclude -fsyntax-only include/SshUmdLoader/Domain.h include/SshUmdLoader/Ports.h include/SshUmdLoader/UseCases.h include/SshUmdLoader/Composition.h`

## Шаг 3. Ядро и инфраструктура

- Реализованы `MarketDataDate`, сравнение дат и парсинг `YYYY.MM.DD`.
- Реализован `BuildDownloadPlanUseCase`: рекурсивный обход удаленной иерархии через `IUmdRemoteRepository`, фильтрация директорий дат, тикеров и `.zst` файлов.
- Реализован `RunDownloadPlanUseCase` с валидацией `MaxConcurrentDownloads`.
- Реализованы инфраструктурные адаптеры:
  - `CliOptionsParser`
  - `IniUmdConfigRepository` на базе `../Base/BaseLib/INI/INIReader`
  - `DefaultPathMapper`
  - `OverwriteTransferPolicy`
  - `ConsoleProgressSink`
  - `Libssh2UmdRemoteRepository`
  - `Libssh2DownloadScheduler`
- Обновлен CMake: подключен `../Base/BaseLib`, добавлена библиотека `sshumdloader_core` и smoke test `sshumdloader_core_smoke_test`.
- Добавлен `tests/CoreSmokeTest.cpp` для проверки ядра и чтения текущего INI-конфига.

После установки `libssh2-1-dev` реализован реальный SFTP-адаптер:

- CMake ищет `libssh2` через `pkg-config`.
- SSH/SFTP-сессии работают через `libssh2_session_set_blocking(session, 0)`.
- Ожидание готовности сокетов реализовано через `select`.
- `Libssh2UmdRemoteRepository` читает директории по SFTP и возвращает `RemoteEntry` с типом и размером.
- `Libssh2DownloadScheduler` запускает до `MaxConcurrentDownloads` активных загрузок без потоков ОС, пишет во временные `.part` файлы и заменяет целевой файл через `rename`.
- Пароль из `pass_base64` декодируется перед password authentication.

Проверка:

- `cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`
- `cmake --build build-debug -j $(nproc)`
- `./build-debug/sshumdloader_core_smoke_test`
- `ldd build-debug/sshumdloader_core_smoke_test | rg 'libssh2|libssl|libcrypto|zlib|zstd'`

## Шаг 4. main, инициализация зависимостей и тестовая загрузка

- Добавлен `src/main.cpp`.
- В `main` собран composition root:
  - `IniUmdConfigRepository`
  - `CliOptionsParser`
  - `Libssh2UmdRemoteRepository`
  - `DefaultPathMapper`
  - `OverwriteTransferPolicy`
  - `Libssh2DownloadScheduler`
  - `ConsoleProgressSink`
- В CMake добавлен исполняемый файл `SshUmdLoader`.
- Исправлена фильтрация инструмента: теперь сравнивается полное имя `SECURITY@BOARD` без суффикса `.zst`, а не только часть до `@`.
- CLI нормализует значение `--securities`: `ALRS-6.26@RTS.zst` и `ALRS-6.26@RTS` считаются одним инструментом.
- Планирование ускорено для инструментов с бордой: стартовый путь строится как `<umd_path>/<BOARD>/<YYYY.MM.DD>/`, без полного обхода всего `/home`.
- Увеличен socket timeout libssh2/select до 120 секунд: на тестовой загрузке большой `ordlog_1` файл не укладывался в прежние 10 секунд.

Проверка:

- `cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`
- `cmake --build build-debug -j $(nproc)`
- `./build-debug/sshumdloader_core_smoke_test`
- `./build-debug/SshUmdLoader`
- `./build-debug/SshUmdLoader MOEX -s ALRS-6.26@RTS -d 2026.06.08`

Результат тестовой загрузки:

- `/home/dev15/umd/RTS/2026.06.08/ba_1/ALRS-6.26@RTS.zst` - 22444 bytes
- `/home/dev15/umd/RTS/2026.06.08/orderbook1_1/ALRS-6.26@RTS.zst` - 59143 bytes
- `/home/dev15/umd/RTS/2026.06.08/ordlog_1/ALRS-6.26@RTS.zst` - 2294479 bytes
- `/home/dev15/umd/RTS/2026.06.08/trades_1/ALRS-6.26@RTS.zst` - 44245 bytes

## Дополнение. Сообщения о подключении

- Добавлен вывод статуса подключения к SSH/SFTP серверу:
  - `Connecting to server <name> (<host>:<port>)...`
  - `Connected to server <name> (<host>:<port>)`
- Сообщения выводятся один раз на сервер за запуск процесса, чтобы не дублировать их для каждой SFTP-сессии.

Проверка:

- `cmake --build build-debug -j $(nproc)`
- `./build-debug/sshumdloader_core_smoke_test`
- `./build-debug/SshUmdLoader`

## Дополнение. Перерисовка progress bar

- `ConsoleProgressSink` больше не печатает новую строку на каждое событие прогресса.
- Для каждого файла строка создается один раз, затем обновляется на месте через ANSI cursor control.
- При завершении работы sink переводит курсор на новую строку, чтобы последующие сообщения не попадали в строку прогресса.

Проверка:

- `cmake --build build-debug -j $(nproc)`
- `./build-debug/sshumdloader_core_smoke_test`
