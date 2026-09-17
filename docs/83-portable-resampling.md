# 83. Переносимый импорт: готовый ресемплер вместо заглушки

## Причина

После устранения ошибок компиляции Ubuntu дошла до CTest. Два существующих
сценария — `background_wav_import` и `aiff_import` — завершались ошибкой, потому
что non-macOS `resampleStereoTo48k` выбрасывал исключение для любой частоты,
отличной от 48 kHz. Проверки не отключаются: заглушка заменена тонким адаптером
готовой библиотеки [libsamplerate](https://github.com/libsndfile/libsamplerate).

## Зависимость

Проверенный стабильный upstream release — 0.2.2, revision
`c96f5e3de9c4488f4e6c97f59f5245f22fda22f7`. BSD-2-Clause notice сохранён в
[third_party/notices/libsamplerate.txt](../third_party/notices/libsamplerate.txt).
CMake получает системную библиотеку через `PkgConfig::SampleRate` и требует
`samplerate>=0.2.2`. SHA в dependency manifest обозначает проверенный upstream,
а не побитовую фиксацию пакета дистрибутива; фактическую версию печатает CI.

На Ubuntu/Debian перед конфигурацией:

```sh
sudo apt-get update
sudo apt-get install -y libsamplerate0-dev pkg-config
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
```

Обычный CMake configure/build ничего не скачивает. macOS по-прежнему использует
системный AudioConverter: libsamplerate туда не линкуется и не включается в `.app`.
Это изменение не добавляет Linux GUI или работу Linux-аудиоустройств.

## Контракт адаптера

Фильтрация выполняется библиотекой в режиме `SRC_SINC_BEST_QUALITY`. Используется
[полный потоковый API](https://libsndfile.github.io/libsamplerate/api_full.html),
а не независимый `src_simple` на каждом блоке: состояние фильтра сохраняется.
Вход и выход — stereo interleaved float32. Наш слой отвечает только за RAII,
ограничение длительности 60 сек, проверку входа, учёт фактически потреблённых и
выданных кадров, progress и проверку cancellation между блоками по 4096 кадров.
Этот код работает в импортирующем worker, не в real-time audio callback.

Длительность округляется до ближайшего кадра, как в native import. Если после
полного EOF библиотека усекла дробную часть последнего кадра, допускается ровно
один нулевой завершающий кадр. Большее расхождение считается ошибкой, а не
дополняется тишиной. Для результата длиной один кадр сохраняется первая stereo
пара исходника, как в существующем macOS пути. Побитовое совпадение разных
ресемплеров не обещается.

## Проверки

Новый `portable_resampler` покрывает 8/44.1/96/192 kHz, точную длительность,
48 kHz identity, независимость каналов, тишину, дробный последний кадр,
подавление тона выше новой частоты Найквиста, malformed/non-finite input,
лимит длительности и отмену перед конвертацией. Существующие WAV/AIFF job tests
проверяют уже публичный импорт: metadata, Ready/progress, cancellation и
освобождение worker permit. Результат конкретного CI-прогона фиксируется в PR.
