/*
 * si4713.c
 *
 * Pure C driver for the Si4713 FM transmitter, ESP-IDF i2c_master driver.
 * See si4713.h for the API. Ported from Adafruit_Si4713 (Arduino C++).
 *
 * Differences from the Adafruit driver, deliberately:
 *  - every bus op returns esp_err_t, nothing is fire-and-forget
 *  - the CTS wait and the tune/measure completion waits are bounded
 *    (SI4713_CTS_TIMEOUT_MS / SI4713_TUNE_TIMEOUT_MS) instead of spinning
 *    forever if a chip is wedged or absent
 *  - no static/global state - si4713_dev_t is fully caller-owned, so this
 *    is safe to use for more than one module on the same bus
 */

#include <string.h>

#include "si4713.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "si4713";

#define SI4713_I2C_TIMEOUT_MS   100  /* single transaction timeout */
#define SI4713_CTS_TIMEOUT_MS   200  /* time allowed for CTS to come up after a command */
#define SI4713_CTS_POLL_MS      2
#define SI4713_TUNE_TIMEOUT_MS  1000 /* time allowed for a tune/measure to complete */
#define SI4713_TUNE_POLL_MS     10
#define SI4713_RESET_PULSE_MS   10

/* ------------------------------------------------------------------------
 * Low-level helpers
 * ---------------------------------------------------------------------- */

/*!
 * @brief Send whatever is currently in dev->cmd[0..len) and block until the
 *        chip raises CTS (Clear To Send), or time out.
 */
static esp_err_t si4713_send_command(si4713_dev_t *dev, uint8_t len)
{
    esp_err_t err = i2c_master_transmit(dev->i2c_dev, dev->cmd, len,
                                         SI4713_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cmd 0x%02x transmit failed: %s", dev->cmd[0], esp_err_to_name(err));
        return err;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)SI4713_CTS_TIMEOUT_MS * 1000;
    uint8_t status = 0;

    for (;;) {
        err = i2c_master_receive(dev->i2c_dev, &status, 1, SI4713_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cmd 0x%02x CTS poll failed: %s", dev->cmd[0], esp_err_to_name(err));
            return err;
        }
        if (status & SI4713_STATUS_CTS) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "cmd 0x%02x timed out waiting for CTS", dev->cmd[0]);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SI4713_CTS_POLL_MS));
    }
}

/* ------------------------------------------------------------------------
 * Init / reset / power
 * ---------------------------------------------------------------------- */

esp_err_t si4713_init(i2c_master_bus_handle_t bus_handle,
                       const si4713_config_t *cfg,
                       si4713_dev_t *dev)
{
    if (!bus_handle || !cfg || !dev) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));
    dev->rst_pin = cfg->rst_pin;

    if (dev->rst_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << dev->rst_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config(rst) failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_addr,
        .scl_speed_hz = cfg->scl_speed_hz ? cfg->scl_speed_hz : 100000,
    };

    return i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev->i2c_dev);
}

esp_err_t si4713_reset(si4713_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->rst_pin < 0) {
        return ESP_OK; /* nothing to do, RST presumably tied high externally */
    }

    esp_err_t err;
    if ((err = gpio_set_level(dev->rst_pin, 1)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(SI4713_RESET_PULSE_MS));
    if ((err = gpio_set_level(dev->rst_pin, 0)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(SI4713_RESET_PULSE_MS));
    if ((err = gpio_set_level(dev->rst_pin, 1)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(SI4713_RESET_PULSE_MS));
    return ESP_OK;
}

esp_err_t si4713_power_up(si4713_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->cmd[0] = SI4713_CMD_POWER_UP;
    dev->cmd[1] = 0x12; /* CTS int disabled, GPO2 disabled, boot normal, xtal on, FM TX */
    dev->cmd[2] = 0x50; /* analog input mode */
    esp_err_t err = si4713_send_command(dev, 3);
    if (err != ESP_OK) {
        return err;
    }

    /* same defaults as the Adafruit driver's powerUp() */
    if ((err = si4713_set_property(dev, SI4713_PROP_REFCLK_FREQ, 32768)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_PREEMPHASIS, 0)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_ACOMP_GAIN, 10)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_ACOMP_ENABLE, 0x0)) != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t si4713_power_down(si4713_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_POWER_DOWN;
    dev->cmd[1] = 0;
    return si4713_send_command(dev, 2);
}

esp_err_t si4713_probe(si4713_dev_t *dev)
{
    esp_err_t err;

    if ((err = si4713_reset(dev)) != ESP_OK) return err;
    if ((err = si4713_power_up(dev)) != ESP_OK) return err;

    uint8_t part_number = 0;
    if ((err = si4713_get_rev(dev, &part_number)) != ESP_OK) return err;

    if (part_number != 13) {
        ESP_LOGE(TAG, "unexpected part number Si47%02u, expected Si4713", part_number);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * Properties
 * ---------------------------------------------------------------------- */

esp_err_t si4713_set_property(si4713_dev_t *dev, uint16_t property, uint16_t value)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_SET_PROPERTY;
    dev->cmd[1] = 0;
    dev->cmd[2] = property >> 8;
    dev->cmd[3] = property & 0xFF;
    dev->cmd[4] = value >> 8;
    dev->cmd[5] = value & 0xFF;
    return si4713_send_command(dev, 6);
}

/* ------------------------------------------------------------------------
 * Rev / status
 * ---------------------------------------------------------------------- */

esp_err_t si4713_get_rev(si4713_dev_t *dev, uint8_t *part_number)
{
    if (!dev || !part_number) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->cmd[0] = SI4713_CMD_GET_REV;
    dev->cmd[1] = 0;
    esp_err_t err = si4713_send_command(dev, 2);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t resp[9];
    err = i2c_master_receive(dev->i2c_dev, resp, sizeof(resp), SI4713_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    *part_number = resp[1];

#ifdef SI4713_CMD_DEBUG
    uint16_t fw = ((uint16_t)resp[2] << 8) | resp[3];
    uint16_t patch = ((uint16_t)resp[4] << 8) | resp[5];
    uint16_t cmp = ((uint16_t)resp[6] << 8) | resp[7];
    ESP_LOGI(TAG, "Si47%u fw=0x%04x patch=0x%04x cmp=0x%04x chiprev=%c",
             *part_number, fw, patch, cmp, resp[8]);
#endif

    return ESP_OK;
}

esp_err_t si4713_get_status(si4713_dev_t *dev, uint8_t *status)
{
    if (!dev || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t cmd = SI4713_CMD_GET_INT_STATUS;
    return i2c_master_transmit_receive(dev->i2c_dev, &cmd, 1, status, 1,
                                        SI4713_I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------------
 * Tuning
 * ---------------------------------------------------------------------- */

esp_err_t si4713_tune_fm(si4713_dev_t *dev, uint16_t freq_khz)
{
    /* freq_khz is in 10 kHz units per the datasheet, see the header comment */
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->cmd[0] = SI4713_CMD_TX_TUNE_FREQ;
    dev->cmd[1] = 0;
    dev->cmd[2] = freq_khz >> 8;
    dev->cmd[3] = freq_khz & 0xFF;
    esp_err_t err = si4713_send_command(dev, 4);
    if (err != ESP_OK) {
        return err;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)SI4713_TUNE_TIMEOUT_MS * 1000;
    uint8_t status;
    for (;;) {
        if ((err = si4713_get_status(dev, &status)) != ESP_OK) {
            return err;
        }
        if ((status & 0x81) == 0x81) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "tune_fm timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SI4713_TUNE_POLL_MS));
    }
}

esp_err_t si4713_set_tx_power(si4713_dev_t *dev, uint8_t power, uint8_t antcap)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_TX_TUNE_POWER;
    dev->cmd[1] = 0;
    dev->cmd[2] = 0;
    dev->cmd[3] = power;
    dev->cmd[4] = antcap;
    return si4713_send_command(dev, 5);
}

esp_err_t si4713_read_tune_status(si4713_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_TX_TUNE_STATUS;
    dev->cmd[1] = 0x1; /* INTACK */
    esp_err_t err = si4713_send_command(dev, 2);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t resp[8];
    err = i2c_master_receive(dev->i2c_dev, resp, sizeof(resp), SI4713_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    dev->curr_freq_khz = ((uint16_t)resp[2] << 8) | resp[3];
    dev->curr_dbuv = resp[5];
    dev->curr_antcap = resp[6];
    dev->curr_noise_level = resp[7];
    return ESP_OK;
}

esp_err_t si4713_read_tune_measure(si4713_dev_t *dev, uint16_t freq_khz)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    /* round down to a multiple of 5 (matches original driver's rounding,
     * which despite the comment rounds to steps of 5, i.e. the raw units
     * the chip expects) */
    if (freq_khz % 5 != 0) {
        freq_khz -= (freq_khz % 5);
    }

    dev->cmd[0] = SI4713_CMD_TX_TUNE_MEASURE;
    dev->cmd[1] = 0;
    dev->cmd[2] = freq_khz >> 8;
    dev->cmd[3] = freq_khz & 0xFF;
    dev->cmd[4] = 0;
    esp_err_t err = si4713_send_command(dev, 5);
    if (err != ESP_OK) {
        return err;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)SI4713_TUNE_TIMEOUT_MS * 1000;
    uint8_t status;
    for (;;) {
        if ((err = si4713_get_status(dev, &status)) != ESP_OK) {
            return err;
        }
        if (status == 0x81) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "read_tune_measure timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SI4713_TUNE_POLL_MS));
    }
}

esp_err_t si4713_read_asq(si4713_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_TX_ASQ_STATUS;
    dev->cmd[1] = 0x1;
    esp_err_t err = si4713_send_command(dev, 2);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t resp[5];
    err = i2c_master_receive(dev->i2c_dev, resp, sizeof(resp), SI4713_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    dev->curr_asq = resp[1];
    dev->curr_in_level = (int8_t)resp[4];
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * RDS
 * ---------------------------------------------------------------------- */

esp_err_t si4713_begin_rds(si4713_dev_t *dev, uint16_t program_id)
{
    esp_err_t err;

    if ((err = si4713_set_property(dev, SI4713_PROP_TX_AUDIO_DEVIATION, 6625)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_DEVIATION, 200)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_INTERRUPT_SOURCE, 0x0001)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_PI, program_id)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_PS_MIX, 0x03)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_PS_MISC, 0x1008)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_PS_REPEAT_COUNT, 3)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_MESSAGE_COUNT, 1)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_PS_AF, 0xE0E0)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_RDS_FIFO_SIZE, 0)) != ESP_OK) return err;
    if ((err = si4713_set_property(dev, SI4713_PROP_TX_COMPONENT_ENABLE, 0x0007)) != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t si4713_set_rds_station(si4713_dev_t *dev, const char *s)
{
    if (!dev || !s) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(s);
    uint8_t slots = (uint8_t)((len + 3) / 4);
    if (slots == 0) {
        slots = 1; /* still send one (blank) slot, matching original for empty strings */
    }

    for (uint8_t i = 0; i < slots; i++) {
        memset(dev->cmd, ' ', 6);
        size_t chunk = strlen(s);
        if (chunk > 4) chunk = 4;
        memcpy(dev->cmd + 2, s, chunk);
        s += 4;
        dev->cmd[6] = 0;

        dev->cmd[0] = SI4713_CMD_TX_RDS_PS;
        dev->cmd[1] = i;
        esp_err_t err = si4713_send_command(dev, 6);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t si4713_set_rds_buffer(si4713_dev_t *dev, const char *s)
{
    if (!dev || !s) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(s);
    uint8_t slots = (uint8_t)((len + 3) / 4);
    if (slots == 0) {
        slots = 1;
    }

    for (uint8_t i = 0; i < slots; i++) {
        memset(dev->cmd, ' ', 8);
        size_t chunk = strlen(s);
        if (chunk > 4) chunk = 4;
        memcpy(dev->cmd + 4, s, chunk);
        s += 4;
        dev->cmd[8] = 0;

        dev->cmd[0] = SI4713_CMD_TX_RDS_BUFF;
        dev->cmd[1] = (i == 0) ? 0x06 : 0x04;
        dev->cmd[2] = 0x20;
        dev->cmd[3] = i;
        esp_err_t err = si4713_send_command(dev, 8);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * GPIO
 * ---------------------------------------------------------------------- */

esp_err_t si4713_set_gpio_ctrl(si4713_dev_t *dev, uint8_t x)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_GPO_CTL;
    dev->cmd[1] = x;
    return si4713_send_command(dev, 2);
}

esp_err_t si4713_set_gpio(si4713_dev_t *dev, uint8_t x)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cmd[0] = SI4713_CMD_GPO_SET;
    dev->cmd[1] = x;
    return si4713_send_command(dev, 2);
}
