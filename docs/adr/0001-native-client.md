# ADR 0001: нативный NordSSH с нуля

Статус: направление согласовано руководителем архитектуры и security-критиком,
проверено QA. Конкретный terminal engine и версии сборочных пакетов проходят
технические gates до переноса интерфейса. Дата: 2026-10-23.

## Решение

C++20, Qt 6 Core/Gui/Widgets/Network, CMake, libssh с OpenSSL 3, libargon2.
Один desktop runtime. Нет WebView, Qt WebEngine, QML/JavaScript, Rust backend,
внутреннего HTTP-сервера или служебного stdio-протокола.

Qt поставляется динамически с собственными разрешёнными библиотеками и plugins.
Официально проверенный на дату ADR libssh security baseline — 0.12.2. Перед сборкой
ещё раз проверяются security advisories, доступность и хеши точных версий.
«OpenSSL 3» и «Qt 6» пока не являются dependency lock: patch pin фиксируется
отдельно до первого бинарного испытания. Не подменять этот шаг auto-latest.

Собственный код — MIT, выбор подтверждён владельцем. Notices/лицензии/исходники
сторонних библиотек и требования relinking учитываются в составе дистрибутива.
MIT проекта не меняет лицензии Qt, libssh или terminal engine.
Подпись официальных обновлений не используется как DRM и не запрещает пользователю
заменять LGPL-библиотеки. Соответствующие исходники/уведомления доступны с поставкой.

## Почему не продолжаем текущую реализацию

В опубликованной бете HTML/TypeScript работают через Tauri/WebView. В исходниках
есть browser clipboard и OS message box для host trust. Добавленные в main
установщики и signed updater ещё не прошли релизные gates и не опубликованы.
Они не делают UI независимым от WebView.

Qt + Rust library или native subprocess технически допустимы. Изначально критик
предпочитал сохранить Rust ради совместимости vault. После разрешения владельца
не переносить прототип этот аргумент снят. Для нового продукта выбран единый
стек без дополнительного FFI/IPC lifecycle. Это не утверждение, что C++ сам по
себе безопаснее или быстрее Rust.

## Границы модулей

```text
Application / Controllers
  ├─ Qt Widgets UI + InputRouter + ClipboardAdapter
  ├─ TerminalModel / VT engine / QWidget renderer
  ├─ ConnectionManager → SessionWorker → libssh / OpenSSL
  ├─ TransferManager → SFTP worker / queue / cancellation
  ├─ VaultService → encrypted files / locks / atomic commits
  │                 └─ DeviceUnlockAdapter: DPAPI / Secret Service
  └─ UpdateService → signed manifest / full bundle / installer-helper
```

- UI отображает состояние и отправляет типизированные действия. Не реализует
  криптографию, SSH или файловые транзакции внутри обработчиков кнопок.
- У каждой libssh session ровно один worker-владелец. Нет параллельных обращений
  нескольких потоков к одной session. GUI остаётся в главном потоке.
- Долгие SFTP-операции не останавливают терминал. Допустим отдельный файловый
  connection/worker с той же политикой проверки host key.
- Во всех async-командах есть lifetime владельца, request/session ID, отмена,
  таймаут и одно конечное состояние. Очереди и вывод имеют backpressure.
- ActivityRegistry запрещает неявное разрушение активных соединений при lock,
  import, переключении vault и update. Исключение — явное подтверждение закрытия.
- Qt signals не несут долгоживущие копии паролей. Секретные buffers имеют явное
  владение/очистку; ошибки не сериализуют содержимое credentials.
- Серверные имена, пути и ошибки отображаются как Qt::PlainText, не AutoText/HTML.

## Состояния подключения и доверия

```text
Idle → Connecting → HostVerification → Authentication → Active
          ↓              ↓                 ↓             ↓
        Failed        Rejected           Failed         Closing → Closed
```

Проверка ключа выполняется до пароля/подписи аутентификации. Диалог внутри
главного окна получает endpoint, алгоритм, SHA256, connection ID и одноразовый
challenge. Ответ привязан к этому challenge. Закрытие, отмена и таймаут запрещают
вход. При changed key нет кнопки «продолжить» в обычном уведомлении об ошибке.
Удаление старого доверия — отдельное подтверждаемое действие.

OpenSSH user certificate и server host certificate — разные механизмы.
Поддержку user certificates проверяем положительными и отрицательными тестами;
не заявляем поддержку host CA без реализованной политики доверия.

## Терминальный gate

Основной кандидат: upstream libvterm + собственный QWidget/QPainter renderer.
Это не окончательно подтверждённый Windows-компонент. Проверяем сборку,
Unicode, VT semantics и security до переноса остальных экранов.

Не использовать архивированный Neovim fork. LXQt QTermWidget не заявляет Windows
и не является готовой заменой для этой задачи. libghostty-vt возможен как
альтернатива spike, но его C API отмечен upstream как нестабильный; нужен pinned
commit и отдельная оценка сопровождения.

Gate: Windows/Linux, vim/tmux/top/less, alternate screen, resize, mouse reports,
bracketed paste, combining/wide characters, cursor/selection/scrollback/search,
bounded memory и hostile OSC/DCS. OSC52 clipboard access выключен по умолчанию.
При провале возвращаемся к выбору engine, не заменяем его QPlainTextEdit с ANSI regex.
Обязательного GPU/OpenGL нет: базовая отрисовка растровая.

InputRouter имеет один маршрут paste. Сочетания клавиш и ПКМ вызывают одно действие,
которое поглощает исходное событие. IME commit не обрабатывается вторым маршрутом.
QClipboard читается только по пользовательскому действию. Byte-level тест на
SSH fixture, а не снимок экрана, доказывает отсутствие дубля.

## Новый vault

Каждый vault — отдельный файл, UUID и случайный data-encryption key. Группы серверов
находятся внутри vault. Используем стандартные OpenSSL EVP AES-256-GCM и Argon2id,
а не собственный шифр. Password wrapper и payload имеют независимые случайные
nonce. Версия/UUID/KDF metadata аутентифицированы через AAD.
Для каждого password wrapper генерируется CSPRNG salt не короче 16 байт;
salt публичный и входит в аутентифицированные metadata. DEK и nonce также CSPRNG.
AEAD-тег проверяется до разбора/использования plaintext. Лимиты файлов, plaintext,
коллекций и KDF проверяются до больших выделений памяти; численные пределы
фиксируются в format-spec до реализации.

Начальный KDF profile: 64 MiB, три прохода, parallelism 1, версия формата фиксирует
допустимые параметры. Импорт не может навязать неограниченную память/время.
Пароль: минимум 7 Unicode code points, ограничение размера в байтах; без trim
и нормализации. Слабый пароль остаётся слабым при краже зашифрованного файла.

QSaveFile используется без direct-write fallback; изменения сверяются с исходным
файлом и защищаются блокировкой. Проверяется нехватка места/отмена/сбой rename.
Резервные копии только зашифрованные. Невалидный файл не заменяется пустым vault.
Права vault/backup ограничены владельцем; ссылки не используются для незаметной
записи в другой файл. Ошибки установки прав не игнорируются молча.

DPAPI/Secret Service хранят только необязательную device-копию unlock key.
Недоступность сервиса не влияет на password unlock. Device records не входят
в экспорт; «забыть устройство» не обещает отзыв уже украденного ключа.

Новый data namespace не совпадает с com.nordssh.desktop старой беты. Новая сигнатура
файла/версия явно различает форматы. Старые файлы не открываются автоматически,
не переписываются и не удаляются. Автоматическая миграция не входит в этот проект.

## Deployment и обновления

Windows per-user installer и portable ZIP; Linux .deb и portable. Runtime не
требует WebView2, Qt SDK, VS, Python, .NET, Node или Rust. Динамические библиотеки
и Qt plugins поставляются как согласованная версия. DLL/plugin search ограничен
пакетом и системными путями, а не CWD/произвольным QT_PLUGIN_PATH.

Подписанный manifest связывает product ID, канал, версию, ОС, архитектуру,
хеш/размер и требования schema/runtime. Подпись проверяется до запуска установки.
Архив проверяется на traversal, ссылки, лимиты распаковки и лишние пути.
Signed manifest не разрешает replay/downgrade старого релиза под новым номером:
обычная установка принимает только новую версию, rollback — отдельное явное
действие. Trust anchor встроен в продукт; ротация ключей требует доверенной цепочки.

Portable обновляет полный versioned bundle, а не EXE. Новый набор распаковывается
рядом, проходит проверку, затем переключается целиком. Данные вне runtime-каталога.
При сбое переключения или health check возвращается предыдущий набор. Небольшой
Windows updater-helper для занятых файлов допустим как часть установки, не backend.
Изменение schema vault не должно ломать rollback: совместимость/восстановление
проверяются до пользовательских записей новой версии, с зашифрованной копией.

Установленные версии обновляются штатным installer/package manager. Права
администратора запрашивает ОС, а не поле пароля NordSSH. Uninstall сохраняет
vault по умолчанию. Подпись updater не выдаётся за Authenticode-сертификат.

## Последовательность доказательств

1. Нативный toolchain/dependency lock и terminal feasibility на Windows/Linux.
2. Crypto known-answer, tamper, wrong-password, пределы и atomic/stale-write tests.
3. Loopback SSH password/key/usercert/2FA/trust/cancel и SFTP integrity/cancel tests.
4. Native Qt UI tests для полного пользовательского потока и RU/EN clipboard.
5. Clean install/upgrade/reinstall/uninstall, portable full-bundle update/rollback.
6. Проверка отсутствия веб-движка, исходники/tag/подписи/полные пакеты в релизе.

Результат каждого gate фиксируется как passed/failed/not-run. Текущие браузерные
и Tauri-тесты не являются доказательством готовности новой архитектуры.

## Первичные источники

- [Qt Widgets](https://doc.qt.io/qt-6/qtwidgets-index.html),
  [Qt threads](https://doc.qt.io/qt-6/threads-qobject.html),
  [Qt clipboard](https://doc.qt.io/qt-6/qclipboard.html).
- [libssh security releases](https://www.libssh.org/),
  [libssh thread ownership](https://api.libssh.org/master/libssh_tutor_threads.html).
- [libvterm upstream](https://www.leonerd.org.uk/code/libvterm/),
  [QTermWidget platforms](https://github.com/lxqt/qtermwidget),
  [libghostty-vt API](https://github.com/ghostty-org/ghostty/blob/main/include/ghostty/vt.h).
