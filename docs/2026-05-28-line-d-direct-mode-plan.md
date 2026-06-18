# ЛІНІЯ D — Direct Mode + AVOID_TEAR — Implementation Plan

**Дата:** 2026-05-28
**Стан перед стартом:** snapshot `backups/phase7_eva_weather-snapshot-2026-05-28-composite-cache`
**Поточний FPS:** 15-19 Hz (медіани) залежно від сцени, max 20 Hz
**Цільовий FPS:** 30-60 Hz (vsync-locked на 60 Hz панелі)
**Ризик:** ВИСОКИЙ — може зламати sw_rotate / координати layout / лv_canvas integration

---

## 1. Що уже зроблено перед цим планом

Перед цим планом завершено три рівні оптимізації (snapshot ladder):

1. **ЛІНІЯ C** (`main.c:2212`) — LVGL display config повернений до дефолтів BSP:
   - `buff_dma=true` (було `false`)
   - `buff_spiram=false` (було `true`)
   - `buffer_size=BSP_LCD_DRAW_BUFF_SIZE` = `BSP_LCD_H_RES*50` ≈ 48 KB (було 750 KB)
   - **Ефект:** `lvgl=` flush time 35.7 ms → 24 ms (-32%).

2. **Text bitmap cache** (`eva_weather_canvas.c`) — clock/temp/desc тепер мають
   bake-once + blit-A8-RGB565 кеш:
   - `text_slot_t` структура з A8 mask 720×120 у PSRAM (3 слоти × 86 KB).
   - `bake_text_slot()` викликає `lv_font_get_glyph_bitmap` лише коли (text,scale) змінились.
   - `blit_text_slot()` робить лінійний RGB565 blend кожен кадр.
   - **Ефект:** усунув ~5-8 ms/frame text rendering з hot path.

3. **Cloud pass skip** (`blend_layer_variant`) — shadow+core PPA blends
   skip коли їх effective alpha < FIB_34 (=34). Light pass завжди йде.
   - **Ефект:** clear-day cl 55→38 ms (-17 ms).

4. **Composite cache** (`s_composite_buf`) — другий рівень кешу зверху над
   існуючим bg cache. Зберігає `bg + text + clouds + cloud-light` у 750 KB
   PSRAM, повторюється `composite_hold_frames(kind)` ticks (2-3 кадри).
   God rays, particles, lightning лишаються fresh поверх.
   - **Ефект:** cl 38-62→12-21 ms, tick +3-6 Hz на всіх сценах.

**Залишковий bottleneck:** `lvgl=24 ms` сам по собі (LVGL flush у DPI
framebuffer через esp_lvgl_port, з SW rotation 270°). 1/24ms = **41 Hz cap**
навіть якщо render час 0. До 30 FPS нам не вистачає тому що **work_us +
lvgl_us > 33 ms** = 30 FPS slot.

---

## 2. Що дає ЛІНІЯ D і чому це ризиковано

### Що дає
- **Direct mode**: LVGL пише прямо у DPI framebuffer, минаючи проміжний
  `draw_buf`. Усуває один 750 KB memcpy на кадр (з draw_buf у DPI fb).
- **AVOID_TEAR**: два DPI framebuffers (A і B). LVGL рендерить у один,
  поки panel зчитує інший через MIPI-DSI DMA. На vsync — swap. Це
  **vsync-locked рефреш** без тиринга, заявлено 60 Hz.
- **DPI_BUFFER_NUMS=2**: вмикає двобуферний DSI. Коштує +750 KB PSRAM.

### Чому ризиковано
1. **`sw_rotate=true` несумісне з `AVOID_TEAR=y`.** Це **зашите в BSP**
   (`esp32_p4_function_ev_board.c:639-643`):
   ```c
   #if CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR
       .sw_rotate = false,  /* Avoid tearing is not supported for SW rotation */
   #else
       .sw_rotate = cfg->flags.sw_rotate,
   #endif
   ```
   **Наслідок:** наш `bsp_display_rotate(disp, LV_DISPLAY_ROTATION_270)`
   у `main.c:2230` буде проігнорований. LVGL малюватиме у нативній
   орієнтації панелі — **480×800 портрет**.

2. **Hardware rotation 90/270° на цій панелі недоступна.**
   `esp32_p4_function_ev_board.c:642` (коментар у самій BSP) каже:
   > Only SW rotation is supported for 90° and 270°

3. **Наш `lv_canvas` рендериться у 800×480 (`EVA_WEATHER_RENDER_W=800`,
   `EVA_WEATHER_RENDER_H=480`).** Якщо LVGL раптом стане 480×800,
   canvas може не вмістити або криво відобразитись.

4. **Наш canvas-tick timer + `lv_obj_invalidate` workflow** може ламатись
   у direct mode. У direct mode LVGL очікує, що ми оголошуємо
   invalidated regions, і LVGL flush'ить **щойно змінені прямокутники**
   у обидва framebuffer'и по черзі (бо коли swap відбувається, другий
   fb теж має містити свіжий вміст). Якщо ми invalidat'имо весь canvas
   щокадру — LVGL зробить два full flush'и (по одному в кожен fb)
   замість одного.

5. **`s_render_buf` зараз same pointer як `s_display_buf == s_buf`**, і
   LVGL читає з нього. У direct mode LVGL пише прямо у DPI fb, тобто
   наш canvas pointer треба буде синхронізувати з DPI fb pointer'ом.

---

## 3. Стратегія: три варіанти

### Варіант D1 — direct_mode + AVOID_TEAR без чіпання sw_rotate (НАЙДЕШЕВШИЙ)
Просто включити три прапори в sdkconfig, побудувати. Якщо BSP сам собою
вимкне sw_rotate і landscape виглядатиме повернутим — потім вирішувати.

**Це швидкий «спробуй і побач», DOA-перевірка.**

### Варіант D2 — direct_mode + AVOID_TEAR + canvas → native portrait 480×800
Якщо D1 показує повернутий екран, переходимо до варіанту, де ми
**робимо canvas 480×800** і весь layout рендериться у портретній
орієнтації — сонце ліворуч, текст вертикально, тощо. Це **великий
рефактор coordinates у `eva_weather_canvas.c`** (~30+ місць де
EVA_WEATHER_RENDER_W/H використовуються).

### Варіант D3 — direct_mode БЕЗ `lv_canvas`, прямий рендер у DPI fb
Найбільший виграш — викинути `lv_canvas` взагалі. Замість того, щоб
малювати у `s_render_buf` і просити LVGL flush'ити у DPI fb, ми берем
DPI fb pointer (`esp_lcd_dpi_panel_get_frame_buffer`) і малюємо туди
**напряму**. LVGL використовуємо лише для управління backbuffer swap.

Це найбільший рефактор. Але архітектурно — найчистіший.

**Рекомендація:** виконувати D1 → D2 → D3 послідовно зі snapshots. На
будь-якому етапі, якщо вийшло — стоп.

---

## 4. Pre-flight checklist

Перед стартом ЛІНІЇ D **обов'язково**:

- [ ] `git status` чистий АБО створити snapshot ladder branch.
- [ ] `cp -r phase7_eva_weather backups/phase7_eva_weather-snapshot-2026-05-28-pre-line-D` (rsync без build/).
- [ ] Перевірити що поточний код прошитий і пристрій дає baseline 15-19 Hz
      (як описано в Section 1).
- [ ] Підготувати rollback скрипт: `rsync -a backups/<pre-line-D>/ phase7_eva_weather/`.

---

## 5. D1 — мінімальний sdkconfig change

### D1.1 Включити три прапори

**Файл:** `phase7_eva_weather/sdkconfig`

Замінити:
```
CONFIG_BSP_LCD_DPI_BUFFER_NUMS=1
```
на:
```
CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2
CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y
CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE=y
```

Також прибрати, якщо стояло `CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH=y`
(перевірити що його нема — він конфліктує з DIRECT_MODE у Kconfig).

Якщо в `sdkconfig.defaults` цих значень нема — додати туди дублікатно,
щоб не загубити після `idf.py reconfigure`.

### D1.2 Відкоригувати `main.c:2212` для AVOID_TEAR

Поточний код:
```c
bsp_display_cfg_t cfg = {
    .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
    .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
    .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
    .flags = {
        .buff_dma = true,
        .buff_spiram = false,
        .sw_rotate = true,         /* буде проігнорований BSP при AVOID_TEAR */
    },
};
```

У AVOID_TEAR режимі `buffer_size` у `cfg` зазвичай **не використовується**
для LVGL draw_buf (бо LVGL пише прямо у DPI fb). Але struct поле лишається —
краще встановити `buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES` (повний кадр),
щоб якщо BSP колись використає для буферного buffer'у — він був повного розміру.

```c
bsp_display_cfg_t cfg = {
    .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
    .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
    .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
    .flags = {
        .buff_dma = true,
        .buff_spiram = true,        /* DPI framebuffer завжди PSRAM */
        .sw_rotate = false,         /* AVOID_TEAR вимагає false */
    },
};
```

### D1.3 Прибрати `bsp_display_rotate` дзвінок

У AVOID_TEAR режимі rotation API не працює. Закоментувати:
```c
// bsp_display_rotate(disp, LV_DISPLAY_ROTATION_270);
```
(Залишити коментар з примітками щоб майбутній читач не повертав.)

### D1.4 Build, flash, observe

```sh
cd phase7_eva_weather
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf"
source $IDF_PATH/export.sh
idf.py fullclean   # ВАЖЛИВО — sdkconfig зміни вимагають
idf.py build
cd ..
./flash-weather.sh
```

**Очікувані сценарії:**

| Що бачимо | Що це означає | Куди далі |
|---|---|---|
| Boot panic у `bsp_display_start_with_config` (NULL disp) | Alloc fail DPI buffer 2 | Перевірити free PSRAM, можливо звільнити інші alloc |
| Boot OK, екран портретний (повернутий 90°) | sw_rotate справді disabled | Переходимо до D2 (layout 480×800) |
| Boot OK, екран landscape як треба, але координати зміщені | Якесь mirror_x/mirror_y несумісне | Гратись з `swap_xy` в DPI config |
| Boot OK, FPS ≥45 Hz, картинка ОК | **JACKPOT** | Snapshot, готово |
| Boot OK, картинка ОК, FPS не змінився | Direct mode не активувався насправді — перевірити логи | Дебажити lvgl_port |

### D1.5 Зміряти FPS після D1

```sh
python3 -c '
import serial, time, statistics, re
s = serial.Serial(); s.port="/dev/cu.usbmodem1234561"; s.baudrate=115200
s.timeout=2.0; s.dtr=True; s.rts=True; s.open(); time.sleep(0.5)
s.reset_input_buffer()
for k in ["clear-day", "overcast", "thunderstorm"]:
    s.write(f"weatherdebug {k} 0\r\n".encode()); time.sleep(0.6)
    end=time.time()+15; ticks=[]
    while time.time() < end:
        try: l=s.readline().decode("utf-8","replace").rstrip()
        except: break
        m=re.search(r"tick=(\d+) Hz",l)
        if m: ticks.append(int(m.group(1)))
    print(f"{k}: median={statistics.median(ticks) if ticks else 0} max={max(ticks) if ticks else 0}")
s.close()'
```

**DoD D1:** boot успішний, FPS ≥ 30 Hz на clear-day, екран не перевернутий і
не криво кольорний.

### D1.6 Snapshot D1
```sh
rsync -a --exclude build/ --exclude managed_components/ --exclude dependencies.lock \
  phase7_eva_weather/ backups/phase7_eva_weather-snapshot-2026-05-28-D1-direct-mode/
```

---

## 6. D2 — Якщо D1 показав повернутий екран

Якщо ви бачите портретну орієнтацію (480×800), потрібно перерендерити
все у портретний layout. Це означає:

### D2.1 Канвас 480×800

`eva_weather_canvas.h` — змінити:
```c
#define EVA_WEATHER_RENDER_W 480
#define EVA_WEATHER_RENDER_H 800
#define EVA_WEATHER_CANVAS_W 480
#define EVA_WEATHER_CANVAS_H 800
```

### D2.2 Layout coordinates

Файли: `eva_weather_canvas.c` (всі позиції), `main.c` (test UI overlays).

Що міняти:
- Sun позиція (`s_sun_x`, `s_sun_y`)
- Cloud strip y-coordinates (HIGH: y=20..120, MID: y=100..260, LOW: y=220..400 — треба перерахувати)
- Clock x-centered, y-centered formula лишається але числа інші
- Particle bounds
- God rays origin
- Lightning bolt coordinates
- Fog band y positions

**Це механічна, але багато роботи.** Реально 30+ місць.

### D2.3 PPA cloud composition

Шифти у `compose_clouds_into_working_buffer` пишуть у фіксованих координатах.
Cloud strip 800px wide PPA blit'иться у різні x_offset. У портреті
треба переробити так, щоб блити в y-strip замість x-strip
(або повернути 90° сам strip).

Складно. **Альтернатива:** залишити cloud strip 800×120 і дозволити йому
блитися як вертикальна смуга. Це візуально буде «хмара згори вниз», не зліва направо.

Кращий варіант — змінити cloud strip geometry на 480 wide і 200 tall,
бо в портретному режимі по логіці хмари все одно мали б рухатись горизонтально.

### D2.4 Snapshot D2
```sh
rsync -a phase7_eva_weather/ backups/phase7_eva_weather-snapshot-2026-05-28-D2-portrait/
```

**Це БАГАТО роботи.** Якщо D1 показав landscape без проблем — D2 пропускаємо.

---

## 7. D3 — Прямий рендер у DPI framebuffer

Якщо D1 і D2 дали результат, але FPS все одно не 60 — наступний крок
прибрати `lv_canvas` overhead.

### D3.1 Отримати DPI framebuffer pointer

```c
#include "esp_lcd_mipi_dsi.h"
void *fb_a, *fb_b;
esp_lcd_dpi_panel_get_frame_buffer(panel_handle, 2, &fb_a, &fb_b);
```
Pancel handle треба зберегти з `bsp_display_new_with_handles`. BSP його
використовує внутрішньо — можливо потрібно експонувати через нову функцію.

### D3.2 Замінити lv_canvas на manual paint
- Прибрати `lv_canvas_create`, `lv_canvas_set_buffer`.
- У `canvas_tick`: рендерити прямо у current backbuffer (fb_a або fb_b),
  чекати vsync callback, swap.
- Для tracking текстових labels (якщо вони не в canvas, а в LVGL widgets) —
  можуть залишитись як LVGL objects.

### D3.3 Sync з DPI panel

Потрібна `esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_color_trans_done = ... }`.
У callback'у — позначити поточний backbuffer як ready, swap pointer'ів.

**Це принципово інша архітектура.** Може взяти 4-8 годин роботи.

---

## 8. Точки відкату (rollback ladder)

| Якщо | Команда rollback |
|---|---|
| D1.4 boot panic | `git checkout -- sdkconfig main.c` + `idf.py fullclean build flash` |
| D1.5 FPS не виріс, картинка крива | `rsync -a backups/phase7_eva_weather-snapshot-2026-05-28-composite-cache/ phase7_eva_weather/` |
| D2 layout зламаний | rollback до D1 snapshot |
| D3 LVGL крашиться | rollback до D2 |
| Все погано | rollback до composite-cache snapshot, FPS лишається 15-19 |

---

## 9. Метрики DoD

| Етап | Цільовий tick (clear-day) | Цільовий lvgl |
|---|---|---|
| Зараз (composite-cache) | 15 Hz | 24 ms |
| Після D1 | ≥30 Hz | ≤15 ms |
| Після D3 (якщо доходимо) | 45-60 Hz (vsync) | n/a |

**Acceptance:** на clear-day, partly-cloudy, overcast — медіана tick ≥ 30 Hz,
max ≥ 35 Hz, без видимого тиринга або візуальних артефактів.

---

## 10. Що НЕ робимо

- Не вмикаємо `CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH=y` (це гірше за direct_mode для нас).
- Не міняємо MIPI-DSI timing constants.
- Не повертаємось на 400×240 render (це останній варіант, якщо все провалиться).
- Не редагуємо BSP code (`esp32_p4_function_ev_board.c`) — лише sdkconfig і
  наш application code.
- Не змінюємо архітектуру cache (bg_buf + composite_buf лишаються як є).

---

## 11. Як читати цей план агенту, якого ви запускаєте

1. **Прочитати Sections 1-4 повністю** — це контекст того що уже зроблено
   і чому ризики саме такі.
2. Виконати **тільки D1** і повернутися з результатом.
3. Не лізти у D2/D3 без явного підтвердження користувача.
4. Snapshot перед кожним кроком — обов'язково.
5. Після build перед flash — `git diff` для рев'ю змін.
6. Логи зберігати у `phase7_eva_weather/docs/D1_result_<timestamp>.log`
   щоб ми могли порівняти.

---

## 12. Корисні файли

- Поточний стан display config: `main.c:2212-2225`
- Поточна rotation initialization: `main.c:2230`
- Canvas init/lifecycle: `eva_weather_canvas.c:3095` (`eva_weather_canvas_init`)
- BSP display init (read-only ref): `common_components/espressif__esp32_p4_function_ev_board/esp32_p4_function_ev_board.c:602-696`
- Попередній план: `../lvgl_fps_plan.md` (більш загальний контекст ЛІНІЇ C+D)
- Snapshot ladder: `../backups/`
- CDC measurement script: див. Section 5 D1.5
