# ESP SH1106 Driver

| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- |

![Version](https://img.shields.io/badge/version-1.1.0-orange)

A lightweight and efficient SH1106 OLED display driver written entirely in C for the ESP-IDF framework, made for the **1.3-inch 128×64 I2C module (XFP1116-07AY)**. Same API and design as `esp_ssd1306_driver` (dirty-page tracking, partial flush, thread-safe I2C), adapted to the SH1106 controller.

Supports **ESP-IDF v6.0** (and v5.1+ in practice).


---

## SH1106 vs SSD1306 (what changed from the SSD1306 driver)

| Topic | SSD1306 | SH1106 (this driver) |
|-------|---------|----------------------|
| Display RAM width | 128 columns | **132 columns** → driver adds a column offset (default **2**) |
| Memory addressing mode (`0x20`) | Page / horizontal / vertical | **Page mode only** — command not sent |
| Charge pump | `0x8D 0x14` | **DC-DC `0xAD 0x8B`** |
| Height options | 64 or 32 px | Fixed **128×64** |
| Display RAM at power-up | Usually blank | **Undefined** → driver clears RAM *before* turning the display on |

The public API is identical, only the prefix changes: `i2c_ssd1306_*` → `i2c_sh1106_*`.

### I2C address (0x78 / 0x7A)

The module says *Address select 0x78 / 0x7A*. Those are **8-bit** addresses (7-bit address shifted left with the R/W bit):

| Silk / datasheet | 7-bit address for ESP-IDF |
|------------------|---------------------------|
| `0x78` | **`0x3C`** (default) |
| `0x7A` | **`0x3D`** |

Which one you have depends on the address-select resistor/jumper on the module. If init reports *No device at I2C address*, try the other one.

---

## Requirements

- **ESP-IDF**: v5.1+ (v6.0 recommended)

---

## How It Works

All display geometry and I2C parameters are resolved at **compile time** from Kconfig constants. The handle only carries runtime state: the I2C device handle, a FreeRTOS mutex, a dirty-page bitmask, and the pixel buffer.

```
── DRAW ────────────────────────────────────────────────────────
 
buffer_fill / buffer_text / buffer_printf / ...
    │
    │  writes pixels into RAM buffer
    │  marks affected pages dirty  (_Atomic bitmask)
    │  does NOT touch I2C bus
 
── FLUSH ───────────────────────────────────────────────────────
 
buffer_to_ram / region_to_ram / page_to_ram
    │
    ├─ reads dirty bitmask
    ├─ clears bit atomically (concurrent writes re-set it)
    ├─ sends address command + page data over I2C (mutex-protected)
    └─ restores bit on I2C failure → retried on next flush
```

**Key design decisions:**

- Geometry and I2C parameters are Kconfig constants — no runtime config struct, no RAM wasted on static configuration.
- `_Atomic dirty_pages` bitmask lets any task or ISR mark pages without a mutex, while flush functions handle per-page TOCTOU correctly.
- The driver mutex serialises I2C transactions only. If multiple tasks draw and flush concurrently, protect the full draw-then-flush sequence with an application-level mutex.
- `printf` buffer size is derived from display width at compile time — `(WIDTH / 8) * 2 + 1` — no manual configuration needed.
- Only `buffer_float` is behind a Kconfig guard, because it is the only function that may pull in C library float-printf support (~3–5 KB flash) if not already present. All other functions (`buffer_int`, `buffer_printf`, `buffer_image`) are always compiled since their dependencies are already present via `ESP_LOG`. On ESP-IDF v6.0 the default C library is Picolibc; on v5.x it is Newlib.

---

## Installation

### Step 1: Add Component

Copy this folder into your project's `components/` directory:

```
your_project/
└── components/
    └── esp_sh1106_driver/
```

Then `idf.py build`. (If you publish it to your own Git repo, use `idf.py add-dependency --git <repo-url> ...` as with the SSD1306 driver.)

### Step 2: Hardware Setup

By default the driver expects **GPIO 22** for SCL and **GPIO 21** for SDA. Update the I2C master bus config in your `app_main.c` if your wiring differs.

> Both SCL and SDA require pull-up resistors for stable I2C communication. Most OLED modules include them. If yours does not, add external 4.7 kΩ resistors or enable internal pull-ups in the bus config.

### Step 3: Kconfig Configuration

```bash
idf.py menuconfig
```

Navigate to: **Component config → ESP SH1106 Driver**

| Option | Default | Description |
|--------|---------|-------------|
| I2C device address | 0x3C | 7-bit address: 0x3C (= 0x78) or 0x3D (= 0x7A) |
| I2C SCL clock speed (Hz) | 400000 | 100000–400000 |
| I2C transaction timeout (ms) | 1000 | 10–1000 — increase only on loaded buses |
| Column offset | 2 | 0–4. Use 2 for most 1.3" modules; try 0 or 4 if the picture is shifted |
| Flip display 180° | No | Useful when module is mounted inverted |
| Enable `buffer_float` | No | Float rendering — may add ~3–5 KB flash |
| Log level | Info | Component verbosity, independent of global log level |

> Geometry is fixed at 128×64 (`ESP_SH1106_DRIVER_WIDTH` / `_HEIGHT`), so there are no width/height options.

---

## Quick Start

```c
#include <driver/i2c_master.h>
#include <esp_sh1106_driver.h>
 
static const i2c_master_bus_config_t i2c_bus_cfg = {
    .i2c_port                = I2C_NUM_0,
    .scl_io_num              = GPIO_NUM_22,
    .sda_io_num              = GPIO_NUM_21,
    .clk_source              = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt       = 7,
    .flags.enable_internal_pullup = true,
};
static i2c_master_bus_handle_t i2c_bus;
static i2c_sh1106_handle_t    sh1106;
 
void app_main(void)
{
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
    ESP_ERROR_CHECK(i2c_sh1106_init(i2c_bus, &sh1106));
 
    // Full screen update
    i2c_sh1106_buffer_text(&sh1106, 0, 0, "Hello, World!", false);
    i2c_sh1106_buffer_to_ram(&sh1106);
 
    // Partial update — only the pages covering y=16..23 are sent (~128 bytes)
    i2c_sh1106_buffer_printf_overwrite(&sh1106, 0, 16, false, "Counter: %d", 42);
    i2c_sh1106_region_to_ram(&sh1106, 16, 23);
}
```

---

## Thread Safety Model

The internal mutex serialises **I2C transfers only**. Buffer-write functions (`buffer_fill`, `buffer_text`, …) are fast RAM operations and are **not** individually mutex-protected.

If multiple FreeRTOS tasks draw and flush concurrently, protect the full draw-then-flush sequence with an **application-level mutex**:

```c
xSemaphoreTake(oled_mutex, portMAX_DELAY);
i2c_sh1106_buffer_text_overwrite(&sh1106, 0, 0, "Task A", false);
i2c_sh1106_region_to_ram(&sh1106, 0, 7);
xSemaphoreGive(oled_mutex);
```

---

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `i2c_sh1106_init(bus, handle)` | Initialize display — probes I2C, sends init sequence, zeroes buffer |
| `i2c_sh1106_deinit(handle)` | Remove I2C device and delete mutex |

### Power management

| Function | Description |
|----------|-------------|
| `i2c_sh1106_display_off(handle)` | Sleep: Display OFF + DC-DC off (a few µA). Display RAM is retained |
| `i2c_sh1106_display_on(handle)` | Wake: DC-DC on, 100 ms settle, Display ON. Flush new data **before** calling it to avoid showing stale content |

Typical use — turn the screen off after inactivity and wake it with a button:

```c
i2c_sh1106_display_off(&oled);          // after 30 s without activity
...
i2c_sh1106_buffer_to_ram(&oled);        // refresh content while still off
i2c_sh1106_display_on(&oled);           // then wake it
```

### Convenience

| Function | Description |
|----------|-------------|
| `i2c_sh1106_clear(handle)` | All pixels off and flush to hardware — equivalent to `buffer_fill(false)` + `buffer_to_ram()` |

---

### Buffer Manipulation

These functions modify the pixel buffer in RAM and mark affected pages dirty. **They do not touch the I2C bus.** Call a flush function afterwards.

#### Fill

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_fill(handle, fill)` | All pixels on (`true`) or off (`false`). Marks all pages dirty |
| `i2c_sh1106_buffer_fill_pixel(handle, x, y, fill)` | Set or clear a single pixel |
| `i2c_sh1106_buffer_fill_space(handle, x1, x2, y1, y2, fill)` | Fill or clear a rectangle |

#### Text

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_text(handle, x, y, text, invert)` | Render string using OR mode — preserves existing pixels |
| `i2c_sh1106_buffer_text_overwrite(handle, x, y, text, invert)` | Clear bounding box, then render — use for dynamic text |

**OR mode vs overwrite:** `buffer_text` uses `|=` and never clears pixels. `buffer_text_overwrite` erases the rectangle occupied by the string before drawing, preventing leftover pixels when the content changes width (e.g. `"9"` → `"10"`).

#### Integer

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_int(handle, x, y, value, invert)` | Format int as decimal, call `buffer_text` |
| `i2c_sh1106_buffer_int_overwrite(handle, x, y, value, invert)` | Format int as decimal, call `buffer_text_overwrite` |

#### Float

> Compiled only when **Enable `buffer_float`** is set in Kconfig. Enabling it may pull in C library float-printf support (~3–5 KB flash) if not already present (Picolibc on ESP-IDF v6.0, Newlib on v5.x).

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_float(handle, x, y, value, decimals, invert)` | Format float, call `buffer_text`. `decimals` clamped to 6 |
| `i2c_sh1106_buffer_float_overwrite(handle, x, y, value, decimals, invert)` | Format float, call `buffer_text_overwrite`. `decimals` clamped to 6 |

#### Printf

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_printf(handle, x, y, invert, fmt, ...)` | Expand format string, call `buffer_text` |
| `i2c_sh1106_buffer_printf_overwrite(handle, x, y, invert, fmt, ...)` | Expand format string, call `buffer_text_overwrite` |

Both functions expand into a stack buffer of `ESP_SH1106_DRIVER_PRINTF_BUF_SIZE` bytes, derived automatically from the display width (`(WIDTH / 8) * 2 + 1`). Output exceeding the buffer is silently truncated.

The `format` GCC attribute enables compile-time type checking of format strings.

**Choosing between `buffer_printf` and `buffer_printf_overwrite`:**

```c
// Bug-prone — leftover pixels from wider previous value remain:
i2c_sh1106_buffer_text_overwrite(&ssd, 0, 16, "Counter: ", false);
i2c_sh1106_buffer_printf(&ssd, 72, 16, false, "%d", i);
 
// Correct — clears the full formatted string as a single region:
i2c_sh1106_buffer_printf_overwrite(&ssd, 0, 16, false, "Counter: %d", i);
```

#### Image

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_image(handle, x, y, image, img_width, img_height, invert)` | Render page-major bitmap. Pixels outside display bounds are clipped |

---

### Flush — Buffer → Display RAM

These functions push bytes from the RAM buffer to the SH1106 over I2C. Each page transfer is an **atomic, mutex-protected sequence** (address command + data) — interleaving from concurrent tasks is impossible.

The dirty bit for each page is cleared atomically **before** reading the buffer. If another task modifies the page during the I2C transfer, that task's `mark_dirty` re-sets the bit, and the page is retried on the next flush.

| Function | Description |
|----------|-------------|
| `i2c_sh1106_buffer_to_ram(handle)` | Flush only dirty pages — **recommended for most use cases** |
| `i2c_sh1106_region_to_ram(handle, y1, y2)` | Flush only pages overlapping pixel rows `[y1…y2]` |
| `i2c_sh1106_page_to_ram(handle, page)` | Flush one full page |
| `i2c_sh1106_pages_to_ram(handle, initial_page, final_page)` | Flush a contiguous range of pages |
| `i2c_sh1106_segment_to_ram(handle, page, segment)` | Flush a single byte |
| `i2c_sh1106_segments_to_ram(handle, page, initial_segment, final_segment)` | Flush a column range within a page |

**Partial update pattern — most efficient for dynamic content:**

```c
// Sends ~128 bytes instead of 1024 — no flicker on unchanged lines
i2c_sh1106_buffer_printf_overwrite(&ssd, 0, 16, false, "Temp: %d C", temp);
i2c_sh1106_region_to_ram(&ssd, 16, 23);
```

---

## Compile-time Constants

Defined in `esp_sh1106_driver.h`. Derived from Kconfig — do not set manually.

| Constant | Formula | Description |
|----------|---------|-------------|
| `ESP_SH1106_DRIVER_TOTAL_PAGES` | `HEIGHT / 8` | Number of pages |
| `ESP_SH1106_DRIVER_BUF_SIZE` | `WIDTH * TOTAL_PAGES` | Pixel buffer size in bytes |
| `ESP_SH1106_DRIVER_PRINTF_BUF_SIZE` | `(WIDTH / 8) * 2 + 1` | Stack buffer for `buffer_printf` |

---

## Full Example

```c
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_sh1106_driver.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
 
static const i2c_master_bus_config_t i2c_bus_cfg = {
    .i2c_port                = I2C_NUM_0,
    .scl_io_num              = GPIO_NUM_22,
    .sda_io_num              = GPIO_NUM_21,
    .clk_source              = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt       = 7,
    .flags.enable_internal_pullup = true,
};
static i2c_master_bus_handle_t i2c_bus;
static i2c_sh1106_handle_t    sh1106;
 
void app_main(void)
{
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus));
    ESP_ERROR_CHECK(i2c_sh1106_init(i2c_bus, &sh1106));
 
    /* ── Full frame ───────────────────────────────────────────── */
    i2c_sh1106_buffer_fill(&sh1106, false);
    i2c_sh1106_buffer_text(&sh1106,  0,  0, "Hello, World!", false);
    i2c_sh1106_buffer_text(&sh1106,  0,  8, "ABCDEFGHIJKLMNOP", false);
    i2c_sh1106_buffer_text(&sh1106,  0, 16, "0123456789!@#$%^", false);
    i2c_sh1106_buffer_fill_space(&sh1106, 0, 127, 24, 24, true);  // horizontal line
    i2c_sh1106_buffer_to_ram(&sh1106);
    vTaskDelay(pdMS_TO_TICKS(3000));
 
    /* ── Partial update counter ───────────────────────────────── */
    for (int i = 0; i < 100; i++)
    {
        i2c_sh1106_buffer_printf_overwrite(&sh1106, 0, 32, false, "Count: %d", i);
        i2c_sh1106_region_to_ram(&sh1106, 32, 39);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
 
    /* ── Integer rendering ────────────────────────────────────── */
    i2c_sh1106_buffer_int_overwrite(&sh1106, 0, 48, 1234567890, false);
    i2c_sh1106_region_to_ram(&sh1106, 48, 55);
    vTaskDelay(pdMS_TO_TICKS(2000));
 
    /* ── Float rendering (requires Enable buffer_float in Kconfig) */
#if CONFIG_ESP_SH1106_DRIVER_ENABLE_FLOAT
    i2c_sh1106_buffer_float_overwrite(&sh1106, 0, 56, 3.14159f, 3, false);
    i2c_sh1106_region_to_ram(&sh1106, 56, 63);
    vTaskDelay(pdMS_TO_TICKS(2000));
#endif
}
```

---

## Convert an Image to a C Array

To render bitmaps using `i2c_sh1106_buffer_image()`, the following Python script converts standard image files (PNG, JPG) into `uint8_t` C arrays in the SH1106 page-major format.

### Requirements

```bash
pip install pillow
```

### Usage

1. Set `image_path` and dimensions in the script.
2. Run: `python image_to_c_array.py`
3. Paste the printed array into your firmware.

### Script

```python
from PIL import Image
 
def printArray(py_array):
    for array in py_array:
        segment = ["" for _ in range(8)]
        for byte in array:
            bin_str = format(byte, "08b")
            for i in range(8):
                segment[7 - i] += "█" if bin_str[i] == "1" else " "
        for line in segment:
            print(line)
 
def formatAsCArray(py_array, c_array_name="img", line_break=16):
    rows = len(py_array)
    cols = len(py_array[0]) if rows > 0 else 0
    c_array = f"uint8_t {c_array_name}[{rows}][{cols}] = {{\n"
    for row in py_array:
        c_array += "    {"
        for i in range(0, len(row), line_break):
            chunk = row[i : i + line_break]
            c_array += ", ".join(f"0x{byte:02X}" for byte in chunk)
            if i + line_break < len(row):
                c_array += ",\n     "
        c_array += "},\n"
    c_array = c_array.rstrip(",\n") + "\n};"
    return c_array
 
def imageToByteArray(image_path, width, height, invert=False, threshold=None):
    image = Image.open(image_path).convert("L")
    image = image.resize((width, height))
 
    if threshold is None:
        histogram = image.histogram()
        total_pixels = sum(histogram)
        sum_brightness = sum(i * histogram[i] for i in range(256))
        sum_b, max_variance, threshold, w_b = 0, 0, 0, 0
        for t in range(256):
            w_b += histogram[t]
            w_f = total_pixels - w_b
            if w_b == 0 or w_f == 0:
                continue
            sum_b += t * histogram[t]
            m_b = sum_b / w_b
            m_f = (sum_brightness - sum_b) / w_f
            variance = w_b * w_f * (m_b - m_f) ** 2
            if variance > max_variance:
                max_variance = variance
                threshold = t
 
    print(f"Threshold: {threshold}")
    image = image.point(lambda p: 255 if p > threshold else 0, mode="1")
    if invert:
        image = image.point(lambda p: 255 if p == 0 else 0, mode="1")
    image.show()
 
    pixels = list(image.getdata())
    rows = [pixels[i * width : (i + 1) * width] for i in range(height)]
    byte_blocks = []
    for group_start in range(0, height, 8):
        group = rows[group_start : group_start + 8]
        block = []
        for col in range(width):
            byte = 0
            for row_idx, row in enumerate(group):
                if row[col] == 0:
                    byte |= 1 << row_idx
            block.append(byte)
        byte_blocks.append(block)
    return byte_blocks
 
# ── Configuration ──────────────────────────────────────────────
image_path = r"C:\Path\to\your\image.png"
width, height = 64, 64
 
byte_array = imageToByteArray(image_path, width, height, invert=True)
c_array    = formatAsCArray(byte_array, c_array_name="my_image")
printArray(byte_array)
print(c_array)
```

---

## Changelog

### 1.1.0

- Added `i2c_sh1106_display_off()` / `i2c_sh1106_display_on()` for power saving.
- `init` now waits 100 ms after enabling the DC-DC converter before Display ON (datasheet requirement).

### 1.0.0

Initial release — port of `esp_ssd1306_driver` 2.0.0 to the SH1106 controller (1.3" 128×64, XFP1116-07AY).

- Page-addressing only; removed `0x20` memory-mode command.
- DC-DC enabled with `0xAD 0x8B` instead of the SSD1306 charge pump.
- 2-column RAM offset (configurable) applied to every flush function.
- Display RAM is cleared before `DISPLAY_ON` to avoid power-up garbage.
- Geometry fixed to 128×64; height/width Kconfig options removed.

---

## Troubleshooting

**Display blank after init**: Verify I2C address in Kconfig (`0x3C` = 0x78 or `0x3D` = 0x7A). Confirm SCL/SDA GPIOs and pull-up resistors are in place. Check I2C SCL speed — try 100 000 Hz if your wiring is long.

**Image shifted by 2 px / garbage column at one edge**: The SH1106 has 132 RAM columns. Change **Column offset** in menuconfig (try 2, then 0 or 4).

**Image upside-down or mirrored**: Toggle **Flip display 180°** in menuconfig.

**Dark / dim display**: Check the module is powered with 3.3 V and that the DC-DC init command (`0xAD 0x8B`) was accepted (no I2C errors in the log).

**Text not erasing on update**: Use `_overwrite` variants (`buffer_text_overwrite`, `buffer_printf_overwrite`) instead of the plain variants. Plain functions use OR mode and never clear pixels.

**Garbage on display with multiple tasks**: Add an application-level mutex around each draw-then-flush sequence. The driver's internal mutex only protects individual I2C transactions.

**`buffer_float` undefined**: Enable **Enable `buffer_float`** in Kconfig (`idf.py menuconfig` → Component config → ESP SH1106 Driver).

**I2C timeout**: Increase **I2C transaction timeout** in menuconfig (Component config → ESP SH1106 Driver) or reduce SCL speed.

**Build error: `esp_driver_i2c` not found**: This component requires ESP-IDF v5.1 or later. The `esp_driver_i2c` component was introduced in v5.1 alongside the new `i2c_master.h` API.

---

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.

**Status**: Beta — tested with a host-side I2C mock; please validate on real hardware.
