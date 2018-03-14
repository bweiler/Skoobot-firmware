/*
      Segger IDE and nRF BLE SDK integrated code. Start of project is example in BLE_Peripheral Template. 
      Segger ARM M version: 3.34
      Nordic Semi SDK nRF5 version: 14.2
      Softdevice (BLE stack, linked in as a precompiled hex file) is S132, version:

      Targets are Sparkfun nRF52832 dongle and Tiny Robot
      Tiny Robot has these features:
      1. VLX6180 distance sensor
      2. 2 motors, left and right, driven through 2 TI DRV8834
      3. Microphone
      4. Buzzer
      5. 1 LED
      6. BLE antenna, robot acts as a peripheral
      There is no LF oscillator, just a single 32Mhz crystaL, makes BLE setup non-standard for SDK

      I am using the nRF SDK characteristics battery_level and button/led "lbs"
      Button is the write byte, so it is like data, it is sent with sendoutbyteBLE()
      Led is read byte, so it can be a command, it is in g_jnbyte


*/
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nordic_common.h"
#include "nrf.h"
#include "app_error.h"
#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include "nrf_sdh_ble.h"
#include "ble_bas.h"        //Battery service test
#include "ble_dis.h"        //Device information structure
#include "app_timer.h"
#include "fds.h"
#include "peer_manager.h"
#include "bsp_btn_ble.h"
#include "sensorsim.h"
#include "ble_conn_state.h"
#include "ble_lbs.h"
#include "nrf_ble_gatt.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "arm_const_structs.h"
#include "nrf_drv_twi.h"
#include "nrf_delay.h"
#include "vl6180.h"
#include "nrf_drv_pdm.h"

#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2    /**< Reply when unsupported features are requested. */

#define DEVICE_NAME                     "Tiny Robot"                       /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME               "William Weiler Eng"                   /**< Manufacturer. Will be passed to Device Information Service. */
#define APP_ADV_INTERVAL                300                                     /**< The advertising interval (in units of 0.625 ms. This value corresponds to 187.5 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      180                                     /**< The advertising timeout in units of seconds. */

#define APP_BLE_OBSERVER_PRIO           3                                       /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_BLE_CONN_CFG_TAG            1                                       /**< A tag identifying the SoftDevice BLE configuration. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(100, UNIT_1_25_MS)        /**< Minimum acceptable connection interval (0.1 seconds). */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(200, UNIT_1_25_MS)        /**< Maximum acceptable connection interval (0.2 second). */
#define SLAVE_LATENCY                   0                                       /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)         /**< Connection supervisory timeout (4 seconds). */

#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000)                   /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000)                  /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                       /**< Number of attempts before giving up the connection parameter negotiation. */

#define SEC_PARAM_BOND                  1                                       /**< Perform bonding. */
#define SEC_PARAM_MITM                  0                                       /**< Man In The Middle protection not required. */
#define SEC_PARAM_LESC                  0                                       /**< LE Secure Connections not enabled. */
#define SEC_PARAM_KEYPRESS              0                                       /**< Keypress notifications not enabled. */
#define SEC_PARAM_IO_CAPABILITIES       BLE_GAP_IO_CAPS_NONE                    /**< No I/O capabilities. */
#define SEC_PARAM_OOB                   0                                       /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE          7                                       /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE          16                                      /**< Maximum encryption key size. */

#define DEAD_BEEF                       0xDEADBEEF                              /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

#define BATTERY_LEVEL_MEAS_INTERVAL         APP_TIMER_TICKS(2000)                   /**< Battery level measurement interval (ticks). */
#define MIN_BATTERY_LEVEL                   81                                      /**< Minimum simulated battery level. */
#define MAX_BATTERY_LEVEL                   100                                     /**< Maximum simulated 7battery level. */
#define BATTERY_LEVEL_INCREMENT             1    

BLE_LBS_DEF(m_lbs); 
BLE_BAS_DEF(m_bas);                                                 /**< Structure used to identify the battery service. */
APP_TIMER_DEF(m_battery_timer_id);
NRF_BLE_GATT_DEF(m_gatt);                                                       /**< GATT module instance. */
BLE_ADVERTISING_DEF(m_advertising);                                             /**< Advertising module instance. */

static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;                        /**< Handle of the current connection. */
static void log_init(void);
static void timers_init(void);
static void buttons_leds_init(bool *erase_bonds);
static void ble_stack_init(void);
static void gap_params_init(void);
static void gatt_init(void);
static void advertising_init(void);
static void services_init(void);
static void conn_params_init(void);
static void peer_manager_init(void);
static void application_timers_start(void);
static void sleep_mode_enter(void);
static void power_manage(void);
static void setoutbyteBLE(uint8_t sendbyte);
static void inbyte_handler(uint16_t conn_handle, ble_lbs_t * p_lbs, uint8_t inbyte);
uint8_t g_inbyte = 0;

// YOUR_JOB: Use UUIDs for service(s) used in your application.
static ble_uuid_t m_adv_uuids[] =                                               /**< Universally unique service identifiers. */
{
    {BLE_UUID_BATTERY_SERVICE,BLE_UUID_TYPE_BLE},    
    {BLE_UUID_DEVICE_INFORMATION_SERVICE, BLE_UUID_TYPE_BLE},
    {LBS_UUID_SERVICE, BLE_UUID_TYPE_BLE}
};


static void advertising_start(bool erase_bonds);

//Start of my section
//This is just a sparkfun reference I am using to develop code, it should always be 0
#define CB_TEST 0
#define MB_TEST 0
#define SPARKFUN 0
#if CB_TEST
#define SPARKFUN 0
#endif
#if MB_TEST
#define SPARKFUN 0
#endif

#define UART_RX_PIN   26
#define UART_TX_PIN   27


#if SPARKFUN

#define BUZZER_10MM   31

//VLX6180
//SHDN
#define GP0       24
//GPIO
#define GP1       23
#define I2C0_SCL  22
#define I2C0_SDA  20

//microphone
#define MIC_CLK   16
#define MIC_DI    17
#define MIC_SEL   18

//motors
#define M1        3
#define M0        12
#define DIR_L     2
#define STEP      1
#define DIR_R     4

//actually red
#define GREEN_LED 7

#else
//Buzzer
#define BUZZER_10MM   10

//microphone
#define MIC_CLK   28
#define MIC_DI    29
//this is placeholder, mic is tied low
#define MIC_SEL   27

//VLX6180
#define GP0       14
#define GP1       13
#define I2C0_SCL  15
#define I2C0_SDA  16

//motors
#define M1        25
#define M0        23
#define DIR_L     22
#define STEP      30
#define SLEEP     27
#if CB_TEST
#define DIR_R     24
#define GREEN_LED 7
#else
#define DIR_R     26
#define GREEN_LED 2
#endif
#endif

// TWI instance ID. 
#define TWI_INSTANCE_ID 0

/* Indicates if operation on TWI has ended. */
volatile bool m_xfer_done = false;

/* TWI instance. */
const nrf_drv_twi_t m_twi = NRF_DRV_TWI_INSTANCE(TWI_INSTANCE_ID);

/* Buffer for samples read from temperature sensor. */
static uint8_t m_sample;



#define GRAPH_WINDOW_HEIGHT              20                              //!< Graph window height used in draw function.

#define FPU_EXCEPTION_MASK               0x0000009F                      //!< FPU exception mask used to clear exceptions in FPSCR register.
#define FPU_FPSCR_REG_STACK_OFF          0x40                            //!< Offset of FPSCR register stacked during interrupt handling in FPU part stack.

// We want to use 44100 Hz sampling rate to reach 22050Hz band. 128 (64 pairs) samples are used
// in FFT calculation with result contains 64 bins (22050Hz/64bins -> ~344,5Hz per bin).
#define FFT_TEST_SAMPLE_FREQ_HZ          44100.0f                        //!< Frequency of complex input samples.
#define FFT_TEST_COMP_SAMPLES_LEN        128                             //!< Complex numbers input data array size. Correspond to FFT calculation this number must be power of two starting from 2^5 (2^4 pairs) with maximum value 2^13 (2^12 pairs).
//#define FFT_TEST_OUT_SAMPLES_LEN         (FFT_TEST_COMP_SAMPLES_LEN / 2) //!< Output array size.

//#define SIGNALS_RESOLUTION               100.0f                          //!< Sine wave frequency and noise amplitude resolution. To count resolution as decimal places in number use this formula: resolution = 1/SIGNALS_RESOLUTION .
//#define SINE_WAVE_FREQ_MAX               20000                           //!< Maximum frequency of generated sine wave.
//#define NOISE_AMPLITUDE                  1                               //!< Amplitude of generated noise added to signal.

static uint32_t  m_ifft_flag             = 0;                            //!< Flag that selects forward (0) or inverse (1) transform.
static uint32_t  m_do_bit_reverse        = 1;                            //!< Flag that enables (1) or disables (0) bit reversal of output.
static float32_t m_fft_input_f32[FFT_TEST_COMP_SAMPLES_LEN];             //!< FFT input array. Time domain.
static float32_t m_fft_output_f32[FFT_TEST_COMP_SAMPLES_LEN];             //!< FFT output data. Frequency domain.
void afunc(void);
void uart_init(void);
void sendbytes(uint8_t a);
uint8_t sendbuffer[64];
uint8_t recvbuffer[64];
void my_configure(void);
void configure_motors(void);
void motors_forward(void);
void motors_backward(void);
void motors_right(void);
void motors_left(void);
void configure_microphone(void);
void pwm_motor_stepping(uint32_t steps_per_second);
void pwm_buzzer_frequency(float freq);
void configure_VLX6180(void);
void VLX6180_init(void);
void read_sensor_data(void);
void twi_init(void);
void charging(void);
void stop_buzzer(void);
void stop_stepping(void);
void audio_handler(nrf_drv_pdm_evt_t const * const evt);
void cb_test(void);
void mb_test(void);
void stepping_mode(uint8_t mode);
void step_mode_experiment(void);

//#define SAMPLE_BUFFER_CNT 8096
#define SAMPLE_BUFFER_CNT 256
int16_t p_rx_buffer[SAMPLE_BUFFER_CNT];

/**
 * Motor might be 15 degrees per step, so 24 steps per revolution.
 * 83.3rpm = 15ms * 2 = 30ms period, 33Hz/24*60s
 */
int main(void)
{
    static uint8_t flipper = 0, lowhigh  = 0, range, stepmode;
    int16_t i;
    uint32_t cnt, cnt2;
    float freq[8] = { 440.0, 460.0, 470.0, 480.0, 2600, 2300, 2100, 1800 };
    bool erase_bonds;
    ret_code_t ret;

    //nrf_gpio_cfg(GREEN_LED,NRF_GPIO_PIN_DIR_OUTPUT,NRF_GPIO_PIN_INPUT_DISCONNECT,NRF_GPIO_PIN_PULLUP,NRF_GPIO_PIN_S0H1,NRF_GPIO_PIN_NOSENSE);

    nrf_gpio_cfg_output(GREEN_LED);
    nrf_gpio_pin_clear(GREEN_LED);
    
    //charging();    
 
    #if SPARKFUN
      uart_init();
    #endif
    #if CB_TEST
      uart_init();
      cb_test();
    #endif
    #if MB_TEST
      mb_test();
    #endif
    
    configure_VLX6180();

    #if SPARKFUN
    configure_microphone();
    #endif

    // Initialize.
    log_init();
    timers_init();
    //buttons_leds_init(&erase_bonds);
    ble_stack_init();
    gap_params_init();
    gatt_init();
    advertising_init();
    services_init();
    conn_params_init();
    peer_manager_init();

    // Start execution.
   // NRF_LOG_INFO("Template example started.");
    application_timers_start();

    advertising_start(erase_bonds);

    configure_motors();
    stepping_mode(1);
 
    my_configure();
   
 
    #if SPARKFUN
    //like 6700 samples in p_rx_buffer
    m_xfer_done = false;
    nrf_drv_pdm_buffer_set(p_rx_buffer, SAMPLE_BUFFER_CNT);
    nrf_drv_pdm_start();
    nrf_delay_ms(400);
    nrf_drv_pdm_stop();
    afunc();            //do fft, then check m_fft_output_f32 64 bytes
    #endif
   
    nrf_delay_ms(10000);     //wait 10 seconds, let me connect BLE
  
   // Enter main loop.
    cnt = 0;
    for (;;)
    {
        //for now is 1 second
        ++cnt;
        switch(cnt)
        {
          case 1:
            pwm_motor_stepping(200);    //go forward
            //pwm_buzzer_frequency(1000);
            break;
          case 2:
            //pwm_buzzer_frequency(2000);
            stop_stepping();
            motors_right();
            break;
          case 3:
            pwm_motor_stepping(100);    //turn right
            //pwm_buzzer_frequency(3000);
            break;
          case 4:
            stop_stepping();
            motors_forward();
            //stop_buzzer();
            pwm_motor_stepping(200);    //go forward
            break;
          case 5:
            stop_stepping();
            motors_right();
            break;
          case 6:
            pwm_motor_stepping(100);    //turn right
            break;
         case 7:
            stop_stepping();
            motors_forward();
            cnt = 0;
            break;
        }
        range = getDistance();
        if (range <= 50)
        {
          nrf_gpio_pin_clear(GREEN_LED);
        }
        nrf_delay_ms(1000);
        nrf_gpio_pin_set(GREEN_LED);
    
      // if (NRF_LOG_PROCESS() == false)
        {
            //power_manage();
        }
    }  

    while(1)
    {
        nrf_gpio_pin_set(GREEN_LED);
        nrf_delay_ms(300);
        nrf_gpio_pin_clear(GREEN_LED);
        nrf_delay_ms(300);

        do
        {
            __WFE();
        }while (m_xfer_done == false);

        read_sensor_data();
    }   
    configure_motors();
    my_configure();

    pwm_buzzer_frequency(2000);
    //pwm_motor_stepping(2000);
    while(1)
    {
      sendbytes(0);
      afunc();
      nrf_gpio_pin_set(GREEN_LED);
      nrf_delay_ms(4000);
      sendbytes(1);
      nrf_gpio_pin_clear(GREEN_LED);
      nrf_delay_ms(500);

    }
    cnt = 39;
    cnt2 = 0;
    i = 0;
    flipper = 0;
    while(1)
    {
         pwm_buzzer_frequency(freq[i++]);
         if (i == 8)
          i = 0;
  
        nrf_delay_ms(500);
        nrf_gpio_pin_clear(GREEN_LED);
        nrf_delay_ms(500);
        nrf_gpio_pin_set(GREEN_LED);
    }
    
}

void configure_microphone(void)
{
  nrf_gpio_cfg_output(MIC_SEL);
  nrf_gpio_pin_clear(MIC_SEL);    //Select Left Channel

  nrf_drv_pdm_config_t pdm_config = NRF_DRV_PDM_DEFAULT_CONFIG(MIC_CLK,MIC_DI);

  pdm_config.clock_freq = NRF_PDM_FREQ_1067K;
  pdm_config.mode = NRF_PDM_MODE_MONO;
  pdm_config.edge = NRF_PDM_EDGE_LEFTRISING;
  pdm_config.gain_l = NRF_PDM_GAIN_DEFAULT;
  pdm_config.gain_r = NRF_PDM_GAIN_DEFAULT;

  nrf_drv_pdm_init(&pdm_config, audio_handler);
 
  return; 
}

void step_mode_experiment(void)
{
    uint8_t stepmode;

    // Enter main loop.
    stepmode = 0;
    for (;;)
    {
        //for now is 1 second
        stepping_mode(stepmode);
        switch(stepmode)
        {
          case 0:
            pwm_motor_stepping(50);    //go forward
            nrf_gpio_pin_clear(GREEN_LED);
            break;
          case 1:
            pwm_motor_stepping(100);    //go forward
            nrf_gpio_pin_set(GREEN_LED);
            break;
          case 2:
            pwm_motor_stepping(200);    //go forward
            break;
          case 3:
            pwm_motor_stepping(400);    //go forward
            break;
          case 4:
            pwm_motor_stepping(800);    //go forward
            break;
          case 5:
            pwm_motor_stepping(1600);    //go forward
            break;
        }
        ++stepmode;
        if (stepmode == 6)
            stepmode = 0;
        nrf_delay_ms(2000);
        stop_stepping();
        nrf_delay_ms(500);
    }
}

void motors_forward(void)
{
  nrf_gpio_pin_clear(DIR_R);
  nrf_gpio_pin_set(DIR_L);
}

void motors_backward(void)
{
   nrf_gpio_pin_clear(DIR_L);
   nrf_gpio_pin_set(DIR_R);
}

void motors_right(void)
{
   nrf_gpio_pin_clear(DIR_L);
   nrf_gpio_pin_clear(DIR_R);
}

void motors_left(void)
{
   nrf_gpio_pin_set(DIR_L);
   nrf_gpio_pin_set(DIR_R);
}

void stepping_mode(uint8_t mode)
{
    switch(mode)
    {
        case 0:     //full step
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M0);
          nrf_gpio_pin_clear(M1);
          break;
        case 1:     //1/2 step
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M0);
          nrf_gpio_pin_clear(M1);
          break;
        case 2:     //1/4 step
          nrf_gpio_cfg_input(M0,NRF_GPIO_PIN_NOPULL);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M1);
          break;
        case 3:     //8 microsteps
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M0);
          nrf_gpio_pin_set(M1);
          break;
       case 4:     //16 microsteps
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M0);
          nrf_gpio_pin_set(M1);
          break;
       case 5:     //32 microsteps
          nrf_gpio_cfg_input(M0,NRF_GPIO_PIN_NOPULL);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M1);
          break;
      default:      //full step
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M0);
          nrf_gpio_pin_clear(M1);
          break;
    }
}

static void battery_level_update(void)
{

    ret_code_t err_code;
    uint8_t  battery_level;

    battery_level = 90;
    
    err_code = ble_bas_battery_level_update(&m_bas, battery_level);
    if ((err_code != NRF_SUCCESS) &&
        (err_code != NRF_ERROR_INVALID_STATE) &&
        (err_code != NRF_ERROR_RESOURCES) &&
        (err_code != BLE_ERROR_GATTS_SYS_ATTR_MISSING)
       )
    {
        APP_ERROR_HANDLER(err_code);
    }
}

static void battery_level_meas_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);
    battery_level_update();
}


void configure_VLX6180(void)
{
    twi_init();
    VLX6180_init();
}

void VLX6180_init(void)
{
    nrf_gpio_cfg_output(GP0);   //power up is chip enable
    nrf_gpio_pin_set(GP0);
    nrf_gpio_cfg_input(GP1,NRF_GPIO_PIN_NOPULL);    //open drain interrupt
  
    VL6180xInit();
    VL6180xDefautSettings();
}

__STATIC_INLINE void data_handler(uint8_t temp)
{

}

void twi_handler(nrf_drv_twi_evt_t const * p_event, void * p_context)
{
    switch (p_event->type)
    {
        case NRF_DRV_TWI_EVT_DONE:
            if (p_event->xfer_desc.type == NRF_DRV_TWI_XFER_RX)
            {
                data_handler(m_sample);
            }
            m_xfer_done = true;
            break;
        default:
            break;
    }
}

void twi_init(void)
{
    ret_code_t err_code;

    const nrf_drv_twi_config_t twi_VLX6180_config = {
       .scl                = I2C0_SCL,
       .sda                = I2C0_SDA,
       .frequency          = NRF_TWI_FREQ_100K,
       .interrupt_priority = APP_IRQ_PRIORITY_HIGH,
       .clear_bus_init     = false
    };

    err_code = nrf_drv_twi_init(&m_twi, &twi_VLX6180_config, NULL, NULL);

    nrf_drv_twi_enable(&m_twi);
}


void audio_handler(nrf_drv_pdm_evt_t const * const evt)
{
    m_xfer_done = true;
}

void pwm_motor_stepping(uint32_t steps_per_second)
{

  volatile static uint32_t pwm_duty[2], cnt_128k;

  cnt_128k = 128000 / steps_per_second;  
  pwm_duty[0] = pwm_duty[1] = cnt_128k / 2;

  NRF_PWM1->COUNTERTOP = (cnt_128k << PWM_COUNTERTOP_COUNTERTOP_Pos); 
  NRF_PWM1->SEQ[0].PTR = pwm_duty;
  NRF_PWM1->SEQ[0].CNT = (1 << PWM_SEQ_CNT_CNT_Pos);
  NRF_PWM1->TASKS_SEQSTART[0] = 1;   
}

//125000/freq=    125000/250
void pwm_buzzer_frequency(float freq)
{
  float main_freq;
  volatile static uint16_t pwm_top, pwm_duty[2];
   
  main_freq = 125000.0 / freq;

  pwm_top = (uint16_t)main_freq;
  pwm_duty[0] = pwm_duty[1] = (uint16_t)(main_freq / 2.0);

  NRF_PWM0->COUNTERTOP = (pwm_top << PWM_COUNTERTOP_COUNTERTOP_Pos); //1 msec
  NRF_PWM0->SEQ[0].PTR = (uint32_t)(pwm_duty);
  NRF_PWM0->SEQ[0].CNT = (1 << PWM_SEQ_CNT_CNT_Pos);
  NRF_PWM0->TASKS_SEQSTART[0] = 1;   
}

void cb_test(void)
{
  configure_motors();
  my_configure();

  for(;;)
  {
    nrf_gpio_pin_clear(GREEN_LED);
    pwm_motor_stepping(200);
    nrf_delay_ms(2000);
    stop_motors();
    nrf_gpio_pin_set(GREEN_LED);
    nrf_delay_ms(2000);
  }

}

void mb_test(void)
{
  configure_motors();
  nrf_gpio_cfg_output(MIC_CLK);
  nrf_gpio_cfg_output(MIC_DI);
  nrf_gpio_cfg_output(MIC_SEL);
  
  for(;;)
  {
    nrf_gpio_pin_clear(GREEN_LED);
    nrf_gpio_pin_clear(STEP);
    nrf_gpio_pin_clear(DIR_L);
    nrf_gpio_pin_clear(DIR_R);
    nrf_gpio_pin_clear(M1);
    nrf_gpio_pin_clear(M0);
    nrf_gpio_pin_clear(MIC_CLK);
    nrf_gpio_pin_clear(MIC_DI);
    nrf_gpio_pin_clear(MIC_SEL);
    nrf_delay_ms(500);
    nrf_gpio_pin_set(GREEN_LED);
    nrf_gpio_pin_set(STEP);
    nrf_gpio_pin_set(DIR_L);
    nrf_gpio_pin_set(DIR_R);
    nrf_gpio_pin_set(M1);
    nrf_gpio_pin_set(M0);
    nrf_gpio_pin_set(MIC_CLK);
    nrf_gpio_pin_set(MIC_DI);
    nrf_gpio_pin_set(MIC_SEL);
    nrf_delay_ms(500);
  }

}

void stop_buzzer(void)
{
  NRF_PWM0->TASKS_STOP = 1;
}

void stop_stepping(void)
{
  NRF_PWM1->TASKS_STOP = 1;
}

#if SPARKFUN || CB_TEST
void uart_init(void)
{
  NRF_UARTE0->BAUDRATE = 0x00275000; //9600bps
  NRF_UARTE0->PSEL.RXD = UART_RX_PIN;
  NRF_UARTE0->PSEL.TXD = UART_TX_PIN;
  NRF_UARTE0->TXD.PTR = sendbuffer;
  NRF_UARTE0->RXD.PTR = recvbuffer;
  NRF_UARTE0->TXD.MAXCNT = 20;
  NRF_UARTE0->RXD.MAXCNT = 20;
  NRF_UARTE0->CONFIG = 0;  //Enable  stop bit, no parity
  NRF_UARTE0->ENABLE = 8;
}
#endif

void my_configure(void)
{

  NRF_PWM1->PSEL.OUT[0] = (STEP << PWM_PSEL_OUT_PIN_Pos) | (PWM_PSEL_OUT_CONNECT_Connected << PWM_PSEL_OUT_CONNECT_Pos);
  NRF_PWM1->ENABLE = (PWM_ENABLE_ENABLE_Enabled << PWM_ENABLE_ENABLE_Pos);
  NRF_PWM1->MODE = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);
  NRF_PWM1->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_128 << PWM_PRESCALER_PRESCALER_Pos);
  NRF_PWM1->LOOP = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);
  NRF_PWM1->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
  NRF_PWM1->SEQ[0].REFRESH = 0;
  NRF_PWM1->SEQ[0].ENDDELAY = 0;
#if CB_TEST == 0
  nrf_gpio_cfg_output(BUZZER_10MM);
  nrf_gpio_pin_clear(BUZZER_10MM);
  NRF_PWM0->PSEL.OUT[0] = (BUZZER_10MM << PWM_PSEL_OUT_PIN_Pos) | (PWM_PSEL_OUT_CONNECT_Connected << PWM_PSEL_OUT_CONNECT_Pos);
  NRF_PWM0->ENABLE = (PWM_ENABLE_ENABLE_Enabled << PWM_ENABLE_ENABLE_Pos);
  NRF_PWM0->MODE = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);
  NRF_PWM0->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_128 << PWM_PRESCALER_PRESCALER_Pos);
  NRF_PWM0->LOOP = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);
  NRF_PWM0->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
  NRF_PWM0->SEQ[0].REFRESH = 0;
  NRF_PWM0->SEQ[0].ENDDELAY = 0;
#endif
}

// 32 microstep M1 high, M0 high-z
//
//
//
//
void configure_motors(void)
{
    nrf_gpio_cfg_output(STEP);
    nrf_gpio_cfg_output(DIR_L);
    nrf_gpio_cfg_output(DIR_R);
 
    stepping_mode(0);

    nrf_gpio_pin_clear(STEP);
    nrf_gpio_pin_set(DIR_L);
    nrf_gpio_pin_clear(DIR_R);
 }


void charging(void)
{
    for (;;)
    {
        nrf_gpio_pin_set(GREEN_LED);
        nrf_delay_ms(500);
        nrf_gpio_pin_clear(GREEN_LED);
        nrf_delay_ms(500);
    }
}

void sendbytes(uint8_t which)
{
  uint8_t i, j;
  static uint8_t buf[32];
  float32_t max_value, scalar_factor;
  uint32_t  max_val_index;
  int16_t actual_value;

  if (which == 0)
  {
    // Search FFT max value in input array.
    arm_max_f32(m_fft_output_f32, 64, &max_value, &max_val_index);

    scalar_factor = max_value / 100.0f;

    for(i=0;(i<64);i++)
    {
        sprintf(buf,"%d,",(uint8_t)(m_fft_output_f32[i]/scalar_factor));
        j=0;
        while(buf[j] != 0)
        {
            sendbuffer[j] = buf[j];
            ++j;
            if (j > 20)
            {
              j = 20;
              break;
            }
        }
        NRF_UARTE0->TXD.MAXCNT = j;
        NRF_UARTE0->TASKS_STARTTX = 1;
        nrf_delay_ms(20);
        while(NRF_UARTE0->EVENTS_ENDTX != 1);
     }
   }
   else
   {

    for(i=0;(i<64);i++)
    {
        actual_value = p_rx_buffer[i+500]; 
        sprintf(buf,"%d,",actual_value);
        j=0;
        while(buf[j] != 0)
        {
            sendbuffer[j] = buf[j];
            ++j;
            if (j > 20)
            {
              j = 20;
              break;
            }
        }
        NRF_UARTE0->TXD.MAXCNT = j;
        NRF_UARTE0->TASKS_STARTTX = 1;
        nrf_delay_ms(20);
        while(NRF_UARTE0->EVENTS_ENDTX != 1);
     }
   }
   sendbuffer[0] = 13;
   sendbuffer[1] = 10;
   NRF_UARTE0->TXD.MAXCNT = 2;
   NRF_UARTE0->TASKS_STARTTX = 1;

}

void afunc(void)
{
    uint16_t fftLen = FFT_TEST_COMP_SAMPLES_LEN;  //currently 128
    arm_status ret;
    arm_rfft_fast_instance_f32 S;
    uint32_t i;
    uint8_t ifftFlag  = 0;


    for(i=0;(i<FFT_TEST_COMP_SAMPLES_LEN);i+=2)
    {
          m_fft_input_f32[i] = p_rx_buffer[i+400];      //magnitude part        
    }

    ret = arm_rfft_fast_init_f32(&S,fftLen);
    
    arm_rfft_fast_f32(&S,m_fft_input_f32,m_fft_output_f32,ifftFlag);

    //arm_cfft_f32(p_input_struct, p_input, m_ifft_flag, m_do_bit_reverse);
    // Calculate the magnitude at each bin using Complex Magnitude Module function.
    //arm_cmplx_mag_f32(p_input, p_output, output_size);


#ifndef FPU_INTERRUPT_MODE
        /* Clear FPSCR register and clear pending FPU interrupts. This code is base on
         * nRF5x_release_notes.txt in documentation folder. It is necessary part of code when
         * application using power saving mode and after handling FPU errors in polling mode.
         */
        __set_FPSCR(__get_FPSCR() & ~(FPU_EXCEPTION_MASK));
        (void) __get_FPSCR();
        NVIC_ClearPendingIRQ(FPU_IRQn);
#endif

}


/**@brief Callback function for asserts in the SoftDevice.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num   Line number of the failing ASSERT call.
 * @param[in] file_name  File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(DEAD_BEEF, line_num, p_file_name);
}


/**@brief Function for handling Peer Manager events.
 *
 * @param[in] p_evt  Peer Manager event.
 */
static void pm_evt_handler(pm_evt_t const * p_evt)
{
    ret_code_t err_code;

    switch (p_evt->evt_id)
    {
        case PM_EVT_BONDED_PEER_CONNECTED:
        {
            NRF_LOG_INFO("Connected to a previously bonded device.");
        } break;

        case PM_EVT_CONN_SEC_SUCCEEDED:
        {
            NRF_LOG_INFO("Connection secured: role: %d, conn_handle: 0x%x, procedure: %d.",
                         ble_conn_state_role(p_evt->conn_handle),
                         p_evt->conn_handle,
                         p_evt->params.conn_sec_succeeded.procedure);
        } break;

        case PM_EVT_CONN_SEC_FAILED:
        {
            /* Often, when securing fails, it shouldn't be restarted, for security reasons.
             * Other times, it can be restarted directly.
             * Sometimes it can be restarted, but only after changing some Security Parameters.
             * Sometimes, it cannot be restarted until the link is disconnected and reconnected.
             * Sometimes it is impossible, to secure the link, or the peer device does not support it.
             * How to handle this error is highly application dependent. */
        } break;

        case PM_EVT_CONN_SEC_CONFIG_REQ:
        {
            // Reject pairing request from an already bonded peer.
            pm_conn_sec_config_t conn_sec_config = {.allow_repairing = false};
            pm_conn_sec_config_reply(p_evt->conn_handle, &conn_sec_config);
        } break;

        case PM_EVT_STORAGE_FULL:
        {
            // Run garbage collection on the flash.
            err_code = fds_gc();
            if (err_code == FDS_ERR_BUSY || err_code == FDS_ERR_NO_SPACE_IN_QUEUES)
            {
                // Retry.
            }
            else
            {
                APP_ERROR_CHECK(err_code);
            }
        } break;

        case PM_EVT_PEERS_DELETE_SUCCEEDED:
        {
            advertising_start(false);
        } break;

        case PM_EVT_LOCAL_DB_CACHE_APPLY_FAILED:
        {
            // The local database has likely changed, send service changed indications.
            pm_local_database_has_changed();
        } break;

        case PM_EVT_PEER_DATA_UPDATE_FAILED:
        {
            // Assert.
            APP_ERROR_CHECK(p_evt->params.peer_data_update_failed.error);
        } break;

        case PM_EVT_PEER_DELETE_FAILED:
        {
            // Assert.
            APP_ERROR_CHECK(p_evt->params.peer_delete_failed.error);
        } break;

        case PM_EVT_PEERS_DELETE_FAILED:
        {
            // Assert.
            APP_ERROR_CHECK(p_evt->params.peers_delete_failed_evt.error);
        } break;

        case PM_EVT_ERROR_UNEXPECTED:
        {
            // Assert.
            APP_ERROR_CHECK(p_evt->params.error_unexpected.error);
        } break;

        case PM_EVT_CONN_SEC_START:
        case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
        case PM_EVT_PEER_DELETE_SUCCEEDED:
        case PM_EVT_LOCAL_DB_CACHE_APPLIED:
        case PM_EVT_SERVICE_CHANGED_IND_SENT:
        case PM_EVT_SERVICE_CHANGED_IND_CONFIRMED:
        default:
            break;
    }
}


/**@brief Function for the Timer initialization.
 *
 * @details Initializes the timer module. This creates and starts application timers.
 */
static void timers_init(void)
{
    // Initialize timer module.
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    // Create timers.
    // Create timers.
    err_code = app_timer_create(&m_battery_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                battery_level_meas_timeout_handler);
    APP_ERROR_CHECK(err_code);
    /* YOUR_JOB: Create any timers to be used by the application.
                 Below is an example of how to create a timer.
                 For every new timer needed, increase the value of the macro APP_TIMER_MAX_TIMERS by
                 one.
       ret_code_t err_code;
       err_code = app_timer_create(&m_app_timer_id, APP_TIMER_MODE_REPEATED, timer_timeout_handler);
       APP_ERROR_CHECK(err_code); */
}


/**@brief Function for the GAP initialization.
 *
 * @details This function sets up all the necessary GAP (Generic Access Profile) parameters of the
 *          device including the device name, appearance, and the preferred connection parameters.
 */
static void gap_params_init(void)
{
    ret_code_t              err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);

    /* YOUR_JOB: Use an appearance value matching the application's use case.
       err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_);
       APP_ERROR_CHECK(err_code); */

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the GATT module.
 */
static void gatt_init(void)
{
    ret_code_t err_code = nrf_ble_gatt_init(&m_gatt, NULL);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling the YYY Service events.
 * YOUR_JOB implement a service handler function depending on the event the service you are using can generate
 *
 * @details This function will be called for all YY Service events which are passed to
 *          the application.
 *
 * @param[in]   p_yy_service   YY Service structure.
 * @param[in]   p_evt          Event received from the YY Service.
 *
 *
static void on_yys_evt(ble_yy_service_t     * p_yy_service,
                       ble_yy_service_evt_t * p_evt)
{
    switch (p_evt->evt_type)
    {
        case BLE_YY_NAME_EVT_WRITE:
            APPL_LOG("[APPL]: charact written with value %s. ", p_evt->params.char_xx.value.p_str);
            break;

        default:
            // No implementation needed.
            break;
    }

}*/

static void setoutbyteBLE(uint8_t sendbyte)
{          
  ret_code_t     err_code;
  
  err_code = ble_lbs_on_button_change(m_conn_handle, &m_lbs, sendbyte);

}

static void inbyte_handler(uint16_t conn_handle, ble_lbs_t * p_lbs, uint8_t inbyte)
{
    g_inbyte = inbyte;

}


/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    ret_code_t     err_code;
    ble_bas_init_t bas_init;
    ble_dis_init_t dis_init;
    ble_lbs_init_t init;

    // Initialize Battery Service.
    memset(&bas_init, 0, sizeof(bas_init));

    // Here the sec level for the Battery Service can be changed/increased.
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_char_attr_md.cccd_write_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_char_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&bas_init.battery_level_char_attr_md.write_perm);

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&bas_init.battery_level_report_read_perm);

    bas_init.evt_handler          = NULL;
    bas_init.support_notification = true;
    bas_init.p_report_ref         = NULL;
    bas_init.initial_batt_level   = 100;

    err_code = ble_bas_init(&m_bas, &bas_init);
    APP_ERROR_CHECK(err_code);

    // Initialize Device Information Service.
    memset(&dis_init, 0, sizeof(dis_init));

    ble_srv_ascii_to_utf8(&dis_init.manufact_name_str, (char *)MANUFACTURER_NAME);

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&dis_init.dis_attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&dis_init.dis_attr_md.write_perm);

    err_code = ble_dis_init(&dis_init);
    APP_ERROR_CHECK(err_code);
    
    init.led_write_handler = inbyte_handler;

    err_code = ble_lbs_init(&m_lbs, &init);
 
    /* YOUR_JOB: Add code to initialize the services used by the application.
       ret_code_t                         err_code;
       ble_xxs_init_t                     xxs_init;
       ble_yys_init_t                     yys_init;

       // Initialize XXX Service.
       memset(&xxs_init, 0, sizeof(xxs_init));

       xxs_init.evt_handler                = NULL;
       xxs_init.is_xxx_notify_supported    = true;
       xxs_init.ble_xx_initial_value.level = 100;

       err_code = ble_bas_init(&m_xxs, &xxs_init);
       APP_ERROR_CHECK(err_code);

       // Initialize YYY Service.
       memset(&yys_init, 0, sizeof(yys_init));
       yys_init.evt_handler                  = on_yys_evt;
       yys_init.ble_yy_initial_value.counter = 0;

       err_code = ble_yy_service_init(&yys_init, &yy_init);
       APP_ERROR_CHECK(err_code);
     */
}


/**@brief Function for handling the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module which
 *          are passed to the application.
 *          @note All this function does is to disconnect. This could have been done by simply
 *                setting the disconnect_on_fail config parameter, but instead we use the event
 *                handler mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    ret_code_t err_code;

    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}


/**@brief Function for handling a Connection Parameters error.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}


/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    ret_code_t             err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for starting timers.
 */
static void application_timers_start(void)
{
    /* YOUR_JOB: Start your timers. below is an example of how to start a timer.
       ret_code_t err_code;
       err_code = app_timer_start(m_app_timer_id, TIMER_INTERVAL, NULL);
       APP_ERROR_CHECK(err_code); */
    ret_code_t err_code;

    // Start application timers.
    err_code = app_timer_start(m_battery_timer_id, BATTERY_LEVEL_MEAS_INTERVAL, NULL);
    APP_ERROR_CHECK(err_code);

}


/**@brief Function for putting the chip into sleep mode.
 *
 * @note This function will not return.
 */
static void sleep_mode_enter(void)
{
    ret_code_t err_code;

    err_code = bsp_indication_set(BSP_INDICATE_IDLE);
    APP_ERROR_CHECK(err_code);

    // Prepare wakeup buttons.
    err_code = bsp_btn_ble_sleep_mode_prepare();
    APP_ERROR_CHECK(err_code);

    // Go to system-off mode (this function will not return; wakeup will cause a reset).
    err_code = sd_power_system_off();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    ret_code_t err_code;

    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("Fast advertising.");
            err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_ADV_EVT_IDLE:
            sleep_mode_enter();
            break;

        default:
            break;
    }
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t err_code = NRF_SUCCESS;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected.");
            // LED indication will be changed when advertising starts.
            break;

        case BLE_GAP_EVT_CONNECTED:
            NRF_LOG_INFO("Connected.");
            err_code = bsp_indication_set(BSP_INDICATE_CONNECTED);
            APP_ERROR_CHECK(err_code);
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            break;

#ifndef S140
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        } break;
#endif

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            NRF_LOG_DEBUG("GATT Client Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            NRF_LOG_DEBUG("GATT Server Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_EVT_USER_MEM_REQUEST:
            err_code = sd_ble_user_mem_reply(p_ble_evt->evt.gattc_evt.conn_handle, NULL);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
        {
            ble_gatts_evt_rw_authorize_request_t  req;
            ble_gatts_rw_authorize_reply_params_t auth_reply;

            req = p_ble_evt->evt.gatts_evt.params.authorize_request;

            if (req.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID)
            {
                if ((req.request.write.op == BLE_GATTS_OP_PREP_WRITE_REQ)     ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL))
                {
                    if (req.type == BLE_GATTS_AUTHORIZE_TYPE_WRITE)
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
                    }
                    else
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                    }
                    auth_reply.params.write.gatt_status = APP_FEATURE_NOT_SUPPORTED;
                    err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                               &auth_reply);
                    APP_ERROR_CHECK(err_code);
                }
            }
        } break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST

        default:
            // No implementation needed.
            break;
    }
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}


/**@brief Function for the Peer Manager initialization.
 */
static void peer_manager_init(void)
{
    ble_gap_sec_params_t sec_param;
    ret_code_t           err_code;

    err_code = pm_init();
    APP_ERROR_CHECK(err_code);

    memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));

    // Security parameters to be used for all security procedures.
    sec_param.bond           = SEC_PARAM_BOND;
    sec_param.mitm           = SEC_PARAM_MITM;
    sec_param.lesc           = SEC_PARAM_LESC;
    sec_param.keypress       = SEC_PARAM_KEYPRESS;
    sec_param.io_caps        = SEC_PARAM_IO_CAPABILITIES;
    sec_param.oob            = SEC_PARAM_OOB;
    sec_param.min_key_size   = SEC_PARAM_MIN_KEY_SIZE;
    sec_param.max_key_size   = SEC_PARAM_MAX_KEY_SIZE;
    sec_param.kdist_own.enc  = 1;
    sec_param.kdist_own.id   = 1;
    sec_param.kdist_peer.enc = 1;
    sec_param.kdist_peer.id  = 1;

    err_code = pm_sec_params_set(&sec_param);
    APP_ERROR_CHECK(err_code);

    err_code = pm_register(pm_evt_handler);
    APP_ERROR_CHECK(err_code);
}


/**@brief Clear bond information from persistent storage.
 */
static void delete_bonds(void)
{
    ret_code_t err_code;

    NRF_LOG_INFO("Erase bonds!");

    err_code = pm_peers_delete();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for handling events from the BSP module.
 *
 * @param[in]   event   Event generated when button is pressed.
 */
static void bsp_event_handler(bsp_event_t event)
{
    ret_code_t err_code;

    switch (event)
    {
        case BSP_EVENT_SLEEP:
            sleep_mode_enter();
            break; // BSP_EVENT_SLEEP

        case BSP_EVENT_DISCONNECT:
            err_code = sd_ble_gap_disconnect(m_conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            if (err_code != NRF_ERROR_INVALID_STATE)
            {
                APP_ERROR_CHECK(err_code);
            }
            break; // BSP_EVENT_DISCONNECT

        case BSP_EVENT_WHITELIST_OFF:
            if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
            {
                err_code = ble_advertising_restart_without_whitelist(&m_advertising);
                if (err_code != NRF_ERROR_INVALID_STATE)
                {
                    APP_ERROR_CHECK(err_code);
                }
            }
            break; // BSP_EVENT_KEY_0

        default:
            break;
    }
}


/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    ret_code_t             err_code;
    ble_advertising_init_t init;

    memset(&init, 0, sizeof(init));

    init.advdata.name_type               = BLE_ADVDATA_FULL_NAME;
    init.advdata.include_appearance      = true;
    init.advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    init.advdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    init.advdata.uuids_complete.p_uuids  = m_adv_uuids;

    init.config.ble_adv_fast_enabled  = true;
    init.config.ble_adv_fast_interval = APP_ADV_INTERVAL;
    init.config.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

    init.evt_handler = on_adv_evt;

    err_code = ble_advertising_init(&m_advertising, &init);
    APP_ERROR_CHECK(err_code);

    ble_advertising_conn_cfg_tag_set(&m_advertising, APP_BLE_CONN_CFG_TAG);
}


/**@brief Function for initializing buttons and leds.
 *
 * @param[out] p_erase_bonds  Will be true if the clear bonding button was pressed to wake the application up.
 */
static void buttons_leds_init(bool * p_erase_bonds)
{
    ret_code_t err_code;
    bsp_event_t startup_event;

    err_code = bsp_init(BSP_INIT_LED | BSP_INIT_BUTTONS, bsp_event_handler);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_btn_ble_init(NULL, &startup_event);
    APP_ERROR_CHECK(err_code);

    *p_erase_bonds = (startup_event == BSP_EVENT_CLEAR_BONDING_DATA);
}


/**@brief Function for initializing the nrf log module.
 */
static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_DEFAULT_BACKENDS_INIT();
}


/**@brief Function for the Power manager.
 */
static void power_manage(void)
{
    ret_code_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for starting advertising.
 */
static void advertising_start(bool erase_bonds)
{
    if (erase_bonds == true)
    {
        delete_bonds();
        // Advertising is started by PM_EVT_PEERS_DELETED_SUCEEDED evetnt
    }
    else
    {
        ret_code_t err_code = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);

        APP_ERROR_CHECK(err_code);
    }
}

