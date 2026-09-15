# VST3 compatibility foundation

B-017 начат после готового B-016. Основа использует официальный VST3 SDK 3.8.1 build 84 под MIT на exact revision `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`; pins четырёх нужных submodules находятся в `dependencies.lock.json`.[^1] SDK скачивается только явной командой `scripts/bootstrap-vst3-sdk.sh` в `build/dependencies/vst3sdk`. Обычный CMake configure сеть не использует.

## Изолированный catalog

`daw_vst3_scan_helper` использует Steinberg Hosting API и работает ступенчато: отдельный helper перечисляет module paths, новый процесс читает каждый module и новый процесс probes каждый audio-effect class. Основное приложение не загружает исследуемый binary во время discovery. Supervisor ограничивает время и объём stdout; crash, timeout и malformed ответ помещают конкретный module/class в quarantine.

Catalog использует 16-byte class FUID в canonical 32-hex форме, module path, name, vendor, version и SHA-256 отпечаток Mach-O executable. Persistent cache ограничен 1 MiB и 2048 entries. Изменение path, class FUID, vendor, version или executable fingerprint инвалидирует старый verdict. Запись cache идёт через same-directory temporary file, file `fsync` и atomic rename.

## State boundary

Новая format-neutral модель различает Audio Unit и VST3. Existing AU state остаётся byte-for-byte raw. VST3 получает bounded envelope `MDVS v1`: class FUID, module metadata/fingerprint и отдельные component/controller state blobs. Это позволяет в следующем срезе сохранить VST3 в существующем opaque plugin state без преждевременной SQL migration.

Scanner foundation продолжена рабочим master-effect slice в [1.16.0](43-vst3-master-effects.md). Editors, instruments, MIDI, sidechain, runtime process isolation и vendor compatibility matrix остаются отдельными gates.

В этом срезе выполнена только compile/build проверка. QA, запуск приложения и сторонние plugins не запускались по текущему режиму разработки.

[^1]: Steinberg Media Technologies, [VST3 SDK release 3.8.1 build 84](https://github.com/steinbergmedia/vst3sdk/releases/tag/v3.8.1_build_84), [official repository](https://github.com/steinbergmedia/vst3sdk) и [processing documentation](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
