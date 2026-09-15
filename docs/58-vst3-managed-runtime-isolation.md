# Managed VST3 runtime isolation

Версия 1.31.0 добавляет managed out-of-process runtime для VST3 inserts. Этот срез отделяет исполняемый код plug-in от процесса приложения, но не объявляет все VST3 совместимыми или пригодными для live recording.

## Модель выполнения

Выбор hosting policy остаётся durable свойством каждого track, bus или master insert: `InProcess` либо `OutOfProcess`. VST3 OOP capability появляется в status ABI только в сборке, где runtime helper действительно включён. Если capability отсутствует, запрос OOP отклоняется до мутации проекта и revision не меняется.

После подготовки OOP insert host запускает один disposable helper именно для этого insert. Helper получает MDVS state, создаёт VST3 component/controller и сам использует его in-process; приложение не загружает выбранный external processor в свой audio callback. Один helper не обслуживает несколько inserts, поэтому crash и teardown одного instance не разделяют process state с другими channels.

IPC использует один shared-memory request/reply pipeline с фиксированным stereo float32 contract: 48 kHz и блоки до 4096 frames. Host отправляет block `N`, а готовый ответ использует для задержанного окна на следующем цикле. Startup block поэтому предсказуемо silent. Pipeline добавляет ровно 4096 frames поверх algorithmic plugin latency; Renderer учитывает эту величину в PDC и публикует её отдельно как `extra_pipeline_latency_frames`.

Automation передаётся вместе с block как bounded parameter events с sample offsets. Они применяются helper-ом к тому же VST3 processing block, поэтому offset не превращается в UI-time update. Audio callback не ждёт helper, не вызывает VST3, не выделяет память и не делает системный IPC call.

## Проверка plug-in и отказ

Перед helper activation MDVS envelope проверяется вместе с сохранённым module path, class FUID и SHA-256 executable fingerprint. Несовпадение fingerprint или malformed state не запускает processor. Ошибка prepare отражается в runtime status, а проект и сохранённая policy не переписываются.

Первый late, missing или malformed reply переводит instance в dry fallback и выдаёт один failure result для существующей telemetry. Последующие blocks продолжают fixed delayed dry path без ожидания worker. Runtime status различает `ACTIVE_ISOLATED`, `DRY_FALLBACK`, `FAILED` и `UNPREPARED`; fault code ограниченно сообщает prepare, restart required, deadline, protocol или helper state. Это runtime telemetry, не новый project state и не причина для изменения revision.

`daw_insert_hosting_status` сохраняет прежний exact-size ABI и описывает policy/capability. Новый exact-size `daw_insert_runtime_status` содержит отдельную версию, фактическое состояние, дополнительную pipeline latency и fault code. Без активного graph bridge возвращает `UNPREPARED` с `RESTART_REQUIRED`. Bypassed insert в подготовленном graph возвращает `UNPREPARED` без ложного restart fault.

## UI и границы

Mixer/Inspector показывают policy и отдельный runtime badge: «готовится», in-process, isolated, dry fallback или failed. При OOP policy generic parameter editor остаётся выключен: remote editor, remote state proxy и vendor UI в этот срез не входят.

Проверка выполнена self-hosted fake helper: silent startup, дополнительная latency, parameter offset, bounded no-reply/corrupt fallback, status и reap. Build/C ABI/bridge tests также проходят. Не выполнены запуск реального стороннего VST3, vendor compatibility matrix, physical output/input path и listening acceptance. Эти проверки остаются release gates, прежде чем обещать надёжность сторонних plug-ins пользователю.
