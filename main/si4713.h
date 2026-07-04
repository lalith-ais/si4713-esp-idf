/*
 * si4713.h
 *
 * Pure C driver for the Silicon Labs Si4713 FM transmitter, targeting
 * ESP-IDF (v5.x i2c_master driver).
 *
 * Ported from the Adafruit_Si4713 Arduino C++ library. Behaviour is kept
 * close to the original (same command set, same property defaults in
 * si4713_begin_rds()), but every bus transaction returns esp_err_t instead
 * of silently spinning forever, and there is no global/static state -
 * multiple devices on the same or different buses are fine.
 *
 * Not thread-safe: if you touch a si4713_dev_t from more than one task,
 * serialise access yourself (e.g. hang it off an actor / owning task).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C addresses ---- */
#define SI4713_ADDR0 0x11 /*!< 7-bit address if SEN is low */
#define SI4713_ADDR1 0x63 /*!< 7-bit address if SEN is high (breakout default) */

#define SI4713_STATUS_CTS 0x80 /*!< CTS bit in status byte */

/* ---- Commands ---- */
#define SI4713_CMD_POWER_UP        0x01
#define SI4713_CMD_GET_REV         0x10
#define SI4713_CMD_POWER_DOWN      0x11
#define SI4713_CMD_SET_PROPERTY    0x12
#define SI4713_CMD_GET_PROPERTY    0x13
#define SI4713_CMD_GET_INT_STATUS  0x14
#define SI4713_CMD_PATCH_ARGS      0x15
#define SI4713_CMD_PATCH_DATA      0x16
#define SI4713_CMD_TX_TUNE_FREQ    0x30
#define SI4713_CMD_TX_TUNE_POWER   0x31
#define SI4713_CMD_TX_TUNE_MEASURE 0x32
#define SI4713_CMD_TX_TUNE_STATUS  0x33
#define SI4713_CMD_TX_ASQ_STATUS   0x34
#define SI4713_CMD_TX_RDS_BUFF     0x35
#define SI4713_CMD_TX_RDS_PS       0x36
#define SI4713_CMD_GPO_CTL         0x80
#define SI4713_CMD_GPO_SET         0x81

/* ---- Properties ---- */
#define SI4713_PROP_GPO_IEN                        0x0001
#define SI4713_PROP_DIGITAL_INPUT_FORMAT            0x0101
#define SI4713_PROP_DIGITAL_INPUT_SAMPLE_RATE       0x0103
#define SI4713_PROP_REFCLK_FREQ                     0x0201
#define SI4713_PROP_REFCLK_PRESCALE                 0x0202
#define SI4713_PROP_TX_COMPONENT_ENABLE             0x2100
#define SI4713_PROP_TX_AUDIO_DEVIATION              0x2101
#define SI4713_PROP_TX_PILOT_DEVIATION              0x2102
#define SI4713_PROP_TX_RDS_DEVIATION                0x2103
#define SI4713_PROP_TX_LINE_LEVEL_INPUT_LEVEL       0x2104
#define SI4713_PROP_TX_LINE_INPUT_MUTE              0x2105
#define SI4713_PROP_TX_PREEMPHASIS                  0x2106
#define SI4713_PROP_TX_PILOT_FREQUENCY              0x2107
#define SI4713_PROP_TX_ACOMP_ENABLE                 0x2200
#define SI4713_PROP_TX_ACOMP_THRESHOLD              0x2201
#define SI4713_PROP_TX_ATTACK_TIME                  0x2202
#define SI4713_PROP_TX_RELEASE_TIME                 0x2203
#define SI4713_PROP_TX_ACOMP_GAIN                   0x2204
#define SI4713_PROP_TX_LIMITER_RELEASE_TIME         0x2205
#define SI4713_PROP_TX_ASQ_INTERRUPT_SOURCE         0x2300
#define SI4713_PROP_TX_ASQ_LEVEL_LOW                0x2301
#define SI4713_PROP_TX_ASQ_DURATION_LOW             0x2302
#define SI4713_PROP_TX_ASQ_LEVEL_HIGH               0x2303
#define SI4713_PROP_TX_ASQ_DURATION_HIGH            0x2304
#define SI4713_PROP_TX_RDS_INTERRUPT_SOURCE         0x2C00
#define SI4713_PROP_TX_RDS_PI                       0x2C01
#define SI4713_PROP_TX_RDS_PS_MIX                   0x2C02
#define SI4713_PROP_TX_RDS_PS_MISC                  0x2C03
#define SI4713_PROP_TX_RDS_PS_REPEAT_COUNT          0x2C04
#define SI4713_PROP_TX_RDS_MESSAGE_COUNT            0x2C05
#define SI4713_PROP_TX_RDS_PS_AF                    0x2C06
#define SI4713_PROP_TX_RDS_FIFO_SIZE                0x2C07

/*!
 * @brief Driver instance. Own the storage yourself (static, heap, whatever)
 *        and pass a pointer in to every call - there is no hidden global state.
 */
typedef struct {
    i2c_master_dev_handle_t i2c_dev;   /*!< device handle, from i2c_master_bus_add_device() */
    gpio_num_t              rst_pin;   /*!< reset GPIO, or GPIO_NUM_NC (-1) if not used */
    uint8_t                 cmd[10];   /*!< scratch command buffer */

    /* last-read state, mirrors the Adafruit driver's public fields */
    uint16_t curr_freq_khz;
    uint8_t  curr_dbuv;
    uint8_t  curr_antcap;
    uint8_t  curr_noise_level;
    uint8_t  curr_asq;
    int8_t   curr_in_level;
} si4713_dev_t;

/*!
 * @brief Bus/timing configuration used to add the device to an already-open
 *        i2c_master_bus_handle_t.
 */
typedef struct {
    uint8_t     i2c_addr;    /*!< 7-bit address, SI4713_ADDR0 or SI4713_ADDR1 */
    uint32_t    scl_speed_hz;/*!< e.g. 100000 */
    gpio_num_t  rst_pin;     /*!< reset GPIO, or GPIO_NUM_NC if the RST pin is tied high externally */
} si4713_config_t;

/*!
 * @brief Attach a Si4713 to an existing I2C master bus. Does NOT reset or
 *        power up the chip - call si4713_reset() then si4713_power_up()
 *        (or si4713_probe() to do both plus a revision check) afterwards.
 *
 * @param bus_handle  an already-initialised i2c_master_bus_handle_t
 * @param cfg         address / speed / reset-pin config
 * @param dev         output, caller-owned storage
 */
esp_err_t si4713_init(i2c_master_bus_handle_t bus_handle,
                       const si4713_config_t *cfg,
                       si4713_dev_t *dev);

/*!
 * @brief Toggle the reset pin (if configured) to put the chip in a known
 *        power-down state. No-op if rst_pin is GPIO_NUM_NC.
 */
esp_err_t si4713_reset(si4713_dev_t *dev);

/*!
 * @brief reset() + powerUp() + getRev() with a check that it's really a
 *        Si4713 (part number 13). Convenience wrapper around the
 *        Adafruit begin().
 */
esp_err_t si4713_probe(si4713_dev_t *dev);

/*! @brief Power up the device into FM transmit / analog input mode and
 *         apply the same default properties as the Adafruit driver
 *         (refclk 32.768kHz, 74us pre-emphasis, ACOMP gain 10, AGC+limiter on). */
esp_err_t si4713_power_up(si4713_dev_t *dev);

/*! @brief Power down the device. */
esp_err_t si4713_power_down(si4713_dev_t *dev);

/*! @brief Read the part number (13 for Si4713). */
esp_err_t si4713_get_rev(si4713_dev_t *dev, uint8_t *part_number);

/*! @brief Tune to a transmit frequency. Units are 10 kHz steps (i.e. this
 *         is the frequency in MHz multiplied by 100) - e.g. 8850 for
 *         88.50 MHz, 10230 for 102.30 MHz. Matches the raw TX_TUNE_FREQ
 *         argument from the datasheet, and the units the original Adafruit
 *         driver actually used despite calling the parameter "freqKHz".
 *         Blocks (with a bounded timeout) until tuning completes. */
esp_err_t si4713_tune_fm(si4713_dev_t *dev, uint16_t freq_khz);

/*! @brief Set output power (0, or 88-115) and antenna tuning capacitor
 *         (0 = auto-tune). */
esp_err_t si4713_set_tx_power(si4713_dev_t *dev, uint8_t power, uint8_t antcap);

/*! @brief Read interrupt status bits. */
esp_err_t si4713_get_status(si4713_dev_t *dev, uint8_t *status);

/*! @brief Query TX_TUNE_STATUS; updates dev->curr_freq_khz, curr_dbuv,
 *         curr_antcap, curr_noise_level. */
esp_err_t si4713_read_tune_status(si4713_dev_t *dev);

/*! @brief Kick off a TX_TUNE_MEASURE at the given frequency (kHz, rounded
 *         down to a multiple of 50 kHz) and wait for completion. Read the
 *         result afterwards with si4713_read_tune_status(). */
esp_err_t si4713_read_tune_measure(si4713_dev_t *dev, uint16_t freq_khz);

/*! @brief Query TX_ASQ_STATUS; updates dev->curr_asq, curr_in_level. */
esp_err_t si4713_read_asq(si4713_dev_t *dev);

/*! @brief Set a chip property (SI4713_PROP_*). */
esp_err_t si4713_set_property(si4713_dev_t *dev, uint16_t property, uint16_t value);

/*! @brief Configure RDS with the same defaults as the Adafruit driver's
 *         beginRDS(): 66.25kHz audio deviation, 2kHz RDS deviation, PI set
 *         to program_id, everything else left at chip default. Also enables
 *         RDS + stereo + pilot tone via TX_COMPONENT_ENABLE. */
esp_err_t si4713_begin_rds(si4713_dev_t *dev, uint16_t program_id);

/*! @brief Set the RDS PS (station name) string, transmitted continuously. */
esp_err_t si4713_set_rds_station(si4713_dev_t *dev, const char *s);

/*! @brief Push a message into the RDS group buffer (RadioText-style). */
esp_err_t si4713_set_rds_buffer(si4713_dev_t *dev, const char *s);

/*! @brief Configure GP1/GP2 direction (SI4713_CMD_GPO_CTL). */
esp_err_t si4713_set_gpio_ctrl(si4713_dev_t *dev, uint8_t x);

/*! @brief Set GP1/GP2 output level (SI4713_CMD_GPO_SET). */
esp_err_t si4713_set_gpio(si4713_dev_t *dev, uint8_t x);

#ifdef __cplusplus
}
#endif
