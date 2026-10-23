# Нативный toolchain: инженерные pins

Дата проверки: 2026-10-23. Машиночитаемая запись:
[native/dependencies.lock.json](../native/dependencies.lock.json).
Это pins инженерной среды, а не завершённый lock поставляемого клиента и не
разрешение на выпуск. Текущий CMake не загружает этот JSON автоматически.

## Windows

| Компонент | Проверенная версия |
| --- | --- |
| Visual Studio Build Tools 2022 | 17.14.42, installation 17.14.37806.7 |
| MSVC compiler | **19.44.35229.0**, как определил CMake при реальной сборке |
| Каталог MSVC toolset | 14.44.35207; это не номер compiler binary |
| Windows SDK | 10.0.26100.0 |
| CMake | 4.4.4 |
| Ninja | Python package 1.13.2; binary reports `1.13.2.git.kitware.jobserver-pipe-1` |
| Qt SDK | 6.12.0, `win64_msvc2022_64`, package `6.12.0-0-202609280346` |
| Zig для VT spike | 0.17.0, x86_64 Windows |

Локальные build tools находятся в
`C:\Users\artyo\Documents\Codex\.tools\nordssh-native\Scripts`.
Python 3.12.14, aqtinstall 3.3.0 и py7zr 1.1.4 использовались для подготовки SDK.
Пользователю NordSSH не нужны ни Python, ни Qt SDK, ни Visual Studio, ни Zig.
Компилятор/SDK installers пока не имеют проверенных hashes в lock; эта таблица
описывает наблюдавшуюся среду, не гарантирует побитовую воспроизводимость.

Qt распакован в `C:\Users\artyo\Documents\Codex\.tools\Qt`.
Архивы содержат `bin`, `lib`, `include` прямо в корне: не добавлять к prefix
несуществующий суффикс `6.12.0\msvc2022_64`. Установлены только qtbase, qttools и
qttranslations. QML/Web-модули не устанавливались; qtsvg пока не установлен.
`qmake -query`, `qtpaths6 --query` и `windeployqt --version` подтвердили 6.12.0
и локальный prefix. Это проверка SDK, не проверка готового GUI или установщика.

## Как проверены Qt-архивы

`download.qt.io` и `master.qt.io` были недоступны из локальной среды. HTTPS-зеркала
FAU и Constant отвечали и отдавали одинаковую `Updates.xml`. Каждый из трёх
архивов скачан отдельно с обоих зеркал. Для каждой копии проверен опубликованный
`.sha1`, затем вычислены и сравнены SHA256 обеих полных копий. TLS-проверка не
отключалась, `--UNSAFE-ignore-hash` не использовался.

| Артефакт | Локально проверенный SHA256 |
| --- | --- |
| Updates.xml на обоих зеркалах | `ba09f355a6e42e1373b939d26d7742df7348f298a465420083b3cfd28ed2914b` |
| qtbase | `219d1b86def3be418e31d704572bc4acc91b2ec373c65595c9fb12b42bf098dc` |
| qttools | `73328e361429378df518f391ebe4d82c51e833f4d5a951692768ab5a5a1d7da5` |
| qttranslations | `328321e8879a3dc119775b4404d6e32ec2a361696e73f760540a732921507917` |

Полные URL, имена файлов и размеры записаны в JSON. Локальные копии и расширенный
журнал находятся в `.tools\qt-cache`, журнал — `qt6120-provenance.json`.
Одинаковые SHA256 — **first-acquisition pins, не подписанные upstream checksums**.
Два зеркала могут иметь общий источник: сравнение не доказывает подлинность
издателя и не заменяет аудит. До выпуска нужно проверить доступные upstream
SHA256/подписи через доверенную среду загрузки, затем повторить security/license review.
Нельзя выдавать опубликованный SHA1 за стойкую современную подпись.

При повторной загрузке проверять SHA256 по lock **до распаковки**. Не менять pin
на новую сумму только потому, что файл не совпал. Смену версии/хеша разбирать
отдельно, с происхождением файла и повторными техническими gates.

## Сборка текущего core

Команды выполняются из корня репозитория, когда CMake доступен в PATH.
На Windows используется multi-config генератор: `--config Debug` и `ctest -C Debug`
не заменяются одним `CMAKE_BUILD_TYPE`.

```powershell
cmake -S native -B native/build-core-check -G "Visual Studio 17 2022" -A x64 -T version=14.44.35207 -DCMAKE_SYSTEM_VERSION=10.0.26100.0
cmake --build native/build-core-check --config Debug --parallel 2
ctest --test-dir native/build-core-check -C Debug --output-on-failure
```

Текущий core проверяет структурный допуск заголовка vault. Это не реализация
шифрования, SSH или терминала. Отвергнутый libvterm experiment по умолчанию
выключен; `NORDSSH_REJECTED_LIBVTERM_EXPERIMENT` не включать для продукта.

Вариант с нативным Qt input gate:

```powershell
cmake -S native -B native/build-input-check -G "Visual Studio 17 2022" -A x64 -T version=14.44.35207 -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DNORDSSH_BUILD_QT_INPUT=ON "-DCMAKE_PREFIX_PATH=C:/Users/artyo/Documents/Codex/.tools/Qt"
cmake --build native/build-input-check --config Debug --parallel 2
ctest --test-dir native/build-input-check -C Debug --output-on-failure
```

Перед запуском Qt-теста DLL должны быть доступны: добавить `Qt\bin` в PATH только
для текущего процесса либо использовать проверенный deployment-каталог.
CTest задаёт `QT_QPA_PLATFORM=offscreen`; такой тест не доказывает настоящую
системную вставку RU/EN, IME, accessibility и clipboard на X11/Wayland.
Реальные UI/SSH byte-level испытания остаются обязательными.

Linux core можно собрать обычным CMake/CTest на Ubuntu 24.04. Но compiler patch,
Qt SDK archives, системный ABI baseline и полный package set Linux ещё не
закреплены. Runner `ubuntu-24.04` сам по себе не является воспроизводимым lock.

## Терминал и оставшиеся кандидаты

libvterm 0.3.3 отвергнут после обнаружения CSI bounds/numeric overflow и
resource-exhaustion проблем. Сборка его C-кода не означает прохождение security
gate. Это выключенный experiment, не продуктовая зависимость.

Следующий кандидат — upstream libghostty-vt, commit
`08a9518340e19487bd5fdf96963583fb32f0c41f`. Его C API нестабилен; Windows/Linux
terminal gates ещё не закрыты. Исходники получены через Git по закреплённому
commit, не через source archive. Windows ReleaseSafe SDK и C++ ABI baseline уже
проверены; полный gate с renderer/input остаётся открыт. Запись пока находится
в `candidates`, а не в `lockedArtifacts` готового runtime.

Для этого spike QA проверил официальный Zig index и реально скачанный
[Zig 0.17.0 Windows archive](https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip):
100268137 байт, SHA256
`b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428`.
Установленный Zig сообщает 0.17.0. Это build tool, не runtime NordSSH.

OpenSSL 3.5.9 LTS, libssh 0.12.2 и Argon2 tag 20190702 теперь собраны и проверены
ограниченными crypto/SSH fixtures. Hashes и границы проверки подписей записаны
в dependency JSON и NATIVE_PROGRESS.md. Argon2 archive hash проверяет CMake;
Linux engineering script также проверяет OpenSSL/libssh перед сборкой. Это ещё
не весь release runtime/packaging gate. Bare `vcpkg install libssh` не гарантирует
нужный patched baseline; проверять конкретный port/overlay.

## Лицензии и поставка

Выбранные Qt runtime-модули должны поставляться с соблюдением LGPLv3;
Qt tools и отдельные компоненты имеют собственные условия. Нельзя скопировать
весь SDK в установщик и считать notices автоматически собранными. Нужны перечень
реально загружаемых DLL/plugins, лицензии, соответствующие исходники и возможность
замены LGPL-библиотек. OpenSSL — Apache-2.0, Argon2 — Apache-2.0 OR CC0-1.0,
Ghostty — MIT; точный COPYING выбранного libssh проверяется перед поставкой.

Установщик и portable должны включать согласованный runtime, не SDK и не
интерпретаторы. Версия Qt SDK не отменяет проверки VC runtime, clean-machine
запуска без WebView, Linux X11/Wayland и full-bundle update/rollback.

Первичные источники: [Qt SDK catalogue](https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_msvc2022_64/),
[OpenSSL downloads](https://openssl-library.org/source/),
[libssh security releases](https://www.libssh.org/),
[Argon2 tags](https://github.com/P-H-C/phc-winner-argon2/tags),
[Ghostty candidate manifest](https://github.com/ghostty-org/ghostty/blob/08a9518340e19487bd5fdf96963583fb32f0c41f/build.zig.zon).
