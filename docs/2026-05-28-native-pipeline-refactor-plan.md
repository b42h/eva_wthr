# Native Hardware Pipeline Refactor — Full Plan

**Дата:** 2026-05-28
**Мета:** ≥30 FPS стабільно без зменшення якості, використовуючи залізо ESP32-P4 нативно.
**Baseline до старту:** snapshot `backups/phase7_eva_weather-snapshot-2026-05-28-pre-D2-portrait/`
**Поточний FPS:** 13-15 Hz median (single-layer light-only, sw_rotate)

---

## 0. Резюме: що буде

```
┌──────────────────────────────────────────────────────────┐
│ ДО REFACTOR (поточний стан)                              │
│                                                          │
│ render_task (CPU)                                        │
│   ├─ s_render_buf 800×480 PSRAM (LANDSCAPE mind)         │
│   ├─ bg cache memcpy 750KB PSRAM→PSRAM                   │
│   └─ PPA blend cloud strips × N passes                   │
│                                                          │
│ ↓ lv_canvas_set_buffer(s_render_buf)                     │
│ ↓ lv_obj_invalidate → LVGL flush_cb                      │
│                                                          │
│ esp_lvgl_port flush_cb                                   │
│   ├─ sw_rotate 270° (CPU loop через PSRAM, ~35→24ms)     │
│   └─ memcpy у DPI fb 480×800 PSRAM                       │
│                                                          │
│ MIPI-DSI DMA → панель (~16ms кадр @ 60Hz)                │
│ → ~13-15 FPS (PSRAM bandwidth bound, sw_rotate CPU cost)│
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ ПІСЛЯ REFACTOR (target)                                  │
│                                                          │
│ render_task (CPU)                                        │
│   ├─ s_render_buf 800×480 PSRAM (LANDSCAPE mind ЗБЕРЕЖЕНО)│
│   ├─ всі layers, passes, текст, sun, godrays — як зараз  │
│   └─ після render: PPA SRM rotate 90° → DPI back buffer  │
│                                                          │
│ ↓ замість LVGL flush_cb — наш direct→PPA→DPI шлях         │
│                                                          │
│ DPI driver double-buffer (DPI_BUFFER_NUMS=2)             │
│   ├─ DPI fb A 480×800 (scanned by MIPI-DSI)              │
│   └─ DPI fb B 480×800 (currently being written via PPA)  │
│   ↳ on vsync: A/B swap atomic                            │
│                                                          │
│ MIPI-DSI DMA → панель (vsync-locked, no tearing)         │
│ → 30-60 FPS (PPA rotation на DMA, паралельно з PSRAM)    │
└──────────────────────────────────────────────────────────┘
```

**Що це дає:**
- Прибирає 24 ms CPU sw_rotate (sw_rotate ходить через PSRAM, contention).
- PPA SRM з rotation 90° робить копію + rotation у DMA engine паралельно CPU.
- vsync-locked картинка через DPI_BUFFER_NUMS=2 + double buffer swap.
- Код **залишається в landscape 800×480 mind** — magic numbers, layout, sun arc — без зачіпання.
- Direct mode AVOID_TEAR викидається — нам не потрібен LVGL прямий доступ до DPI fb, у нас власний PPA pipeline.

**Що НЕ дає:**
- Не зменшує PSRAM bandwidth contention від bg cache (750KB) і cloud strip PPA passes (по факту bandwidth — це фундаментальний ліміт PSRAM hex 200 MHz).
- Не зменшує work_us самого render у `s_render_buf` (cl=37ms лишається).
- Real gain: -24ms (sw_rotate) + vsync sync = chunk towards 30+ FPS.

---

## 1. Поточна архітектура: повна карта pipeline

### 1.1 Що зараз робить кожен компонент

| Компонент | Where | Що робить | Час |
|---|---|---|---|
| `render_weather()` | canvas_tick LVGL timer | пише у `s_render_buf` 800×480 RGB565 PSRAM | ~30-60 ms |
| bg cache | render_weather | sky+sun+fog у `s_bg_buf`, hit memcpy 750KB | bg=9-15 ms |
| text overlays | render_weather | A8 bake cache → blit на s_buf | ~1-3 ms |
| cloud composition | render_weather | PPA blend 1 layer × 1 pass × 2 bands | cl=37 ms |
| sun god rays | render_weather | CPU loop overlay | ~3 ms |
| particles/lightning | render_weather | CPU loop overlay | <0.1 ms |
| `lv_obj_invalidate(canvas)` | canvas_tick | LVGL marks entire canvas region dirty | <1 ms |
| LVGL flush_cb | esp_lvgl_port | sw_rotate 270° + memcpy → DPI fb | lvgl=24 ms |
| MIPI-DSI DMA | hardware | reads DPI fb 480×800, sends to panel | bg, ~16ms @ 60 Hz |

### 1.2 Поточні буфери

| Буфер | Розмір | Розташування | Власник |
|---|---|---|---|
| `s_render_buf` | 800×480×2 = 750 KB | PSRAM aligned 128 | render_task |
| `s_bg_buf` | 800×480×2 = 750 KB | PSRAM aligned 128 | render_task (bg cache) |
| text slot A8 × 3 | 720×120×1 × 3 = 260 KB | PSRAM | render_task |
| cloud strip variants | 480×(167+267+300) × 3 × 2 = ~2.1 MB | PSRAM | render_task |
| LVGL draw_buf | 480×50×2 = 48 KB | internal SRAM (DMA) | LVGL flush |
| DPI fb × 1 | 480×800×2 = 750 KB | PSRAM | DPI driver |
| **PSRAM total** | ~4.6 MB | / 16 MB available | |

### 1.3 Чому sw_rotate дороге

`esp_lvgl_port` при `sw_rotate=true` + `LV_DISPLAY_ROTATION_270` робить наступне у flush_cb:
1. Має партикулярні rectangle invalidated regions від LVGL.
2. Для кожного rectangle обчислює перетворені координати у DPI fb space.
3. CPU loop читає по 1 пікселю з draw_buf (internal SRAM, ОК), пише у DPI fb (PSRAM, повільно).
4. Кожен записаний піксель = 2-byte PSRAM write з кешем флешем.
5. Для full canvas invalidate це 800×480 = 384K пікселів × 2 bytes write = 768KB PSRAM трафіку.
6. PSRAM hex 200 MHz write bandwidth ~400 MB/s теор → 768KB / 400 = 1.9 ms у вакуумі.
7. Реально 24 ms через contention з render task + другий цикл reading from draw_buf.

PPA SRM rotation робить **те саме** (rect copy with rotation) на dedicated DMA engine. CPU вільний. PSRAM bandwidth таки contended, але без CPU stall.

---

## 2. Цільова архітектура

### 2.1 Pipeline

```
render_task                    PPA SRM             DPI driver       MIPI-DSI
─────────────                  ───────             ──────────       ────────
                                                                   
[1] render →                                                        
    s_render_buf 800×480 PSRAM                                      
                                                                   
[2] wait PPA done (prev frame)                                      
                                                                   
[3] swap DPI back buffer:                                           
    back_idx ^= 1                                                   
    back_fb = dpi_get_fb(back_idx)                                  
                                                                   
[4] start PPA SRM:                                                  
    src=s_render_buf 800×480                                        
    dst=back_fb 480×800                                             
    rotation = 90° CW         → ←─PPA reads src,                    
                                  writes dst with rotation          
                                  (DMA, ~3-5ms wall)                
                                                                   
[5] register vsync callback                                         
    that will swap fb when                                          
    PPA done + next vsync                                           
                                                                   
[6] next frame begins ────────                                     
    render → s_render_buf                                           
                                                                   
                              ─── vsync fires ───→ scan_idx ^= 1   
                                                   panel sees                  
                                                   new fb                  
```

### 2.2 Key insight

LVGL більше **не керує** flush. Ми відмовляємось від `lv_canvas` і `lv_obj_invalidate` концепції. Натомість:
- `s_render_buf` — наш приватний буфер.
- LVGL ініціалізується для widget overlays (test mode panel, status overlays), малює у власний LVGL framebuffer.
- LVGL framebuffer і наш render buf **композитяться** через PPA blend перед/під час PPA rotation у DPI fb.

Це **середньо складна частина**. Альтернатива — викинути LVGL widgets взагалі (test mode переробити на native canvas overlay), тоді pipeline зовсім простий.

### 2.3 Буфери (нова таблиця)

| Буфер | Розмір | Розташування | Власник | Зміни |
|---|---|---|---|---|
| `s_render_buf` | 800×480×2 = 750 KB | PSRAM | render_task | без змін |
| `s_bg_buf` | 800×480×2 = 750 KB | PSRAM | render_task | без змін |
| text slot A8 × 3 | ~260 KB | PSRAM | render_task | без змін |
| cloud strip variants | ~2.1 MB | PSRAM | render_task | без змін |
| DPI fb A | 480×800×2 = 750 KB | PSRAM | DPI driver | без змін |
| **DPI fb B** | 480×800×2 = 750 KB | **PSRAM** | DPI driver | **новий (DPI_BUFFER_NUMS=2)** |
| LVGL framebuffer | (опц.) 800×480 | PSRAM | LVGL widgets only | переробити або викинути |
| **PSRAM total** | ~5.3 MB / 16 MB | | | +750 KB для DPI fb B |

---

## 3. Implementation phases

### Phase A: Підготовка (low risk, snapshot ladder)

**A1. Snapshot ladder**
```sh
cp -r phase7_eva_weather backups/phase7_eva_weather-snapshot-2026-05-28-pre-native-pipeline/
```

**A2. Зафіксувати baseline FPS**
Запустити вимірювання, записати в `docs/baseline_pre_native_pipeline.log`:
```python
for scene in [clear-day, partly-cloudy-day, overcast, rain, thunderstorm]:
    measure 30s
    log median tick, max tick, bg, cl, lvgl
```

**A3. Інвентаризація LVGL widgets**
Знайти всі `lv_*` widget create calls у `main.c`. Categorize:
- **Render-only canvas** (`s_canvas` у `eva_weather_canvas.c`) — це викидаємо.
- **Test mode panel** (test/checkbox/sliders/labels у main.c) — переробляти або викидати.
- **Status overlays** (IP overlay, FPS chip, wifi status) — переробляти або викидати.

### Phase B: DPI driver direct access (medium risk)

**B1. sdkconfig: DPI_BUFFER_NUMS=2, AVOID_TEAR=n, DIRECT_MODE=n**
Не використовуємо LVGL avoid_tear бо нам потрібен **наш** swap control через PPA done callback.
```
CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2
# CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR is not set
# CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE is not set
```

**B2. Експозиція DPI panel handle з BSP**
BSP у `bsp_display_new_with_handles` повертає `bsp_lcd_handles_t {io, panel, control}`. Зараз `bsp_display_start` приховує `panel`. Треба:
- Викликати `bsp_display_new_with_handles(NULL, &lcd_panels)` напряму у `main.c`.
- Зберегти `lcd_panels.panel` як global `s_panel_handle`.
- Використовувати `esp_lcd_dpi_panel_get_frame_buffer(s_panel_handle, 2, &fb_a, &fb_b)` для отримання DPI fb pointers.

**B3. Реєстрація vsync (refresh_ready) callback**
```c
esp_lcd_dpi_panel_event_callbacks_t cbs = {
    .on_refresh_done = on_dpi_vsync_done,
};
esp_lcd_dpi_panel_register_event_callbacks(s_panel_handle, &cbs, NULL);
```
У callback'у — signал semaphore що vsync відбувся, можна свапати back buffer.

**B4. Сlinacle test: панель показує static pattern**
Перед refactor weather pipeline — переконатись що DPI fb manually filled працює:
```c
uint16_t *fb_a = ...;
fill_solid_color(fb_a, 480, 800, rgb565(255, 0, 0));
esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, 480, 800, fb_a);
```
Якщо панель червона — DPI direct working. Якщо ні — debug первинно.

**DoD B:** панель показує статичний колір з нашого DPI fb без LVGL участі.

### Phase C: PPA SRM rotation у DPI fb (high risk)

**C1. Allocate PPA SRM client** (вже є `s_ppa_srm`).

**C2. Функція `render_to_dpi_fb()`**
```c
static void render_to_dpi_fb(uint16_t *dpi_fb_back)
{
    /* PPA SRM: rotate s_render_buf (800×480) 90° CW into dpi_fb_back (480×800).
     * Source: s_render_buf, src_w=800, src_h=480
     * Dest:   dpi_fb_back, dst_w=480, dst_h=800
     * Rotation: 90° (CCW або CW залежно від фізичної орієнтації) */
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = s_render_buf,
            .pic_w = 800, .pic_h = 480,
            .block_w = 800, .block_h = 480,
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dpi_fb_back,
            .buffer_size = 480 * 800 * 2,
            .pic_w = 480, .pic_h = 800,
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,  /* ← async! */
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg);
    /* На next vsync — swap. */
}
```

**C3. Сamuel chronology у canvas_tick**
```c
static void canvas_tick(lv_timer_t *timer)
{
    /* 1. Wait previous PPA done. */
    xSemaphoreTake(s_ppa_done_sem, portMAX_DELAY);
    
    /* 2. Wait next vsync, then swap the panel scan buffer to back buffer
     *    (the one PPA just finished writing). */
    xSemaphoreTake(s_vsync_sem, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, 480, 800, s_dpi_back_fb);
    
    /* 3. Swap: back_fb = old scan_fb (для наступного PPA write). */
    SWAP(s_dpi_back_fb, s_dpi_scan_fb);
    
    /* 4. Render weather frame у s_render_buf (CPU). */
    render_weather(dt);
    
    /* 5. Start PPA rotation s_render_buf → s_dpi_back_fb (async). */
    render_to_dpi_fb(s_dpi_back_fb);
    /* PPA done callback set s_ppa_done_sem. */
}
```

**C4. PPA done event callback**
```c
static bool on_ppa_event(ppa_client_handle_t client, ppa_event_data_t *data, void *user)
{
    BaseType_t hp_wake = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done_sem, &hp_wake);
    return hp_wake == pdTRUE;
}
```

**C5. Smoke test з статичним s_render_buf**
Перш ніж викликати render_weather — заповнити s_render_buf тестовим pattern (червоний/синій смуги horizontal). Прошити, переконатись:
- Панель показує **horizontally striped pattern повернутий на 90°**.
- Smooth refresh (no tearing).
- Logged FPS ≥ 60 Hz (бо render miss).

**C6. Інтеграція render_weather**
Якщо C5 OK — підключити справжній render_weather.

**DoD C:** weather render видно на панелі, FPS ≥ 30 Hz median на clear-day.

### Phase D: LVGL widget integration (medium risk)

**D1. Перевірити чи LVGL ще потрібен**

LVGL використовується для:
- `weather_screen_init(lv_screen_active())` — тільки створює `lv_canvas` для render.
  Уже не потрібен — рендеримо напряму.
- Test mode panel — sliders, labels, dropdowns у main.c (test_apply_scene, test sliders).
  Це debugging feature. Опції: (a) переробити overlays на native canvas, (b) викинути test mode.
- IP overlay, WiFi status — small labels у main.c.
  Можна намалювати як native canvas overlay.
- FPS chip — small label.
  Native canvas overlay.

**D2. Опція A — викинути LVGL повністю**

Найпростіше:
- Прибрати `bsp_display_start_with_config` LVGL call.
- Прибрати `lv_timer_create` для canvas_tick (use FreeRTOS task на iether core).
- Прибрати всі `lv_*` widget calls у main.c.
- Native overlays через `blend_px` напряму у `s_render_buf`.
- Test mode переробити на CDC-only (через `test on/off` commands, без graphical sliders).

**D3. Опція B — LVGL для тільки widgets, не для canvas**

- `bsp_display_start_with_config` залишається, але `buff_dma=true, buff_spiram=false`.
- LVGL рендерить у власний `lv_disp_draw_buf` 48KB internal SRAM як partial.
- LVGL flush_cb тепер пише у **наш intermediate widget buffer** (800×480 PSRAM, окремий від render_buf).
- Після наш render_weather написав `s_render_buf` — композитим widget buffer поверх через alpha blend.
- PPA SRM rotation працює на composite.

Це **складно**. Рекомендую опцію A.

**DoD D:** test mode працює через CDC, weather render без LVGL flush.

### Phase E: Cleanup і optimize

**E1. Прибрати bg memcpy 750KB**
Тепер коли pipeline зрозумілий, оптимізувати bg cache. Замість memcpy `s_bg_buf → s_render_buf`, тримати **два render buffers**: один is bg+overlays-composited і scrollable, інший — fresh write target. На bg cache hit просто swap pointer'и.

**E2. Cloud cache (повернути композит)**
Тепер коли всі rotation на PPA — bg memcpy не блокує. Можна спробувати композит cache для cloud passes (2-3 frame reuse). Окремий етап після base pipeline стабільний.

**E3. Async cloud bake**
`bake_strip_for_layer` все ще блокує render task на morph spike. Виносити в окремий FreeRTOS task на iether core. Spike 30-80ms → 0 (background).

**E4. Подальші опції**
- Half-res render 400×240 + PPA upscale до 800×480 → rotate → DPI fb. Pixelated 2× але масштабований pipeline.
- LCD pixel format перевести на RGB888 якщо потрібно більше точності кольору (дорожче PSRAM).

---

## 4. Risk matrix і rollback ladder

| Phase | Risk | Rollback |
|---|---|---|
| A (preparation) | None | N/A |
| B (DPI direct access) | Medium — BSP API may not expose everything | `rsync backups/pre-native-pipeline/` |
| C (PPA rotation) | High — PPA + DPI swap timing complex | Rollback to B (LVGL still works), iterate |
| D (LVGL removal) | Medium — test mode features lost | Rollback to C, скоротити scope test mode |
| E (optimize) | Low — incremental | Per-step rollback |

---

## 5. Метрики DoD

| Етап | Цільовий tick (clear-day) | Цільовий lvgl | Notes |
|---|---|---|---|
| Baseline (now) | 13-15 Hz | 24 ms | sw_rotate, single layer |
| After C | ≥30 Hz | 0 ms (no LVGL flush) | PPA rotation на DMA |
| After D | ≥30 Hz | 0 ms | clean pipeline |
| After E2 | ≥40 Hz | 0 ms | cloud cache + async bake |

**Acceptance criteria:**
- Median tick ≥ 30 Hz на всіх сценах (clear-day, partly-cloudy, overcast, rain, storm).
- Max tick ≥ 35 Hz.
- No visible tearing (vsync-locked through double DPI fb).
- All cloud layers (3) і shadow/core passes повернуті.
- Quality ≥ baseline ладшафту з усіма layers.

---

## 6. Часовий ескимет

| Phase | Складність | Час (нарисами) |
|---|---|---|
| A | Низька | 1 година |
| B | Середня | 3-4 години |
| C | Висока | 6-8 годин |
| D | Середня (opt A) | 2-3 години |
| E | Низька-середня | 4-6 годин |
| **Total** | | **16-22 години** |

З urval debugging — ймовірно 25-30 годин чистого часу.

---

## 7. Що зробити перш ніж розпочати

1. **Зробити snapshot** (Phase A1).
2. **Зафіксувати baseline FPS** на всіх сценах (Phase A2). Це наш honest "before" — без цього неможливо порівняти результат.
3. **Прочитати ESP-IDF v5.5.4 docs для:**
   - `esp_lcd_mipi_dsi.h` — DPI panel API, `esp_lcd_dpi_panel_get_frame_buffer`, `esp_lcd_dpi_panel_register_event_callbacks`, `esp_lcd_panel_draw_bitmap`.
   - `driver/ppa.h` — SRM operations, rotation, non-blocking mode, callbacks.
   - Example `external_examples/620bb409/rgb_avoid_tearing/main/lvgl_port_v8.c` — як організовано DPI fb direct access у reference code.
4. **Прийняти рішення про LVGL** (Phase D — option A vs B).
5. **Підготувати robust CDC measurement script** з timeout/reset логікою (CDC re-enum problem каже про `swap_xy=false` BSP edit на line 622).

---

## 8. Контракт «що НЕ робимо»

- Не міняємо layout coordinate system (залишається 800×480 landscape mind у render код).
- Не повертаємось на 400×240 render у Phase C — це fallback на Phase E.
- Не вмикаємо direct_mode + AVOID_TEAR через LVGL — у нас власний vsync control через PPA + DPI callbacks.
- Не лізьмо у DSI timing constants (vp/hp/hsync/vsync) — працює.
- Не редагуємо BSP code (`esp32_p4_function_ev_board.c`) **за винятком** експозиції panel handle (Phase B2) — це або новий public BSP function, або копія `bsp_display_new_with_handles` логіки у main.c.

---

## 9. Корисні референси

- ESP-IDF reference example: `phase7_eva_weather/common_components/espressif__esp_lcd_st7701/external_examples/620bb409/rgb_avoid_tearing/main/lvgl_port_v8.c` — рядки 600-650 показують `lvgl_get_lcd_frame_buffer = esp_lcd_dpi_panel_get_frame_buffer` pattern.
- PPA API: `~/.espressif/v5.5.4/esp-idf/components/esp_driver_ppa/include/driver/ppa.h`
- DPI panel: `~/.espressif/v5.5.4/esp-idf/components/esp_lcd/include/esp_lcd_mipi_dsi.h`
- Поточні координати render (для довідки коли refactor подалі):
  - Sun: `eva_weather_canvas.c:1816-1819`
  - Cloud strips: `eva_weather_canvas.c:191-206`
  - Text positions: `eva_weather_canvas.c:879-895`
- Поточний bg cache: `eva_weather_canvas.c:2956-2974`
- Поточна PPA blend: `eva_weather_canvas.c:2300-2362`

---

## 10. Як читати агенту, якому передається цей план

1. **Прочитати Sections 0-2 повністю** — це повний контекст.
2. Виконати **тільки Phase A** першим заходом. Звітувати baseline numbers.
3. Після підтвердження → Phase B + C smoke test (B4, C5). Не виконувати C6/D без підтвердження.
4. Після C smoke + рішення про LVGL — виконати C6 і D.
5. **Snapshot перед кожною phase**.
6. Логи зберігати у `phase7_eva_weather/docs/native_pipeline_phaseN.log`.
7. **НЕ змінювати layout magic numbers**. Це окремий refactor, не цей.
8. Якщо PPA rotation не дає expected results — порадитись перед лізти у глибше debugging.
