/*
      Tiny Robot Code, v0.2
      by Bill Weiler

      Segger Embedded Studio - full version is free for Nordic chips, using ARM M version v3.34
      Nordic nRF5 SDK v14.2
      Nordic SDK BLE_Peripheral_Template example used as starting point 
      CMSIS DSP Library
      Nordic chip: nRF52832 QFN48 AA
      nRF52832 runs at 64Mhz, has 512k flash and 64k SRAM, and hardware single precision floating point
      Nordic BLE Stack called Softdevice, is S132 v5.0.0, linked a hex file

      Targets for this code are Tiny Robot and Sparkfun nRF52832 reference design
      Tiny Robot has these features:
      1. VLX6180 distance sensor through i2c
      2. 2 motors, left and right, driven through 2 TI DRV8834 driven by uC gpio's, plus the PWM0 peripheral for stepping
      3. Microphone through PDM peripheral
      4. Buzzer through PWM1 peripheral
      5. 1 LED through gpio, active low (clearing it (0) turns led on)
      6. BLE antenna, s132 supports central and peripheral, observer and broadcaster, total 20 connections (that's 19 robots and a cellphone!)

      There is no crystal LF oscillator in Tiny Robot, the softdevice uses the internal RC oscillator, there is a 32Mhz crystal HF oscillator for CPU

      GCC is not always smart enough, use idioms or intrinsics for floating point, or just check disassembly to make sure FPU instructions were generated.

      Watch out modifying code, the BLE stack takes peripherals and scheduling. Notably it takes Timer0, some flash and ram, and a lot of cpu cycles. If you
      have problems, use their API for scheduling time slots. Check the Softdevice documentation for blocked peripherals.

      Read this stuff:
      1. nRF52832 datasheet, watch out for old versions, like on Sparkfun, I am using v1.4
      2. Softdevice Specification
      3. Errata
      4. Softdevice API - online only
      5. Nordic devzone forums
      6. Compatability list (really boring but useful)
      7. VLX6180 datasheet
      8. Microphone - stuff about PDM and PCM, can play raw audio you grab out of IDE in Audacity (free download)
*/
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "sdk_common.h"
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
#include "app_timer.h"
#include "fds.h"
#include "peer_manager.h"
#include "ble_conn_state.h"
#include "nrf_ble_gatt.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "arm_const_structs.h"
#include "nrf_drv_twi.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_pwm.h"
#include "nrf_timer.h"
#include "vl6180.h"
#include "nrf_drv_pdm.h"

//COMMAND SET FOR BLE
#define MOTORS_RIGHT      0x10
#define MOTORS_LEFT       0x11
#define MOTORS_FORWARD    0x12
#define MOTORS_BACKWARD   0x13
#define MOTORS_STOP       0x14
#define MOTORS_SLEEP      0x15
#define INC_STEP_MODE     0x16
#define PLAY_BUZZER       0x17

#define GET_DISTANCE      0x22
#define GET_AMBIENT       0x21

#define RECORD_SOUND      0x30
#define INCREASE_GAIN     0x31
#define DECREASE_GAIN     0x32

#define ROVER_MODE        0x40
/*
    Conditional compilation
    ========================================================
    MB_TEST 1 = mainboard (or frontboard FB) tester
    SPARKFUN 1 = Sparkfun reference used for testing BB
    SPARTFUN 0 = Tiny Robot firmware
*/
#define MB_TEST 0
#define MICROPHONE 0
#define SPARKFUN 0
#if MB_TEST
#define SPARKFUN 0
#endif

#if SPARKFUN

#define UART_RX_PIN   26
#define UART_TX_PIN   27

#define SLEEP     18
#define DIR_R     19
//actually red
#define GREEN_LED 7

#else

#define UART_RX_PIN   12
#define UART_TX_PIN   11

#define SLEEP     27
#define DIR_R     26

#define GREEN_LED 2

#endif

//Buzzer
#define BUZZER_10MM   8

//microphone
#define MIC_CLK   28
#define MIC_DI    29

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

//BLE defines, refactored from SDK
#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2    /**< Reply when unsupported features are requested. */

#define DEVICE_NAME                     "Skoobot"                               /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME               "William Weiler Eng"                    /**< Manufacturer. Will be passed to Device Information Service. */
#define APP_BLE_OBSERVER_PRIO           3                                       /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_BLE_CONN_CFG_TAG            1                                       /**< A tag identifying the SoftDevice BLE configuration. */

#define APP_ADV_INTERVAL                64                                      /**< The advertising interval (in units of 0.625 ms; this value corresponds to 40 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED   /**< The advertising time-out (in units of seconds). When set to 0, we will never time out. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(100, UNIT_1_25_MS)        /**< Minimum acceptable connection interval (0.5 seconds). */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(200, UNIT_1_25_MS)        /**< Maximum acceptable connection interval (1 second). */
#define SLAVE_LATENCY                   0                                       /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)         /**< Connection supervisory time-out (4 seconds). */

#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(20000)                  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (15 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(5000)                   /**< Time between each call to sd_ble_gap_conn_param_update after the first call (5 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                       /**< Number of attempts before giving up the connection parameter negotiation. */

#define BUTTON_DETECTION_DELAY          APP_TIMER_TICKS(50)                     /**< Delay from a GPIOTE event until a button is reported as pushed (in number of timer ticks). */

#define DEAD_BEEF                       0xDEADBEEF                              /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

void ble_bill_on_ble_evt(ble_evt_t const * p_ble_evt, void * p_context);
#define BLE_BILL_DEF(_name)                                                 \
static uint8_t _name;                                                       \
NRF_SDH_BLE_OBSERVER(_name ## _obs,                                         \
                     2,                                                     \
                     ble_bill_on_ble_evt, &_name)

#define LBS_UUID_BASE        {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, \
                              0xDE, 0xEF, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00}
#define LBS_UUID_SERVICE     0x1523
#define LBS_UUID_DATA_CHAR   0x1524
#define LBS_UUID_CMD_CHAR    0x1525

BLE_BILL_DEF(m_bill);
NRF_BLE_GATT_DEF(m_gatt);                                                       /**< GATT module instance. */

// BLE Data, declare and Initialize
//using these names, button, led for now to help me with refactored SDK code, but led will becomes command from phone to robot, button will become robot to phone logging
static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;                        /**< Handle of the current connection. */
uint8_t g_inbyte = 0;
uint16_t svc_handle;
ble_gatts_char_handles_t button_handle, led_handle;
uint8_t uuid_type;
uint8_t cmd_value = 0, data_value = 0, BLE_Connected = 0, new_cmd = 0;  
//BLE prototype functions
static void timers_init(void);
static void log_init(void);
static void ble_stack_init(void);
static void gap_params_init(void);
static void gatt_init(void);
static void services_init(void);
static void advertising_init(void);
static void conn_params_init(void);
static void advertising_start(void);
static uint32_t add_cmd_characteristic(void);
static uint32_t add_data_characteristic(void);
uint32_t update_remote_byte(void);    //sends button_value
void ble_bill_on_ble_evt(ble_evt_t const * p_ble_evt, void * p_context);

//FFT defines
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
#define FFT_TEST_COMP_SAMPLES_LEN        128    
static float32_t m_fft_input_f32[FFT_TEST_COMP_SAMPLES_LEN];             //!< FFT input array. Time domain.
static float32_t m_fft_output_f32[FFT_TEST_COMP_SAMPLES_LEN];             //!< FFT output data. Frequency domain.
static int32_t p_out_buffer[FFT_TEST_COMP_SAMPLES_LEN/2+1];
void do_dft(void);

//Microphone
#define SAMPLE_BUFFER_CNT 16*1024
int16_t p_rx_buffer[SAMPLE_BUFFER_CNT];
uint8_t mic_gain = NRF_PDM_GAIN_DEFAULT;
volatile bool m_xfer_done = false;
void configure_microphone(void);
void audio_handler(nrf_drv_pdm_evt_t const * const evt);                //Just sets xfer_done

//Distance sensor
#define TWI_INSTANCE_ID 0
const nrf_drv_twi_t m_twi = NRF_DRV_TWI_INSTANCE(TWI_INSTANCE_ID);
void configure_VLX6180(void);
void VLX6180_init(void);
void twi_init(void);

//UART
uint8_t sendbuffer[64];
uint8_t recvbuffer[64];
void uart_init(void);
void sendbytes(uint8_t a);
void TxUART(uint8_t* buf);

//Motors
uint8_t timer1_enabled_for_motors = 0, step_loop_done = 0;;
uint32_t timer1_counter, timer1_match_value, timer1_toggle_step;
void motors_forward();
void motors_backward(void);
void motors_right(void);
void motors_left(void);
void turn_left_90_degrees(void);
void pwm_motor_stepping(uint32_t steps_per_second, uint32_t number_steps);
void stop_stepping(void);
void stepping_mode(uint8_t mode);
void step_mode_experiment(void);
void motors_sleep(void);
void motors_wake(void);
void start_stepping_gpio(uint16_t freq);
void stop_stepping_gpio(void);

//Buzzer
uint8_t buzzer_loops_done = 0, song_playing = 0;
void pwm_buzzer_frequency(float freq, uint32_t loops);
void stop_buzzer(void);
struct notes_struct {
  float32_t freq;
  uint16_t duration;
};
struct song_struct {
  uint16_t num_notes; 
  struct notes_struct *notes;
} song;

//General
void my_configure(void);
void bb_test(void);
void mb_test(void);
void led_off(void);
void led_on(void);
void idle_while_charging(void);
void rover(void);

/**
 * Motor might be 15 degrees per step, so 24 steps per revolution.
 * 83.3rpm = 15ms * 2 = 30ms period, 33Hz/24*60s
 */
int main(void)
{
    uint8_t step_mode = 1, counter = 0;
    uint32_t freq = 100, steps = 200, i;
    float32_t ambient_value;
    static uint8_t range, buf[64], distance, callonce;
    struct notes_struct basic[4] = { 440.0, 100, 470.0, 100, 2600.0, 200, 1000.0, 500 };
    song.num_notes = 3;
    song.notes = basic;
 
    nrf_gpio_cfg_output(GREEN_LED);
    led_off();
    my_configure();         //sets gpios and PWM0 for step, PWM1 for buzzer
    motors_sleep();
    stepping_mode(1);
     
    uart_init();
    
    #if MICROPHONE
      configure_microphone();
    #endif
    #if SPARKFUN
      bb_test();
    #else
      configure_VLX6180();
      #if MB_TEST
        mb_test();
      #endif
    #endif

    // Initialize.
    timers_init();
    ble_stack_init();
    gap_params_init();
    gatt_init();
    services_init();
    advertising_init();
    conn_params_init();
    advertising_start();

    #if MICROPHONE
    //like 6700 samples in p_rx_buffer
    m_xfer_done = false;
    nrf_drv_pdm_buffer_set(p_rx_buffer, SAMPLE_BUFFER_CNT);
    nrf_pdm_gain_set(NRF_PDM_GAIN_MAXIMUM,NRF_PDM_GAIN_MAXIMUM);
    nrf_pdm_event_clear(NRF_PDM_EVENT_END);
    nrf_drv_pdm_start();
    while( nrf_pdm_event_check(NRF_PDM_EVENT_END) == false );
    //nrf_pdm_task_trigger(NRF_PDM_TASK_STOP);
    nrf_drv_pdm_stop();
    do_dft();            //do fft, then check m_fft_output_f32 64 bytes
    for(i=0;(i<SAMPLE_BUFFER_CNT);i++)
    { 
     sprintf(buf,"%d,",p_rx_buffer[i]);
     TxUART(buf);
     nrf_delay_ms(5); 
    }
    #endif

    new_cmd = 0;
    for (;;)
    {
          //Trap here when BLE not connected
          callonce = 1;
          while (BLE_Connected == 0)
          {
            if (callonce == 1)
            {
              stop_buzzer();
              stop_stepping();
              motors_sleep();
              callonce = 0;
            }
            nrf_delay_ms(200);
            //distance = getDistance();
          }
          if (new_cmd == 2)   //Just test code, write to button shouldn't happen
          {
            led_on();
            nrf_delay_ms(500);
            led_off();
            nrf_delay_ms(500);
            led_on();
            nrf_delay_ms(500);
            led_off();
            nrf_delay_ms(500);
          }
          if (new_cmd == 1)
          {
            if (cmd_value != GET_DISTANCE && cmd_value != GET_AMBIENT)
              led_on();
            switch(cmd_value)
            {
            case MOTORS_RIGHT:          //go right 90 degrees
              motors_right();           
              motors_wake();
              pwm_motor_stepping(100,24);
              while(step_loop_done == 0);       
              motors_sleep();
              break;
            case MOTORS_LEFT:
              motors_left();
              motors_wake();
              pwm_motor_stepping(100,24);
              while(step_loop_done == 0);       
              motors_sleep();
              break;
            case MOTORS_FORWARD:
              motors_forward();
              motors_wake();
              pwm_motor_stepping(100,200);
              while(step_loop_done == 0);       
              motors_sleep();
              break;
            case MOTORS_BACKWARD:
              motors_backward();
              motors_wake();
              pwm_motor_stepping(100,200);
              while(step_loop_done == 0);       
              motors_sleep();
              break;
            case MOTORS_STOP:
              motors_sleep();
              break;
            case ROVER_MODE:
              rover();
              break;
            case MOTORS_SLEEP:
              motors_sleep();
              break;
            case PLAY_BUZZER:
              pwm_buzzer_frequency(1000.0, 100);
              while(buzzer_loops_done == 0);
              break;
            case INC_STEP_MODE:
              motors_sleep();
              motors_forward();
              ++step_mode;
              if (step_mode == 6)
              {
                step_mode = 0;
                freq = 50;
                steps = 100;
              }
              else
              {
                freq = 100 * step_mode;
                steps = 200 * step_mode;
              }
              stepping_mode(step_mode);
              pwm_motor_stepping(freq,steps);
              motors_wake();
              while(step_loop_done == 0);       
              motors_sleep();
              data_value = step_mode;
              update_remote_byte();
              break;
            case GET_DISTANCE:
              data_value = getDistance();
              update_remote_byte();
              break;
            case GET_AMBIENT:
              ambient_value = getAmbientLight(GAIN_5);      //indoors LUX is likely 10-1000
              data_value = ambient_value;                   //single byte won't cut it
              update_remote_byte();
              break;
            case RECORD_SOUND:
              m_xfer_done = false;
              nrf_drv_pdm_buffer_set(p_rx_buffer, SAMPLE_BUFFER_CNT);
              nrf_pdm_gain_set(NRF_PDM_GAIN_MAXIMUM,NRF_PDM_GAIN_MAXIMUM);
              nrf_pdm_event_clear(NRF_PDM_EVENT_END);
              nrf_drv_pdm_start();
              while( nrf_pdm_event_check(NRF_PDM_EVENT_END) == false );
              nrf_drv_pdm_stop();
              do_dft();            //do fft, then check m_fft_output_f32 64 bytes
              ++counter;
              data_value = counter;
              update_remote_byte();
              break;
            case INCREASE_GAIN:
              mic_gain += 5;
              if (mic_gain > NRF_PDM_GAIN_MAXIMUM)
                mic_gain = NRF_PDM_GAIN_MAXIMUM;
              nrf_pdm_gain_set(mic_gain,mic_gain);
              data_value = mic_gain;
              update_remote_byte();
              break;
            case DECREASE_GAIN:
              if (mic_gain < NRF_PDM_GAIN_MINIMUM+5)
                  mic_gain = NRF_PDM_GAIN_MINIMUM;
              else
                  mic_gain -= 5;
              nrf_pdm_gain_set(mic_gain,mic_gain);
              data_value = mic_gain;
              update_remote_byte();
              break;
           default:
               break;
          }
          new_cmd = 0;
      }
      else
      {
        led_off();
        //play_song();
      }
   }  
}

void rover(void)
{
  uint8_t distance;

  motors_sleep();
  motors_forward();
  stepping_mode(2);
  pwm_motor_stepping(100,200);       
  motors_wake();
  new_cmd = 0;
  while(new_cmd == 0)
  {
      if (step_loop_done == 1)
        pwm_motor_stepping(100,200);       
      distance = getDistance();
      if (distance < 50)
      {
        led_on();
        motors_left();
      }
      else
      {
        motors_forward();
        led_off();
      }
      nrf_delay_ms(50);
  }
  motors_sleep();
  return;
}

void play_song(void)
{
    static uint8_t i = 0;

    if (buzzer_loops_done == 0)
      return;

    pwm_buzzer_frequency(song.notes[i].freq,song.notes[i].duration);
    if (i == song.num_notes-1)
      i = 0;
    else  
      ++i;
}

void turn_left_90_degrees(void)
{
  motors_left();            //turn left
  pwm_motor_stepping(50,10);   
  nrf_delay_ms(100);        //this should be 5 steps each motor for 90 degree turn
  stop_stepping();          //if it doesn't work I'll try doing 5 steps manually
}

//Left channel configured by default
void configure_microphone(void)
{

  nrf_drv_pdm_config_t pdm_config = NRF_DRV_PDM_DEFAULT_CONFIG(MIC_CLK,MIC_DI);

  pdm_config.clock_freq = NRF_PDM_FREQ_1032K;
  pdm_config.mode = NRF_PDM_MODE_MONO;
  pdm_config.edge = NRF_PDM_EDGE_LEFTRISING;
  pdm_config.gain_l = mic_gain;
  pdm_config.gain_r = mic_gain;

  nrf_drv_pdm_init(&pdm_config, audio_handler);
 
  return; 
}

void step_mode_experiment(void)
{
    uint8_t stepmode, cnt_of_trys;
    uint32_t freq;

    motors_sleep();
    for (cnt_of_trys=0;(cnt_of_trys<5);cnt_of_trys++)
    {
        freq = 40;
        for(stepmode=0;(stepmode<6);stepmode++)
        {
            stepping_mode(stepmode);
            motors_forward();           
            pwm_motor_stepping(100,100);
            if (freq == 40)
              led_on();                 //mark start of test
            else
              led_off();
            motors_wake();
            nrf_delay_ms(2000);
            stop_stepping();
            motors_sleep();
            nrf_delay_ms(500);
            freq *= 2;
        }
    }
    return;
}

inline void led_off(void)
{
    nrf_gpio_pin_set(GREEN_LED);
}

inline void led_on(void)
{
    nrf_gpio_pin_clear(GREEN_LED);
}

inline void motors_sleep(void)
{
    nrf_gpio_pin_clear(SLEEP);
}

inline void motors_wake(void)
{
    nrf_gpio_pin_set(SLEEP);
    nrf_delay_us(1000);       //delay 1ms for DRV to reset
}

void motors_forward()
{
  nrf_gpio_pin_clear(DIR_L);
  nrf_gpio_pin_set(DIR_R);   
}

void motors_backward(void)
{
   nrf_gpio_pin_set(DIR_L);
   nrf_gpio_pin_clear(DIR_R);
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

void stop_stepping_gpio(void)
{
  nrf_timer_task_trigger(NRF_TIMER1,NRF_TIMER_TASK_STOP);
}

void start_stepping_gpio(uint16_t freq)
{
  timer1_toggle_step = 1;
  timer1_counter = 0;
  timer1_match_value = 500 / freq;    //does 2 edges, so half of 1s in ms
  timer1_enabled_for_motors = 1;
  nrf_timer_task_trigger(NRF_TIMER1,NRF_TIMER_TASK_START);    
}

//TIMER1 - this is the motor timer, runs at 1ms
void TIMER1_IRQHandler(void)
{
  if (timer1_enabled_for_motors == 1)
  {
    ++timer1_counter;
    if (timer1_counter == timer1_match_value)
    {
       if (timer1_toggle_step == 1)
       {
          nrf_gpio_pin_set(STEP);
          timer1_toggle_step = 0;
       }
       else
       {
          nrf_gpio_pin_clear(STEP);
          timer1_toggle_step = 1;
       }
       timer1_counter = 0;
    }
  }
  nrf_timer_event_clear(NRF_TIMER1,NRF_TIMER_EVENT_COMPARE0);

}

void twi_handler(nrf_drv_twi_evt_t const * p_event, void * p_context)
{
    switch (p_event->type)
    {
        case NRF_DRV_TWI_EVT_DONE:
            if (p_event->xfer_desc.type == NRF_DRV_TWI_XFER_RX)
            {             
                //data_handler(m_sample);
            }
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
       .frequency          = NRF_TWI_FREQ_400K,
       .interrupt_priority = APP_IRQ_PRIORITY_HIGH,
       .clear_bus_init     = false
    };

    err_code = nrf_drv_twi_init(&m_twi, &twi_VLX6180_config, NULL, NULL);

    nrf_drv_twi_enable(&m_twi);
}


void audio_handler(nrf_drv_pdm_evt_t const * const evt)
{
   if (evt->buffer_requested == false)
       m_xfer_done = true;
}

//in common mode and refresh mode, number of steps should be even
//wheels with tires travel approximately 1.67mm per step, so 300 steps is 5cm
//a 90 degree turn should be 10 steps
void pwm_motor_stepping(uint32_t steps_per_second, uint32_t number_steps)
{
  volatile static uint16_t pwm_duty[2];
  volatile static uint32_t pwm_top;

  pwm_top = 125000 / steps_per_second; 
  if( pwm_top > 0xffff)
    pwm_top = 0xffff;
  pwm_duty[0] = pwm_duty[1] = pwm_top / 2;

  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_shorts_set(NRF_PWM0, NRF_PWM_SHORT_LOOPSDONE_SEQSTART0_MASK);
  NRF_PWM0->COUNTERTOP = (uint16_t)pwm_top; 
  NRF_PWM0->SEQ[0].PTR = (uint32_t)(pwm_duty);
  NRF_PWM0->SEQ[0].CNT = 1;
  NRF_PWM0->SEQ[1].PTR = (uint32_t)(pwm_duty);
  NRF_PWM0->SEQ[1].CNT = 1;
  NRF_PWM0->LOOP = number_steps / 2;
  nrf_pwm_task_trigger(NRF_PWM0, NRF_PWM_TASK_SEQSTART0);
  step_loop_done = 0;
}

void PWM0_IRQHandler(void)
{
    stop_stepping();
}

void PWM1_IRQHandler(void)
{
    stop_buzzer();
}
//125000/freq=    125000/250
void pwm_buzzer_frequency(float32_t freq, uint32_t loops)
{
  float32_t main_freq;
  volatile static uint16_t pwm_top, pwm_duty[2];
   
  main_freq = 125000.0 / freq;

  pwm_top = (uint16_t)main_freq;
  pwm_duty[0] = pwm_duty[1] = (uint16_t)(main_freq / 2.0);

  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_shorts_set(NRF_PWM1, NRF_PWM_SHORT_LOOPSDONE_SEQSTART0_MASK);
  NRF_PWM1->COUNTERTOP = (uint16_t)pwm_top; 
  NRF_PWM1->SEQ[0].PTR = (uint32_t)(pwm_duty);
  NRF_PWM1->SEQ[0].CNT = 1;
  NRF_PWM1->SEQ[1].PTR = (uint32_t)(pwm_duty);
  NRF_PWM1->SEQ[1].CNT = 1;
  NRF_PWM1->LOOP = loops / 2;
  nrf_pwm_task_trigger(NRF_PWM1, NRF_PWM_TASK_SEQSTART0);
  buzzer_loops_done = 0;
}

void bb_test(void)
{
  motors_forward();
  for(;;)
  {
    led_on();
    pwm_motor_stepping(100,300);
    motors_wake();
    while(step_loop_done == 0);       
    motors_sleep();
    led_off();
    nrf_delay_ms(500);
  }
}

void mb_test(void)
{
  static uint8_t range, buf[64];

  nrf_gpio_cfg_output(MIC_CLK);
  nrf_gpio_cfg_output(MIC_DI);
  nrf_gpio_cfg_output(SLEEP);
  
  for(;;)
  {
    led_on();
    nrf_gpio_pin_clear(STEP);
    nrf_gpio_pin_clear(DIR_L);
    nrf_gpio_pin_clear(DIR_R);
    nrf_gpio_pin_clear(M1);
    nrf_gpio_pin_clear(M0);
    nrf_gpio_pin_clear(MIC_CLK);
    nrf_gpio_pin_clear(MIC_DI);
    nrf_gpio_pin_clear(SLEEP);
    nrf_delay_ms(500);
    led_off();
    nrf_gpio_pin_set(STEP);
    nrf_gpio_pin_set(DIR_L);
    nrf_gpio_pin_set(DIR_R);
    nrf_gpio_pin_set(M1);
    nrf_gpio_pin_set(M0);
    nrf_gpio_pin_set(MIC_CLK);
    nrf_gpio_pin_set(MIC_DI);
    nrf_gpio_pin_set(SLEEP);
    nrf_delay_ms(500);
    range = getDistance();
    sprintf(buf,"Range is %d\r\n",range);
    TxUART(buf);

    nrf_delay_ms(500);
  }

}

void stop_buzzer(void)
{
  if (nrf_pwm_event_check(NRF_PWM1, NRF_PWM_EVENT_LOOPSDONE))
  {
    nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_LOOPSDONE);  
    if (nrf_pwm_event_check(NRF_PWM1, NRF_PWM_EVENT_STOPPED) == 0)
    {
        nrf_pwm_task_trigger(NRF_PWM1, NRF_PWM_TASK_STOP);
        while(nrf_pwm_event_check(NRF_PWM1, NRF_PWM_EVENT_STOPPED) == 0);
        buzzer_loops_done = 1;
    }
  }
}

void stop_stepping(void)
{
  if (nrf_pwm_event_check(NRF_PWM0, NRF_PWM_EVENT_LOOPSDONE))
  {
    nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_LOOPSDONE);  
    if (nrf_pwm_event_check(NRF_PWM0, NRF_PWM_EVENT_STOPPED) == 0)
    {
        nrf_pwm_task_trigger(NRF_PWM0, NRF_PWM_TASK_STOP);
        while(nrf_pwm_event_check(NRF_PWM0, NRF_PWM_EVENT_STOPPED) == 0);
        step_loop_done = 1;
    }
  }
}

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

void my_configure(void)
{
  nrf_gpio_cfg_output(DIR_L);
  nrf_gpio_cfg_output(DIR_R);
  nrf_gpio_cfg_output(SLEEP);
  motors_sleep();                 //sleep motors
  nrf_gpio_pin_set(DIR_L);        //set direction forward
  nrf_gpio_pin_clear(DIR_R);
  stepping_mode(1);               //initial mode 1, 1/2 stepping

  nrf_gpio_pin_clear(STEP);
  nrf_gpio_cfg_output(STEP);
  NRF_PWM0->PSEL.OUT[0] = STEP;

  NRF_PWM0->ENABLE = PWM_ENABLE_ENABLE_Enabled;
  NRF_PWM0->MODE = PWM_MODE_UPDOWN_Up;
  NRF_PWM0->PRESCALER = PWM_PRESCALER_PRESCALER_DIV_128;
  NRF_PWM0->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
  NRF_PWM0->SEQ[0].REFRESH = 0;
  NRF_PWM0->SEQ[0].ENDDELAY = 0;
  NRF_PWM0->SEQ[1].REFRESH = 0;
  NRF_PWM0->SEQ[1].ENDDELAY = 0;
  nrf_pwm_shorts_set(NRF_PWM0, 0);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_int_set(NRF_PWM0, NRF_PWM_INT_LOOPSDONE_MASK);
  nrf_drv_common_irq_enable(PWM0_IRQn,APP_IRQ_PRIORITY_LOWEST);

  nrf_timer_event_clear(NRF_TIMER1,NRF_TIMER_EVENT_COMPARE0);
  nrf_timer_mode_set(NRF_TIMER1,NRF_TIMER_MODE_TIMER);
  nrf_timer_bit_width_set(NRF_TIMER1,NRF_TIMER_BIT_WIDTH_32);
  nrf_timer_frequency_set(NRF_TIMER1,NRF_TIMER_FREQ_125kHz);
  nrf_timer_cc_write(NRF_TIMER1,NRF_TIMER_CC_CHANNEL0,125);
  nrf_timer_int_enable(NRF_TIMER1,NRF_TIMER_INT_COMPARE0_MASK);


#if CB_TEST == 0
  nrf_gpio_pin_clear(BUZZER_10MM);
  nrf_gpio_cfg_output(BUZZER_10MM);
  NRF_PWM1->PSEL.OUT[0] = BUZZER_10MM;

  NRF_PWM1->ENABLE = PWM_ENABLE_ENABLE_Enabled;
  NRF_PWM1->MODE = PWM_MODE_UPDOWN_Up;
  NRF_PWM1->PRESCALER = PWM_PRESCALER_PRESCALER_DIV_128;
  NRF_PWM1->LOOP = PWM_LOOP_CNT_Disabled;
  NRF_PWM1->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) | (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
  NRF_PWM1->SEQ[0].REFRESH = 0;
  NRF_PWM1->SEQ[0].ENDDELAY = 0;
  NRF_PWM1->SEQ[1].REFRESH = 0;
  NRF_PWM1->SEQ[1].ENDDELAY = 0;
  nrf_pwm_shorts_set(NRF_PWM1, 0);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_int_set(NRF_PWM1, NRF_PWM_INT_LOOPSDONE_MASK);
  nrf_drv_common_irq_enable(PWM1_IRQn,APP_IRQ_PRIORITY_LOWEST);
#endif
}

void idle_while_charging(void)
{
    for (;;)
    {
        led_off();
        nrf_delay_ms(500);
        led_on();
        nrf_delay_ms(500);
    }
}

//expects null terminated string, so use sprintf for buf
void TxUART(uint8_t* buf)
{
    uint8_t j;

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

    //NRF_UARTE0->RXD.MAXCNT = 1;
    //NRF_UARTE0->TASKS_STARTRX = 1;
    nrf_delay_ms(20);
    while(NRF_UARTE0->EVENTS_ENDTX != 1);
    //while(NRF_UARTE0->EVENTS_ENDRX != 1);
}

void sendbytes(uint8_t which)
{
  uint8_t i;
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
          TxUART(buf);
      }
   }
   else
   {

    for(i=0;(i<64);i++)
    {
        actual_value = p_rx_buffer[i+500]; 
        sprintf(buf,"%d,",actual_value);
        TxUART(buf);
     }
   }
   sendbuffer[0] = 13;
   sendbuffer[1] = 10;
   NRF_UARTE0->TXD.MAXCNT = 2;
   NRF_UARTE0->TASKS_STARTTX = 1;

}

void do_dft(void)
{
    uint16_t fftLen = FFT_TEST_COMP_SAMPLES_LEN;  //currently 128
    arm_status ret;
    arm_rfft_fast_instance_f32 S;
    uint32_t i;
    uint8_t ifftFlag  = 0;


    for(i=0;(i<FFT_TEST_COMP_SAMPLES_LEN);i++)
    {
          m_fft_input_f32[i] = p_rx_buffer[i+50];      //magnitude part        
    }

    ret = arm_rfft_fast_init_f32(&S,fftLen);
    
    arm_rfft_fast_f32(&S,m_fft_input_f32,m_fft_output_f32,ifftFlag);

    for(i=0;(i<FFT_TEST_COMP_SAMPLES_LEN/2);i++)
    {
          p_out_buffer[i] = m_fft_output_f32[i];         
    }
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


/**@brief Function for initializing the Advertising functionality.
 *
 * @details Encodes the required advertising data and passes it to the stack.
 *          Also builds a structure to be passed to the stack when starting advertising.
 */
static void advertising_init(void)
{
    ret_code_t    err_code;
    ble_advdata_t advdata;
    ble_advdata_t srdata;

    ble_uuid_t adv_uuids[] = {{LBS_UUID_SERVICE, uuid_type}};

    // Build and set advertising data
    memset(&advdata, 0, sizeof(advdata));

    advdata.name_type          = BLE_ADVDATA_FULL_NAME;
    advdata.include_appearance = true;
    advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;


    memset(&srdata, 0, sizeof(srdata));
    srdata.uuids_complete.uuid_cnt = sizeof(adv_uuids) / sizeof(adv_uuids[0]);
    srdata.uuids_complete.p_uuids  = adv_uuids;

    err_code = ble_advdata_set(&advdata, &srdata);
    APP_ERROR_CHECK(err_code);
}


static uint32_t add_cmd_characteristic(void)
{
    ble_gatts_char_md_t char_md;          //server characteristic metadata
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          ble_uuid;
    ble_gatts_attr_md_t attr_md;

    memset(&char_md, 0, sizeof(char_md));

    char_md.char_props.read  = 1;
    char_md.char_props.write = 1;
    char_md.p_char_user_desc = NULL;
    char_md.p_char_pf        = NULL;
    char_md.p_user_desc_md   = NULL;
    char_md.p_cccd_md        = NULL;
    char_md.p_sccd_md        = NULL;

    ble_uuid.type = uuid_type;
    ble_uuid.uuid = LBS_UUID_CMD_CHAR;

    memset(&attr_md, 0, sizeof(attr_md));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    attr_md.vloc    = BLE_GATTS_VLOC_STACK;
    attr_md.rd_auth = 0;
    attr_md.wr_auth = 0;
    attr_md.vlen    = 0;

    memset(&attr_char_value, 0, sizeof(attr_char_value));

    attr_char_value.p_uuid    = &ble_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = sizeof(uint8_t);
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = sizeof(uint8_t);
    attr_char_value.p_value   = NULL;

    return sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &led_handle);

}

static uint32_t add_data_characteristic(void)
{
    ret_code_t     err_code;
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;          //client characteristic metadata
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          ble_uuid;
    ble_gatts_attr_md_t attr_md;
       
    memset(&cccd_md, 0, sizeof(cccd_md));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
    cccd_md.vloc = BLE_GATTS_VLOC_STACK;

    memset(&char_md, 0, sizeof(char_md));

    char_md.char_props.read   = 1;
    char_md.char_props.notify = 1;
    char_md.p_char_user_desc  = NULL;
    char_md.p_char_pf         = NULL;
    char_md.p_user_desc_md    = NULL;
    char_md.p_cccd_md         = &cccd_md;
    char_md.p_sccd_md         = NULL;

    ble_uuid.type = uuid_type;
    ble_uuid.uuid = LBS_UUID_DATA_CHAR;

    memset(&attr_md, 0, sizeof(attr_md));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    attr_md.vloc    = BLE_GATTS_VLOC_STACK;
    attr_md.rd_auth = 0;
    attr_md.wr_auth = 0;
    attr_md.vlen    = 0;

    memset(&attr_char_value, 0, sizeof(attr_char_value));

    attr_char_value.p_uuid    = &ble_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = sizeof(uint8_t);
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = sizeof(uint8_t);
    attr_char_value.p_value   = NULL;

    err_code = sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &button_handle);   
    VERIFY_SUCCESS(err_code);
}
/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    ret_code_t     err_code;
    ble_uuid_t ble_uuid;
  
  // Add service.
    ble_uuid128_t base_uuid = {LBS_UUID_BASE};
    err_code = sd_ble_uuid_vs_add(&base_uuid, &uuid_type);
    VERIFY_SUCCESS(err_code);

    ble_uuid.uuid = LBS_UUID_SERVICE;
    ble_uuid.type = uuid_type;

    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid, &svc_handle);
    VERIFY_SUCCESS(err_code);
       
    err_code = add_cmd_characteristic();
    VERIFY_SUCCESS(err_code);
    
    err_code = add_data_characteristic();
    VERIFY_SUCCESS(err_code);
    
    APP_ERROR_CHECK(err_code);
}

uint32_t update_remote_byte(void)
{
    ble_gatts_hvx_params_t params;
    uint16_t len = 1;

    memset(&params, 0, sizeof(params));
    params.type   = BLE_GATT_HVX_NOTIFICATION;
    params.handle = button_handle.value_handle;
    params.p_data = &data_value;
    params.p_len  = &len;

    return sd_ble_gatts_hvx(m_conn_handle, &params);
}

/**@brief Function for handling the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module that
 *          are passed to the application.
 *
 * @note All this function does is to disconnect. This could have been done by simply
 *       setting the disconnect_on_fail config parameter, but instead we use the event
 *       handler mechanism to demonstrate its use.
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


/**@brief Function for starting advertising.
 */
static void advertising_start(void)
{
    ret_code_t           err_code;
    ble_gap_adv_params_t adv_params;

    // Start advertising
    memset(&adv_params, 0, sizeof(adv_params));

    adv_params.type        = BLE_GAP_ADV_TYPE_ADV_IND;
    adv_params.p_peer_addr = NULL;
    adv_params.fp          = BLE_GAP_ADV_FP_ANY;
    adv_params.interval    = APP_ADV_INTERVAL;
    adv_params.timeout     = APP_ADV_TIMEOUT_IN_SECONDS;

    err_code = sd_ble_gap_adv_start(&adv_params, APP_BLE_CONN_CFG_TAG);
    APP_ERROR_CHECK(err_code);

}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t err_code;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            BLE_Connected = 1;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            BLE_Connected  = 0;
            advertising_start();
            break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported
            err_code = sd_ble_gap_sec_params_reply(m_conn_handle,
                                                   BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
                                                   NULL,
                                                   NULL);
            APP_ERROR_CHECK(err_code);
            break;

#ifndef S140
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        } break;
#endif

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            // No system attributes have been stored.
            err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
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

void on_write(ble_evt_t const * p_ble_evt)
{    
    ble_gatts_evt_write_t const * p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;

    if ((p_evt_write->handle == led_handle.value_handle)
        && (p_evt_write->len == 1))
    {
         cmd_value = p_evt_write->data[0];
         new_cmd = 1;
    }
    else
    {
      if ((p_evt_write->handle == button_handle.value_handle)
          && (p_evt_write->len == 1))
      {
           //data_value = p_evt_write->data[0];
           new_cmd = 2;
      }
    }

}

void ble_bill_on_ble_evt(ble_evt_t const * p_ble_evt, void * p_context)
{

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GATTS_EVT_WRITE:
            on_write(p_ble_evt);
            break;

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

static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);
}


/*
*@brief Function for the Power Manager.
 */
static void power_manage(void)
{
    ret_code_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}

/*
*@brief Function for the Timer initialization.
 */
static void timers_init(void)
{
    // Initialize timer module, making it use the scheduler
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
}