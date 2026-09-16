# 84. Проверка настоящего VST3-хоста

## Что добавлено

Основной CI без SDK остаётся независимым. Отдельный workflow
[.github/workflows/vst3-sdk.yml](../.github/workflows/vst3-sdk.yml) явно вызывает
существующий bootstrap закреплённого Steinberg VST3 SDK и собирает настоящий
хост, scanner и runtime helper под macOS arm64. Обычная сборка ничего не скачивает.

ADelay берётся из **того же закреплённого SDK**, без собственного DSP-плагина,
без VSTGUI и без установки примера в пользовательские каталоги плагинов.
Он используется только как test fixture, в поставляемый `.app` не копируется.
Параметр CMake `DAW_VST3_TEST_PLUGIN` задаёт явный путь к примеру; неверный путь
или отсутствующие helpers приводят к ошибке конфигурации, а не пропуску теста.

## Контракт проверки

`vst3_sdk_smoke` запускает настоящий scanner (`--list-module`, `--probe`),
проверяет FUID/отпечаток/тип и загружает обнаруженный ADelay через наш хост.
Изменяет Delay до 0.25 сек, сохраняет состояние компонента, заново создаёт
процессор и проверяет два независимых импульса: левый с задержкой ровно
12000 кадров при 48 kHz, правый — ещё на 7 кадров позже. Dry fallback не может
пройти такую проверку. Затем состояние проходит SQLite save/read и звук
проверяется повторно. Дополнительно проверяется настоящий runtime helper:
изменение параметра и восстановление состояния в отдельном процессе.

ADelay не реализует собственное состояние контроллера: базовый SDK
`EditController::getState` возвращает `kNotImplemented`. Хост принимает только
этот случай с пустым stream и без ранее сохранённого controller state.
Состояние DSP остаётся обязательным; ошибки и непустые старые данные не
игнорируются. Пустой MemoryStream не использует арифметику null-указателя.
При ошибке восстановления после initialize/connect хост завершает уже
инициализированные SDK-объекты перед разрушением module/host.

## Достоверность CI

Все pipeline-команды исполняются явным Bash с `pipefail`: `tee` не подменяет
код ошибки компилятора или CTest своим успешным завершением. Дополнительный
[check_test_report.py](../scripts/check_test_report.py) читает JUnit через
стандартный XML parser и требует реально исполненные `vst3_runtime_isolation`
и `vst3_sdk_smoke`, без ошибок, пропусков или отсутствующих тестов. Его
негативные случаи проверяются [unittest](../tests/test_ci_reports.py).

Поддержка нестандартного warning-флага проверяется готовым
`CheckCXXCompilerFlag` по положительной форме; несовместимый `-Wno-*` не
передаётся AppleClang. `-Werror`, ASan/UBSan и fatal UB не отключаются.

Workflow сохраняет compiler/test logs, JUnit, toolchain и снимок отслеживаемых
исходников проверенной merge revision. После успешных тестов создаётся отдельный
архив VST3-enabled `.app` с обоими helpers, лицензией SDK и SHA-256.

## Запуск

```sh
python3 -m unittest discover -s tests -p test_ci_reports.py -v
./scripts/bootstrap-vst3-sdk.sh
# Соберите adelay из SDK командами workflow; укажите путь к его bundle:
cmake -S . -B build/vst3-ci -DCMAKE_BUILD_TYPE=Debug -DDAW_SANITIZERS=ON \
  -DDAW_BUILD_VST3_SCAN_HELPER=ON -DDAW_BUILD_VST3_RUNTIME_HELPER=ON \
  -DDAW_VST3_TEST_PLUGIN=/absolute/path/adelay.vst3
cmake --build build/vst3-ci --parallel 2
ctest --test-dir build/vst3-ci --output-on-failure --no-tests=error --output-junit ctest.xml
python3 scripts/check_test_report.py build/vst3-ci/ctest.xml \
  --require vst3_runtime_isolation --require vst3_sdk_smoke
```

## Границы

Результат конкретного прогона публикуется в PR с head/merge SHA. Наличие workflow
само по себе не доказывает успешное выполнение. Пример эффекта ADelay не
доказывает совместимость со всеми vendor plugins, VST3-инструментами, MIDI,
GUI, железом или live audio. Отдельные TSan, Tracktion и physical/listening
проверки остаются самостоятельными. Архив приложения — development build,
не нотарифицированный релиз.

## Первоисточники

- [SDK 3.8.1](https://github.com/steinbergmedia/vst3sdk/tree/3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96).
- [ADelay](https://github.com/steinbergmedia/vst3_public_sdk/tree/586dc5e6c8012c3e4b01c79389375cbe96bdb1da/samples/vst/adelay).
- [SDK EditController](https://github.com/steinbergmedia/vst3_public_sdk/blob/586dc5e6c8012c3e4b01c79389375cbe96bdb1da/source/vst/vsteditcontroller.cpp).
- [Порядок сохранения/восстановления](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Persistence.html).
- [CheckCXXCompilerFlag](https://cmake.org/cmake/help/latest/module/CheckCXXCompilerFlag.html).
