# План реализации нативного NordSSH

Основание: [ТЗ N01–N22](NATIVE_REQUIREMENTS.md) и
[ADR 0001](adr/0001-native-client.md). Дата: 2026-10-23.

## Текущее состояние, 2026-10-24

Этап 2 «Модули соединений и данных» завершён на уровне worker/controller API.
Приёмка и точные результаты: [NATIVE_STAGE_2_ACCEPTANCE.md](NATIVE_STAGE_2_ACCEPTANCE.md).
Это не готовый пользовательский интерфейс и не нативный релиз.

Остались три крупных этапа: 3 — интерфейс, 4 — поставка/обновления,
5 — финальная приёмка и публикация. Рабочий нативный терминал уже есть;
новые vault/SFTP/2FA/tunnel модули ещё нужно подключить к полным UI-сценариям.

## Состояние на момент архитектурного решения

| Часть | Состояние |
| --- | --- |
| ТЗ и архитектура | Проверены руководителем, QA и security-критиком |
| MIT собственного кода | Подтверждена владельцем |
| Нативная реализация | Собраны crypto, SSH transport, input router и VT ABI; приложения ещё нет |
| Точная версия Qt/OpenSSL, dependency lock | Инженерные pins проверены; release runtime/packaging ещё не закрыты |
| Windows/Linux terminal gate | Windows Ghostty SDK/ABI проверены; два upstream отказа на правах symlink; cross-platform gate идёт |
| Нативные SSH/SFTP/vault tests | Crypto и loopback password/PTY subset пройдены; SFTP, остальные auth и persistence ещё не выполнены |
| Нативные установщики и обновления | Не собраны, не проверены |
| Новый нативный релиз | Не опубликован |

Установленные локально compiler/build utilities — подготовка среды, не функция
продукта. Python для загрузки Qt SDK допустим как build tool, но Python runtime
не входит в NordSSH и не требуется пользователю.

## 1. Проверяемый фундамент

- Проверить актуальные security releases и закрепить Qt patch, libssh, OpenSSL,
  libargon2 и выбранный VT engine с source hashes и лицензиями.
- Поднять CMake-сборку Windows/Linux только с разрешёнными Qt-модулями.
- Провести terminal feasibility spike: настоящий loopback SSH shell, VT, Unicode,
  resize, mouse, scrollback, EN/RU и byte-level paste tests.
- Подготовить format-spec: magic/version, AEAD/AAD, salt/nonce, пределы KDF/файлов,
  права, атомарность, lock, отказ при внешнем изменении.
- Проверить crypto known-answer/negative tests и host trust до authentication.

Выход: воспроизводимая невебовая сборка и доказательство пригодности терминала/
криптобиблиотек на обеих ОС. Это внутренние инженерные пробы, не пользовательский релиз.

## 2. Модули соединений и данных

- VaultService, независимые vaults, unlock/lock, optional device remember,
  экспорт/импорт и сохранение резервной копии.
- ConnectionManager и состояние Connecting/HostVerification/Authentication/Active/
  Closing/Failed; отмена, таймауты, keepalive, повторное подключение.
- Password/key/RSA-SHA2/OpenSSH usercert/keyboard-interactive и отрицательные tests.
- TransferManager: SFTP navigation/CRUD, папки, очередь, progress/cancel,
  безопасные temporary files, containment и overwrite.
- Jump host и local forwarding с loopback defaults.

Выход: UI-независимые функциональные/негативные tests против изолированного
OpenSSH/SFTP стенда. Реальные серверы и пользовательские credentials не используются.

## 3. Интерфейс

- Пересоздать понравившийся визуальный стиль на Qt Widgets, не переносить DOM.
- Hosts/groups/favorites/search, terminal tabs, SFTP panels, snippets/settings.
- Первое создание/открытие vault: минимум 7, показ пароля, отдельные ошибки,
  запоминание устройства, понятный прогресс без блокировки GUI.
- Внутренние диалоги trust/auth/update; незаметных Win32/Web permission окон нет.
- Один InputRouter/QClipboard route, Ctrl+C не ломается, IME и paste не дублируются.
- Native UI/accessibility tests, DPI и отсутствие технического ТЗ в рабочих экранах.

Выход: законченные пользовательские сценарии N03–N18 на настоящем нативном клиенте.
Mock UI, фотографии макета и компиляция не закрывают этот этап.

## 4. Поставка и обновления

- Windows per-user installer и complete portable ZIP с Qt/OpenSSL/plugins/notices.
- Linux .deb и portable с зафиксированным системным ABI baseline.
- Signed manifest, полнопакетное обновление, staging/limits/path validation,
  interprocess lock, encrypted backup, rollback и проверка schema compatibility.
- Стабильный app-data namespace отдельно от старой беты; старые данные не трогаются.
- Clean install → upgrade → reinstall → uninstall; portable update и обрыв операции.
- Проверка запуска без WebView2/WebKit, Qt SDK, Node/Python/.NET/Rust runtime.

Выход: проверенные нативные установщики и portable, не отдельный сырой EXE.

## 5. Финальная приёмка

- Для каждого N01–N22 приложить результат и тестовый артефакт/лог.
- Зафиксировать реальные поддержанные ОС/окружения, не расширять список по аналогии.
- Закрыть release-blocking defect list; не заменять дефекты примечанием «пока beta».
- Повторить security/dependency/license review и проверить содержимое архивов.
- Опубликовать tag, исходники, полный набор бинарников, manifest/подписи/SHA256
  и честный отчёт приёмки в GitHub Releases.

## Роли команды

- Основной агент: реализация и интеграция.
- Руководитель архитектуры: решения на границах модулей и go/no-go технических gates.
- QA: сценарии N01–N22, воспроизведение дефектов и native acceptance tests.
- Security-критик: опасные состояния, сохранность данных, trust/update/terminal review.

При споре решение и доводы фиксируются в ADR. Не возвращаться к отвергнутому
веб-движку или дополнительному backend-процессу как к тихому обходному пути.
