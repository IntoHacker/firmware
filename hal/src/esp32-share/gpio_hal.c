/**
******************************************************************************
Copyright (c) 2013-2014 IntoRobot Team.  All right reserved.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, see <http://www.gnu.org/licenses/>.
******************************************************************************
*/

/* Includes -----------------------------------------------------------------*/
#include "hw_config.h"
#include "gpio_hal.h"
#include "pinmap_impl.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"

#include "esp32-hal-gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "esp_attr.h"
#include "esp_intr.h"
#include "rom/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/rtc_io_reg.h"

/* #include "esp32-hal.h" */


PinMode digitalPinModeSaved = PIN_MODE_NONE;

#define ETS_GPIO_INUM       12

const int8_t esp32_adc2gpio[20] = {36, -1, -1, 39, 32, 33, 34, 35, -1, -1, 4, 0, 2, 15, 13, 12, 14, 27, 25, 26};

const DRAM_ATTR esp32_gpioMux_t esp32_gpioMux[GPIO_PIN_COUNT]={
    {0x44, 11, 11, 1},
    {0x88, -1, -1, -1},
    {0x40, 12, 12, 2},
    {0x84, -1, -1, -1},
    {0x48, 10, 10, 0},
    {0x6c, -1, -1, -1},
    {0x60, -1, -1, -1},
    {0x64, -1, -1, -1},
    {0x68, -1, -1, -1},
    {0x54, -1, -1, -1},
    {0x58, -1, -1, -1},
    {0x5c, -1, -1, -1},
    {0x34, 15, 15, 5},
    {0x38, 14, 14, 4},
    {0x30, 16, 16, 6},
    {0x3c, 13, 13, 3},
    {0x4c, -1, -1, -1},
    {0x50, -1, -1, -1},
    {0x70, -1, -1, -1},
    {0x74, -1, -1, -1},
    {0x78, -1, -1, -1},
    {0x7c, -1, -1, -1},
    {0x80, -1, -1, -1},
    {0x8c, -1, -1, -1},
    {0, -1, -1, -1},
    {0x24, 6, 18, -1}, //DAC1
    {0x28, 7, 19, -1}, //DAC2
    {0x2c, 17, 17, 7},
    {0, -1, -1, -1},
    {0, -1, -1, -1},
    {0, -1, -1, -1},
    {0, -1, -1, -1},
    {0x1c, 9, 4, 9},
    {0x20, 8, 5, 8},
    {0x14, 4, 6, -1},
    {0x18, 5, 7, -1},
    {0x04, 0, 0, -1},
    {0x08, 1, -1, -1},
    {0x0c, 2, -1, -1},
    {0x10, 3, 3, -1}
};

typedef void (*voidFuncPtr)(void);
static voidFuncPtr __pinInterruptHandlers[GPIO_PIN_COUNT] = {0,};

#include "driver/rtc_io.h"

/* Private function prototypes ----------------------------------------------*/

inline bool is_valid_pin(pin_t pin) __attribute__((always_inline));
inline bool is_valid_pin(pin_t pin)
{
    return (pin < TOTAL_PINS);
}

PinMode HAL_Get_Pin_Mode(pin_t pin)
{
    return (!is_valid_pin(pin)) ? PIN_MODE_NONE : HAL_Pin_Map()[pin].pin_mode;
}

PinFunction HAL_Validate_Pin_Function(pin_t pin, PinFunction pinFunction)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();

    if (!is_valid_pin(pin))
        return PF_NONE;
    /* if (pinFunction==PF_ADC && PIN_MAP[pin].adc_channel!=ADC_CHANNEL_NONE) */
    if (pinFunction==PF_ADC && PIN_MAP[pin].pin_mode==ANALOG)
        return PF_ADC;
    if (pinFunction==PF_TIMER && PIN_MAP[pin].timer_peripheral!=NONE)
        return PF_TIMER;
    return PF_DIO;
}

/*
 * @brief Set the mode of the pin to OUTPUT, INPUT, INPUT_PULLUP,
 * or INPUT_PULLDOWN
 */
void HAL_Pin_Mode(pin_t pin, PinMode setMode)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(!digitalPinIsValid(gpio_pin)) {
        return;
    }

    uint32_t rtc_reg = rtc_gpio_desc[gpio_pin].reg;
    if(setMode == ANALOG) {

        PIN_MAP[pin].pin_mode = ANALOG;

        if(!rtc_reg) {
            return;//not rtc pin
        }
        //lock rtc
        uint32_t reg_val = ESP_REG(rtc_reg);
        if(reg_val & rtc_gpio_desc[gpio_pin].mux){
            return;//already in adc mode
        }
        reg_val &= ~(
                (RTC_IO_TOUCH_PAD1_FUN_SEL_V << rtc_gpio_desc[gpio_pin].func)
                |rtc_gpio_desc[gpio_pin].ie
                |rtc_gpio_desc[gpio_pin].pullup
                |rtc_gpio_desc[gpio_pin].pulldown);
        ESP_REG(RTC_GPIO_ENABLE_W1TC_REG) = (1 << (rtc_gpio_desc[gpio_pin].rtc_num + RTC_GPIO_ENABLE_W1TC_S));
        ESP_REG(rtc_reg) = reg_val | rtc_gpio_desc[gpio_pin].mux;
        //unlock rtc
        ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioMux[gpio_pin].reg) = ((uint32_t)2 << MCU_SEL_S) | ((uint32_t)2 << FUN_DRV_S) | FUN_IE;
        return;
    } else if(rtc_reg) {
        //lock rtc
        ESP_REG(rtc_reg) = ESP_REG(rtc_reg) & ~(rtc_gpio_desc[gpio_pin].mux);
        if(setMode & PULLUP) {
            ESP_REG(rtc_reg) = (ESP_REG(rtc_reg) | rtc_gpio_desc[gpio_pin].pullup) & ~(rtc_gpio_desc[gpio_pin].pulldown);
        } else if(setMode & PULLDOWN) {
            ESP_REG(rtc_reg) = (ESP_REG(rtc_reg) | rtc_gpio_desc[gpio_pin].pulldown) & ~(rtc_gpio_desc[gpio_pin].pullup);
        } else {
            ESP_REG(rtc_reg) = ESP_REG(rtc_reg) & ~(rtc_gpio_desc[gpio_pin].pullup | rtc_gpio_desc[gpio_pin].pulldown);
        }
        //unlock rtc
    }

    uint32_t pinFunction = 0, pinControl = 0;

    //lock gpio
    if(setMode & INPUT) {
        if(gpio_pin < 32) {
            GPIO.enable_w1tc = ((uint32_t)1 << gpio_pin);
        } else {
            GPIO.enable1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
        }

        PIN_MAP[pin].pin_mode = INPUT;

        /* if(setMode & PULLUP) { */
        if(setMode & INPUT_PULLUP) {
            pinFunction |= FUN_PU;

            PIN_MAP[pin].pin_mode = INPUT_PULLUP;
        /* } else if(setMode & PULLDOWN) { */
        } else if(setMode & INPUT_PULLDOWN) {
            pinFunction |= FUN_PD;

            PIN_MAP[pin].pin_mode = INPUT_PULLDOWN;
        }

    } else if(setMode & OUTPUT) {
        if(gpio_pin > 33){
            //unlock gpio
            return;//pins above 33 can be only inputs
        } else if(gpio_pin < 32) {
            GPIO.enable_w1ts = ((uint32_t)1 << gpio_pin);
        } else {
            GPIO.enable1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
        }

        PIN_MAP[pin].pin_mode = OUTPUT;
    }

    pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers?
    pinFunction |= FUN_IE;//input enable but required for output as well?

    if(setMode & (INPUT | OUTPUT)) {
        pinFunction |= ((uint32_t)2 << MCU_SEL_S);
    } else if(setMode == SPECIAL) {
        pinFunction |= ((uint32_t)(((gpio_pin)==1||(gpio_pin)==3)?0:1) << MCU_SEL_S);
    } else {
        pinFunction |= ((uint32_t)(setMode >> 5) << MCU_SEL_S);
    }

    ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioMux[gpio_pin].reg) = pinFunction;

    if(setMode & OPEN_DRAIN) {
        pinControl = (1 << GPIO_PIN0_PAD_DRIVER_S);
    }

    GPIO.pin[gpio_pin].val = pinControl;
}

/*
 * @brief Saves a pin mode to be recalled later.
 */
void HAL_GPIO_Save_Pin_Mode(PinMode mode)
{
    digitalPinModeSaved = mode;
}

/*
 * @brief Recalls a saved pin mode.
 */
PinMode HAL_GPIO_Recall_Pin_Mode()
{
    return digitalPinModeSaved;
}

/*
 * @brief Sets a GPIO pin to HIGH or LOW.
 */
void HAL_GPIO_Write(uint16_t pin, uint8_t value)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if (gpio_pin > 39) {
        return;
    }

    if(value) {
        if(gpio_pin < 32) {
            GPIO.out_w1ts = ((uint32_t)1 << gpio_pin);
        } else  if(pin < 34){
            GPIO.out1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
        }
    } else {
        if(gpio_pin < 32) {
            GPIO.out_w1tc = ((uint32_t)1 << gpio_pin);
        } else if(pin< 34){
            GPIO.out1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
        }
    }
}

/*
 * @brief Reads the value of a GPIO pin. Should return either 1 (HIGH) or 0 (LOW).
 */
int32_t HAL_GPIO_Read(uint16_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin < 32) {
        return (GPIO.in >> gpio_pin) & 0x1;
    } else if(pin < 40){
        return (GPIO.in1.val >> (gpio_pin - 32)) & 0x1;
    }
    return 0;
}

/*
 * @brief   blocking call to measure a high or low pulse
 * @returns uint32_t pulse width in microseconds up to 3 seconds,
 *          returns 0 on 3 second timeout error, or invalid pin.
 */
uint32_t HAL_Pulse_In(pin_t pin, uint16_t value)
{
    #if 0
    EESP32_Pin_Info* SOLO_PIN_MAP = HAL_Pin_Map();
#define pinReadFast(_pin) HAL_pinReadFast(_pin)

    // FIXME: SYTME_TICK_COUNTER change to system_get_time which return the micro seconds
    volatile uint32_t timeoutStart = system_get_time(); // total 3 seconds for entire function!

    /* If already on the value we want to measure, wait for the next one.
     * Time out after 3 seconds so we don't block the background tasks
     */
    while (pinReadFast(pin) == value) {
        if (sytem_get_time() - timeoutStart > 3000000UL) {
            return 0;
        }
    }

    /* Wait until the start of the pulse.
     * Time out after 3 seconds so we don't block the background tasks
     */
    while (pinReadFast(pin) != value) {
        if (system_get_time() - timeoutStart > 3000000UL) {
            return 0;
        }
    }

    /* Wait until this value changes, this will be our elapsed pulse width.
     * Time out after 3 seconds so we don't block the background tasks
     */
    volatile uint32_t pulseStart = system_get_time();
    while (pinReadFast(pin) == value) {
        if (system_get_time()- timeoutStart > 3000000UL) {
            return 0;
        }
    }

    return (system_get_time() - pulseStart);
    #endif
    return 0;
}

void HAL_pinSetFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin < 32) {
        GPIO.out_w1ts = ((uint32_t)1 << gpio_pin);
    } else {
        GPIO.out1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
    }
}

void HAL_pinResetFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;
    if(gpio_pin > 39) {
        return 0;
    }

    if(gpio_pin < 32) {
        GPIO.out_w1tc = ((uint32_t)1 << gpio_pin);
    } else {
        GPIO.out1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
    }
}

int32_t HAL_pinReadFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin > 39) {
        return 0;
    }
    if(gpio_pin < 32) {
        return (GPIO.in >> gpio_pin) & 0x1;
    } else {
        return (GPIO.in1.val >> (gpio_pin - 32)) & 0x1;
    }
}


#if   0
// old sdk
#define ESP_REG(addr) *((volatile uint32_t *)(addr))

#define NOP() asm volatile ("nop")

PinMode digitalPinModeSaved = PIN_MODE_NONE;

const uint8_t esp32_gpioToFn[40] = {
    0x44,//0
    0x88,//1
    0x40,//2
    0x84,//3
    0x48,//4
    0x6c,//5
    0x60,//6
    0x64,//7
    0x68,//8
    0x54,//9
    0x58,//10
    0x5c,//11
    0x34,//12
    0x38,//13
    0x30,//14
    0x3c,//15
    0x4c,//16
    0x50,//17
    0x70,//18
    0x74,//19
    0x78,//20
    0x7c,//21
    0x80,//22
    0x8c,//23
    0xFF,//N/A
    0x24,//25
    0x28,//26
    0x2c,//27
    0xFF,//N/A
    0xFF,//N/A
    0xFF,//N/A
    0xFF,//N/A
    0x1c,//32
    0x20,//33
    0x14,//34
    0x18,//35
    0x04,//36
    0x08,//37
    0x0c,//38
    0x10 //39
};
/* Extern variables ---------------------------------------------------------*/


/* Private function prototypes ----------------------------------------------*/

inline bool is_valid_pin(pin_t pin) __attribute__((always_inline));
inline bool is_valid_pin(pin_t pin)
{
    return (pin < TOTAL_PINS);
}

PinMode HAL_Get_Pin_Mode(pin_t pin)
{
    return (!is_valid_pin(pin)) ? PIN_MODE_NONE : HAL_Pin_Map()[pin].pin_mode;
}

PinFunction HAL_Validate_Pin_Function(pin_t pin, PinFunction pinFunction)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();

    if (!is_valid_pin(pin))
        return PF_NONE;
    if (pinFunction==PF_ADC && PIN_MAP[pin].adc_channel!=ADC_CHANNEL_NONE)
        return PF_ADC;
    if (pinFunction==PF_TIMER && PIN_MAP[pin].timer_peripheral!=NONE)
        return PF_TIMER;
    return PF_DIO;
}

/*
 * @brief Set the mode of the pin to OUTPUT, INPUT, INPUT_PULLUP,
 * or INPUT_PULLDOWN
 */
void HAL_Pin_Mode(pin_t pin, PinMode setMode)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if (gpio_pin > 39 || esp32_gpioToFn[gpio_pin] == 0xFF) {
    /* if (gpio_pin > 39 || esp32_gpioMux_t[gpio_pin] == 0xFF) { */
        return;
    }

    uint32_t pinFunction = 0, pinControl = 0;

    switch (setMode)
    {
        case OUTPUT:
            if(gpio_pin < 32) {
                GPIO.enable_w1ts = ((uint32_t)1 << gpio_pin);
            } else {
                GPIO.enable1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
            }
            pinFunction |= ((uint32_t)2 << MCU_SEL_S);

            // common part
            pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers?
            pinFunction |= FUN_IE;//input enable but required for output as well?
            ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioToFn[gpio_pin]) = pinFunction;
            GPIO.pin[gpio_pin].val = pinControl;

            PIN_MAP[pin].pin_mode = OUTPUT;
            break;

        case INPUT:
            if(gpio_pin < 32) {
                GPIO.enable_w1tc = ((uint32_t)1 << gpio_pin);
            } else {
                GPIO.enable1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
            }
            pinFunction |= ((uint32_t)2 << MCU_SEL_S);

            // common part
            pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers?
            pinFunction |= FUN_IE;//input enable but required for output as well?
            ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioToFn[gpio_pin]) = pinFunction;
            GPIO.pin[gpio_pin].val = pinControl;

            PIN_MAP[pin].pin_mode = INPUT;
            break;

        case INPUT_PULLUP:
            if(gpio_pin < 32) {
                GPIO.enable_w1tc = ((uint32_t)1 << gpio_pin);
            } else {
                GPIO.enable1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
            }
            pinFunction |= FUN_PU;
            pinFunction |= ((uint32_t)2 << MCU_SEL_S);

            // common part
            pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers?
            pinFunction |= FUN_IE;//input enable but required for output as well?
            ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioToFn[gpio_pin]) = pinFunction;
            GPIO.pin[gpio_pin].val = pinControl;

            PIN_MAP[pin].pin_mode = INPUT_PULLUP;
            break;

        case INPUT_PULLDOWN:
            if(gpio_pin < 32) {
                GPIO.enable_w1tc = ((uint32_t)1 << gpio_pin);
            } else {
                GPIO.enable1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
            }
            pinFunction |= FUN_PD;
            pinFunction |= ((uint32_t)2 << MCU_SEL_S);

            // common part
            pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers?
            pinFunction |= FUN_IE;//input enable but required for output as well?
            ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioToFn[gpio_pin]) = pinFunction;
            GPIO.pin[gpio_pin].val = pinControl;

            PIN_MAP[pin].pin_mode = INPUT_PULLDOWN;
            break;

        default:
            break;
    }

    /* // common part */
    /* pinFunction |= ((uint32_t)2 << FUN_DRV_S);//what are the drivers? */
    /* pinFunction |= FUN_IE;//input enable but required for output as well? */

    /* ESP_REG(DR_REG_IO_MUX_BASE + esp32_gpioToFn[gpio_pin]) = pinFunction; */
    /* GPIO.pin[gpio_pin].val = pinControl; */
}

/*
 * @brief Saves a pin mode to be recalled later.
 */
void HAL_GPIO_Save_Pin_Mode(PinMode mode)
{
    digitalPinModeSaved = mode;
}

/*
 * @brief Recalls a saved pin mode.
 */
PinMode HAL_GPIO_Recall_Pin_Mode()
{
    return digitalPinModeSaved;
}

/*
 * @brief Sets a GPIO pin to HIGH or LOW.
 */
void HAL_GPIO_Write(uint16_t pin, uint8_t value)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if (gpio_pin > 39) {
        return;
    }

    if(value) {
        if(gpio_pin < 32) {
            GPIO.out_w1ts = ((uint32_t)1 << gpio_pin);
        } else {
            GPIO.out1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
        }
    } else {
        if(gpio_pin < 32) {
            GPIO.out_w1tc = ((uint32_t)1 << gpio_pin);
        } else {
            GPIO.out1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
        }
    }
}

/*
 * @brief Reads the value of a GPIO pin. Should return either 1 (HIGH) or 0 (LOW).
 */
int32_t HAL_GPIO_Read(uint16_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin < 32) {
        return (GPIO.in >> gpio_pin) & 0x1;
    } else {
        return (GPIO.in1.val >> (gpio_pin - 32)) & 0x1;
    }
}

/*
 * @brief   blocking call to measure a high or low pulse
 * @returns uint32_t pulse width in microseconds up to 3 seconds,
 *          returns 0 on 3 second timeout error, or invalid pin.
 */
uint32_t HAL_Pulse_In(pin_t pin, uint16_t value)
{
    #if 0
    EESP32_Pin_Info* SOLO_PIN_MAP = HAL_Pin_Map();
#define pinReadFast(_pin) HAL_pinReadFast(_pin)

    // FIXME: SYTME_TICK_COUNTER change to system_get_time which return the micro seconds
    volatile uint32_t timeoutStart = system_get_time(); // total 3 seconds for entire function!

    /* If already on the value we want to measure, wait for the next one.
     * Time out after 3 seconds so we don't block the background tasks
     */
    while (pinReadFast(pin) == value) {
        if (sytem_get_time() - timeoutStart > 3000000UL) {
            return 0;
        }
    }

    /* Wait until the start of the pulse.
     * Time out after 3 seconds so we don't block the background tasks
     */
    while (pinReadFast(pin) != value) {
        if (system_get_time() - timeoutStart > 3000000UL) {
            return 0;
        }
    }

    /* Wait until this value changes, this will be our elapsed pulse width.
     * Time out after 3 seconds so we don't block the background tasks
     */
    volatile uint32_t pulseStart = system_get_time();
    while (pinReadFast(pin) == value) {
        if (system_get_time()- timeoutStart > 3000000UL) {
            return 0;
        }
    }

    return (system_get_time() - pulseStart);
    #endif
    return 0;
}

void HAL_pinSetFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin < 32) {
        GPIO.out_w1ts = ((uint32_t)1 << gpio_pin);
    } else {
        GPIO.out1_w1ts.val = ((uint32_t)1 << (gpio_pin - 32));
    }
}

void HAL_pinResetFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;
    if(gpio_pin > 39) {
        return 0;
    }

    if(gpio_pin < 32) {
        GPIO.out_w1tc = ((uint32_t)1 << gpio_pin);
    } else {
        GPIO.out1_w1tc.val = ((uint32_t)1 << (gpio_pin - 32));
    }
}

int32_t HAL_pinReadFast(pin_t pin)
{
    EESP32_Pin_Info* PIN_MAP = HAL_Pin_Map();
    pin_t gpio_pin = PIN_MAP[pin].gpio_pin;

    if(gpio_pin > 39) {
        return 0;
    }
    if(gpio_pin < 32) {
        return (GPIO.in >> gpio_pin) & 0x1;
    } else {
        return (GPIO.in1.val >> (gpio_pin - 32)) & 0x1;
    }
}

#endif
