#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sdkconfig.h>

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* =========================================================================
 * Compile-time constants
 * ====================================================================== */

/* The 1.3" module (XFP1116-07AY) is a fixed 128x64 panel. */
#define ESP_SH1106_DRIVER_WIDTH 128u
#define ESP_SH1106_DRIVER_HEIGHT 64u

/* The SH1106 display RAM is 132 columns wide; the 128-pixel panel is centred on it. */
#define ESP_SH1106_DRIVER_RAM_WIDTH 132

#define ESP_SH1106_DRIVER_TOTAL_PAGES (ESP_SH1106_DRIVER_HEIGHT / 8)
#define ESP_SH1106_DRIVER_BUF_SIZE (ESP_SH1106_DRIVER_WIDTH * ESP_SH1106_DRIVER_TOTAL_PAGES)
#define ESP_SH1106_DRIVER_PRINTF_BUF_SIZE (((ESP_SH1106_DRIVER_WIDTH / 8u) * 2u) + 1u)

/* =========================================================================
 * Handle
 * ====================================================================== */

/**
 * @brief Runtime handle for the I2C SH1106 display.
 *
 * Initialise with i2c_sh1106_init(). Do not write fields directly.
 *
 * Thread safety: the mutex serialises I2C transfers only. Buffer-write
 * functions are unprotected RAM operations. Wrap draw-then-flush sequences
 * in an application-level mutex when shared across tasks.
 */
typedef struct
{
    i2c_master_dev_handle_t i2c_master_dev;
    SemaphoreHandle_t mutex;
    _Atomic uint8_t dirty_pages;
    uint8_t buffer[ESP_SH1106_DRIVER_BUF_SIZE];
} i2c_sh1106_handle_t;

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

/**
 * @brief Initialise the SH1106 display.
 *
 * All parameters are taken from Kconfig. On success the display is on,
 * the pixel buffer is zeroed, and the dirty-page mask is cleared.
 * Cleans up all allocated resources on failure.
 *
 * @param bus    Initialised I2C master bus handle.
 * @param handle Pointer to an uninitialised handle to be filled in.
 * @return ESP_OK, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT,
 *         or an ESP-IDF I2C driver error.
 */
esp_err_t i2c_sh1106_init(i2c_master_bus_handle_t bus, i2c_sh1106_handle_t *handle);

/**
 * @brief Deinitialise the SH1106 display.
 *
 * Removes the I2C device from the bus and deletes the mutex.
 * The handle must not be used after this call.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an ESP-IDF driver error code.
 */
esp_err_t i2c_sh1106_deinit(i2c_sh1106_handle_t *handle);

/* =========================================================================
 * Power management
 * ====================================================================== */

/**
 * @brief Put the display to sleep (lowest power, a few uA).
 *
 * Sends Display OFF and switches the internal DC-DC converter off. The
 * display RAM keeps its content, so nothing needs to be re-sent on wake-up.
 * While asleep the buffer functions still work; only I2C traffic matters.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_sh1106_display_off(i2c_sh1106_handle_t *handle);

/**
 * @brief Wake the display from sleep.
 *
 * Turns the DC-DC converter on, waits 100 ms for the panel voltage to
 * stabilise (as required by the datasheet) and sends Display ON. To avoid
 * flashing stale content, update the buffer and flush it BEFORE calling this.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_sh1106_display_on(i2c_sh1106_handle_t *handle);

/* =========================================================================
 * Convenience
 * ====================================================================== */

/**
 * @brief Clear the display (all pixels off) and flush to hardware.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_sh1106_clear(i2c_sh1106_handle_t *handle);

/* =========================================================================
 * Buffer manipulation — no I2C traffic; call a _to_ram function to flush.
 * ====================================================================== */

/**
 * @brief Fill or clear the entire pixel buffer. Marks all pages dirty.
 *
 * @param handle Pointer to the handle.
 * @param fill   true → all pixels on; false → all pixels off.
 * @return ESP_OK.
 */
esp_err_t i2c_sh1106_buffer_fill(i2c_sh1106_handle_t *handle, bool fill);

/**
 * @brief Set or clear a single pixel in the buffer.
 *
 * @param handle Pointer to the handle.
 * @param x      Column (0 … WIDTH-1).
 * @param y      Row    (0 … HEIGHT-1).
 * @param fill   true → pixel on; false → pixel off.
 * @return ESP_OK or ESP_ERR_INVALID_ARG if (x, y) is out of range.
 */
esp_err_t i2c_sh1106_buffer_fill_pixel(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, bool fill);

/**
 * @brief Fill or clear a rectangular region of the buffer.
 *
 * @param handle Pointer to the handle.
 * @param x1     Left edge   (≤ x2, 0 … WIDTH-1).
 * @param x2     Right edge  (0 … WIDTH-1).
 * @param y1     Top edge    (≤ y2, 0 … HEIGHT-1).
 * @param y2     Bottom edge (0 … HEIGHT-1).
 * @param fill   true → on; false → off.
 * @return ESP_OK or ESP_ERR_INVALID_ARG if coordinates are out of range.
 */
esp_err_t i2c_sh1106_buffer_fill_space(i2c_sh1106_handle_t *handle, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2, bool fill);

/**
 * @brief Render text into the buffer using OR mode (preserves existing pixels).
 *
 * Use the _overwrite variant when updating dynamic content to avoid stale pixels.
 *
 * @param handle Pointer to the handle.
 * @param x      Starting column.
 * @param y      Starting row.
 * @param text   Null-terminated string.
 * @param invert Invert character colours if true.
 * @return ESP_OK or ESP_ERR_INVALID_ARG if text is NULL/empty or out of range.
 */
esp_err_t i2c_sh1106_buffer_text(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert);

/**
 * @brief Clear the text bounding box, then render text into the buffer.
 *
 * Use for dynamic text that can change width across calls (e.g. sensor values).
 *
 * @param handle Pointer to the handle.
 * @param x      Starting column.
 * @param y      Starting row.
 * @param text   Null-terminated string.
 * @param invert Invert character colours if true.
 * @return ESP_OK or ESP_ERR_INVALID_ARG if text is NULL/empty or out of range.
 */
esp_err_t i2c_sh1106_buffer_text_overwrite(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert);

/**
 * @brief Format a signed integer and render it with i2c_sh1106_buffer_text().
 */
esp_err_t i2c_sh1106_buffer_int(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert);

/**
 * @brief Format a signed integer and render it with i2c_sh1106_buffer_text_overwrite().
 *
 * Prefer over i2c_sh1106_buffer_int() when the value can change digit count.
 */
esp_err_t i2c_sh1106_buffer_int_overwrite(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert);

#if CONFIG_ESP_SH1106_DRIVER_ENABLE_FLOAT
/**
 * @brief Format a float with @p decimals places (clamped to 6) and render it.
 *
 * @warning Pulls in C library float-printf (~3–5 KB flash). Disable in Kconfig if unused.
 */
esp_err_t i2c_sh1106_buffer_float(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert);

/**
 * @brief Format a float and render it with i2c_sh1106_buffer_text_overwrite().
 *
 * @warning Pulls in C library float-printf (~3–5 KB flash). Disable in Kconfig if unused.
 */
esp_err_t i2c_sh1106_buffer_float_overwrite(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert);
#endif /* CONFIG_ESP_SH1106_DRIVER_ENABLE_FLOAT */

/**
 * @brief Expand a printf-style format string and render it.
 *
 * Output is truncated to ESP_SH1106_DRIVER_PRINTF_BUF_SIZE bytes.
 * GCC/Clang type-check the format string at compile time.
 */
esp_err_t i2c_sh1106_buffer_printf(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/**
 * @brief Expand a printf-style format string and render it with overwrite.
 *
 * Prefer over i2c_sh1106_buffer_printf() when output width can change:
 *   i2c_sh1106_buffer_printf_overwrite(&h, 0, 16, false, "Counter: %d", i);
 */
esp_err_t i2c_sh1106_buffer_printf_overwrite(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/**
 * @brief Render a bitmap stored in page-major, column-minor order.
 *
 * Pixels outside the display boundary are clipped.
 *
 * @param handle     Pointer to the handle.
 * @param x          Starting column.
 * @param y          Starting row.
 * @param image      Pointer to the image data (page-major, col-minor).
 * @param img_width  Image width in pixels.
 * @param img_height Image height in pixels (should be a multiple of 8).
 * @param invert     Invert colours if true.
 * @return ESP_OK or ESP_ERR_INVALID_ARG if image is NULL or coordinates are invalid.
 */
esp_err_t i2c_sh1106_buffer_image(i2c_sh1106_handle_t *handle, uint8_t x, uint8_t y, const uint8_t *image, uint8_t img_width, uint8_t img_height, bool invert);

/* =========================================================================
 * Buffer → display RAM transfer
 *
 * Each page transfer is a single atomic, mutex-protected I2C sequence.
 * The dirty bit is cleared atomically before each transfer and restored
 * on failure, so no concurrent write is lost.
 * ====================================================================== */

/**
 * @brief Flush only dirty pages to display RAM. Recommended for most use cases.
 */
esp_err_t i2c_sh1106_buffer_to_ram(i2c_sh1106_handle_t *handle);

/**
 * @brief Flush only the pages overlapping pixel rows [y1 … y2].
 *
 * For a single 8-pixel text row this sends ~128 bytes instead of 1024.
 *
 * @param handle Pointer to the handle.
 * @param y1     Top pixel row    (0 … HEIGHT-1).
 * @param y2     Bottom pixel row (≥ y1).
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_sh1106_region_to_ram(i2c_sh1106_handle_t *handle, uint8_t y1, uint8_t y2);

/**
 * @brief Flush one full page to display RAM.
 *
 * @param handle Pointer to the handle.
 * @param page   Page number (0 … TOTAL_PAGES-1).
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_sh1106_page_to_ram(i2c_sh1106_handle_t *handle, uint8_t page);

/**
 * @brief Flush pages [initial_page … final_page] to display RAM.
 *        Returns immediately on the first error.
 */
esp_err_t i2c_sh1106_pages_to_ram(i2c_sh1106_handle_t *handle, uint8_t initial_page, uint8_t final_page);

/**
 * @brief Flush columns [initial_segment … final_segment] of @p page in one I2C transaction.
 */
esp_err_t i2c_sh1106_segments_to_ram(i2c_sh1106_handle_t *handle, uint8_t page, uint8_t initial_segment, uint8_t final_segment);

/**
 * @brief Flush a single byte at (page, segment) to display RAM.
 */
esp_err_t i2c_sh1106_segment_to_ram(i2c_sh1106_handle_t *handle, uint8_t page, uint8_t segment);