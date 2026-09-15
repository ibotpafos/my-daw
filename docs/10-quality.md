# Проверка качества и производительности

Все числа ниже — начальные цели. Они не измерены для My DAW, поскольку реализации ещё нет.

## Fixtures и машины

**A:** M1, 16 GiB RAM, внутренний SSD. **B:** M4, 16+ GiB RAM, внутренний SSD. Для обоих фиксировать точную модель, OS build, power mode, audio device/driver, sample rate и buffer size. Нельзя сравнивать результаты разных конфигураций как один benchmark.

**F0:** 8 mono tracks, 5 минут, built-in gain/pan. **F1:** 32 mono tracks, 10 минут, gain + 3-band EQ на каждой, stereo master, 2 recording inputs. **F2:** 64 tracks / 1,000 clips для UI, без сторонних plugins. **F3:** отдельная именованная AU-матрица; не смешивать её с F1.

Fixtures генерируются с фиксированным seed; реальные вокальные сессии — отдельный consented набор. Синтетические sine/impulse/noise не заменяют вокальную приёмку.

## Начальные бюджеты

| Метрика | Цель | Метод |
|---|---|---|
| Dropouts F1 | 0 за 30 минут при 48k/128, отдельно A и B | Engine counters + loopback recording |
| Callback duration F1 | p99.9 <70% блока; max <100% в тестовом прогоне | Low-overhead timestamp counters |
| RT allocations собственного ядра | 0 после prepare | Allocation guard + code review |
| UI response обычной команды | p95 <50 ms | Event→visible state signpost |
| Timeline frame time F2 | p95 ≤16.7 ms на 60 Hz при pan/zoom | Instruments, 60-second capture |
| RAM F1 без ML/plugins | <1 GiB app+helpers | Resident footprint, фиксированные caches |
| Warm open F1 | <2 s до редактируемого timeline | Media уже локальные; plugin readiness отдельно |
| Recovery после process kill | Все durable revisions; ≤1 s незавершённого audio как цель | 20 injection points |

Плохой результат ведёт к profiling и корректировке реализации; не менять fixture незаметно ради зелёного отчёта. 64 frames и 96 kHz — исследовательские нагрузки, не обещанные S1 targets.

## Проверки по уровням

1. **Domain:** split/trim bounds, Undo round-trip, stale revision, idempotency, batch atomicity, cycle rejection.
2. **DSP:** impulse/frequency response, gain/pan, fades, discontinuity, NaN/Inf, denormals, channel layouts, variable block sizes, sample-offset automation, finite/infinite plug-in tail declarations и tail drain limits.
3. **Engine integration:** loop wrap, seek during play, EOF, buffer underrun, delay compensation с несколькими путями, plan retirement и queue-full.
4. **Storage:** crash между file rename и DB commit, disk full, corrupt media, Save As, WAL backup, future format, migration rollback.
5. **Host:** matrix из hosting spec, plugin missing/crash/hang/state restore.
6. **Physical:** USB disconnect, sample-rate change, sleep/wake, loopback latency, 30-minute recording, 2-hour playback soak.
7. **UX/listening:** RU/EN shortcuts, VoiceOver, small display, A/B vocal edits, loudness-matched comparisons, WAV tail policy (automatic/no-tail/2/5/15/30 seconds), range copy и отмена export.

ASan/UBSan и TSan запускать отдельными jobs. Instrumented builds не подходят для итогового realtime performance report. No-data-loss утверждается только для проверенного failure mode.

## Отчёт

Фиксировать git SHA, dependency SHAs, toolchain, device, fixture hash, run duration, p50/p95/p99.9/max callback, queue overflows, xruns, RAM, UI frame time, итог прослушивания. Артефакты — JSON metrics + log + waveform/loopback comparison. Последнее поле: `acceptance: pass|fail|not_run` и reason.

## Релизные уровни

S0 internal: domain/storage/render tests и один физический recording proof. S1 alpha: полный vocal flow, compatibility matrix и восстановление. Public beta: подписанная/notarized сборка, clean-machine install, format migration fixtures и 10 реальных сессий. Release 1.0 требует отдельного решения по незакрытым ошибкам потери данных и поддерживаемой платформенной матрице.
