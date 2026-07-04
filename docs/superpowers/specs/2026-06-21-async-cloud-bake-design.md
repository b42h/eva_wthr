# Дизайн: асинхронний бейк масок хмар

**Дата:** 2026-06-21
**Файл реалізації:** `main/eva_weather_canvas.c` (обидві копії: `42_EVE_FW/phase7_eva_weather` + `eva_wthr`)
**Платформа:** ESP32-P4, FreeRTOS, рендер 800×480

## Проблема (доведено на пристрої)

Коли cloud-морф завершується, `update_cloud_lifecycle()` викликає
`bake_strip_for_layer()` **синхронно в render-таску** (`eva_weather_canvas.c`,
~рядок 3379). Виміряно on-device: цей бейк коштує **до 1.25 с** для LOW-шару
(~0.66 с для MID) — 3× memset маски 800×768 (~600 КБ кожна, PSRAM) + сотні
гаусових блобів. Це блокує render-loop → видимий фріз ~раз на 30 с
(інтервали морфу `morph_hold_s` = FIB_55/34/21 с дають нерегулярні сплески).

## Мета

Прибрати фріз: винести бейк у фонову FreeRTOS-задачу. Render-таск ніколи не
бейкає — лише запитує бейк і перемикає варіант, коли маска готова.

## Контекст коду (як є)

- `cloud_strip_t` має `variant[CLOUD_VARIANT_COUNT]` (=2): `active` + `hidden`.
  Морф робить crossfade від active до hidden, потім міняє їх місцями.
- Маски (`a8_light/shadow/core`) виділяються в **PSRAM** через
  `heap_caps_aligned_alloc` у `init_cloud_strips()`, що викликається **лише на
  старті** (рядки 4811/4909). `eva_weather_canvas_set_weather()` **НЕ**
  реалоцирує маски — `kind_changed` лише скидає прапорці/тінти (рядок 5095).
- Render крутиться в одній задачі `native_render_task` (priority 5, core 1).
- `update_cloud_lifecycle()` (render-таск) керує морфом і викликає бейк.

## Архітектура

Окрема задача `cloud_bake_task` (priority 3, core 0 — щоб не конкурувати з
render на core 1). Координація через **FreeRTOS task notification** + per-strip
`volatile` стан. Третій буфер НЕ потрібен — бейкер пише в `hidden`-варіант,
який render не читає, поки показує `active`.

## Модель станів (per-strip)

Додати в `cloud_strip_t`:
```c
volatile uint8_t bake_state;   /* BAKE_IDLE / REQUESTED / RUNNING / DONE */
uint8_t bake_variant;          /* який варіант пекти (hidden) */
```

- `BAKE_IDLE` — обидва варіанти готові; морф працює як зараз.
- `BAKE_REQUESTED` — морф завершився; render попросив бейк, чекає.
- `BAKE_RUNNING` — бейкер пече hidden-маску.
- `BAKE_DONE` — маска готова; наступний морф цього шару дозволений.

## Потік даних

**Render-таск (`update_cloud_lifecycle`):**
- При `morph_t >= 1.0`: перемкнути `active_variant` (атомарний запис uint8_t),
  виставити `bake_variant = hidden`, `bake_state = BAKE_REQUESTED`, і
  `xTaskNotify(s_bake_task, (1<<layer), eSetBits)`. **НЕ бейкати самому.**
- Наступний морф шару НЕ стартує, поки `bake_state != BAKE_DONE` (просто довший
  hold — візуально непомітно, хмари й так повільні).
- Коли бачить `BAKE_DONE` → `bake_state = BAKE_IDLE`.

**Бейкер (`cloud_bake_task`):**
- `xTaskNotifyWait` блокується (0% CPU без роботи).
- На нотифікацію: для кожного зведеного біта-шару → `bake_state = BAKE_RUNNING`,
  `bake_strip_for_layer(layer, &strip->variant[bake_variant], strip_h)`,
  потім `bake_state = BAKE_DONE`.

**Безпека:** бейкер пише в hidden, render малює active — різні маски, без гонки.
`active_variant` міняє ТІЛЬКИ render (бейкер лише читає, який hidden). Кожне
`volatile`-поле має одного писаря → м'ютекс не потрібен (uint8_t запис атомарний
на RISC-V).

## Граничні випадки

1. **Бейк не встиг до наступного морфу:** морф чекає `BAKE_DONE` — hold довший,
   render не блокується. ОК.
2. **Зміна погоди (`set_weather`):** НЕ реалоцирує маски (доведено) → гонки на
   free() немає. Безпечно під час бейку.
3. **Init:** перший бейк лишається синхронним (один раз, до старту render-таску
   — фрізу нема).
4. **Деструкція/реініт** (`init_cloud_strips` вдруге, shutdown): ПЕРЕД free()
   масок — дочекатися, що бейкер не в `BAKE_RUNNING` (короткий spin або
   нотифікація-стоп). Це єдине місце, де маски звільняються.
5. **Подвійний морф того самого hidden:** неможливо — морф не стартує поки
   `!BAKE_DONE`.

## Ресурси

- Стек `cloud_bake_task`: ~4 КБ (бейк без рекурсії, мало локальних масивів).
- Пріоритет 3 (< render 5), core 0.
- Без третього буфера — пише в наявний hidden.

## Тестування

- **Host-тест** логіки переходів станів IDLE→REQUESTED→RUNNING→DONE та правила
  «морф не стартує поки !DONE».
- **On-device:** 70-с лог у `cloudy`, порівняти jitter до/після. Очікування:
  немає сплесків jitter при морфі (раніше до 1.3 с). Тимчасова інструментація
  `BAKE took … us` має показати, що бейк більше не в render-таску.
- Перевірити, що морф візуально працює (хмари змінюють форму, без розривів).

## Поза обсягом (YAGNI)

- Інкрементальний бейк (розбиття на кадри) — не обрано.
- Зменшення розміру strip / кількості блобів — не чіпаємо вигляд.
- Прибирання fixed `bg`/`ppa_rot` floor (це окрема пайплайн-проблема з §7 CLAUDE.md).

## Ризики

- **Видимість пам'яті між core:** `volatile` + task notification дають бар'єри;
  на ESP32-P4 (без агресивного reordering для PSRAM) цього достатньо. Якщо
  з'явиться розрив — додати `__sync_synchronize()` навколо зміни стану.
- **Перший морф після старту:** переконатися, що `bake_state` ініціалізований
  `BAKE_DONE`/`IDLE` для обох готових варіантів, інакше морф не стартує ніколи.
