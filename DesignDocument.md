# SshUmdLoader
SshUmdLoader загружает маркет-дату (umd) по SSH, сохраняя исходную иерархию директорий на локальном диске.

## Иерархия файлов umd
```text
<umd_dir>/
└── CME
    └── 
        YYYY.MM.DD/
        └── orderbook1_1/
            ├── ESH5@CME.zst
            ├── ESM5@CME.zst
            └── ...
        └── trade_1/
            ├── ESH5@CME.zst
            ├── ESM5@CME.zst
            └── ...
        ...
    RTS
    └── 
        ...
```

## Пример конфигурации

[конфиг](UmdServersConfig.ini)
```text
DownloadPath = ~/umd/
MaxConcurrentDownloads = 2

[UmdServer0]
name = UMD-CA-NEW
host = u394291.your-storagebox.de
port = 23
user = u394291-sub4
pass_base64='QmJ6NWpSZ0Q2OTdUTHFBOA=='
umd_path = /home/
```


## Примеры запуска команды

```bash
SshUmdLoader UMD-CA-NEW  --securities 'ETHUSDC@BINANCE_FUT,BTCUSDC@BINANCE_FUT' --day 2026.03.26
SshUmdLoader UMD-CA-NEW --securities ETHUSDC@BINANCE_FUT --day 2026.03.26 -end_day 2026.04.02
SshUmdLoader UMD-CA-NEW -s ETHUSDC@BINANCE_FUT -d 2026.03.26 -e 2026.04.02
```

## Архитектурные решения

### Шаг 1. Основные абстракции

SshUmdLoader проектируется в стиле Clean Architecture: ядро не зависит от SSH, CLI, INI-файлов, файловой системы и конкретной библиотеки скачивания. Внешние детали подключаются через интерфейсы, а сценарий загрузки работает только с доменными моделями и портами.

#### Ядро приложения

- `MarketDataDate` - дата торгового дня в формате `YYYY.MM.DD`. Отвечает за валидацию и безопасное построение диапазона дат.
- `Security` - тикер инструмента без привязки к бирже или транспортному протоколу.
- `UmdServer` - доменное описание выбранного UMD-сервера: имя, корневой удаленный путь и параметры подключения, переданные из конфигурации.
- `DownloadRequest` - нормализованные параметры запуска: сервер, список тикеров, начальный день, конечный день, локальный корень загрузки.
- `RemoteFile` - найденный удаленный файл с относительным путем, размером и метаданными, если сервер их предоставляет.
- `DownloadTask` - конкретная задача скачивания: удаленный путь, локальный путь, ожидаемый размер и стратегия записи.
- `DownloadPlan` - список задач, построенный из `DownloadRequest` после обхода удаленной иерархии и фильтрации по датам и тикерам.

#### Use cases

- `BuildDownloadPlanUseCase` строит план загрузки. Он обходит удаленные директории через абстрактный источник файлов, применяет фильтр дат и тикеров, сохраняет исходную иерархию директорий относительно `umd_path`.
- `RunDownloadPlanUseCase` выполняет план. Он ограничивает параллелизм параметром `MaxConcurrentDownloads`, запускает неблокирующие операции скачивания и передает события прогресса наружу.

#### Порты ядра

- `IUmdRemoteRepository` - перечисляет удаленные директории и файлы без знания о SSH-клиенте.
- `IDownloadScheduler` - выполняет набор `DownloadTask` неблокирующим способом и соблюдает лимит параллельности.
- `ITransferPolicy` - определяет, как писать локальный файл. В версии v1 используется политика `OverwriteTransferPolicy`, которая всегда создает или перезаписывает файл.
- `IProgressSink` - получает события старта, изменения прогресса, завершения и ошибки по каждой задаче.
- `IClock` - дает текущую дату/время для логирования и тестов, если это понадобится.

#### Инфраструктура

- `CliOptionsParser` разбирает аргументы командной строки и преобразует их в `DownloadRequest`.
- `IniUmdConfigRepository` читает `UmdServersConfig.ini` через `INI/INIReader` из `../Base/BaseLib`.
- `Libssh2Session` и `Libssh2SftpClient` реализуют SSH/SFTP-инфраструктуру. Для первой версии выбрана `libssh2`, потому что она предоставляет неблокирующий API на уровне сокетов и хорошо подходит для event loop без создания потоков ОС.
- `SelectEventLoop` или `PollEventLoop` ожидает готовность сокетов и двигает активные SSH/SFTP операции вперед.
- `ConsoleProgressSink` рисует отдельный прогресс-бар для каждого скачиваемого файла.
- `LocalFilesystem` создает директории, открывает временные файлы и атомарно заменяет целевой файл после успешной загрузки.

#### Будущая поддержка resume

В v1 все загрузки выполняются безусловно через `OverwriteTransferPolicy`: локальный файл перезаписывается с начала. Для v2 бизнес-логика не должна измениться: достаточно добавить новую реализацию `ITransferPolicy`, например `ResumeTransferPolicy`, которая проверит локальный размер, выберет начальное смещение и передаст его инфраструктурному загрузчику. `DownloadTask` уже должен содержать поля `remote_size` и `offset`, чтобы планировщик не зависел от конкретной политики.

#### Границы зависимостей

Зависимости направлены внутрь:

```text
main
  -> CLI / INI / SSH / FS / Console adapters
  -> application use cases
  -> domain models and interfaces
```

Ядро может компилироваться и тестироваться без `libssh2`, `INIReader` и реального файлового ввода-вывода. Это позволит отдельно покрыть тестами построение путей, фильтрацию дат, фильтрацию тикеров и ограничение плана загрузки.

### Шаг 2. Контракты и Dependency Injection

Контракты вынесены в публичные заголовки `include/SshUmdLoader`. Они разделены по ответственности:

- `Domain.h` содержит доменные структуры: `MarketDataDate`, `DateRange`, `Security`, `UmdServer`, `DownloadRequest`, `RemoteEntry`, `RemoteFile`, `TransferDecision`, `DownloadTask`, `DownloadPlan`, `ProgressEvent`.
- `Ports.h` содержит интерфейсы внешних зависимостей: `IAppConfigRepository`, `ICliOptionsParser`, `IUmdRemoteRepository`, `IPathMapper`, `ITransferPolicy`, `IDownloadScheduler`, `IProgressSink`.
- `UseCases.h` объявляет сценарии `BuildDownloadPlanUseCase`, `RunDownloadPlanUseCase` и фасад `SshUmdLoaderApplication`.
- `Composition.h` показывает, как инфраструктурные реализации будут инжектироваться в use case через конструкторы.

Принятое решение: use case получают зависимости через указатели на интерфейсы, но не владеют ими. Владение остается на уровне composition root в `main`, где будут созданы конкретные адаптеры:

```cpp
ApplicationDependencies dependencies;
dependencies.config_repository = &ini_config_repository;
dependencies.cli_parser = &cli_parser;
dependencies.remote_repository = &ssh_remote_repository;
dependencies.path_mapper = &path_mapper;
dependencies.transfer_policy = &overwrite_transfer_policy;
dependencies.download_scheduler = &libssh2_download_scheduler;
dependencies.progress_sink = &console_progress_sink;

ApplicationGraph graph = ComposeApplication(dependencies);
return graph.application.Run(argc, argv);
```

Такой подход оставляет бизнес-логику независимой от `libssh2`, `INIReader`, консоли и файловой системы. Для v1 будет подключена `OverwriteTransferPolicy`, а для v2 можно добавить `ResumeTransferPolicy`, не меняя `BuildDownloadPlanUseCase` и `RunDownloadPlanUseCase`.

### Шаг 3. Ядро и инфраструктурные адаптеры

Реализована первая версия ядра и проверяемых инфраструктурных адаптеров:

- `MarketDataDate` умеет парсить формат `YYYY.MM.DD`, валидировать дату, форматировать ее обратно и переходить к следующему дню.
- `BuildDownloadPlanUseCase` рекурсивно обходит удаленное дерево через `IUmdRemoteRepository`, находит директории дат, фильтрует файлы по расширению `.zst` и тикерам из CLI, затем строит `DownloadTask` с сохранением относительной удаленной иерархии.
- `RunDownloadPlanUseCase` валидирует `MaxConcurrentDownloads` и передает выполнение в `IDownloadScheduler`.
- `CliOptionsParser` поддерживает формы запуска из документа: `--securities`/`-s`, `--day`/`-d`, `--end_day`/`-end_day`/`-e`.
- `IniUmdConfigRepository` читает `UmdServersConfig.ini` через `INI/INIReader` из `../Base/BaseLib`.
- `DefaultPathMapper` строит локальный путь и раскрывает `~/`.
- `OverwriteTransferPolicy` реализует v1-поведение: всегда писать с offset `0` во временный файл `<target>.part`.
- `ConsoleProgressSink` выводит прогресс по каждому `DownloadTask`.

Для SSH-инфраструктуры реализованы классы `Libssh2UmdRemoteRepository` и `Libssh2DownloadScheduler`. Сессии переводятся в non-blocking режим через `libssh2_session_set_blocking(session, 0)`, операции SFTP двигаются вперед при готовности сокета через `select`, потоки ОС не создаются. `Libssh2DownloadScheduler` держит не больше `MaxConcurrentDownloads` активных SFTP transfer-сессий, пишет данные во временный файл `<target>.part`, затем атомарно заменяет целевой файл через `rename`.

Проверка ядра выполняется через `sshumdloader_core_smoke_test`: fake remote repository подтверждает фильтрацию даты/тикера и построение локального пути, а INI-проверка подтверждает чтение текущего `UmdServersConfig.ini`.

### Шаг 4. Точка входа, сборка и тестовая загрузка

Добавлена точка входа `src/main.cpp`. `main` является composition root: создает `IniUmdConfigRepository`, `CliOptionsParser`, `Libssh2UmdRemoteRepository`, `DefaultPathMapper`, `OverwriteTransferPolicy`, `Libssh2DownloadScheduler`, `ConsoleProgressSink`, собирает `ApplicationGraph` и запускает `SshUmdLoaderApplication`.

CMake теперь собирает исполняемый файл `SshUmdLoader` вместе с библиотекой ядра и smoke test.

После исправления примеров с бордой изменена фильтрация инструментов: CLI принимает полное имя инструмента `SECURITY@BOARD` с опциональным суффиксом `.zst`, а ядро сравнивает его с именем удаленного файла без `.zst`. Для ускорения поиска при наличии борды в инструменте планировщик строит стартовые директории вида:

```text
<umd_path>/<BOARD>/<YYYY.MM.DD>/
```

Это исключает полный рекурсивный обход всего `<umd_path>` для обычных запросов с `security@board`.

Тестовая загрузка выполнена командой:

```bash
./build-debug/SshUmdLoader MOEX -s ALRS-6.26@RTS -d 2026.06.08
```

Загружены файлы:

```text
~/umd/RTS/2026.06.08/ba_1/ALRS-6.26@RTS.zst
~/umd/RTS/2026.06.08/orderbook1_1/ALRS-6.26@RTS.zst
~/umd/RTS/2026.06.08/ordlog_1/ALRS-6.26@RTS.zst
~/umd/RTS/2026.06.08/trades_1/ALRS-6.26@RTS.zst
```
