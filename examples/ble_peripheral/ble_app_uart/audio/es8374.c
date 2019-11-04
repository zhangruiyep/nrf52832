/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

//#include "esp_system.h"
//#include "esp_log.h"
//#include "driver/i2c.h"
#include "es8374.h"
//#include "board_pins_config.h"

/* nrf52832 port begin */
#include "nrf_log.h"
#include "nrf_drv_twi.h"
#include "nrf_drv_i2s.h"
#include "nrf_delay.h"

#define ESP_LOGE(TAG, ...) do {\
    NRF_LOG_ERROR(##__VA_ARGS__);\
}while(0)

#define ESP_LOGW(TAG, ...) do {\
    NRF_LOG_WARNING(##__VA_ARGS__);\
}while(0)

#define ESP_LOGI(TAG, ...) do {\
    NRF_LOG_INFO(##__VA_ARGS__);\
}while(0)

#define ESP_LOGD(TAG, ...) do {\
    NRF_LOG_DEBUG(##__VA_ARGS__);\
}while(0)

/* TWI instance ID. */
#define TWI_INSTANCE_ID     1

/* Indicates if operation on TWI has ended. */
static volatile bool m_xfer_done = false;

/* TWI instance. */
static const nrf_drv_twi_t m_twi = NRF_DRV_TWI_INSTANCE(TWI_INSTANCE_ID);

/**
 * @brief TWI events handler.
 */
void twi_handler(nrf_drv_twi_evt_t const * p_event, void * p_context)
{
    switch (p_event->type)
    {
        case NRF_DRV_TWI_EVT_DONE:
            if (p_event->xfer_desc.type == NRF_DRV_TWI_XFER_RX)
            {
                //data_handler(m_sample);
            }
            m_xfer_done = true;
            break;
        default:
            break;
    }
}

#define I2S_DATA_BLOCK_WORDS    512
static uint32_t m_buffer_rx[2][I2S_DATA_BLOCK_WORDS];
static uint32_t m_buffer_tx[2][I2S_DATA_BLOCK_WORDS];

// Delay time between consecutive I2S transfers performed in the main loop
// (in milliseconds).
#define PAUSE_TIME          500
// Number of blocks of data to be contained in each transfer.
#define BLOCKS_TO_TRANSFER  20

static uint8_t volatile m_blocks_transferred     = 0;
static uint8_t          m_zero_samples_to_ignore = 0;
static uint16_t         m_sample_value_to_send;
static uint16_t         m_sample_value_expected;
static bool             m_error_encountered;

static uint32_t       * volatile mp_block_to_fill  = NULL;
static uint32_t const * volatile mp_block_to_check = NULL;

static void prepare_tx_data(uint32_t * p_block)
{
    // These variables will be both zero only at the very beginning of each
    // transfer, so we use them as the indication that the re-initialization
    // should be performed.
    if (m_blocks_transferred == 0 && m_zero_samples_to_ignore == 0)
    {
        // Number of initial samples (actually pairs of L/R samples) with zero
        // values that should be ignored - see the comment in 'check_samples'.
        m_zero_samples_to_ignore = 2;
        m_sample_value_to_send   = 0xCAFE;
        m_sample_value_expected  = 0xCAFE;
        m_error_encountered      = false;
    }

    // [each data word contains two 16-bit samples]
    uint16_t i;
    for (i = 0; i < I2S_DATA_BLOCK_WORDS; ++i)
    {
        uint16_t sample_l = m_sample_value_to_send - 1;
        uint16_t sample_r = m_sample_value_to_send + 1;
        ++m_sample_value_to_send;

        uint32_t * p_word = &p_block[i];
        ((uint16_t *)p_word)[0] = sample_l;
        ((uint16_t *)p_word)[1] = sample_r;
    }
}


static bool check_samples(uint32_t const * p_block)
{
    // [each data word contains two 16-bit samples]
    uint16_t i;
    for (i = 0; i < I2S_DATA_BLOCK_WORDS; ++i)
    {
        uint32_t const * p_word = &p_block[i];
        uint16_t actual_sample_l = ((uint16_t const *)p_word)[0];
        uint16_t actual_sample_r = ((uint16_t const *)p_word)[1];

        // Normally a couple of initial samples sent by the I2S peripheral
        // will have zero values, because it starts to output the clock
        // before the actual data is fetched by EasyDMA. As we are dealing
        // with streaming the initial zero samples can be simply ignored.
        if (m_zero_samples_to_ignore > 0 &&
                actual_sample_l == 0 &&
                actual_sample_r == 0)
        {
            --m_zero_samples_to_ignore;
        }
        else
        {
            m_zero_samples_to_ignore = 0;

            uint16_t expected_sample_l = m_sample_value_expected - 1;
            uint16_t expected_sample_r = m_sample_value_expected + 1;
            ++m_sample_value_expected;

            if (actual_sample_l != expected_sample_l ||
                    actual_sample_r != expected_sample_r)
            {
                NRF_LOG_INFO("%3u: %04x/%04x, expected: %04x/%04x (i: %u)",
                        m_blocks_transferred, actual_sample_l, actual_sample_r,
                        expected_sample_l, expected_sample_r, i);
                return false;
            }
        }
    }
    NRF_LOG_INFO("%3u: OK", m_blocks_transferred);
    return true;
}


static void check_rx_data(uint32_t const * p_block)
{
    ++m_blocks_transferred;

    if (!m_error_encountered)
    {
        m_error_encountered = !check_samples(p_block);
    }

    if (m_error_encountered)
    {
        //bsp_board_led_off(LED_OK);
        //bsp_board_led_invert(LED_ERROR);
        NRF_LOG_INFO("m_error_encountered");
    }
    else
    {
        //bsp_board_led_off(LED_ERROR);
        //bsp_board_led_invert(LED_OK);
    }
}

static void i2s_data_handler(nrf_drv_i2s_buffers_t const * p_released,
        uint32_t                      status)
{
    // 'nrf_drv_i2s_next_buffers_set' is called directly from the handler
    // each time next buffers are requested, so data corruption is not
    // expected.
    ASSERT(p_released);

    // When the handler is called after the transfer has been stopped
    // (no next buffers are needed, only the used buffers are to be
    // released), there is nothing to do.
    if (!(status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED))
    {
        return;
    }

    // First call of this handler occurs right after the transfer is started.
    // No data has been transferred yet at this point, so there is nothing to
    // check. Only the buffers for the next part of the transfer should be
    // provided.
    if (!p_released->p_rx_buffer)
    {
        nrf_drv_i2s_buffers_t const next_buffers = {
            .p_rx_buffer = m_buffer_rx[1],
            .p_tx_buffer = m_buffer_tx[1],
        };
        APP_ERROR_CHECK(nrf_drv_i2s_next_buffers_set(&next_buffers));

        mp_block_to_fill = m_buffer_tx[1];
    }
    else
    {
        mp_block_to_check = p_released->p_rx_buffer;
        // The driver has just finished accessing the buffers pointed by
        // 'p_released'. They can be used for the next part of the transfer
        // that will be scheduled now.
        APP_ERROR_CHECK(nrf_drv_i2s_next_buffers_set(p_released));

        // The pointer needs to be typecasted here, so that it is possible to
        // modify the content it is pointing to (it is marked in the structure
        // as pointing to constant data because the driver is not supposed to
        // modify the provided data).
        mp_block_to_fill = (uint32_t *)p_released->p_tx_buffer;
    }
}

static ret_code_t i2s_init(void)
{
    ret_code_t err_code;

    nrf_drv_i2s_config_t config = NRF_DRV_I2S_DEFAULT_CONFIG;
    // In Master mode the MCK frequency and the MCK/LRCK ratio should be
    // set properly in order to achieve desired audio sample rate (which
    // is equivalent to the LRCK frequency).
    // For the following settings we'll get the LRCK frequency equal to
    // 15873 Hz (the closest one to 16 kHz that is possible to achieve).
    config.sck_pin   = I2S_SCK_PIN;
    config.lrck_pin  = I2S_LRCK_PIN;
    config.mck_pin   = I2S_MCK_PIN;
    config.sdin_pin  = I2S_SDIN_PIN;
    config.sdout_pin = I2S_SDOUT_PIN;

    config.mode      = NRF_I2S_MODE_MASTER;
    config.format    = NRF_I2S_FORMAT_I2S;
    config.sample_width = NRF_I2S_SWIDTH_16BIT; //采样宽度16bit
    config.mck_setup = NRF_I2S_MCK_32MDIV15;    //MCK=2.13333 MHz
    config.ratio     = NRF_I2S_RATIO_48X;       //采样率=44.44444 KHz
    config.channels  = NRF_I2S_CHANNELS_STEREO; //双声道
    /* 偏差率=44.444/44.1-1=0.78%*/
    /* 比特率=44.1K*16bit*2(双声道)=1411.2kbps */
    err_code = nrf_drv_i2s_init(&config, i2s_data_handler);
    APP_ERROR_CHECK(err_code);

    return err_code;
}

void i2s_test(void)
{
    ret_code_t err_code;

    m_blocks_transferred = 0;
    mp_block_to_fill  = NULL;
    mp_block_to_check = NULL;

    prepare_tx_data(m_buffer_tx[0]);

    nrf_drv_i2s_buffers_t const initial_buffers = {
        .p_tx_buffer = m_buffer_tx[0],
        .p_rx_buffer = m_buffer_rx[0],
    };
    err_code = nrf_drv_i2s_start(&initial_buffers, I2S_DATA_BLOCK_WORDS, 0);
    APP_ERROR_CHECK(err_code);

    do {
        // Wait for an event.
        __WFE();
        // Clear the event register.
        __SEV();
        __WFE();

        if (mp_block_to_fill)
        {
            prepare_tx_data(mp_block_to_fill);
            mp_block_to_fill = NULL;
        }
        if (mp_block_to_check)
        {
            check_rx_data(mp_block_to_check);
            mp_block_to_check = NULL;
        }
    } while (m_blocks_transferred < BLOCKS_TO_TRANSFER);

    nrf_drv_i2s_stop();
}
/* nrf52832 port end */

#define ES8374_TAG "ES8374_DRIVER"

#define ES_ASSERT(a, format, b, ...) \
    if ((a) != 0) { \
        ESP_LOGE(ES8374_TAG, format, ##__VA_ARGS__); \
        return b;\
    }

#define LOG_8374(fmt, ...)   ESP_LOGW(ES8374_TAG, fmt, ##__VA_ARGS__)

static int codec_init_flag = 0;

#if 0
static i2c_config_t es_i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000
};

audio_hal_func_t AUDIO_CODEC_ES8374_DEFAULT_HANDLE = {
    .audio_codec_initialize = es8374_codec_init,
    .audio_codec_deinitialize = es8374_codec_deinit,
    .audio_codec_ctrl = es8374_codec_ctrl_state,
    .audio_codec_config_iface = es8374_codec_config_i2s,
    .audio_codec_set_mute = es8374_set_voice_mute,
    .audio_codec_set_volume = es8374_codec_set_voice_volume,
    .audio_codec_get_volume = es8374_codec_get_voice_volume,
};
#endif

static bool es8374_codec_initialized()
{
    return codec_init_flag;
}

#if 0
int es_write_reg(uint8_t slaveAdd, uint8_t regAdd, uint8_t data)
{
    int res = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    res |= i2c_master_start(cmd);
    res |= i2c_master_write_byte(cmd, slaveAdd, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_write_byte(cmd, regAdd, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_write_byte(cmd, data, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_stop(cmd);
    res |= i2c_master_cmd_begin(0, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    ES_ASSERT(res, "ESCodecWriteReg error", -1);
    return res;
}

int es_read_reg(uint8_t slaveAdd, uint8_t regAdd, uint8_t *pData)
{
    uint8_t data;
    int res = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    res |= i2c_master_start(cmd);
    res |= i2c_master_write_byte(cmd, slaveAdd, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_write_byte(cmd, regAdd, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_stop(cmd);
    res |= i2c_master_cmd_begin(0, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    cmd = i2c_cmd_link_create();
    res |= i2c_master_start(cmd);
    res |= i2c_master_write_byte(cmd, slaveAdd | 0x01, 1 /*ACK_CHECK_EN*/);
    res |= i2c_master_read_byte(cmd, &data, 0x01/*NACK_VAL*/);
    res |= i2c_master_stop(cmd);
    res |= i2c_master_cmd_begin(0, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    ES_ASSERT(res, "Es8374ReadReg error", -1);
    *pData = data;
    return res;
}
#endif

static int i2c_init(void)
{
#if 0
    int res;
    res = get_i2c_pins(I2C_NUM_0, &es_i2c_cfg);
    res = i2c_param_config(I2C_NUM_0, &es_i2c_cfg);
    res |= i2c_driver_install(I2C_NUM_0, es_i2c_cfg.mode, 0, 0, 0);
    ES_ASSERT(res, "i2c_init error", -1);
    return res;
#else
    ret_code_t err_code;

    const nrf_drv_twi_config_t twi_es8374_config = {
        .scl                = I2C_SCL_PIN,
        .sda                = I2C_SDA_PIN,
        .frequency          = NRF_DRV_TWI_FREQ_100K,
        .interrupt_priority = APP_IRQ_PRIORITY_HIGH,
        .clear_bus_init     = false
    };

    err_code = nrf_drv_twi_init(&m_twi, &twi_es8374_config, twi_handler, NULL);
    APP_ERROR_CHECK(err_code);

    nrf_drv_twi_enable(&m_twi);

    return (int)err_code;
#endif
}

esp_err_t es8374_write_reg(uint8_t regAdd, uint8_t data)
{
    ret_code_t err_code;

    uint8_t reg[2] = {regAdd, data};
    err_code = nrf_drv_twi_tx(&m_twi, ES8374_ADDR, reg, sizeof(reg), false);
    APP_ERROR_CHECK(err_code);
    while (m_xfer_done == false);

    return (esp_err_t)err_code;
}

esp_err_t es8374_read_reg(uint8_t regAdd, uint8_t *regv)
{
    ret_code_t err_code;
    uint8_t reg[1] = {regAdd};

    /* write reg address */
    m_xfer_done = false;
    err_code = nrf_drv_twi_tx(&m_twi, ES8374_ADDR, reg, sizeof(reg), true);
    APP_ERROR_CHECK(err_code);
    while (m_xfer_done == false);

    /* read data */
    m_xfer_done = false;
    err_code = nrf_drv_twi_rx(&m_twi, ES8374_ADDR, regv, sizeof(*regv));
    APP_ERROR_CHECK(err_code);
    while (m_xfer_done == false);

    NRF_LOG_INFO("ES8374 I2C read %02x from addr %02x", *regv, regAdd);

    return (esp_err_t)err_code;

}

void es8374_read_all()
{
    for (int i = 0; i < 50; i++) {
        uint8_t reg = 0;
        es8374_read_reg(i, &reg);
        ESP_LOGI(ES8374_TAG, "%x: %x", i, reg);
    }
}

esp_err_t es8374_set_voice_mute(bool enable)
{
    int res = 0;
    uint8_t reg = 0;

    res |= es8374_read_reg(0x36, &reg);
    if (res == 0) {
        reg = reg & 0xdf;
        res |= es8374_write_reg(0x36, reg | (((int)enable) << 5));
    }

    return res;
}

int es8374_get_voice_mute(void)
{
    int res = 0;
    uint8_t reg = 0;

    res |= es8374_read_reg(0x36, &reg);
    if (res == ESP_OK) {
        reg = reg & 0x40;
    }

    return res == ESP_OK ? reg : res;
}

int es8374_set_bits_per_sample(es_module_t mode, es_bits_length_t bit_per_sample)
{
    int res = 0;
    uint8_t reg = 0;
    int bits = (int)bit_per_sample & 0x0f;

    if (mode == ES_MODULE_ADC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_read_reg(0x10, &reg);
        if (res == 0) {
            reg = reg & 0xe3;
            res |=  es8374_write_reg(0x10, reg | (bits << 2));
        }
    }
    if (mode == ES_MODULE_DAC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_read_reg(0x11, &reg);
        if (res == 0) {
            reg = reg & 0xe3;
            res |= es8374_write_reg(0x11, reg | (bits << 2));
        }
    }

    return res;
}

int es8374_config_fmt(es_module_t mode, uint8_t fmt)
{
    int res = 0;
    uint8_t reg = 0;
    es_bits_length_t fmt_tmp;
    uint8_t fmt_i2s;

    fmt_tmp = (es_bits_length_t)((fmt & 0xf0) >> 4);
    fmt_i2s =  fmt & 0x0f;
    if (mode == ES_MODULE_ADC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_read_reg(0x10, &reg);
        if (res == 0) {
            reg = reg & 0xfc;
            res |= es8374_write_reg(0x10, reg | fmt_i2s);
            res |= es8374_set_bits_per_sample(mode, fmt_tmp);
        }
    }
    if (mode == ES_MODULE_DAC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_read_reg(0x11, &reg);
        if (res == 0) {
            reg = reg & 0xfc;
            res |= es8374_write_reg(0x11, reg | (fmt_i2s));
            res |= es8374_set_bits_per_sample(mode, fmt_tmp);
        }
    }

    return res;
}

int es8374_start(es_module_t mode)
{
    int res = 0;
    uint8_t reg = 0;

    if (mode == ES_MODULE_LINE) {
        res |= es8374_read_reg(0x1a, &reg);       //set monomixer
        reg |= 0x60;
        reg |= 0x20;
        reg &= 0xf7;
        res |= es8374_write_reg( 0x1a, reg);
        res |= es8374_read_reg(0x1c, &reg);        // set spk mixer
        reg |= 0x40;
        res |= es8374_write_reg( 0x1c, reg);
        res |= es8374_write_reg(0x1D, 0x02);      // spk set
        res |= es8374_write_reg(0x1F, 0x00);      // spk set
        res |= es8374_write_reg(0x1E, 0xA0);      // spk on
    }
    if (mode == ES_MODULE_ADC || mode == ES_MODULE_ADC_DAC || mode == ES_MODULE_LINE) {
        res |= es8374_read_reg(0x21, &reg);       //power up adc and input
        reg &= 0x3f;
        res |= es8374_write_reg(0x21, reg);
        res |= es8374_read_reg(0x10, &reg);       //power up adc and input
        reg &= 0x3f;
        res |= es8374_write_reg(0x10, reg);
    }

    if (mode == ES_MODULE_DAC || mode == ES_MODULE_ADC_DAC || mode == ES_MODULE_LINE) {
        res |= es8374_read_reg(0x1a, &reg);       //disable lout
        reg |= 0x08;
        res |= es8374_write_reg( 0x1a, reg);
        reg &= 0xdf;
        res |= es8374_write_reg( 0x1a, reg);
        res |= es8374_write_reg(0x1D, 0x12);      // mute speaker
        res |= es8374_write_reg(0x1E, 0x20);      // disable class d
        res |= es8374_read_reg(0x15, &reg);        //power up dac
        reg &= 0xdf;
        res |= es8374_write_reg(0x15, reg);
        res |= es8374_read_reg(0x1a, &reg);        //disable lout
        reg |= 0x20;
        res |= es8374_write_reg( 0x1a, reg);
        reg &= 0xf7;
        res |= es8374_write_reg( 0x1a, reg);
        res |= es8374_write_reg(0x1D, 0x02);      // mute speaker
        res |= es8374_write_reg(0x1E, 0xa0);      // disable class d

        res |= es8374_set_voice_mute(false);
    }

    return res;
}

int es8374_stop(es_module_t mode)
{
    int res = 0;
    uint8_t reg = 0;

    if (mode == ES_MODULE_LINE) {
        res |= es8374_read_reg(0x1a, &reg);       //disable lout
        reg |= 0x08;
        res |= es8374_write_reg( 0x1a, reg);
        reg &= 0x9f;
        res |= es8374_write_reg( 0x1a, reg);
        res |= es8374_write_reg(0x1D, 0x12);      // mute speaker
        res |= es8374_write_reg(0x1E, 0x20);      // disable class d
        res |= es8374_read_reg(0x1c, &reg);        // disable spkmixer
        reg &= 0xbf;
        res |= es8374_write_reg( 0x1c, reg);
        res |= es8374_write_reg(0x1F, 0x00);      // spk set
    }
    if (mode == ES_MODULE_DAC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_set_voice_mute(true);

        res |= es8374_read_reg(0x1a, &reg);        //disable lout
        reg |= 0x08;
        res |= es8374_write_reg( 0x1a, reg);
        reg &= 0xdf;
        res |= es8374_write_reg( 0x1a, reg);
        res |= es8374_write_reg(0x1D, 0x12);      // mute speaker
        res |= es8374_write_reg(0x1E, 0x20);      // disable class d
        res |= es8374_read_reg(0x15, &reg);        //power up dac
        reg |= 0x20;
        res |= es8374_write_reg(0x15, reg);
    }
    if (mode == ES_MODULE_ADC || mode == ES_MODULE_ADC_DAC) {

        res |= es8374_read_reg(0x10, &reg);       //power up adc and input
        reg |= 0xc0;
        res |= es8374_write_reg(0x10, reg);
        res |= es8374_read_reg(0x21, &reg);       //power up adc and input
        reg |= 0xc0;
        res |= es8374_write_reg(0x21, reg);
    }

    return res;
}

int es8374_i2s_config_clock(es_i2s_clock_t cfg)
{

    int res = 0;
    uint8_t reg = 0;

    res |= es8374_read_reg(0x0f, &reg);       //power up adc and input
    reg &= 0xe0;
    int divratio = 0;
    switch (cfg.sclk_div) {
        case MCLK_DIV_1:
            divratio = 1;
            break;
        case MCLK_DIV_2: // = 2,
            divratio = 2;
            break;
        case MCLK_DIV_3: // = 3,
            divratio = 3;
            break;
        case MCLK_DIV_4: // = 4,
            divratio = 4;
            break;
        case MCLK_DIV_5: // = 20,
            divratio = 5;
            break;
        case MCLK_DIV_6: // = 5,
            divratio = 6;
            break;
        case MCLK_DIV_7: //  = 29,
            divratio = 7;
            break;
        case MCLK_DIV_8: // = 6,
            divratio = 8;
            break;
        case MCLK_DIV_9: // = 7,
            divratio = 9;
            break;
        case MCLK_DIV_10: // = 21,
            divratio = 10;
            break;
        case MCLK_DIV_11: // = 8,
            divratio = 11;
            break;
        case MCLK_DIV_12: // = 9,
            divratio = 12;
            break;
        case MCLK_DIV_13: // = 30,
            divratio = 13;
            break;
        case MCLK_DIV_14: // = 31
            divratio = 14;
            break;
        case MCLK_DIV_15: // = 22,
            divratio = 15;
            break;
        case MCLK_DIV_16: // = 10,
            divratio = 16;
            break;
        case MCLK_DIV_17: // = 23,
            divratio = 17;
            break;
        case MCLK_DIV_18: // = 11,
            divratio = 18;
            break;
        case MCLK_DIV_20: // = 24,
            divratio = 19;
            break;
        case MCLK_DIV_22: // = 12,
            divratio = 20;
            break;
        case MCLK_DIV_24: // = 13,
            divratio = 21;
            break;
        case MCLK_DIV_25: // = 25,
            divratio = 22;
            break;
        case MCLK_DIV_30: // = 26,
            divratio = 23;
            break;
        case MCLK_DIV_32: // = 27,
            divratio = 24;
            break;
        case MCLK_DIV_33: // = 14,
            divratio = 25;
            break;
        case MCLK_DIV_34: // = 28,
            divratio = 26;
            break;
        case MCLK_DIV_36: // = 15,
            divratio = 27;
            break;
        case MCLK_DIV_44: // = 16,
            divratio = 28;
            break;
        case MCLK_DIV_48: // = 17,
            divratio = 29;
            break;
        case MCLK_DIV_66: // = 18,
            divratio = 30;
            break;
        case MCLK_DIV_72: // = 19,
            divratio = 31;
            break;
        default:
            break;
    }
    reg |= divratio;
    res |= es8374_write_reg(0x0f, reg);

    int dacratio_l = 0;
    int dacratio_h = 0;

    switch (cfg.lclk_div) {
        case LCLK_DIV_128:
            dacratio_l = 128 % 256;
            dacratio_h = 128 / 256;
            break;
        case LCLK_DIV_192:
            dacratio_l = 192 % 256;
            dacratio_h = 192 / 256;
            break;
        case LCLK_DIV_256:
            dacratio_l = 256 % 256;
            dacratio_h = 256 / 256;
            break;
        case LCLK_DIV_384:
            dacratio_l = 384 % 256;
            dacratio_h = 384 / 256;
            break;
        case LCLK_DIV_512:
            dacratio_l = 512 % 256;
            dacratio_h = 512 / 256;
            break;
        case LCLK_DIV_576:
            dacratio_l = 576 % 256;
            dacratio_h = 576 / 256;
            break;
        case LCLK_DIV_768:
            dacratio_l = 768 % 256;
            dacratio_h = 768 / 256;
            break;
        case LCLK_DIV_1024:
            dacratio_l = 1024 % 256;
            dacratio_h = 1024 / 256;
            break;
        case LCLK_DIV_1152:
            dacratio_l = 1152 % 256;
            dacratio_h = 1152 / 256;
            break;
        case LCLK_DIV_1408:
            dacratio_l = 1408 % 256;
            dacratio_h = 1408 / 256;
            break;
        case LCLK_DIV_1536:
            dacratio_l = 1536 % 256;
            dacratio_h = 1536 / 256;
            break;
        case LCLK_DIV_2112:
            dacratio_l = 2112 % 256;
            dacratio_h = 2112 / 256;
            break;
        case LCLK_DIV_2304:
            dacratio_l = 2304 % 256;
            dacratio_h = 2304 / 256;
            break;
        case LCLK_DIV_96:
            dacratio_l = 96 % 256;
            dacratio_h = 96 / 256;
            break;        
        case LCLK_DIV_48:
            dacratio_l = 48 % 256;
            dacratio_h = 48 / 256;
            break;        
        case LCLK_DIV_125:
            dacratio_l = 125 % 256;
            dacratio_h = 125 / 256;
            break;
        case LCLK_DIV_136:
            dacratio_l = 136 % 256;
            dacratio_h = 136 / 256;
            break;
        case LCLK_DIV_250:
            dacratio_l = 250 % 256;
            dacratio_h = 250 / 256;
            break;
        case LCLK_DIV_272:
            dacratio_l = 272 % 256;
            dacratio_h = 272 / 256;
            break;
        case LCLK_DIV_375:
            dacratio_l = 375 % 256;
            dacratio_h = 375 / 256;
            break;
        case LCLK_DIV_500:
            dacratio_l = 500 % 256;
            dacratio_h = 500 / 256;
            break;
        case LCLK_DIV_544:
            dacratio_l = 544 % 256;
            dacratio_h = 544 / 256;
            break;
        case LCLK_DIV_750:
            dacratio_l = 750 % 256;
            dacratio_h = 750 / 256;
            break;
        case LCLK_DIV_1000:
            dacratio_l = 1000 % 256;
            dacratio_h = 1000 / 256;
            break;
        case LCLK_DIV_1088:
            dacratio_l = 1088 % 256;
            dacratio_h = 1088 / 256;
            break;
        case LCLK_DIV_1496:
            dacratio_l = 1496 % 256;
            dacratio_h = 1496 / 256;
            break;
        case LCLK_DIV_1500:
            dacratio_l = 1500 % 256;
            dacratio_h = 1500 / 256;
            break;
        default:
            break;
    }
    res |= es8374_write_reg( 0x06, dacratio_h);  //ADCFsMode,singel SPEED,RATIO=256
    res |= es8374_write_reg( 0x07, dacratio_l);  //ADCFsMode,singel SPEED,RATIO=256

    return res;
}

int es8374_config_dac_output(es_dac_output_t output)
{
    int res = 0;
    uint8_t reg = 0;

    reg = 0x1d;

    res = es8374_write_reg(reg, 0x02);
    res |= es8374_read_reg(0x1c, &reg); // set spk mixer
    reg |= 0x80;
    res |= es8374_write_reg(0x1c, reg);
    res |= es8374_write_reg(0x1D, 0x02); // spk set
    res |= es8374_write_reg(0x1F, 0x00); // spk set
    res |= es8374_write_reg(0x1E, 0xA0); // spk on

    return res;
}

#ifdef FEATURE_MIC_EN
int es8374_config_adc_input(es_adc_input_t input)
{
    int res = 0;
    uint8_t reg = 0;

    res |= es8374_read_reg(0x21, &reg);
    if (res == 0) {
        reg = (reg & 0xcf) | 0x14;
        res |= es8374_write_reg( 0x21, reg);
    }

    return res;
}

int es8374_set_mic_gain(es_mic_gain_t gain)
{
    int res = 0;

    if (gain > MIC_GAIN_MIN && gain < MIC_GAIN_24DB) {
        int gain_n = 0;
        gain_n = (int)gain / 3;
        res = es8374_write_reg(0x22, gain_n | (gain_n << 4)); //MIC PGA
    } else {
        res = -1;
        LOG_8374("invalid microphone gain!");
    }

    return res;
}
#endif

int es8374_codec_set_voice_volume(int volume)
{
    int res = 0;

    if (volume < 0) {
        volume = 192;
    } else if (volume > 96) {
        volume = 0;
    } else {
        volume = 192 - volume * 2;
    }

    res = es8374_write_reg(0x38, volume);

    return res;
}

int es8374_codec_get_voice_volume(int *volume)
{
    int res = 0;
    uint8_t reg = 0;

    res = es8374_read_reg(0x38, &reg);

    if (res == ESP_FAIL) {
        *volume = 0;
    } else {
        *volume = (192 - reg) / 2;
        if (*volume > 96) {
            *volume = 100;
        }
    }

    return res;
}

static int es8374_set_adc_dac_volume(int mode, int volume, int dot)
{
    int res = 0;

    if ( volume < -96 || volume > 0 ) {
        LOG_8374("Warning: volume < -96! or > 0!");
        if (volume < -96) {
            volume = -96;
        } else {
            volume = 0;
        }
    }
    dot = (dot >= 5 ? 1 : 0);
    volume = (-volume << 1) + dot;
    if (mode == ES_MODULE_ADC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_write_reg(0x25, volume);
    }
    if (mode == ES_MODULE_DAC || mode == ES_MODULE_ADC_DAC) {
        res |= es8374_write_reg(0x38, volume);
    }

    return res;
}

//#ifdef FEATURE_MIC_EN
#if 0
static int es8374_set_d2se_pga(es_d2se_pga_t gain)
{
    int res = 0;
    uint8_t reg = 0;

    if (gain > D2SE_PGA_GAIN_MIN && gain < D2SE_PGA_GAIN_MAX) {
        res = es8374_read_reg(0x21, &reg);
        reg &= 0xfb;
        reg |= gain << 2;
        res = es8374_write_reg(0x21, reg); //MIC PGA
    } else {
        res = 0xff;
        LOG_8374("invalid microphone gain!");
    }

    return res;
}
#endif

static int es8374_init_reg(es_mode_t ms_mode, uint8_t fmt, es_i2s_clock_t cfg, es_dac_output_t out_channel, es_adc_input_t in_channel)
{
    int res = 0;
    uint8_t reg;

    res |= es8374_write_reg(0x00, 0x3F); //IC Rst start
    res |= es8374_write_reg(0x00, 0x03); //IC Rst stop
    res |= es8374_write_reg(0x01, 0x7F); //IC clk on

    res |= es8374_read_reg(0x0F, &reg);
    reg &= 0x7f;
    reg |=  (ms_mode << 7);
    res |= es8374_write_reg( 0x0f, reg); //CODEC IN I2S SLAVE MODE

    res |= es8374_write_reg(0x6F, 0xA0); //pll set:mode enable
    res |= es8374_write_reg(0x72, 0x41); //pll set:mode set
    res |= es8374_write_reg(0x09, 0x01); //pll set:reset on ,set start
    res |= es8374_write_reg(0x0C, 0x22); //pll set:k
    res |= es8374_write_reg(0x0D, 0x2E); //pll set:k
    res |= es8374_write_reg(0x0E, 0xC6); //pll set:k
    res |= es8374_write_reg(0x0A, 0x3A); //pll set:
    res |= es8374_write_reg(0x0B, 0x07); //pll set:n
    res |= es8374_write_reg(0x09, 0x41); //pll set:reset off ,set stop

    res |= es8374_i2s_config_clock(cfg);

    res |= es8374_write_reg(0x24, 0x08); //adc set
    res |= es8374_write_reg(0x36, 0x00); //dac set
    res |= es8374_write_reg(0x12, 0x30); //timming set
    res |= es8374_write_reg(0x13, 0x20); //timming set

    res |= es8374_config_fmt(ES_MODULE_ADC, fmt);
    res |= es8374_config_fmt(ES_MODULE_DAC, fmt);

    res |= es8374_write_reg(0x21, 0x50); //adc set: SEL LIN1 CH+PGAGAIN=0DB
    res |= es8374_write_reg(0x22, 0xFF); //adc set: PGA GAIN=0DB
    res |= es8374_write_reg(0x21, 0x14); //adc set: SEL LIN1 CH+PGAGAIN=18DB
    res |= es8374_write_reg(0x22, 0x55); //pga = +15db
    res |= es8374_write_reg(0x08, 0x21); //set class d divider = 33, to avoid the high frequency tone on laudspeaker
    res |= es8374_write_reg(0x00, 0x80); // IC START

    res |= es8374_set_adc_dac_volume(ES_MODULE_ADC, 0, 0);      // 0db
    res |= es8374_set_adc_dac_volume(ES_MODULE_DAC, 0, 0);      // 0db

    res |= es8374_write_reg(0x14, 0x8A); // IC START
    res |= es8374_write_reg(0x15, 0x40); // IC START
    res |= es8374_write_reg(0x1A, 0xA0); // monoout set
    res |= es8374_write_reg(0x1B, 0x19); // monoout set
    res |= es8374_write_reg(0x1C, 0x90); // spk set
    res |= es8374_write_reg(0x1D, 0x01); // spk set
    res |= es8374_write_reg(0x1F, 0x00); // spk set
    res |= es8374_write_reg(0x1E, 0x20); // spk on
    res |= es8374_write_reg(0x28, 0x00); // alc set
    res |= es8374_write_reg(0x25, 0x00); // ADCVOLUME on
    res |= es8374_write_reg(0x38, 0x00); // DACVOLUME on
    res |= es8374_write_reg(0x37, 0x30); // dac set
    res |= es8374_write_reg(0x6D, 0x60); //SEL:GPIO1=DMIC CLK OUT+SEL:GPIO2=PLL CLK OUT
    res |= es8374_write_reg(0x71, 0x05); //for automute setting
    res |= es8374_write_reg(0x73, 0x70);

    res |= es8374_config_dac_output(out_channel);  //0x3c Enable DAC and Enable Lout/Rout/1/2
#ifdef FEATURE_MIC_EN
    res |= es8374_config_adc_input(in_channel);  //0x00 LINSEL & RINSEL, LIN1/RIN1 as ADC Input; DSSEL,use one DS Reg11; DSR, LINPUT1-RINPUT1
#endif
    res |= es8374_codec_set_voice_volume(0);

    res |= es8374_write_reg(0x37, 0x00); // dac set

    return res;
}

int es8374_codec_init(void)
{
    if (es8374_codec_initialized()) {
        ESP_LOGW(ES8374_TAG, "The es8374 codec has already been initialized!");
        return ESP_FAIL;
    }
    int res = 0;
    es_i2s_clock_t clkdiv;

    clkdiv.lclk_div = LCLK_DIV_48;
    //SCK = 2 * LRCK * CONFIG.SWIDTH 
    clkdiv.sclk_div = MCLK_DIV_3;

    i2s_init();

    i2c_init(); // ESP32 in master mode

    res |= es8374_stop(ES8374_MODULE_DEFAULT);
    res |= es8374_init_reg(ES8374_MODE_DEFAULT, (ES8374_BIT_LENGTH_DEFAULT << 4) | ES8374_I2S_FMT_DEFAULT, clkdiv,
            ES8374_OUTPUT_DEFAULT, ES8374_INPUT_DEFAULT);
#ifdef FEATURE_MIC_EN
    //res |= es8374_set_mic_gain(MIC_GAIN_15DB);
    //res |= es8374_set_d2se_pga(D2SE_PGA_GAIN_EN);
#endif
    /* already in es8374_init_reg(), no need to config again */
    //res |= es8374_config_fmt(ES8374_MODULE_DEFAULT, ES8374_I2S_FMT_DEFAULT);
    //res |= es8374_codec_config_i2s(ES8374_MODULE_DEFAULT, &(cfg->i2s_iface));
    codec_init_flag = 1;
    return res;
}

int es8374_codec_deinit(void)
{
    codec_init_flag = 0;
    return es8374_write_reg(0x00, 0x7F); // IC Reset and STOP
}

int es8374_codec_config_i2s(es_module_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    int res = 0;
    es_bits_length_t tmp = BIT_LENGTH_16BITS;
    res |= es8374_config_fmt(mode, iface->fmt);
    if (iface->bits == AUDIO_HAL_BIT_LENGTH_16BITS) {
        tmp = BIT_LENGTH_16BITS;
    } else if (iface->bits == AUDIO_HAL_BIT_LENGTH_24BITS) {
        tmp = BIT_LENGTH_24BITS;
    } else {
        tmp = BIT_LENGTH_32BITS;
    }
    res |= es8374_set_bits_per_sample(ES_MODULE_ADC_DAC, tmp);
    return res;
}

int es8374_codec_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    int res = 0;
    es_module_t es_mode_t = ES_MODULE_DAC;
    switch (mode) {
        case AUDIO_HAL_CODEC_MODE_ENCODE:
            es_mode_t  = ES_MODULE_ADC;
            break;
        case AUDIO_HAL_CODEC_MODE_LINE_IN:
            es_mode_t  = ES_MODULE_LINE;
            break;
        case AUDIO_HAL_CODEC_MODE_DECODE:
            es_mode_t  = ES_MODULE_DAC;
            break;
        case AUDIO_HAL_CODEC_MODE_BOTH:
            es_mode_t  = ES_MODULE_ADC_DAC;
            break;
        default:
            es_mode_t = ES_MODULE_DAC;
            ESP_LOGW(ES8374_TAG, "Codec mode not support, default is decode mode");
            break;
    }
    if (AUDIO_HAL_CTRL_STOP == ctrl_state) {
        res = es8374_stop(es_mode_t);
    } else {
        res = es8374_start(es_mode_t);
        ESP_LOGD(ES8374_TAG, "start default is decode mode:%d", es_mode_t);
    }
    return res;
}



