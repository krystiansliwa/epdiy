/* Simple firmware for a ESP32 displaying a static image on an EPaper Screen.
 *
 * Write an image into a header file using a 3...2...1...0 format per pixel,
 * for 4 bits color (16 colors - well, greys.) MSB first.  At 80 MHz, screen
 * clears execute in 1.075 seconds and images are drawn in 1.531 seconds.
 */

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "epd_driver.h"
#ifdef CONFIG_EPD_DISPLAY_TYPE_ED060SC4
#include "firasans_12pt.h"
#else
#include "firasans.h"
#include "unicode.h"

#endif

#define ECHO_TEST_TXD  (GPIO_NUM_1)
#define ECHO_TEST_RXD  (GPIO_NUM_3)
#define ECHO_TEST_RTS  (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS  (UART_PIN_NO_CHANGE)

#define BUF_SIZE (1024)

#define COLUMNS 40
#define ROWS 20

int min(int a, int b) {
  return a < b ? a : b;
}

int max(int a, int b) {
  return a > b ? a : b;
}

void delay(uint32_t millis) { vTaskDelay(millis / portTICK_PERIOD_MS); }

uint32_t millis() { return esp_timer_get_time() / 1000; }

int log_to_uart(const char* fmt, va_list args) {
    char buffer[256];
    int result = vsprintf(buffer, fmt, args);
    uart_write_bytes(UART_NUM_1, buffer, strnlen(buffer, 256));
    return result;
}

// inspired by the st - the simple terminal, suckless.org

typedef struct {
  bool dirty: 1;
  uint8_t color: 4;
} CharMeta;

typedef struct {
  CharMeta chars[COLUMNS];
  bool dirty;
} LineMeta;

// A line is a sequence of code points.
typedef uint32_t Line[COLUMNS];

typedef struct {
    int x;
    int y;
} Cursor;

typedef struct {
    /// Number of rows.
    int row;
    /// Number of columns.
    int col;
    Line line[ROWS];

    Line old_line[ROWS];

    LineMeta meta[ROWS];
    Cursor cursor;

    /// the pixel position of the first column
    int pixel_start_x;
    /// the pixel position of the first row
    int pixel_start_y;
} Term;


static uint8_t uart_str_buffer[BUF_SIZE];
static uint8_t* uart_buffer_end = uart_str_buffer;
static uint8_t* uart_buffer_start = uart_str_buffer;
static Term term;

uint32_t read_char() {
    int remaining = uart_buffer_end - uart_buffer_start;
    if (uart_buffer_start >= uart_buffer_end
            || utf8_len(*uart_buffer_start) > remaining) {

        memmove(uart_str_buffer, uart_buffer_start, remaining);
        uart_buffer_start = uart_str_buffer;
        uart_buffer_end = uart_buffer_start + remaining;
        int unfilled = uart_str_buffer + BUF_SIZE - uart_buffer_end;
        int len = uart_read_bytes(UART_NUM_1, uart_buffer_end, unfilled, 20 / portTICK_RATE_MS);
        uart_buffer_end += len;
        if (len < 0) {
            ESP_LOGE("terminal", "uart read error");
            return 0;
        } else if (len == 0) {
            return 0;
        }
        remaining = uart_buffer_end - uart_buffer_start;
    }

    int bytes = utf8_len(*uart_buffer_start);
    if (remaining < bytes) {
      return 0;
    }

    uint32_t cp = to_cp((char*)uart_buffer_start);
    uart_buffer_start += bytes;
    return cp;
}

int calculate_horizontal_advance(GFXfont* font, Line line, int col) {
  int total = 0;
  for (int i = 0; i < col; i++) {
    int cp = line[i];
    GFXglyph* glyph;
    get_glyph(font, cp, &glyph);

    if (!glyph) {
      ESP_LOGW("terminal", "no glyph for %d", cp);
    }
    total += glyph->advance_x;
  }
  return total;
}


void tmoveto(int x, int y) {
  term.cursor.x = min(max(x, 0), COLUMNS);
  term.cursor.y = min(max(y, 0), ROWS);
}

void tputc(uint32_t chr) {
  term.meta[term.cursor.y].dirty = true;
  term.meta[term.cursor.y].chars[term.cursor.x].dirty = true;
  term.line[term.cursor.y][term.cursor.x] = chr;
}

void render() {
  for (int y = 0; y < ROWS; y++) {
    if (!term.meta[y].dirty) {
      continue;
    }

    for (int x = 0; x < COLUMNS; x++) {
      CharMeta* cm = &term.meta[y].chars[x];
      if (!cm->dirty) {
        continue;
      }

      uint32_t chr = term.line[y][x];
      uint32_t old_chr = term.old_line[y][x];

      // the character has changed -> delete it
      // Overwriting only works well for monospaced fonts.
      if (chr != old_chr && old_chr) {
        int horizontal_advance = calculate_horizontal_advance((GFXfont *) &FiraSans, term.old_line[y], x);
        int px_x = term.pixel_start_x + horizontal_advance;
        int px_y = term.pixel_start_y + FiraSans.advance_y * y;

        char data[5];
        to_utf8(data, old_chr);

        epd_poweron();
        write_mode((GFXfont *) &FiraSans, (char *) data, &px_x, &px_y, NULL, WHITE_ON_WHITE);
        epd_poweroff();
      }

      // draw new character
      if (chr != old_chr && chr) {
        int horizontal_advance = calculate_horizontal_advance((GFXfont *) &FiraSans, term.line[y], x);
        int px_x = term.pixel_start_x + horizontal_advance;
        int px_y = term.pixel_start_y + FiraSans.advance_y * y;
        char data[5];
        to_utf8(data, chr);

        // FIXME: color currently ignored
        epd_poweron();
        writeln((GFXfont *) &FiraSans, (char *) data, &px_x, &px_y, NULL);
        epd_poweroff();
      }
      cm->dirty = false;
    }
    term.meta[y].dirty = false;
  }
  memcpy(term.old_line, term.line, ROWS * sizeof(Line));
}

void epd_task() {
    epd_init();
    delay(300);
    epd_poweron();
    epd_clear();
    epd_poweroff();

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS);
    uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0);

    // Still log to the serial output
    esp_log_set_vprintf(log_to_uart);

    ESP_LOGI("terminal", "terminal struct size: %u", sizeof(Term));

    delay(1000);

    uart_write_bytes(UART_NUM_1, "listening\n", 11);

    term.pixel_start_x = 50;
    term.pixel_start_y = 50;
    term.col = 0;
    term.row = 0;

    while (true) {

        uint32_t chr = read_char();
        if (chr == 0) {
          continue;
        };

        ESP_LOGI("terminal", "read char %d", chr);

        switch (chr) {
          case '\b':
            tmoveto(term.cursor.x - 1, term.cursor.y);
            tputc(0);
            render();
            break;
          case '\r':
            tmoveto(0, term.cursor.y);
            break;
          case '\n':
            tmoveto(0, term.cursor.y + 1);
            break;
          default:
            if (chr >= 32) {
              tputc(chr);
              tmoveto(term.cursor.x + 1, term.cursor.y);
              render();
            } else {
              ESP_LOGI("terminal", "unhandled control: %u", chr);
            }
        }
    }
}

void app_main() {
  xTaskCreatePinnedToCore(&epd_task, "epd task", 10000, NULL, 2, NULL, 1);
}