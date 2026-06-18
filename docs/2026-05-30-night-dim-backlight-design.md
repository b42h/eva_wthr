# Нічне приглушення підсвітки (01:00–06:00 → 10%) — дизайн

> Дата: 2026-05-30
> Файли: `main/main.c`, `main/eva_clock.c` (+ `eva_clock` struct)
> Тип: нова фіча (керування яскравістю backlight за часом).

## Ціль

З **01:00 до 06:00** (включно початок, виключно кінець — `[01:00, 06:00)`)
яскравість екрана **10%**; решту доби **100%**. Переходи на межах —
**плавний fade** (~0.5 с).

## Механізм (що вже є)

BSP `esp32_p4_function_ev_board` має готовий PWM-контроль підсвітки:
- `bsp_display_brightness_init(void)` — ініціалізує LEDC PWM на пін backlight.
- `bsp_display_brightness_set(int percent)` — виставляє яскравість 0..100 %.

**Поточний стан коду:** `main.c` кличе лише `bsp_display_backlight_on()`
(= повна яскравість) і **НЕ** кличе `bsp_display_brightness_init()`. Тому
`bsp_display_brightness_set()` зараз не спрацював би (PWM не ініціалізований).

## Зміни

### 1. `main/main.c` — ініціалізація яскравості

Біля існуючого backlight-init (рядок ~2161, після
`esp_lcd_panel_disp_on_off(..., true)`):

```c
ESP_ERROR_CHECK(bsp_display_brightness_init());   /* NEW: enable PWM control */
ESP_ERROR_CHECK(bsp_display_backlight_on());       /* існуюче: вмикає backlight */
```

`bsp_display_backlight_on()` лишається — він вмикає підсвітку на 100 %
після ініціалізації PWM. Старт завжди зі 100 %.

### 2. `main/eva_clock.c` — розклад приглушення

`eva_clock_tick()` кличеться з clock-task кожні ~987 мс і вже обчислює
`tm_now.tm_hour` коректно (TZ + test hour-offset + uptime-fallback). Це
єдина правильна точка для розкладу — не плодимо окремий таймер.

**Нове поле** в `struct eva_clock_s`:
```c
int cur_brightness;   /* останнє виставлене значення; -1 = ще не виставляли */
```
Ініціалізується в `eva_clock_create` через `calloc` у 0 → виставимо явно
-1 одразу після alloc, щоб перший tick гарантовано застосував ціль.

**У кінці `eva_clock_tick()`** (після обчислення `tm_now.tm_hour`):
```c
int target = (tm_now.tm_hour >= 1 && tm_now.tm_hour < 6) ? 10 : 100;
if (target != self->cur_brightness) {
    fade_brightness(self->cur_brightness, target);
    self->cur_brightness = target;
}
```

**`fade_brightness(from, to)`** — статична функція у `eva_clock.c`:
```c
#define BRIGHT_FADE_STEPS 16
#define BRIGHT_FADE_STEP_MS 30
static void fade_brightness(int from, int to)
{
    if (from < 0) {                 /* перший виклик: без fade, одразу */
        bsp_display_brightness_set(to);
        return;
    }
    for (int i = 1; i <= BRIGHT_FADE_STEPS; ++i) {
        int v = from + (to - from) * i / BRIGHT_FADE_STEPS;
        bsp_display_brightness_set(v);
        vTaskDelay(pdMS_TO_TICKS(BRIGHT_FADE_STEP_MS));
    }
}
```
~16 × 30 мс ≈ 0.5 с плавно. Виконується в clock-task (prio 2) — блокує
лише сам годинник на час fade (раз на добу), нічого критичного.

`eva_clock.c` отримує доступ до BSP через `#include "bsp/display.h"`.

## Граничні випадки

- **Test hour-offset** (`clockoffset` CDC): `tm_now` уже враховує offset,
  тож приглушення слідує за тестовим часом — зручно для перевірки.
- **Час не синхронізовано** (uptime-fallback у `eva_clock_tick`): година
  з uptime; розклад усе одно працює (не за реальним часом). Прийнятно.
- **PWM init fail**: `ESP_ERROR_CHECK` на `bsp_display_brightness_init`
  заабортить на boot, що видно одразу. (Якщо плата не підтримує — це
  виявиться на першому ж прошитті.) `bsp_display_brightness_set` помилки
  ігноруємо в fade (екран лишиться як був).
- **Старт у вікні 01–06**: перший tick (cur=-1) одразу виставить 10 % без
  fade — коректно.

## Тест (залізо + CDC)

1. Прошити. На boot екран має бути 100 %.
2. `clockoffset` зсунути годинник у вікно (напр. поточна 13:xx → offset,
   щоб вийшло ~02:00) → екран плавно приглушується до 10 %.
3. Зсунути назад поза вікно (напр. ~07:00) → плавно повертається 100 %.
4. Переконатись на залізі, що 10 % не гасить екран повністю (видно
   зображення). Якщо надто темно — підняти ціль (15–20 %), окремою правкою.

## Поза обсягом

- Окремий CDC-команд для ручного керування яскравістю — не робимо (не
  просили).
- Конфігурованість годин/відсотка через NVS — не робимо (YAGNI; значення
  захардкоджені 01/06/10).

---

## Фінальні нотатки реалізації (відвантажено 2026-05-30)

**Статус: зроблено, перевірено на залізі.** Повний цикл день↔ніч (вхід,
вихід, повторний вхід) — плавний fade у правильному напрямку, uptime росте
монотонно (без ребутів).

### Знайдений і виправлений баг: гонка LEDC (краш на dim→day)

Перша версія тримала розклад **усередині `eva_clock_tick()`**. Але
`eva_clock_tick` викликається з **двох контекстів**: таску годинника
(періодично) **і** CDC-шляху `eva_clock_set_hour_offset` (команда
`clockoffset`). Тобто `clockoffset` запускав fade у CDC-таску, а таск
годинника міг паралельно запустити свій fade → **дві одночасні fade-петлі**
смикали неперевходимий LEDC-драйвер + спільне поле `cur_brightness`. У логах
це було видно як другий fade, що стартує одразу після першого в зворотному
напрямку (`...100% → 15%`), і плата **ребутилась на переході dim→day**.

**Фікс:** розклад винесено в окрему `apply_brightness_schedule()`, яка
викликається **лише з таску годинника** (`clock_task`). `eva_clock_tick`
більше не чіпає яскравість; `set_hour_offset` лише оновлює час і миттєво
повертає. Гонки більше немає.

Урок (записано і в CLAUDE.md §6): **не викликати fade/brightness-set із
кількох контекстів** — LEDC не reentrant, fade блокуючий.

### Перевірка на залізі (CDC `clockoffset` + serial-лог)

- Вхід у вікно (offset → ~02:00, ~04:00): плавний `95→…→10%`.
- Вихід (offset → день): плавний `15→…→100%` (раніше тут краш — тепер ОК).
- Реальний час (день): стабільні 100 %, без зайвих fade.
- uptime 18→24→30→36 с упродовж циклу — ребутів немає.
