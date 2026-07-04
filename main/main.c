/*
 * example_usage.c - not part of the component, just a reference for how
 * to wire si4713_dev_t up to an I2C bus and get it transmitting.
 *
 * Drop si4713.c/.h into components/si4713/ (with the CMakeLists.txt) and
 * call something like this from your app.
 */

#include "si4713.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define I2C_SDA_GPIO 21
#define I2C_SCL_GPIO 22
#define SI4713_RST_GPIO 17

static const char *TAG = "example";

void app_main(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    si4713_config_t cfg = {
        .i2c_addr = SI4713_ADDR1,
        .scl_speed_hz = 100000,
        .rst_pin = SI4713_RST_GPIO,
    };

    static si4713_dev_t radio; /* static: cheap way to keep it alive past this function */
    ESP_ERROR_CHECK(si4713_init(bus_handle, &cfg, &radio));

    esp_err_t err = si4713_probe(&radio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Si4713 not found: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(si4713_tune_fm(&radio, 10730)); /* 88.50 MHz, kHz/10 units */
    ESP_ERROR_CHECK(si4713_set_tx_power(&radio, 115, 0));

    ESP_ERROR_CHECK(si4713_begin_rds(&radio, 0xADAF));
    ESP_ERROR_CHECK(si4713_set_rds_station(&radio, "harmony"));
    ESP_ERROR_CHECK(si4713_set_rds_buffer(&radio, "Now playing: hamony radio"));

    ESP_LOGI(TAG, "Si4713 up and transmitting");
}
