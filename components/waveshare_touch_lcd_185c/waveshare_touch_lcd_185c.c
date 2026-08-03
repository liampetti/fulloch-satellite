#include "waveshare_touch_lcd_185c.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "waveshare_rev2";

#define I2C_PORT I2C_NUM_0
#define I2C_SDA GPIO_NUM_11
#define I2C_SCL GPIO_NUM_10
#define I2S_PORT I2S_NUM_1
#define I2S_LRCK GPIO_NUM_38
#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_DIN GPIO_NUM_39
#define I2S_DOUT GPIO_NUM_47
#define PA_ENABLE GPIO_NUM_15
#define BACKLIGHT GPIO_NUM_5
#define TCA9554_ADDRESS 0x20
#define CST816_ADDRESS 0x15
#define TCA9554_OUTPUT_REGISTER 0x01
#define TCA9554_CONFIG_REGISTER 0x03
#define TCA9554_TOUCH_RESET_BIT BIT(0)
#define TCA9554_LCD_RESET_BIT BIT(1)
#define I2S_SAMPLE_RATE_HZ 16000
#define CAPTURE_SAMPLES_PER_FRAME (I2S_SAMPLE_RATE_HZ / 50)
#define PLAYBACK_BLOCK_SAMPLES 128
#define LCD_WIDTH 360
#define LCD_HEIGHT 360
#define LCD_ROWS_PER_TRANSFER 10
#define LCD_TRANSFER_BYTES (LCD_WIDTH * LCD_ROWS_PER_TRANSFER * sizeof(uint16_t))
#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_CLOCK GPIO_NUM_40
#define LCD_SPI_DATA0 GPIO_NUM_46
#define LCD_SPI_DATA1 GPIO_NUM_45
#define LCD_SPI_DATA2 GPIO_NUM_42
#define LCD_SPI_DATA3 GPIO_NUM_41
#define LCD_SPI_CS GPIO_NUM_21
#define LCD_WRITE_COMMAND_OPCODE 0x02U
#define LCD_WRITE_COLOR_OPCODE 0x32U
#define LCD_QSPI_COMMAND(command, opcode) (((uint32_t)(opcode) << 24) | ((uint32_t)(command) << 8))

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t expander;
static i2c_master_dev_handle_t touch;
static uint8_t expander_output;
static esp_lcd_panel_io_handle_t lcd_io;
static bool board_io_initialized;
static i2s_chan_handle_t i2s_tx;
static i2s_chan_handle_t i2s_rx;
static esp_codec_dev_handle_t playback;
static esp_codec_dev_handle_t capture;
static SemaphoreHandle_t display_transfer_complete;
static int16_t capture_samples[FULLOCH_AEC_CAPTURE_MAX_SAMPLES * FULLOCH_AEC_CAPTURE_CHANNELS];
static uint16_t lcd_rows[LCD_WIDTH * 10];
static int32_t playback_samples[PLAYBACK_BLOCK_SAMPLES * 2];
static const int16_t silence_samples[PLAYBACK_BLOCK_SAMPLES];

typedef struct {
    uint8_t command;
    uint8_t data;
} st77916_register_t;

static bool display_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                  esp_lcd_panel_io_event_data_t *event_data, void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(display_transfer_complete, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t wait_for_display_transfer(void)
{
    return xSemaphoreTake(display_transfer_complete, pdMS_TO_TICKS(1000)) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static const uint8_t st77916_new_gamma_positive[] = {
    0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34,
};
static const uint8_t st77916_new_gamma_negative[] = {
    0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33,
};

/* Exact Waveshare sequence for the panel revision used by the shipped firmware. */
static const st77916_register_t st77916_setup_new[] = {
    {0xF0, 0x28}, {0xF2, 0x28}, {0x73, 0xF0}, {0x7C, 0xD1}, {0x83, 0xE0}, {0x84, 0x61},
    {0xF2, 0x82}, {0xF0, 0x00}, {0xF0, 0x01}, {0xF1, 0x01}, {0xB0, 0x56}, {0xB1, 0x4D},
    {0xB2, 0x24}, {0xB4, 0x87}, {0xB5, 0x44}, {0xB6, 0x8B}, {0xB7, 0x40}, {0xB8, 0x86},
    {0xBA, 0x00}, {0xBB, 0x08}, {0xBC, 0x08}, {0xBD, 0x00}, {0xC0, 0x80}, {0xC1, 0x10},
    {0xC2, 0x37}, {0xC3, 0x80}, {0xC4, 0x10}, {0xC5, 0x37}, {0xC6, 0xA9}, {0xC7, 0x41},
    {0xC8, 0x01}, {0xC9, 0xA9}, {0xCA, 0x41}, {0xCB, 0x01}, {0xD0, 0x91}, {0xD1, 0x68},
    {0xD2, 0x68}, {0xF5, 0x00}, {0xDD, 0x4F}, {0xDE, 0x4F}, {0xF1, 0x10}, {0xF0, 0x00}, {0xF0, 0x02},
    {0xE0, 0xF0}, {0xE1, 0xF0}, {0xF0, 0x10}, {0xF3, 0x10}, {0xE0, 0x07}, {0xE1, 0x00},
    {0xE2, 0x00}, {0xE3, 0x00}, {0xE4, 0xE0}, {0xE5, 0x06}, {0xE6, 0x21}, {0xE7, 0x01},
    {0xE8, 0x05}, {0xE9, 0x02}, {0xEA, 0xDA}, {0xEB, 0x00}, {0xEC, 0x00}, {0xED, 0x0F},
    {0xEE, 0x00}, {0xEF, 0x00}, {0xF8, 0x00}, {0xF9, 0x00}, {0xFA, 0x00}, {0xFB, 0x00},
    {0xFC, 0x00}, {0xFD, 0x00}, {0xFE, 0x00}, {0xFF, 0x00},
    {0x60, 0x40}, {0x61, 0x04}, {0x62, 0x00}, {0x63, 0x42}, {0x64, 0xD9}, {0x65, 0x00},
    {0x66, 0x00}, {0x67, 0x00}, {0x68, 0x00}, {0x69, 0x00}, {0x6A, 0x00}, {0x6B, 0x00},
    {0x70, 0x40}, {0x71, 0x03}, {0x72, 0x00}, {0x73, 0x42}, {0x74, 0xD8}, {0x75, 0x00},
    {0x76, 0x00}, {0x77, 0x00}, {0x78, 0x00}, {0x79, 0x00}, {0x7A, 0x00}, {0x7B, 0x00},
    {0x80, 0x48}, {0x81, 0x00}, {0x82, 0x06}, {0x83, 0x02}, {0x84, 0xD6}, {0x85, 0x04},
    {0x86, 0x00}, {0x87, 0x00}, {0x88, 0x48}, {0x89, 0x00}, {0x8A, 0x08}, {0x8B, 0x02},
    {0x8C, 0xD8}, {0x8D, 0x04}, {0x8E, 0x00}, {0x8F, 0x00}, {0x90, 0x48}, {0x91, 0x00},
    {0x92, 0x0A}, {0x93, 0x02}, {0x94, 0xDA}, {0x95, 0x04}, {0x96, 0x00}, {0x97, 0x00},
    {0x98, 0x48}, {0x99, 0x00}, {0x9A, 0x0C}, {0x9B, 0x02}, {0x9C, 0xDC}, {0x9D, 0x04},
    {0x9E, 0x00}, {0x9F, 0x00}, {0xA0, 0x48}, {0xA1, 0x00}, {0xA2, 0x05}, {0xA3, 0x02},
    {0xA4, 0xD5}, {0xA5, 0x04}, {0xA6, 0x00}, {0xA7, 0x00}, {0xA8, 0x48}, {0xA9, 0x00},
    {0xAA, 0x07}, {0xAB, 0x02}, {0xAC, 0xD7}, {0xAD, 0x04}, {0xAE, 0x00}, {0xAF, 0x00},
    {0xB0, 0x48}, {0xB1, 0x00}, {0xB2, 0x09}, {0xB3, 0x02}, {0xB4, 0xD9}, {0xB5, 0x04},
    {0xB6, 0x00}, {0xB7, 0x00}, {0xB8, 0x48}, {0xB9, 0x00}, {0xBA, 0x0B}, {0xBB, 0x02},
    {0xBC, 0xDB}, {0xBD, 0x04}, {0xBE, 0x00}, {0xBF, 0x00},
    {0xC0, 0x10}, {0xC1, 0x47}, {0xC2, 0x56}, {0xC3, 0x65}, {0xC4, 0x74}, {0xC5, 0x88},
    {0xC6, 0x99}, {0xC7, 0x01}, {0xC8, 0xBB}, {0xC9, 0xAA}, {0xD0, 0x10}, {0xD1, 0x47},
    {0xD2, 0x56}, {0xD3, 0x65}, {0xD4, 0x74}, {0xD5, 0x88}, {0xD6, 0x99}, {0xD7, 0x01},
    {0xD8, 0xBB}, {0xD9, 0xAA}, {0xF3, 0x01}, {0xF0, 0x00},
};

static esp_err_t expander_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(expander, data, sizeof(data), 100);
}

static esp_err_t expander_reset(uint8_t bit)
{
    expander_output &= ~bit;
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_OUTPUT_REGISTER, expander_output), TAG, "reset assert failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    expander_output |= bit;
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_OUTPUT_REGISTER, expander_output), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus), TAG, "I2C bus init failed");

    const i2c_device_config_t expander_config = {
        .device_address = TCA9554_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &expander_config, &expander), TAG, "expander init failed");
    ESP_RETURN_ON_ERROR(expander_write(TCA9554_CONFIG_REGISTER, 0), TAG, "expander output config failed");
    expander_output = 0;
    ESP_RETURN_ON_ERROR(expander_reset(TCA9554_TOUCH_RESET_BIT), TAG, "touch reset failed");
    ESP_RETURN_ON_ERROR(expander_reset(TCA9554_LCD_RESET_BIT), TAG, "LCD reset failed");

    const i2c_device_config_t touch_config = {
        .device_address = CST816_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &touch_config, &touch), TAG, "touch controller init failed");
    uint8_t register_address = 0xA7;
    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(touch, &register_address, sizeof(register_address), &chip_id, sizeof(chip_id), 100), TAG, "touch controller probe failed");
    const uint8_t disable_auto_sleep[] = {0xFE, 1};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(touch, disable_auto_sleep, sizeof(disable_auto_sleep), 100), TAG,
                        "touch auto-sleep disable failed");
    ESP_LOGI(TAG, "CST816 chip ID: 0x%02X", chip_id);
    return ESP_OK;
}

static esp_err_t init_backlight(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel_config = {
        .gpio_num = BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "backlight timer init failed");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG, "backlight channel init failed");
    return waveshare_touch_lcd_185c_set_backlight(25);
}

static esp_err_t init_board_io(void)
{
    if (board_io_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C initialization failed");
    ESP_RETURN_ON_ERROR(init_backlight(), TAG, "backlight initialization failed");
    board_io_initialized = true;
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t duty = ((1U << LEDC_TIMER_13_BIT) - 1U) * percent / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "backlight duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t lcd_command(uint8_t command, const void *data, size_t data_size)
{
    return esp_lcd_panel_io_tx_param(lcd_io, LCD_QSPI_COMMAND(command, LCD_WRITE_COMMAND_OPCODE), data, data_size);
}

static esp_err_t st77916_write_registers(const st77916_register_t *registers, size_t count)
{
    for (size_t index = 0; index < count; ++index) {
        const void *data = &registers[index].data;
        size_t data_size = 1;
        if (registers == st77916_setup_new && registers[index].command == 0xE0 && registers[index].data == 0xF0) {
            data = st77916_new_gamma_positive;
            data_size = sizeof(st77916_new_gamma_positive);
        } else if (registers == st77916_setup_new && registers[index].command == 0xE1 && registers[index].data == 0xF0) {
            data = st77916_new_gamma_negative;
            data_size = sizeof(st77916_new_gamma_negative);
        } else if (registers == st77916_setup_new && registers[index].command == 0xF5) {
            static const uint8_t f5[] = {0x00, 0xA5};
            data = f5;
            data_size = sizeof(f5);
        }
        ESP_RETURN_ON_ERROR(lcd_command(registers[index].command, data, data_size), TAG,
                            "ST77916 register write failed");
    }
    return ESP_OK;
}

static esp_err_t st77916_initialize(void)
{
    const uint8_t madctl = 0xc0;
    const uint8_t color_mode = 0x55;
    ESP_RETURN_ON_ERROR(lcd_command(0x01, NULL, 0), TAG, "LCD software reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(lcd_command(0x36, &madctl, sizeof(madctl)), TAG, "LCD memory access setup failed");
    ESP_RETURN_ON_ERROR(lcd_command(0x3A, &color_mode, sizeof(color_mode)), TAG, "LCD color mode failed");
    ESP_RETURN_ON_ERROR(st77916_write_registers(st77916_setup_new,
                                                 sizeof(st77916_setup_new) / sizeof(*st77916_setup_new)),
                        TAG, "ST77916 setup failed");
    ESP_RETURN_ON_ERROR(lcd_command(0x21, NULL, 0), TAG, "LCD inversion setup failed");
    ESP_RETURN_ON_ERROR(lcd_command(0x11, NULL, 0), TAG, "LCD sleep-out failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    return lcd_command(0x29, NULL, 0);
}

esp_err_t waveshare_touch_lcd_185c_init_display(void)
{
    if (lcd_io != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(init_board_io(), TAG, "board I/O initialization failed");

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_SPI_CLOCK,
        .data0_io_num = LCD_SPI_DATA0,
        .data1_io_num = LCD_SPI_DATA1,
        .data2_io_num = LCD_SPI_DATA2,
        .data3_io_num = LCD_SPI_DATA3,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_TRANSFER_BYTES,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "LCD SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_SPI_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    display_transfer_complete = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(display_transfer_complete != NULL, ESP_ERR_NO_MEM, TAG, "LCD transfer semaphore init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &lcd_io), TAG, "LCD panel IO init failed");
    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = display_transfer_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(lcd_io, &callbacks, NULL), TAG,
                        "LCD transfer callback init failed");

    ESP_RETURN_ON_ERROR(st77916_initialize(), TAG, "ST77916 initialization failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return waveshare_touch_lcd_185c_fill_display(0);
}

esp_err_t waveshare_touch_lcd_185c_fill_display(uint16_t rgb565)
{
    ESP_RETURN_ON_FALSE(lcd_io != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD is not initialized");
    const uint8_t columns[] = {0, 0, (LCD_WIDTH - 1) >> 8, (LCD_WIDTH - 1) & 0xff};
    for (size_t pixel = 0; pixel < LCD_WIDTH * LCD_ROWS_PER_TRANSFER; ++pixel) {
        lcd_rows[pixel] = rgb565;
    }

    ESP_RETURN_ON_ERROR(lcd_command(0x2A, columns, sizeof(columns)), TAG, "LCD column address failed");
    for (uint16_t row = 0; row < LCD_HEIGHT; row += LCD_ROWS_PER_TRANSFER) {
        const uint16_t end_row = row + LCD_ROWS_PER_TRANSFER - 1;
        const uint8_t row_range[] = {row >> 8, row & 0xff, end_row >> 8, end_row & 0xff};
        ESP_RETURN_ON_ERROR(lcd_command(0x2B, row_range, sizeof(row_range)), TAG, "LCD row address failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(lcd_io, LCD_QSPI_COMMAND(0x2C, LCD_WRITE_COLOR_OPCODE), lcd_rows, sizeof(lcd_rows)), TAG, "LCD row write failed");
        ESP_RETURN_ON_ERROR(wait_for_display_transfer(), TAG, "LCD row transfer timed out");
    }
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_draw_rgb565(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                                                const uint16_t *pixels)
{
    ESP_RETURN_ON_FALSE(lcd_io != NULL && pixels != NULL && x1 <= x2 && y1 <= y2 &&
                            x2 < LCD_WIDTH && y2 < LCD_HEIGHT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid display region");
    const uint8_t columns[] = {x1 >> 8, x1 & 0xff, x2 >> 8, x2 & 0xff};
    const uint8_t rows[] = {y1 >> 8, y1 & 0xff, y2 >> 8, y2 & 0xff};
    const size_t pixel_count = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1);
    ESP_RETURN_ON_ERROR(lcd_command(0x2A, columns, sizeof(columns)), TAG, "LCD column address failed");
    ESP_RETURN_ON_ERROR(lcd_command(0x2B, rows, sizeof(rows)), TAG, "LCD row address failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(lcd_io, LCD_QSPI_COMMAND(0x2C, LCD_WRITE_COLOR_OPCODE), pixels,
                                                   pixel_count * sizeof(*pixels)), TAG, "LCD region write failed");
    return wait_for_display_transfer();
}

bool waveshare_touch_lcd_185c_read_touch(uint16_t *x, uint16_t *y)
{
    uint8_t register_address = 0x02;
    uint8_t data[5];
    if (x == NULL || y == NULL || touch == NULL ||
        i2c_master_transmit_receive(touch, &register_address, sizeof(register_address), data, sizeof(data), 20) != ESP_OK ||
        (data[0] & 0x0f) == 0) {
        return false;
    }
    const uint16_t raw_x = ((uint16_t)(data[1] & 0x0f) << 8) | data[2];
    const uint16_t raw_y = ((uint16_t)(data[3] & 0x0f) << 8) | data[4];
    if (raw_x >= LCD_WIDTH || raw_y >= LCD_HEIGHT) {
        return false;
    }
    *x = LCD_WIDTH - 1 - raw_x;
    *y = LCD_HEIGHT - 1 - raw_y;
    return true;
}

static esp_err_t init_audio(void)
{
    ESP_RETURN_ON_FALSE(i2s_rx != NULL, ESP_ERR_INVALID_STATE, TAG, "I2S RX channel is not initialized");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_rx), TAG, "I2S RX enable failed");

    audio_codec_i2s_cfg_t data_config = {.port = I2S_PORT, .rx_handle = i2s_rx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "audio data interface init failed");

    esp_codec_dev_sample_info_t format = {
        .sample_rate = I2S_SAMPLE_RATE_HZ,
        .channel = 2,
        .bits_per_sample = 32,
    };

    audio_codec_i2c_cfg_t input_i2c = {.addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus, .clock_speed_hz = 400000};
    const audio_codec_ctrl_if_t *input_control = audio_codec_new_i2c_ctrl(&input_i2c);
    ESP_RETURN_ON_FALSE(input_control != NULL, ESP_FAIL, TAG, "ES7210 control init failed");
    es7210_codec_cfg_t input_config = {
        .ctrl_if = input_control,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *input_codec = es7210_codec_new(&input_config);
    ESP_RETURN_ON_FALSE(input_codec != NULL, ESP_ERR_NO_MEM, TAG, "ES7210 codec init failed");
    esp_codec_dev_cfg_t input_device_config = {
        .codec_if = input_codec,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
    };
    capture = esp_codec_dev_new(&input_device_config);
    ESP_RETURN_ON_FALSE(capture != NULL, ESP_ERR_NO_MEM, TAG, "capture device init failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(capture, &format), TAG, "capture open failed");
    for (int channel = 0; channel < FULLOCH_AEC_CAPTURE_CHANNELS; ++channel) {
        if (esp_codec_dev_set_in_channel_gain(capture, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel), 30) !=
            ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_init_playback(void)
{
    if (playback != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(gpio_set_direction(PA_ENABLE, GPIO_MODE_OUTPUT), TAG, "amplifier pin init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_ENABLE, 0), TAG, "amplifier disable failed");
    ESP_RETURN_ON_ERROR(init_board_io(), TAG, "board I/O initialization failed");

    const i2s_chan_config_t channel_config = {
        .id = I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &i2s_tx, &i2s_rx), TAG, "I2S channel init failed");
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_LRCK,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s_tx, &i2s_config), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s_rx, &i2s_config), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s_tx), TAG, "I2S TX enable failed");

    audio_codec_i2s_cfg_t data_config = {.port = I2S_PORT, .tx_handle = i2s_tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "playback data interface init failed");
    audio_codec_i2c_cfg_t i2c_config = {.addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus, .clock_speed_hz = 400000};
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&i2c_config);
    ESP_RETURN_ON_FALSE(control_if != NULL, ESP_FAIL, TAG, "ES8311 control init failed");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "codec GPIO init failed");
    es8311_codec_cfg_t codec_config = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .pa_pin = PA_ENABLE,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG, "ES8311 codec init failed");
    esp_codec_dev_cfg_t device_config = {
        .codec_if = codec_if,
        .data_if = data_if,
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    };
    playback = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(playback != NULL, ESP_ERR_NO_MEM, TAG, "playback device init failed");
    esp_codec_dev_sample_info_t format = {
        .sample_rate = I2S_SAMPLE_RATE_HZ,
        .channel = 2,
        .bits_per_sample = 32,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(playback, &format), TAG, "playback open failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(playback, 50) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "playback volume setup failed");
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_start_playback(void)
{
    ESP_RETURN_ON_FALSE(playback != NULL, ESP_ERR_INVALID_STATE, TAG, "playback is not initialized");
    return gpio_set_level(PA_ENABLE, 1);
}

esp_err_t waveshare_touch_lcd_185c_set_playback_volume(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(playback != NULL && percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "invalid playback volume");
    return esp_codec_dev_set_out_vol(playback, percent) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t waveshare_touch_lcd_185c_play_mono_s16le(const int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(playback != NULL && samples != NULL && sample_count > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid playback samples");
    for (size_t offset = 0; offset < sample_count; offset += PLAYBACK_BLOCK_SAMPLES) {
        const size_t block_samples = sample_count - offset < PLAYBACK_BLOCK_SAMPLES
                                         ? sample_count - offset
                                         : PLAYBACK_BLOCK_SAMPLES;
        for (size_t sample = 0; sample < block_samples; ++sample) {
            const int32_t expanded = (int32_t)samples[offset + sample] * 65536;
            playback_samples[sample * 2] = expanded;
            playback_samples[sample * 2 + 1] = expanded;
        }
        const size_t bytes = block_samples * 2 * sizeof(*playback_samples);
        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(i2s_tx, playback_samples, bytes, &bytes_written, portMAX_DELAY),
                            TAG, "I2S playback write failed");
        ESP_RETURN_ON_FALSE(bytes_written == bytes, ESP_FAIL, TAG, "short I2S playback write");
    }
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_play_silence(size_t sample_count)
{
    ESP_RETURN_ON_FALSE(sample_count == PLAYBACK_BLOCK_SAMPLES, ESP_ERR_INVALID_ARG, TAG,
                        "invalid silence sample count");
    return waveshare_touch_lcd_185c_play_mono_s16le(silence_samples, sample_count);
}

esp_err_t waveshare_touch_lcd_185c_stop_playback(void)
{
    ESP_RETURN_ON_FALSE(playback != NULL, ESP_ERR_INVALID_STATE, TAG, "playback is not initialized");
    return gpio_set_level(PA_ENABLE, 0);
}

esp_err_t waveshare_touch_lcd_185c_init_capture(void)
{
    ESP_RETURN_ON_ERROR(init_board_io(), TAG, "board I/O initialization failed");
    return init_audio();
}

esp_err_t waveshare_touch_lcd_185c_read_mono_s16le(int16_t output[FULLOCH_AUDIO_FRAME_SAMPLES])
{
    ESP_RETURN_ON_FALSE(capture != NULL && output != NULL, ESP_ERR_INVALID_STATE, TAG, "capture is not initialized");
    const int result = esp_codec_dev_read(capture, capture_samples,
                                          FULLOCH_AUDIO_FRAME_SAMPLES * FULLOCH_AEC_CAPTURE_CHANNELS *
                                              sizeof(*capture_samples));
    ESP_RETURN_ON_FALSE(result == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "capture read failed: %d", result);

    for (size_t sample = 0; sample < FULLOCH_AUDIO_FRAME_SAMPLES; ++sample) {
        output[sample] = capture_samples[sample * FULLOCH_AEC_CAPTURE_CHANNELS + 1];
    }
    static uint32_t capture_blocks;
    if (++capture_blocks <= 5 || capture_blocks % 100 == 0) {
        int16_t minimum = output[0];
        int16_t maximum = output[0];
        for (size_t sample = 1; sample < FULLOCH_AUDIO_FRAME_SAMPLES; ++sample) {
            if (output[sample] < minimum) {
                minimum = output[sample];
            }
            if (output[sample] > maximum) {
                maximum = output[sample];
            }
        }
        ESP_LOGI(TAG, "Raw M1 input: range=%d..%d", minimum, maximum);
    }
    return ESP_OK;
}

esp_err_t waveshare_touch_lcd_185c_read_aec_input(int16_t *output, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(capture != NULL && output != NULL && sample_count > 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid AEC capture buffer");
    // The vendor board maps the ES7210's four raw slots as RMNM. ESP-SR must
    // receive this hardware-synchronous stream without a software reference.
    ESP_RETURN_ON_FALSE(sample_count <= FULLOCH_AEC_CAPTURE_MAX_SAMPLES, ESP_ERR_INVALID_SIZE, TAG,
                        "AEC capture block is too large");
    const size_t bytes = sample_count * FULLOCH_AEC_CAPTURE_CHANNELS * sizeof(*output);
    // I2S DMA requires internal RAM; copy the completed interleaved block to the
    // PSRAM AFE buffer after the driver has finished the DMA transfer.
    const int result = esp_codec_dev_read(capture, capture_samples, bytes);
    ESP_RETURN_ON_FALSE(result == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "AEC capture read failed: %d", result);
    memcpy(output, capture_samples, bytes);
    static uint32_t capture_blocks;
    if (++capture_blocks <= 5 || capture_blocks % 100 == 0) {
        int16_t minimum[FULLOCH_AEC_CAPTURE_CHANNELS];
        int16_t maximum[FULLOCH_AEC_CAPTURE_CHANNELS];
        memcpy(minimum, output, sizeof(minimum));
        memcpy(maximum, output, sizeof(maximum));
        for (size_t sample = 1; sample < sample_count; ++sample) {
            for (size_t channel = 0; channel < FULLOCH_AEC_CAPTURE_CHANNELS; ++channel) {
                const int16_t value = output[sample * FULLOCH_AEC_CAPTURE_CHANNELS + channel];
                if (value < minimum[channel]) {
                    minimum[channel] = value;
                }
                if (value > maximum[channel]) {
                    maximum[channel] = value;
                }
            }
        }
        ESP_LOGI(TAG, "AFE input RMNM: R=%d..%d M1=%d..%d N=%d..%d M2=%d..%d",
                 minimum[0], maximum[0], minimum[1], maximum[1], minimum[2], maximum[2],
                 minimum[3], maximum[3]);
    }
    return ESP_OK;
}
