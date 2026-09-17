# Tracktion Engine vertical proof

Этот изолированный console spike проверяет реальную цепочку без аудиоустройства и без изменения пользовательского проекта:

1. создаёт 48 kHz WAV;
2. создаёт Tracktion `Edit` с двумя audio tracks;
3. собирает B-011 fixture: post-fader `AuxSendPlugin` на первой дорожке,
   `AuxReturnPlugin` на второй и implicit master `Edit`;
4. вставляет clip, меняет gain и задаёт loop range;
5. сохраняет `.tracktionedit`;
6. делает offline render и сравнивает peak с детерминированной gain-моделью из
   [`b011-routing-fixture.json`](b011-routing-fixture.json).

Зависимости не vendored и не подключены к `My DAW.app`. Используются только exact revisions из [`dependencies.lock.json`](../../dependencies.lock.json). Это намеренная граница до принятия лицензии продукта: Tracktion Engine — GPL-3.0-or-later/commercial, его pinned JUCE — AGPL-3.0/commercial.

```sh
git clone --no-checkout https://github.com/Tracktion/tracktion_engine.git build/spikes/tracktion_engine
git -C build/spikes/tracktion_engine checkout 00fe42753a995c79dd857efe69dde17550e27e78
git -C build/spikes/tracktion_engine -c url."https://github.com/".insteadOf="git@github.com:" submodule update --init --depth 1 modules/juce
cmake -S spikes/tracktion -B build/spikes/tracktion-proof \
  -DTRACKTION_ENGINE_ROOT="$PWD/build/spikes/tracktion_engine" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/spikes/tracktion-proof --target mydaw_tracktion_proof -j 8
build/spikes/tracktion-proof/mydaw_tracktion_proof_artefacts/Release/mydaw_tracktion_proof
```

Успешный вывод включает `fixture=post_fader_aux_to_master`. Это proof
соответствия одного простого B-011 маршрута: две дорожки, post-fader send,
aux-bus/return и master. Он не доказывает эквивалентность всех графов: в нём
нет вложенных bus-цепочек, pre-fader mute semantics, pan law, automation,
plugin latency compensation и third-party plugins. JSON-manifest служит
общим golden-contract, а не форматом проекта или обещанием интеграции
Tracktion в приложение.
