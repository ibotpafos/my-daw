# Ограниченные фоновые jobs — 1.2.0

15 сентября 2026. Save и offline export больше не создают неограниченное число detached threads через внутренний C bridge. Общий process-wide budget допускает четыре одновременно выполняющихся job. Попытка запустить пятую завершается до создания worker с ошибкой `Background job capacity reached`.

## Ownership

Каждый успешно полученный permit захватывается lambda worker вместе с frozen `State` и `shared_ptr` результата. Permit освобождается после публикации финального status либо при исключении. Если создание `std::thread` бросает исключение, локальный permit также освобождается. Atomic counter использует CAS и не превышает четыре при конкурентном старте.

`daw_release_save` и `daw_release_export` удаляют только caller handle. Worker продолжает владеть result и snapshot, поэтому handle можно отпустить и уничтожить исходный `daw_session` сразу после старта. Ни save, ни export после захвата не обращаются к session, transport, Core Audio output или UI.

## Saturation contract

Лимит общий для save и export, поскольку обе операции выполняют заметные CPU, memory и disk действия. Текущий AppKit-клиент держит максимум manual save, recovery save и один export, оставляя ещё один слот. Внутренний C API пока передаёт busy как текст последней ошибки; типизированный error code появится вместе с публичным command API.

Очередь намеренно не накапливает снимки. Caller может повторить операцию после завершения текущей job. Для recovery UI уже использует такой режим: пропущенный checkpoint пробуется снова на следующем интервале.

## Проверка

`storage_jobs_crash` детерминированно занимает все четыре permits, проверяет отказ пятого save и возврат счётчика к нулю. Затем он запускает save, немедленно освобождает handle и уничтожает session; опубликованный draft должен остаться читаемым. `offline_wav_export` выполняет тот же lifetime-сценарий для WAV.

Все шесть CTest проходят в Debug и ASan/UBSan. Отдельный `thread-sanitizer` preset собирает ядро с TSan; focused `storage_jobs_crash` и `offline_wav_export` проходят без обнаруженной гонки. Это закрывает SP-03 для текущих фоновых C bridge jobs. Retirement активного Core Audio render plan и физический shutdown устройства относятся к B-005 и не доказаны этими тестами.
