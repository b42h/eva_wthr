# next_update — план переходу на плавний (60 fps) рендер weather scene

Цей документ — повний робочий план переходу від поточного стану
(`tick ≈ 10–15 Hz`, native 800×480 через `lv_canvas`, текст у LVGL labels) до
цільового стану:

- **60 fps стійко** у будь-яку погоду (включно з грозою).
- **3D-перспектива неба**: сонце сходить зліва, сідає справа; хмари
  пливуть з горизонту на південь (наближаються до глядача), вітер дає
  бічний дрейф.
- **Хмари перекривають текст годинника** (occlusion).
- **Без шрифтових тіней/rim-light** як стек із LVGL labels.

План розбито на фази так, щоб кожна фаза:
- збиралася, прошивалася і тестувалася окремо;
- давала вимірюваний приріст fps або візуальну ціль;
- мала чіткий критерій готовності (DoD).

Усі вимірювання — через CDC `tick=N Hz, work_us=W (bg=… cl=… pa=… li=… up=…)`
з логу `eva_canvas`. Тестові сцени: `clear-day`, `partly-cloudy-day`,
`cloudy`, `thunderstorm`. Заміри **завжди** — після ≥10 секунд від boot
(після Wi-Fi/NTP) і не менше 8 секунд safe window.

---

## Поточний стан (baseline на дату плану)

| Сцена         | tick  | work_us | breakdown                                  |
|---------------|------:|--------:|--------------------------------------------|
| clear-day     | 24 Hz |  ~7 ms  | bg 2.7 / cl 0 / pa 0 / up 0 (native)       |
| cloudy        | 14 Hz | ~12 ms  | bg 10.8 / cl 0.6 cached / pa 0 / up 0      |
| thunderstorm  | 10 Hz | ~20 ms  | bg 10.7 / cl 2.5–3.7 / pa 3.0 / li 0–5     |

**Архітектурний bottleneck зараз**:

1. `bg memcpy 750 KB у PSRAM ≈ 10 ms` на кожен «cold» кадр — обмежено
   пропускною здатністю PSRAM write (~100 MB/s).
2. `lv_timer` запускає `canvas_tick` із того ж LVGL refresh task, що й
   `lv_obj_invalidate` flush — серіалізація з MIPI DPI flush.
3. `lv_canvas` (single-buffer) не дозволяє паралельний рендер у `s_buf`
   поки LVGL читає його для flush.

Усі три обмеження — структурні. План нижче їх знімає по черзі.

---

## Фаза 0 — стабілізація та інструменти (0.5 дня)

Мета: повернутися до контрольованого стану, прибрати незавершені експерименти,
закласти основу для вимірювань.

### 0.1 Очистити mertvий код
- [next_update.md](next_update.md) — цей файл (готовий).
- Прибрати `upscale_render_to_display*` (вже зроблено).
- Прибрати залишкові посилання на `s_clock_shadow_labels[]`,
  `s_clock_light_label`, `s_clock_inner_shadow_label` у `main.c` — об'єкти
  не створюються, тримати NULL-guard у геттерах.
- Прибрати `g_prof_*` лічильники з шапки (винести у внутрішні `static`).
- Залишити `eva_weather_canvas_last_tick_hz/work_us` і FPS overlay — це
  основний інструмент валідації фаз.

### 0.2 Зробити debug-мітки точними
- Додати у `tick log` поле `lvgl_us` — час витрачений LVGL refresh task
  від кінця render до моменту коли `canvas_tick` отримує наступний слот.
  Вимірюється у `canvas_tick` через `esp_timer_get_time()` на вході мінус
  попередній вихід. Це дозволить розрізняти «render повільний» від «LVGL
  не дає процесорний слот».
- Додати у tick log `vsync_us` — час між послідовними `lv_obj_invalidate`
  завершеннями (LVGL flush durations).

### 0.3 Зафіксувати baseline
Зробити screenshots і записати tick для всіх 4 сцен у
[PERF_PLAN.md](PERF_PLAN.md) як «Phase 0 baseline». Без цього не можна буде
довести регресію/прогрес у наступних фазах.

**DoD фази 0:**
- `next_update.md` мерджений.
- Прибраний мертвий код, build чистий.
- Baseline зафіксований.

---

## Фаза 1 — bypass LVGL canvas, прямий framebuffer (1.5 дня)

Мета: вийти з-під `lv_canvas` і `lv_timer`-based render. Це знімає
обмеження №2 і №3 з baseline. Очікуваний приріст: +15–30 fps на cloudy/storm.

### 1.1 Архітектура

Замість єдиного `s_buf == s_display_buf` створити **тришарову структуру**:

```
┌─ canvas render task ─┐         ┌─ LVGL refresh task ─┐
│ render_weather()     │         │ обробляє ТІЛЬКИ      │
│ → s_buf[front]       │         │ overlays (текст,     │
│ atomic swap front/back         │ test-mode banner,    │
│ → s_buf[back]        │         │ fps label) над       │
│                      │         │ panel framebuffer    │
└──────────────────────┘         └──────────────────────┘
        ↓ DMA copy (PPA SRM scale=1, async)
┌─ MIPI panel ─────────┐
│ panel_framebuffer    │ ← LVGL flush також пише сюди для overlays
│ (esp_lcd direct)     │
└──────────────────────┘
```

- `s_buf[2]` — double-buffer 800×480 RGB565, обидва у PSRAM.
- `render_weather` пише у `s_buf[back]` без жодних LVGL locks.
- По завершенні рендеру — atomic swap front/back під коротким spinlock
  (~10 ns).
- Окремий DMA-копіювач (PPA SRM, scale=1) у фоні переносить `s_buf[front]`
  у `panel_framebuffer`. Це **async** — CPU не чекає.
- LVGL продовжує існувати тільки для overlays: годинник, weather labels,
  тестовий чекбокс, fps overlay. LVGL рендерить ці labels БЕЗ `lv_canvas`,
  напряму у той самий `panel_framebuffer` як child draws поверх скопійованого
  canvas-shot.

### 1.2 Реалізація: новий рендер-task

```c
static StaticTask_t s_canvas_task_tcb;
static StackType_t  s_canvas_task_stack[6144];

static void canvas_render_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        render_weather(get_dt());            /* пише у s_buf[back] */
        swap_buffers();                       /* portMUX spinlock */
        kick_dma_copy_to_panel();             /* PPA async start */
        log_tick(esp_timer_get_time() - t0);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TIMER_MS));
    }
}

/* створюється з пріоритетом 5, pinned на CPU1 */
xTaskCreateStaticPinnedToCore(canvas_render_task, "canvas", 6144, NULL, 5,
                              s_canvas_task_stack, &s_canvas_task_tcb, 1);
```

LVGL refresh task (pinned CPU0, priority 4) працює незалежно, не блокує.

### 1.3 Реалізація: DMA copy callback

```c
static SemaphoreHandle_t s_dma_done;

static bool dma_copy_done_cb(ppa_client_handle_t c, ppa_event_data_t *d,
                             void *user) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_done, &hp);
    return hp == pdTRUE;
}

static void kick_dma_copy_to_panel(void) {
    /* Чекаємо попередню копію (якщо ще йде) */
    xSemaphoreTake(s_dma_done, portMAX_DELAY);
    ppa_srm_oper_config_t cfg = {
        .in.buffer = s_buf[s_front_idx],
        .out.buffer = panel_fb,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg);
    /* PPA on_trans_done викличе dma_copy_done_cb */
}
```

### 1.4 Реалізація: LVGL поверх canvas

`s_canvas` (lv_canvas) **видалити**. Натомість — `lv_obj_t *s_canvas_img =
lv_image_create(scr); lv_image_set_src(s_canvas_img, &s_canvas_image_dsc);`
де `s_canvas_image_dsc.data = panel_fb`. LVGL `move_background(s_canvas_img)`,
ніколи не invalidate'ить його.

Текст годинника, weather, fps — `lv_label_create` поверх. LVGL bufferflush
тільки невеликі області labels — це швидко (десятки ms на 144-px glyphs
зрідка, мікросекунди коли тексту немає).

### 1.5 Очікуваний результат

| Сцена         | tick  | work_us (render) | DMA copy (паралельно) |
|---------------|------:|-----------------:|----------------------:|
| clear-day     | 60+ Hz |  3 ms          | ~7 ms async           |
| cloudy        | 30–45 Hz | 10 ms        | ~7 ms async           |
| thunderstorm  | 25–35 Hz | 18 ms        | ~7 ms async           |

**DoD фази 1:**
- `tick ≥ 30 Hz` у `thunderstorm`.
- `tick ≥ 50 Hz` у `clear-day`.
- LVGL refresh лог відсутній у hot path (jpeg screenshot все ще працює).
- Жодних race conditions — screenshots без tearing-артефактів.

### 1.6 Ризики та як їх знімати

| Ризик | Mitigation |
|-------|------------|
| PPA non-blocking з max_pending=1 → блокує другий kick | Збільшити `max_pending_trans_num` до 4 при `ppa_register_client` |
| LVGL flush сам копіює `panel_fb` у власний draw_buf і це повільно | Налаштувати LVGL display з `direct_mode=1`, `panel_fb` як активний buf |
| `lv_image_dsc_t` LVGL читає при кожному refresh навіть якщо не змінився | OK — це лише копія `panel_fb` адреси, не вмісту |
| Tearing під час swap | Buffer swap робиться по DMA-done IRQ, тобто між двома фреймами |

---

## Фаза 2 — текст у canvas, occlusion хмарами (1 день)

Мета: годинник малюється у `s_buf` як частина рендеру. Хмари природньо
перекривають його, бо рендеряться поверх. Заодно прибираємо весь LVGL
label flush для clock — economія на LVGL refresh.

### 2.1 Шрифт-рендер

Використовуємо існуючі `eva_font_clock_144*` (LVGL bitmap fonts, bpp=4).
Створюємо власний rasterizer:

```c
typedef struct {
    const lv_font_t *font;
    int bpp;
} eva_font_t;

void eva_draw_text(uint16_t *buf, int buf_w, int buf_h,
                   const char *utf8, int x, int y,
                   uint16_t color, uint8_t alpha,
                   const eva_font_t *font);
```

Алгоритм:
1. Парсимо UTF-8 → list of code points.
2. Для кожного: `lv_font_get_glyph_dsc` → bitmap + box.
3. Розпаковуємо 4-bit alpha → blend з `buf` пікселем-за-пікселем
   (`blend_px(buf, x+gx, y+gy, color, alpha * glyph_a / 255)`).
4. Просуваємо `x += dsc.adv_w / 16`.

Кешування: `lv_font_get_glyph_dsc` дешевий, не кешуємо. Glyph bitmaps вже
у flash (LV_FONT_DECLARE).

### 2.2 Інтеграція у render_weather

Порядок шарів у `s_buf` (zorder):

```
1. fill_gradient (sky)
2. draw_sun_or_moon          ← sun з НОВОЇ траєкторії (див. Фазу 3)
3. eva_draw_clock(s_buf, "HH:MM", ...)  ← НОВЕ, ПЕРЕД хмарами
4. compose_clouds (3 layers) ← хмари перекривають текст
5. update_and_draw_particles
6. composite_lightning
```

`eva_draw_clock` пише в `s_buf` напряму. У темних сценах
(thunderstorm/clear-night) колір тексту 0xFFFFFF з alpha 240 — добре
читається. Хмари alpha=0..240 — частково перекривають.

### 2.3 Прибрати LVGL clock label

У [main.c](main.c):

```c
// Видалити:
// s_clock_label = lv_label_create(scr);
// s_clock_timer = lv_timer_create(clock_tick_cb, CLOCK_UPDATE_MS, NULL);

// Замінити на:
static char s_clock_text[16] = "00:00";
static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;

void eva_weather_canvas_set_clock_text(const char *txt) {
    portENTER_CRITICAL(&s_clock_lock);
    strncpy(s_clock_text, txt, sizeof(s_clock_text) - 1);
    portEXIT_CRITICAL(&s_clock_lock);
}
```

У `render_weather` зчитуємо `s_clock_text` під тим самим spinlock — без
LVGL involvement.

`update_clock_label` залишається як timer LVGL → форматує текст і кличе
`eva_weather_canvas_set_clock_text("HH:MM")` раз на 30 секунд.

### 2.4 Очікуваний результат

| Сцена | tick (delta vs Фаза 1) |
|-------|-----------------------:|
| clear-day    | +5 Hz (LVGL menos clock label) |
| cloudy       | +10 Hz (clock label invalidate щосекунди — geben) |
| thunderstorm | +5 Hz |

**DoD фази 2:**
- Хмари видимо перекривають текст годинника на storm/cloudy.
- Жодного `lv_label_create(s_clock_label)`.
- `eva_draw_clock` під CDC `screenshot` показує годинник у JPEG.

### 2.5 Окремий випадок: `+24C` температура та `Дощ (тест)` опис

Так само переносимо у canvas: `eva_draw_text(...)` для temperature і
description. `s_temp_label` / `s_desc_label` LVGL labels — видаляємо.
Залишаємо тільки overlays що рідко змінюються (Wi-Fi status, fps overlay,
test mode banner, checkbox).

---

## Фаза 3 — 3D-перспектива неба (2 дні)

Мета: візуально досягти ефекту погляду «прямо на горизонт»:
- сонце сходить **зліва на горизонті** (~y=20% screen, x rises 5%→50%),
  досягає зеніту (x=50%, y=10%), сідає **справа** (x=95%, y=20%);
- хмари **наближаються** з горизонту (top, small) до глядача (bottom, big);
- вітер дає бічний дрейф (схід-захід зсуває весь шар).

### 3.1 Нова модель cloud strip

Замість поточного `strip_h` фіксованої висоти і `scroll_x` горизонтального
руху, кожна хмара — окремий **2D particle** з полем `(x, y, scale)`:

```c
typedef struct {
    float x;                   /* 0..1 screen normalised */
    float y;                   /* 0..1 — 0 horizon, 1 viewer */
    float scale;               /* 0.3 horizon → 2.0 close */
    uint8_t shape_id;          /* index у preset cloud sprite atlas */
    uint8_t variant;
    uint8_t alpha;             /* base alpha, modulated by sun_pos */
    int8_t  vx;                /* horizontal drift (wind) */
} cloud_3d_t;

#define CLOUD_3D_MAX 24
static cloud_3d_t s_clouds_3d[CLOUD_3D_MAX];
```

Кожен tick:

```c
for (int i = 0; i < CLOUD_3D_MAX; ++i) {
    cloud_3d_t *c = &s_clouds_3d[i];
    /* approach: y росте, scale росте — клауд "наближається" */
    c->y += dt * (0.15f + c->scale * 0.10f);   /* ближчі швидше */
    c->scale = 0.3f + c->y * 1.7f;
    c->x += dt * (c->vx / 255.0f) * 0.05f;     /* боковий вітер */
    if (c->y > 1.15f) {
        /* respawn at horizon */
        c->y = -0.05f;
        c->x = rnd_uniform();
        c->shape_id = rnd_pick();
        c->scale = 0.3f;
    }
}
```

### 3.2 Pre-baked cloud sprite atlas

Замість дорогих PPA blends 3 strips × 3 passes, генеруємо **8 sprite shapes**
розміром 200×100 кожен (A8 mask). Зберігаємо в PSRAM (1.6 MB) **один раз**
при init. У runtime — PPA blend кожного active хмара = **1 PPA per cloud**.

```c
typedef struct {
    uint8_t *a8;          /* 200×100 alpha mask */
    uint8_t  base_r, base_g, base_b;
} cloud_sprite_t;

static cloud_sprite_t s_cloud_atlas[8];
```

`scale` застосовується через PPA SRM `scale_x = scale_y = c->scale` коли
blend'имо: PPA hardware масштабує sprite до бажаного розміру.

24 active clouds × 1 PPA scale-blend = 24 PPA ops. Кожна `~0.5 ms` (sprite
малий). Загалом `~12 ms`. Якщо багато — фільтруємо: рендеримо тільки **8
найближчих** (`c->y > 0.4`), решта скрізна (`alpha *= y * 2`).

### 3.3 Сонячна траєкторія

```c
static void compute_sun_pos(float progress, float *sun_x, float *sun_y) {
    /* progress 0..1, 0=sunrise (left horizon), 1=sunset (right horizon) */
    *sun_x = 0.05f + progress * 0.90f;
    /* y: parabolic arc, мінімум (зеніт) у progress=0.5 */
    float arc = 4.0f * progress * (1.0f - progress);  /* 0→1→0 */
    *sun_y = 0.20f - arc * 0.10f;   /* 20% бортах → 10% у зеніті */
}
```

Sun disc розміром відносно `(1 - sun_y) * base_r` (більше біля горизонту
для атмосферного ефекту збільшення).

### 3.4 Атмосферне шарування

Sky gradient тепер двофазний:

```c
/* y < 0.4: верхнє небо (cool blue) */
/* y > 0.6: нижнє небо (warmer, ближче до горизонту) */
/* у проміжку — interpolate */
```

При sunrise/sunset (progress < 0.15 або > 0.85) додаємо помаранчевий tint
на нижні 30% екрана.

### 3.5 Колір/освітлення хмар від сонця

Кожна хмара отримує tint:

```c
float side_dot = (c->x - sun_x);      /* -1..1 */
uint8_t r = lerp(120, 255, 1 - fabsf(side_dot));   /* біло-яскравіше з боку сонця */
```

На заході — теплий tint (255, 180, 120).

### 3.6 Очікуваний результат

| Сцена | tick |
|-------|-----:|
| clear-day    | 60 Hz (мало хмар) |
| cloudy       | 35–45 Hz |
| thunderstorm | 25–30 Hz |

Візуально:
- Чітке відчуття «дивлюся вперед на горизонт».
- Хмари ростуть з точки в зеніті/горизонті, наближаються.
- Sun сходить зліва, сідає справа.

**DoD фази 3:**
- Скрін на 06:00, 13:00, 20:00 — sun у трьох різних позиціях.
- Storm scene: хмари **різного розміру** одночасно на екрані (perspective).
- Test-mode swipe top/bottom змінює годину і sun помітно рухається.

---

## Фаза 4 — поліровка, кешування, edge cases (1 день)

### 4.1 Cloud sprite cache by scale
PPA scale кожен tick — стабільно ~12 ms. Якщо `scale` мало змінився
(<5%), кешувати result у RGB565 sprite у PSRAM. При наступному tick —
просто copy без scale. Це ефективно для повільних руху (далекі хмари).

### 4.2 Adaptive cloud count
Зараз `CLOUD_3D_MAX=24`. Якщо `tick < 30 Hz` протягом 2 секунд — зменшити
до 16. Якщо `tick > 50` — наростити назад. Аналог `adapt_budget` для
particles.

### 4.3 Lightning без full-screen blend
Поточний `composite_lightning_on_render` пробігає всі 800×480 pixels.
Замість цього — два режими:
- `flash`: один PPA fill rectangle над усім екраном (constant white, alpha
  scaled) → 1 ms hardware.
- `bolt`: тонкі лінії, рендеримо як було.

### 4.4 Sun halo через A8 sprite
Замість 4 concentric `draw_filled_circle` (CPU-bound, ~3 ms на storm cold
frame), pre-bake один 200×200 A8 halo sprite з gaussian falloff. PPA
blend з sun position. ~0.5 ms.

### 4.5 Particles: SIMD
ESP32-P4 RISC-V має P-extension (SIMD-lite). Rain particle update —
векторні add у groups of 4. Виграш ~2× на pa cost.

### 4.6 Test mode polish
- Swipe horizontal — циклю погод **сповільнено** (cooldown 300 ms між
  swipes щоб не пропускати 3 за 1 свайп).
- Swipe vertical — показати індикатор «+2h» секунду візуально, потім
  fade.
- Checkbox у куті: при ON додавати тонку червону рамку 2px по
  периметру.

### 4.7 Поведінка при NTP sync
Зараз при першому NTP `time(NULL)` стрибає з 0 на real. `update_clock_label`
бачить різницю і викликає `clock_set_text_all` → invalidate. У новій
архітектурі (Phase 2) text у canvas — нема цього проблеми.

**DoD фази 4:**
- Усі сцени `tick ≥ 30 Hz`. `clear-day ≥ 60`.
- Lightning не падає fps нижче 25.
- Test mode swipe responsive і має візуальний feedback.

---

## Фаза 5 — звільнення пам'яті, audit, документація (0.5 дня)

### 5.1 Memory audit
- `s_buf[2]` + `s_bg_buf` + `panel_fb` + `s_cloud_atlas[8]` ≈ 4.5 MB PSRAM.
- ESP32-P4 board: 16 MB PSRAM. Запас OK.
- Перевірити `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` після boot →
  має бути ≥ 8 MB вільно.

### 5.2 CDC `perf` команда
Додати CDC `perf` → друкує:
```
tick=NN Hz, work=NN ms (bg=N cl=N pa=N li=N)
clouds_active=NN/CLOUD_3D_MAX
sun=(x.xx, y.yy) phase=N
lvgl_us=NN  vsync_us=NN
```
Для швидкого debugging без чекання `LOG_EVERY_FRAMES`.

### 5.3 Оновити документацію
- [PERF_PLAN.md](PERF_PLAN.md) — фінальні метрики post-Phase-3.
- [9-Eva_Firmware/memory/00-current-state.md](../../9-Eva_Firmware/memory/00-current-state.md)
  — згадати, що `weather_fetch` ігнорується у test mode і що clock тепер
  малюється у canvas.
- Видалити `EVA_WEATHER_HANDOFF.md` reference на старий monolith — додати
  посилання на цей файл як live spec.

**DoD фази 5:**
- 8+ MB PSRAM вільно.
- CDC `perf` працює.
- Документи оновлені.

---

## Загальна шкала і ресурси

| Фаза | Дні | Ризик | Виграш fps  |
|------|----:|------:|------------:|
| 0    | 0.5 | низький | 0 (база)   |
| 1    | 1.5 | високий (refactor LVGL pipeline) | +15–30 |
| 2    | 1.0 | середній | +5–10      |
| 3    | 2.0 | високий (нова cloud model) | +5–15 |
| 4    | 1.0 | низький | +5         |
| 5    | 0.5 | низький | 0           |
| **Σ** | **6.5 днів** | | **~30→60 fps** |

---

## Контракт «що *не* робимо»

- Не міняємо LVGL версію.
- Не міняємо PPA driver — це частина ESP-IDF, оновлення тільки разом з IDF.
- Не вмикаємо `LV_USE_GPU_*` — PPA вже використовується вручну там, де
  доречно.
- Не змінюємо MIPI panel timing.
- Не виходимо на frame-rate >60 — пристрій 60 Hz фізично, вище не має
  сенсу.
- Не використовуємо float у hot loops (cloud update, particles) — або
  fixed-point Q8.8, або з float але без `expf`/`sinf` (precompute LUT).

---

## Точки відкату

Кожна фаза має tag у git: `phase0-baseline`, `phase1-direct-fb`, ...,
`phase5-done`. Якщо фаза провалює DoD за 1.5× від запланованого часу —
відкочуємось на попередній tag і робимо ревізію.

---

## Порядок merge

1. `phase0` → main: невидимий рефакторинг, безпечний.
2. `phase1` → main за feature flag `EVA_DIRECT_FB`. Дві тижні в production
   з fallback на старий шлях (`lv_canvas`) якщо щось не так. Видалити
   старий шлях у Phase 5.
3. `phase2`-`phase5` — фічі, мерж по готовності, без feature flags.

---

## Тестова матриця перед merge

| Сценарій | Очікувано |
|----------|-----------|
| Cold boot з Wi-Fi | clock '00:00' → NTP → 'HH:MM' без пропуску кадру |
| Cold boot без Wi-Fi | fallback clock з `esp_timer`, fps стабільний |
| Перехід day→night | плавно (≥30 кадрів transition) |
| Перехід sunny→storm | хмари рост через 5 секунд |
| Test mode swipe×100 | без leaks (heap stable) |
| Screenshot під storm | JPEG without tearing (Phase 1 DMA-done sync) |
| 30 хвилин ідлу | tick стабільний (без пресажу через NTP/Wi-Fi rotations) |

---

Кінець плану.
