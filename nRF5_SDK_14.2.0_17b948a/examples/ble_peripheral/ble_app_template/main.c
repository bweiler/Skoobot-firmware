/*
      Tiny Robot Code, v0.7
      by Bill Weiler

      These are the current versions of the tools. As time goes by, I'll have to upgrade.
      Segger Embedded Studio - full version is free for Nordic chips, using ARM M version v3.34
      Segger J-Link EDU or Adafruit J-Link - this is a cheaper programmer for learning
      Nordic nRF5 SDK v14.2
      Nordic SDK BLE_Peripheral_Template and nRF Blinky example used as starting point 
      CMSIS DSP Library
      Nordic chip: nRF52832 QFN48 AA
      nRF52832 runs at 64Mhz, has 512k flash and 64k SRAM, and hardware single precision floating point
      Nordic BLE Stack is called Softdevice and only comes in hex form, I'm using S132 v5.0.0

      Targets for this code are Tiny Robot and the Sparkfun nRF52832 design I use for my test rigs.
      Tiny Robot has these features:
      1. ST VLX6180 distance sensor controlled through i2c
      2. 2 motors, left and right, driven through 2 TI DRV8834. The DRV's are driven by gpio's
      3. Microphone into microcontroller PDM peripheral that outputs PCM signed 16bit mono audio
      4. Buzzer out of microcontroller PWM1 peripheral
      5. 1 LED out from a gpio, active low (clearing it (setting it to 0) turns led on)
      6. BLE, the s132 softdevice supports central and peripheral, observer and broadcaster, total 20 connections (that's 19 robots and a cellphone!)

      There is no physical crystal LF oscillator in Tiny Robot, the softdevice uses the internal RC oscillator, 
      there is however, a physical 32Mhz crystal HF oscillator for the CPU

      Something to watch out for: the BLE s132 stack uses peripherals and needs scheduling. Notably it takes Timer0, 
      some flash and ram, and a lot of cpu cycles. If you have problems, use their API for scheduling time slots. 
      Check the Softdevice documentation for occasional blocking of peripherals.

      Motors are bi-polar stepping motors. The manufacturer says they don't like microstepping, but it seems fine. 
      Some specs are a step is 18 degrees, about 1.67mm of wheel travel. Microstepping is weirdly non-linear, so
      going from 1/4 to 1/8 just doubling steps doesn't result in the same speed.
      Minimum freq for full-step seems to be 200
      Maxmimum freq full-step for starting rotation is 800

      For true hardware floating point, GCC is not always smart enough, you can use idioms or intrinsics, 
      or just check disassembly to make sure FPU instructions were used.

      For understanding firmware to master it, read this stuff (it's a big amount):
      1. nRF52832 datasheet, watch out for old versions, like on Sparkfun, I am using v1.4
      2. Nordic Softdevice Specification
      3. Nordic Errata (skim, too boring)
      4. Nordic Softdevice API - online only
      5. Nordic devzone forums
      6. Nordic Compatability list (really boring but skim)
      7. ST VLX6180 datasheet (some tricks you can do)
      8. Knowles Microphone datasheet - The datasheet is minimally useful, it just has audio specs. I would read stuff 
         about PDM and PCM. You can download the audio to your PC and play it with Audacity (Audacity is free).
         Audacity gotcha is, the file extension must be .wav, not the .bin Segger IDE captures and writes by default.

      I tried to get faster uploading of the sound file by using larger BLE packets sizes. Cheap phones don't support larger
      sizes however, I went back to 20 bytes from trying 128 bytes.
      One arcane thing I did change for greater than 20byte transfers from the default project:
      I changed this line in ble_app_template_pca10040_s132.emProject
       
       before: RAM_START=0x200020e0;RAM_SIZE=0xdf20"
       after:  RAM_START=0x20002400;RAM_SIZE=0xdc00"

      This increased reserved ram from 0x20e0 to 0x2400. I increased NRF_SDH_BLE_GATT_MAX_MTU_SIZE from 23 to 128.
      I went back to and am using 23 for my Moto E4. If I set it to 128, My Moto E4 rejects the larger MTU request 
      and won't even connect.
      128 MTU plus a 20ms connection interval can transfer 32k in 5s
      23 MTU plus a 10ms connection interval takes 15s
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
#include "ble_db_discovery.h"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"
#include "arm_const_structs.h"
#include "nrf_drv_twi.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_pwm.h"
#include "nrf_timer.h"
#include "nrf_uarte.h"    
#include "vl6180.h"
#include "nrf_drv_pdm.h"

//COMMAND SET FOR BLE
#define MOTORS_RIGHT_30     0x08
#define MOTORS_LEFT_30      0x09
#define MOTORS_RIGHT        0x10
#define MOTORS_LEFT         0x11
#define MOTORS_FORWARD      0x12
#define MOTORS_BACKWARD     0x13
#define MOTORS_STOP         0x14
#define STOP_TURNING        0x15
#define MOTORS_SLEEP        0x16
#define PLAY_BUZZER         0x17
//The next two in Android yet
#define DEC_STEP_MODE       0x18
#define INC_STEP_MODE       0x19
#define GET_AMBIENT         0x21
#define GET_DISTANCE        0x22
#define CONNECT_DISCONNECT  0x23
#define RECORD_SOUND        0x30
#define INCREASE_GAIN       0x31
#define DECREASE_GAIN       0x32
#define RECORD_SOUND_PI     0x33
#define ROVER_MODE          0x40
#define FOTOV_MODE          0x41
#define ROVER_MODE_REV      0x42
/*
    Conditional compilation
    ========================================================
    MB_TEST = mainboard (also called FB) tester
    SPARKFUN 1 = Sparkfun reference used for testing daugtherboard (also called BB)
    SPARTFUN 0 = Tiny Robot firmware
    ADHOC_TEST = ad hoc test - targeted for debugging a specific thing
    MOTORS_STEPPING_PWM = enables PWM functions for use in stepping, you must disable gpio/timer1
*/
#define MB_TEST     0
#define MICROPHONE  0
#define SPARKFUN    0
#define ADHOC_TEST  0
#define MOTORS_STEPPING_PWM 0
#if MB_TEST
#define SPARKFUN 0
#endif

#if SPARKFUN

#define UART_RX_PIN   26
#define UART_TX_PIN   27
#define SLEEP         18
#define DIR_R         19
//actually red
#define GREEN_LED     7

#else

#define UART_RX_PIN   12
#define UART_TX_PIN   11
#define SLEEP         27
#define DIR_R         26
#define GREEN_LED     2

#endif

//Buzzer
#define BUZZER_10MM   8

//microphone
#define MIC_CLK       28
#define MIC_DI        29

//VLX6180
#define GP0           14
#define GP1           13
#define I2C0_SCL      15
#define I2C0_SDA      16

//motors
#define M1            25
#define M0            23
#define DIR_L         22
#define STEP          30

//stepping modes
#define FULL_STEP     0
#define HALF_STEP     1
#define QUARTER_STEP  2
#define n8_STEP       3
#define n16_STEP      4
#define n32_STEP      5

//BLE defines, refactored from SDK
#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2    /**< Reply when unsupported features are requested. */

#define DEVICE_NAME                     "Skoobot"                               /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME               "William Weiler Eng"                    /**< Manufacturer. Will be passed to Device Information Service. */
#define APP_BLE_OBSERVER_PRIO           3                                       /**< Application's BLE observer priority. You shouldn't need to modify this value. */
#define APP_BLE_CONN_CFG_TAG            1                                       /**< A tag identifying the SoftDevice BLE configuration. */

#define SCAN_INTERVAL                   0x00A0                              /**< Determines scan interval in units of 0.625 millisecond. */
#define SCAN_WINDOW                     0x0050                              /**< Determines scan window in units of 0.625 millisecond. */
#define SCAN_TIMEOUT                    0x0000                              /**< Timout when scanning. 0x0000 disables timeout. */
#define MIN_CONNECTION_INTERVAL         MSEC_TO_UNITS(7.5, UNIT_1_25_MS)    /**< Determines minimum connection interval in milliseconds. */
#define MAX_CONNECTION_INTERVAL         MSEC_TO_UNITS(30, UNIT_1_25_MS)     /**< Determines maximum connection interval in milliseconds. */
#define SUPERVISION_TIMEOUT             MSEC_TO_UNITS(4000, UNIT_10_MS)     /**< Determines supervision time-out in units of 10 milliseconds. */
#define UUID16_SIZE                     2                                   /**< Size of a UUID, in bytes. */

#define APP_ADV_INTERVAL                64                                      /**< The advertising interval (in units of 0.625 ms; this value corresponds to 40 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED   /**< The advertising time-out (in units of seconds). When set to 0, we will never time out. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(10, UNIT_1_25_MS)        /**< Minimum acceptable connection interval 50Hz*/
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(40, UNIT_1_25_MS)        /**< Maximum acceptable connection interval 25Hz*/
#define SLAVE_LATENCY                   0                                       /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)         /**< Connection supervisory time-out (4 seconds). */

#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(20000)                  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (15 seconds). */
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(5000)                   /**< Time between each call to sd_ble_gap_conn_param_update after the first call (5 seconds). */
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                       /**< Number of attempts before giving up the connection parameter negotiation. */

#define DEAD_BEEF                       0xDEADBEEF                              /**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

static uint8_t m_gap_role     = BLE_GAP_ROLE_INVALID;                           /**< BLE role for this connection, see @ref BLE_GAP_ROLES */
static char const m_target_periph_name[] = "Skoobot";                           /**< Name of the device we try to connect to. This name is searched in the scan report data*/
static void on_ble_gap_evt_connected(ble_gap_evt_t const * p_gap_evt);
static void db_disc_handler(ble_db_discovery_evt_t * p_evt);
BLE_DB_DISCOVERY_DEF(m_db_disc);                                                /**< DB discovery module instance. */
/**@brief Parameters used when scanning. */
static ble_gap_scan_params_t const m_scan_params =
{
    .active   = 1,
    .interval = SCAN_INTERVAL,
    .window   = SCAN_WINDOW,
    .timeout  = SCAN_TIMEOUT,
    #if (NRF_SD_BLE_API_VERSION <= 2)
        .selective   = 0,
        .p_whitelist = NULL,
    #endif
    #if (NRF_SD_BLE_API_VERSION >= 3)
        .use_whitelist = 0,
    #endif
};
/**@brief Connection parameters requested for connection. */
static ble_gap_conn_params_t const m_connection_param =
{
    (uint16_t)MIN_CONNECTION_INTERVAL,
    (uint16_t)MAX_CONNECTION_INTERVAL,
    (uint16_t)SLAVE_LATENCY,
    (uint16_t)SUPERVISION_TIMEOUT
};
/**@brief Variable length data encapsulation in terms of length and pointer to data. */
typedef struct
{
    uint8_t * p_data;   /**< Pointer to data. */
    uint16_t  data_len; /**< Length of data. */
} data_t;

void ble_skoobot_p_on_ble_evt(ble_evt_t const * p_ble_evt, void * p_context);
#define BLE_SKOOBOT_DEF(_name)                                                \
static uint8_t _name;                                                         \
NRF_SDH_BLE_OBSERVER(_name ## _obs,                                           \
                     2,                                                       \
                     ble_skoobot_p_on_ble_evt, &_name)

//stole these from Nordic Blinky app
#define LBS_UUID_BASE        {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, \
                              0xDE, 0xEF, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00}
#define LBS_UUID_SERVICE     0x1523
#define LBS_UUID_DATA_CHAR   0x1524
#define LBS_UUID_CMD_CHAR    0x1525
#define LBS_UUID_BYTE2_CHAR  0x1526
#define LBS_UUID_BYTE128_CHAR 0x1527
#define LBS_UUID_BYTE4_CHAR  0x1528

BLE_SKOOBOT_DEF(m_skoobot_p);
BLE_SKOOBOT_DEF(m_skoobot_c);
NRF_BLE_GATT_DEF(m_gatt);                                                       /**< GATT module instance. */

// BLE Data, declare and Initialize
ble_uuid_t ble_uuid_svc;
uint16_t m_conn_p_handle = BLE_CONN_HANDLE_INVALID;                      
uint16_t m_conn_c_handle = BLE_CONN_HANDLE_INVALID;                      
uint8_t g_inbyte = 0;
uint16_t svc_handle;
ble_gatts_char_handles_t data_handle, cmd_handle, remote_cmd_handle;
ble_gatts_char_handles_t data_2byte_handle, data_4byte_handle, data_128byte_handle;
uint8_t uuid_type;
uint8_t cmd_value = 0, data_value = 0, BLE_P_Connected = 0, BLE_C_Connected = 0, new_cmd = 0;
uint8_t data_2byte_val[2], data_4byte_val[4];
ble_gatts_value_t sound_value, sound_flag, data4_value, data_val;    //for Raspberry Pi and Samsung phone
uint8_t pi_reads_active = 0, found_skoobot = 0;
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
static uint32_t add_data2_characteristic(void);
static uint32_t add_mult_data_characteristics(void);
static uint32_t add_cmd4_characteristic(void);
static uint32_t update_remote_byte(void);    //sends uint8_t data_value
static uint32_t update_remote_2byte(void);    //sends 2 uint8_t or unit16_t data2_value
static void db_disc_handler(ble_db_discovery_evt_t * p_evt);
static void db_discovery_init(void);
static void scan_start(void);
//len = 128 supported by BLE 4.1 and 4.2, 5.0 (my iPhone6)
//len = 20 supported by BLE 4.0 (my Moto E4)
#define MULTI_LEN 20
uint8_t data_128byte_val[MULTI_LEN];
static uint32_t update_remote_multi_byte(uint32_t i);  

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

//Microphone, 16k is 1s of audio
#define SAMPLE_BUFFER_CNT 16*1024
int16_t p_rx_buffer[SAMPLE_BUFFER_CNT+6];    //+6 makes it 16,380/20=1638 20 byte packets
uint8_t mic_gain = NRF_PDM_GAIN_DEFAULT;
uint32_t load_buffer_offset = 0;
volatile bool m_xfer_done = false;
void configure_microphone(void);
void audio_handler(nrf_drv_pdm_evt_t const * const evt);                //Just sets xfer_done

//Distance sensor
#define TWI_INSTANCE_ID 0
uint16_t data2_value;
const nrf_drv_twi_t m_twi = NRF_DRV_TWI_INSTANCE(TWI_INSTANCE_ID);
void configure_VLX6180(void);
void VLX6180_init(void);
void twi_init(void);
volatile ret_code_t vl6180_err_code;

//UART
uint8_t sendbuffer[64];
uint8_t recvbuffer[64];
void uart_init(void);
void sendbytes(uint8_t a);
void TxUART(uint8_t* buf);
static uint8_t teststr[2] = { 'A',0 };

//Motors
uint8_t timer1_enabled_for_motors = 0, step_loop_done = 0, turn_flag = 0, motor_state;
uint32_t timer1_counter, timer1_match_value, timer1_toggle_step, timer_turn_step_count, timer_turn_match;
uint16_t timer1_turn_count = 0;
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
uint8_t buzzer_loops_done = 1, song_playing = 0;
void pwm_buzzer_frequency(float freq, uint32_t loops);
void stop_buzzer(void);
//This song feature isn't done
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
void adhoc_robot_test(void);
void mb_test(void);
void led_off(void);
void led_on(void);
void rover(uint32_t freq, uint8_t dirmode);        //does current step mode
static uint8_t range, buf[64], distance, callonce;
struct notes_struct basic[4] = { 440.0, 100, 470.0, 100, 2600.0, 200, 1000.0, 500 };

//Entry point of firmware
int main(void)
{
    uint8_t step_mode = n32_STEP, counter = 0, recording_flag=0, last_cmd=0;
    uint8_t photovore_mode=0, recording_flag_pi = 0;
    uint16_t lux_threshold;
    uint32_t freq = 1000, steps = 200, i, ms_cnt;
    float32_t ambient_value;
    ret_code_t err_code;
    song.num_notes = 3;
    song.notes = basic;
 
    nrf_gpio_cfg_output(GREEN_LED);
    led_off();
    my_configure();           //sets gpios and PWM0 for step, PWM1 for buzzer
    motors_sleep();
    stepping_mode(step_mode); //step mode at 32
     
    uart_init();
    
    configure_microphone();

    #if SPARKFUN == 0
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
    db_discovery_init();
    advertising_start();

    #if ADHOC_TEST
      adhoc_robot_test();
    #endif
    #if 0
    //SPARKFUN
      bb_test();
    #endif

    //Show user robot is indeed on and ready
    led_on();                         
    pwm_buzzer_frequency(4000.0, 400);    //higher frequencies are louder with this buzzer          
    while(buzzer_loops_done == 0);
    pwm_buzzer_frequency(5000.0, 400);              
    while(buzzer_loops_done == 0);
    pwm_buzzer_frequency(6000.0, 400);              
    while(buzzer_loops_done == 0);
    led_off();

    recording_flag = 0;
    recording_flag_pi = 0;
    pi_reads_active = 0;
    new_cmd = 0;
    last_cmd = 0;
    motor_state = MOTORS_SLEEP;
    turn_flag = 0;
    for (;;)
    {
          //Trap here when BLE not connected
          callonce = 1;
          while (BLE_P_Connected == 0)
          {
            if (callonce == 1)
            {
              stop_buzzer();
              stop_stepping_gpio();
              motors_sleep();
              callonce = 0;
            }
            nrf_delay_ms(200);      //BLE not connected robot heartbeat
            led_on();
            nrf_delay_ms(200);
            led_off();
            TxUART(teststr);
          }
          if (recording_flag == 1)
          {
            if (nrf_pdm_event_check(NRF_PDM_EVENT_END) == true)
            {
              led_off();
              recording_flag = 0;
              nrf_drv_pdm_stop();
              //do_dft();             //do fft, then check m_fft_output_f32 64 bytes
              data_value = 255;       //kind of a not good flag
              update_remote_byte();
              ms_cnt = 0;
              i=0;
              while(i<SAMPLE_BUFFER_CNT)    //this is tricky, int16_t == 2 bytes * 10 = 20 bytes
              {
                  //This tough, assume 50Hz or every 20ms. It will take 5s to transfer 32k, if perfect
                  err_code = update_remote_multi_byte(i);
                  if (err_code == NRF_ERROR_RESOURCES || err_code == NRF_ERROR_BUSY)
                  {
                    nrf_delay_ms(2);  //extra delay
                    continue;
                  }
                  else
                  {
                    i+=MULTI_LEN/2;            //only increment is successful (assume)
                  }
                  nrf_delay_ms(19);   //cause slight overrun
                  ++ms_cnt;
                  if (!(ms_cnt % 20))
                    led_off();
                  if (!(ms_cnt % 40))
                    led_on();
               }
               data_value = 127;       //kind of a not good flag
               update_remote_byte();
            }
          }
          if (recording_flag_pi == 1)
          {
            if (nrf_pdm_event_check(NRF_PDM_EVENT_END) == true)
            {
              led_off();
              nrf_drv_pdm_stop();
              load_buffer_offset = 0;
              recording_flag_pi = 0;
              //do_dft();           
              sound_flag.len = 1;                           //sound flag is a 1byte characteristic struct
              data_value = 255;                             //recording done flag, tell Pi to start reading
              sound_flag.p_value = &data_value;
              sound_flag.offset = 0;
              sd_ble_gatts_value_set(m_conn_p_handle,data_handle.value_handle,&sound_flag); //signal pi to start reading
              pi_reads_active = 1;
            }
          }
          if (pi_reads_active == 1)
          {
              if (load_buffer_offset >= SAMPLE_BUFFER_CNT-1)  //end of 10 uint16_t chunks of buffer
              {
                sound_flag.len = 1;
                data_value = 127;                 //reading done flag, tell Pi
                sound_flag.p_value = &data_value;
                sound_flag.offset = 0;
                sd_ble_gatts_value_set(m_conn_p_handle,data_handle.value_handle,&sound_flag);
                pi_reads_active = 0;
                load_buffer_offset = 0;
              }
          }
          if (photovore_mode == 1)
          {
              ambient_value = getAmbientLight(GAIN_1);      //indoors LUX is likely 10-1000
              data2_value = (uint16_t)ambient_value;            
              if (data2_value > lux_threshold+4)
              {
                motors_wake();
              }
              else
              {
                motors_sleep();
              }
              update_remote_2byte();
          }
          if (new_cmd == 1)
          {
            if (cmd_value != GET_DISTANCE && cmd_value != GET_AMBIENT)
              led_on();
            photovore_mode = 0;   //any new command cancels mode
            switch(cmd_value)
            {
            case MOTORS_RIGHT:          //go right 30 degrees
              if (motor_state == MOTORS_FORWARD || motor_state == MOTORS_BACKWARD)
              {
                motors_right();
              }
              else
              {
                motors_right();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              break;
            case MOTORS_LEFT:           //go left 30 degrees
              if (motor_state == MOTORS_FORWARD || motor_state == MOTORS_BACKWARD)
              {
                motors_left();
              }
              else
              {
                motors_left();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              break;
            case MOTORS_RIGHT_30:          //go right 30 degrees
              if (motor_state == MOTORS_FORWARD || motor_state == MOTORS_BACKWARD)
              {
                timer1_turn_count = 0;
                motors_right();
                while (timer1_turn_count < (step_mode+1)*24);
                if (motor_state == MOTORS_FORWARD)
                  motors_forward();
                else
                  motors_backward();
              }
              else
              {
                motors_right();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              break;
            case MOTORS_LEFT_30:           //go left 30 degrees
              if (motor_state == MOTORS_FORWARD || motor_state == MOTORS_BACKWARD)
              {
                timer1_turn_count = 0;
                motors_left();
                while (timer1_turn_count < (step_mode+1)*24);
                if (motor_state == MOTORS_FORWARD)
                  motors_forward();
                else
                  motors_backward();
              }
              else
              {
                motors_left();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              break;
            case MOTORS_FORWARD:
              if (motor_state != MOTORS_FORWARD && motor_state != MOTORS_BACKWARD)
              {
                motor_state = MOTORS_FORWARD;
                motors_forward();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              else
              {
                motor_state = MOTORS_FORWARD;
                motors_forward();
              }
              break;
            case MOTORS_BACKWARD:
              if (motor_state != MOTORS_FORWARD && motor_state != MOTORS_BACKWARD)
              {
                motor_state = MOTORS_BACKWARD;
                motors_backward();
                motors_wake();
                start_stepping_gpio(freq);     
              }
              else
              {
                motor_state = MOTORS_BACKWARD;
                motors_backward();
              }
              break;
            case STOP_TURNING:
              switch(motor_state)
              {
                case MOTORS_FORWARD: 
                  motors_forward();
                  break;
                case MOTORS_BACKWARD:
                  motors_backward();
                  break;
                default:
                  break;
              }
              break;
            case MOTORS_STOP:
              motor_state = MOTORS_STOP;
              stop_stepping_gpio();
              motors_sleep();
              break;
            case ROVER_MODE:
              rover(freq,0);
              motor_state = MOTORS_STOP;
              break;
            case ROVER_MODE_REV:
              rover(freq,1);
              motor_state = MOTORS_STOP;
              break;
            case FOTOV_MODE:
              motor_state = MOTORS_STOP;
              photovore_mode = 1;
              motors_forward();
              start_stepping_gpio(freq);
              ambient_value = getAmbientLight(GAIN_1);      //indoors LUX is likely 10-1000
              ambient_value = getAmbientLight(GAIN_1);      
              lux_threshold = (uint16_t)ambient_value;            
              break;
            case MOTORS_SLEEP:
              motor_state = MOTORS_SLEEP;
              motors_sleep();
              break;
            case PLAY_BUZZER:
              if(buzzer_loops_done == 1)
                pwm_buzzer_frequency(4000.0, 1000); 
              break;
            case INC_STEP_MODE:
              motors_sleep();
              ++step_mode;
              if (step_mode == 6)
              {
                step_mode = 0;
              }
              switch(step_mode)
              {
                case FULL_STEP:
                  freq = 100;
                  break;
                case HALF_STEP:
                  freq = 200;
                  break;
                case QUARTER_STEP:
                  freq = 400;
                  break;
                case n8_STEP:
                  freq = 400;
                  break;
                case n16_STEP:
                  freq = 500;  
                  break;
                case n32_STEP:
                  freq = 1000;
                  break;
              }
              stepping_mode(step_mode);
              data_value = step_mode;
              update_remote_byte();
              break;
            case DEC_STEP_MODE:
              motors_sleep();
              if (step_mode != 0)
              {
                --step_mode;
              }
              switch(step_mode)
              {
                case FULL_STEP:
                  freq = 100;
                  break;
                case HALF_STEP:
                  freq = 200;
                  break;
                case QUARTER_STEP:
                  freq = 300;
                  break;
                case n8_STEP:
                  freq = 400;
                  break;
                case n16_STEP:
                  freq = 500;  
                  break;
                case n32_STEP:
                  freq = 1000;
                  break;
              }
              stepping_mode(step_mode);
              data_value = step_mode;
              update_remote_byte();
              break;
            case GET_DISTANCE:
              data_value = getDistance();                   //first set characteristic for host reads
              data_val.len = 1;                             //also send for notification
              data_val.p_value = &data_value;
              data_val.offset = 0;
              sd_ble_gatts_value_set(m_conn_p_handle,data_handle.value_handle,&data_val);
              update_remote_byte();
              sprintf(buf,"%u\r\n",data_value);
              TxUART(buf);
              break;
            case GET_AMBIENT:                               //I see Ambient LUX 34-65
              ambient_value = getAmbientLight(GAIN_1);      //if this returns 0, step through and get error code
              data2_value = (uint16_t)ambient_value;            
              data_val.len = 2;                             //set for host reads, then notify
              data_val.p_value = &data2_value;
              data_val.offset = 0;
              sd_ble_gatts_value_set(m_conn_p_handle,data_2byte_handle.value_handle,&data_val);
              update_remote_2byte();
              sprintf(buf,"%u\r\n",data2_value);
              TxUART(buf);
              break;
            case RECORD_SOUND:
              data_value = 0;       //using data_value this way is not a good use for a flag
              update_remote_byte();
              m_xfer_done = false;
              for(i=0;(i<SAMPLE_BUFFER_CNT);i++)
                p_rx_buffer[i] = 0;
              nrf_drv_pdm_buffer_set(p_rx_buffer, SAMPLE_BUFFER_CNT);
              nrf_pdm_gain_set(NRF_PDM_GAIN_MAXIMUM,NRF_PDM_GAIN_MAXIMUM);
              nrf_pdm_event_clear(NRF_PDM_EVENT_END);
              led_on();
              nrf_drv_pdm_start();
              recording_flag = 1;
              break;
            case RECORD_SOUND_PI:
              sound_flag.len = 1;
              data_value = 0;                   //tell Pi recording, not ready to read yet
              sound_flag.p_value = &data_value;
              sound_flag.offset = 0;
              sd_ble_gatts_value_set(m_conn_p_handle,data_handle.value_handle,&sound_flag);
              m_xfer_done = false;
              for(i=0;(i<SAMPLE_BUFFER_CNT);i++)
                p_rx_buffer[i] = 0;
              nrf_drv_pdm_buffer_set(p_rx_buffer, SAMPLE_BUFFER_CNT);
              nrf_pdm_gain_set(NRF_PDM_GAIN_MAXIMUM,NRF_PDM_GAIN_MAXIMUM);
              nrf_pdm_event_clear(NRF_PDM_EVENT_END);
              led_on();
              nrf_drv_pdm_start();
              recording_flag_pi = 1;
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
           case CONNECT_DISCONNECT:
              if (BLE_C_Connected == 0)
                scan_start();
              else
                err_code = sd_ble_gap_disconnect(m_conn_c_handle,BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
              break;
           default:
               break;
          }
          if (BLE_C_Connected == 1)       //if connected forward command
          {
              data_value = cmd_value;
              ble_gattc_write_params_t params;
              params.write_op = BLE_GATT_OP_WRITE_CMD;
              params.flags = BLE_GATT_EXEC_WRITE_FLAG_PREPARED_WRITE;
              params.handle = remote_cmd_handle.value_handle;
              params.offset = 0;
              params.p_value = &data_value;
              params.len = 1;
              err_code = sd_ble_gattc_write(m_conn_c_handle,&params);	
          }
          new_cmd = 0;
          last_cmd = cmd_value;
      }
      else
      {
        if (recording_flag == 0 && recording_flag_pi == 0)
        {
          led_off();
        }
        //play_song();
      }
   }  
}

//This is bug specific, please ignore
void adhoc_robot_test(void)
{
    uint8_t val;

    stepping_mode(FULL_STEP);
    motors_backward();
    motors_wake();
    start_stepping_gpio(100);
    val = 0;
    while(1) 
    {
        switch(val)
        {
          case 0:
            led_on();
            motors_forward();
            motors_wake();
            break;
          case 1:
            led_off();
            motors_sleep();
            break;
          case 2:
            led_on();
            motors_backward();
            motors_wake();
            break;
          case 3:
            led_off();
            motors_sleep();
            break;
          default:
            motors_wake();
            stop_stepping_gpio();     //make sure stopped before re-starting
            start_stepping_gpio(100);
            break;
        }
        ++val;


        if (val == 4)
        if (val == 4)
          val = 0;
        nrf_delay_ms(2000);
    }
}

void bb_test(void)
{
    motors_backward();
    motors_wake();
    start_stepping_gpio(100);
    while(1) 
    {
       if (new_cmd == 1)
       {
          switch(cmd_value)
          {
            case MOTORS_FORWARD:
              motors_forward();
              break;
            case MOTORS_BACKWARD:
              motors_backward();
              break;
            case MOTORS_STOP:
              stop_stepping_gpio();
              motors_sleep();
              break;
            default:
              motors_wake();
              stop_stepping_gpio();     //make sure stopped before re-starting
              start_stepping_gpio(100);
              break;
          }
          new_cmd = 0;
      }
      nrf_delay_ms(500);
      led_on();
      nrf_delay_ms(500);
      led_off();
    }
}

//Inherits stepping mode and freq from main()
void rover(uint32_t freq, uint8_t rover_dir)
{
  uint8_t distance;

  if (rover_dir == 0)
    motors_forward();
  else
    motors_backward();
  motors_wake();
  start_stepping_gpio(freq);       
  new_cmd = 0;
  while(new_cmd == 0)
  {
      distance = getDistance();
      if (distance < 50)
      {
        led_on();
        motors_left();
      }
      else
      {
        if (rover_dir == 0)
          motors_forward();
        else
          motors_backward();
        led_off();
      }
      nrf_delay_ms(50);
  }
  stop_stepping_gpio();
  motors_sleep();
  return;
}

//This isn't done
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

//orphaned function, obsolete
void turn_left_90_degrees(void)
{
  motors_left();            //turn left
  motors_wake();
  start_stepping_gpio(50);   
  nrf_delay_ms(100);        //this should be 5 steps each motor for 90 degree turn
  stop_stepping_gpio();          //if it doesn't work I'll try doing 5 steps manually
  motors_sleep();
}

//Left channel mono configured, continuous int16, no alternating right channel
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

//This is obsolete
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
            start_stepping_gpio(100);
            if (freq == 40)
              led_on();                 //mark start of test
            else
              led_off();
            motors_wake();
            nrf_delay_ms(2000);
            stop_stepping_gpio();
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
  nrf_gpio_pin_set(DIR_L);
  nrf_gpio_pin_clear(DIR_R);   
}

void motors_backward(void)
{
   nrf_gpio_pin_clear(DIR_L);
   nrf_gpio_pin_set(DIR_R);
}

void motors_left(void)
{
   nrf_gpio_pin_clear(DIR_L);
   nrf_gpio_pin_clear(DIR_R);
}

void motors_right(void)
{
   nrf_gpio_pin_set(DIR_L);
   nrf_gpio_pin_set(DIR_R);
}

void stepping_mode(uint8_t mode)
{
    switch(mode)
    {
        case FULL_STEP:     //full step
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M0);
          nrf_gpio_pin_clear(M1);
          break;
        case HALF_STEP:     //1/2 step
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M0);
          nrf_gpio_pin_clear(M1);
          break;
        case QUARTER_STEP:     //1/4 step
          nrf_gpio_cfg_input(M0,NRF_GPIO_PIN_NOPULL);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M1);
          break;
        case n8_STEP:     //8 microsteps
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_clear(M0);
          nrf_gpio_pin_set(M1);
          break;
       case n16_STEP:     //16 microsteps
          nrf_gpio_cfg_output(M0);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M0);
          nrf_gpio_pin_set(M1);
          break;
       case n32_STEP:     //32 microsteps
          nrf_gpio_cfg_input(M0,NRF_GPIO_PIN_NOPULL);
          nrf_gpio_cfg_output(M1);
          nrf_gpio_pin_set(M1);
          break;
      default:            //full step
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

void stop_stepping_gpio(void)
{
  nrf_timer_task_trigger(NRF_TIMER1,NRF_TIMER_TASK_STOP);
}

void start_stepping_gpio(uint16_t freq)
{
  nrf_gpio_cfg_output(STEP);
  nrf_gpio_pin_clear(STEP);
 
  timer1_toggle_step = 1;
  timer1_counter = 0;
  timer1_match_value = 2500 / freq;    //does 2 edges, so 5000/2 per sec
  timer1_enabled_for_motors = 1;
  nrf_timer_shorts_enable(NRF_TIMER1,NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK);
  nrf_timer_task_trigger(NRF_TIMER1,NRF_TIMER_TASK_START);    
}

//TIMER1 - this is the motor timer, runs at 200us or 5kHz
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
          ++timer1_turn_count;
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

#if MOTORS_STEPPING_PWM
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
  nrf_pwm_configure(NRF_PWM0,PWM_PRESCALER_PRESCALER_DIV_128,PWM_MODE_UPDOWN_Up,pwm_top);
  nrf_pwm_seq_ptr_set(NRF_PWM0,0,pwm_duty);
  nrf_pwm_seq_cnt_set(NRF_PWM0,0,1);
  nrf_pwm_seq_ptr_set(NRF_PWM0,1,pwm_duty);
  nrf_pwm_seq_cnt_set(NRF_PWM0,1,1);
  nrf_pwm_loop_set(NRF_PWM0,number_steps / 2);
  nrf_pwm_task_trigger(NRF_PWM0, NRF_PWM_TASK_SEQSTART0);
  step_loop_done = 0;
}

void PWM0_IRQHandler(void)
{
    stop_stepping();
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
#endif

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
  nrf_pwm_configure(NRF_PWM1,PWM_PRESCALER_PRESCALER_DIV_128,PWM_MODE_UPDOWN_Up,pwm_top);
  nrf_pwm_seq_ptr_set(NRF_PWM1,0,pwm_duty);
  nrf_pwm_seq_cnt_set(NRF_PWM1,0,1);
  nrf_pwm_seq_ptr_set(NRF_PWM1,1,pwm_duty);
  nrf_pwm_seq_cnt_set(NRF_PWM1,1,1);
  nrf_pwm_loop_set(NRF_PWM1,loops / 2);
  nrf_pwm_task_trigger(NRF_PWM1, NRF_PWM_TASK_SEQSTART0);
  buzzer_loops_done = 0;
}

//just toggles gpios for now
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

void uart_init(void)
{
  nrf_uarte_baudrate_set(NRF_UARTE0,NRF_UARTE_BAUDRATE_115200); 
  nrf_uarte_txrx_pins_set(NRF_UARTE0,UART_TX_PIN,UART_RX_PIN);
  nrf_uarte_configure(NRF_UARTE0,NRF_UARTE_PARITY_EXCLUDED,NRF_UARTE_HWFC_DISABLED);  //no parity, no hw flow control
  nrf_uarte_rx_buffer_set(NRF_UARTE0,recvbuffer,20);
  nrf_uarte_tx_buffer_set(NRF_UARTE0,sendbuffer,20);
  nrf_uarte_enable(NRF_UARTE0);
}

void my_configure(void)
{
  nrf_gpio_cfg_output(DIR_L);
  nrf_gpio_cfg_output(DIR_R);
  nrf_gpio_cfg_output(SLEEP);
  motors_sleep();               
  motors_forward();
  stepping_mode(HALF_STEP);         

  nrf_gpio_pin_clear(STEP);
  nrf_gpio_cfg_output(STEP);

#if MOTORS_STEPPING_PWM
  NRF_PWM0->PSEL.OUT[0] = STEP;

  nrf_pwm_enable(NRF_PWM0);
  nrf_pwm_configure(NRF_PWM0,PWM_PRESCALER_PRESCALER_DIV_128,PWM_MODE_UPDOWN_Up,1000);
  nrf_pwm_loop_set(NRF_PWM0,0);
  nrf_pwm_decoder_set(NRF_PWM0,PWM_DECODER_LOAD_Common,PWM_DECODER_MODE_RefreshCount);
  nrf_pwm_seq_refresh_set(NRF_PWM0,0,0);
  nrf_pwm_seq_end_delay_set(NRF_PWM0,0,0);
  nrf_pwm_seq_refresh_set(NRF_PWM0,1,0);
  nrf_pwm_seq_end_delay_set(NRF_PWM0,1,0);
  nrf_pwm_shorts_set(NRF_PWM0, 0);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM0, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_int_set(NRF_PWM0, NRF_PWM_INT_LOOPSDONE_MASK);
  nrf_drv_common_irq_enable(PWM0_IRQn,APP_IRQ_PRIORITY_LOWEST);
#else
  //use gpio STEP and timer1
  nrf_timer_event_clear(NRF_TIMER1,NRF_TIMER_EVENT_COMPARE0);
  nrf_timer_mode_set(NRF_TIMER1,NRF_TIMER_MODE_TIMER);
  nrf_timer_bit_width_set(NRF_TIMER1,NRF_TIMER_BIT_WIDTH_32);
  nrf_timer_frequency_set(NRF_TIMER1,NRF_TIMER_FREQ_125kHz);
  nrf_timer_cc_write(NRF_TIMER1,NRF_TIMER_CC_CHANNEL0,25);        // 200us per interrupt
  nrf_timer_int_enable(NRF_TIMER1,NRF_TIMER_INT_COMPARE0_MASK);
  nrf_drv_common_irq_enable(TIMER1_IRQn,APP_IRQ_PRIORITY_LOWEST);
#endif

  nrf_gpio_pin_clear(BUZZER_10MM);
  nrf_gpio_cfg_output(BUZZER_10MM);
  NRF_PWM1->PSEL.OUT[0] = BUZZER_10MM;

  nrf_pwm_enable(NRF_PWM1);
  nrf_pwm_configure(NRF_PWM1,PWM_PRESCALER_PRESCALER_DIV_128,PWM_MODE_UPDOWN_Up,1000);
  nrf_pwm_loop_set(NRF_PWM1,0);
  nrf_pwm_decoder_set(NRF_PWM1,PWM_DECODER_LOAD_Common,PWM_DECODER_MODE_RefreshCount);
  nrf_pwm_seq_refresh_set(NRF_PWM1,0,0);
  nrf_pwm_seq_end_delay_set(NRF_PWM1,0,0);
  nrf_pwm_seq_refresh_set(NRF_PWM1,1,0);
  nrf_pwm_seq_end_delay_set(NRF_PWM1,1,0);
  nrf_pwm_shorts_set(NRF_PWM1, 0);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_LOOPSDONE);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND0);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_SEQEND1);
  nrf_pwm_event_clear(NRF_PWM1, NRF_PWM_EVENT_STOPPED);
  nrf_pwm_int_set(NRF_PWM1, NRF_PWM_INT_LOOPSDONE_MASK);
  nrf_drv_common_irq_enable(PWM1_IRQn,APP_IRQ_PRIORITY_LOWEST);
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
    nrf_uarte_tx_buffer_set(NRF_UARTE0,sendbuffer,j);
    nrf_uarte_event_clear(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX);
    nrf_uarte_task_trigger(NRF_UARTE0,NRF_UARTE_TASK_STARTTX);
    while( nrf_uarte_event_check(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX) == 0);   //This blocks on transmit, maybe fix for parallelism
    nrf_uarte_event_clear(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX);
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
  sendbuffer[0] = 13;   //send carriage return and linefeed
  sendbuffer[1] = 10;
  nrf_uarte_tx_buffer_set(NRF_UARTE0,sendbuffer,2);
  nrf_uarte_event_clear(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX);
  nrf_uarte_task_trigger(NRF_UARTE0,NRF_UARTE_TASK_STARTTX);
  while( nrf_uarte_event_check(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX) == 0);   //This blocks on transmit, maybe fix for parallelism
  nrf_uarte_event_clear(NRF_UARTE0,NRF_UARTE_EVENT_ENDTX);
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

// BLE CENTRAL CODE
/**
 * @brief Parses advertisement data, providing length and location of the field in case
 *        matching data is found.
 *
 * @param[in]  type       Type of data to be looked for in advertisement data.
 * @param[in]  p_advdata  Advertisement report length and pointer to report.
 * @param[out] p_typedata If data type requested is found in the data report, type data length and
 *                        pointer to data will be populated here.
 *
 * @retval NRF_SUCCESS if the data type is found in the report.
 * @retval NRF_ERROR_NOT_FOUND if the data type could not be found.
 */
static uint32_t adv_report_parse(uint8_t type, data_t * p_advdata, data_t * p_typedata)
{
    uint32_t  index = 0;
    uint8_t * p_data;

    p_data = p_advdata->p_data;

    while (index < p_advdata->data_len)
    {
        uint8_t field_length = p_data[index];
        uint8_t field_type   = p_data[index + 1];

        if (field_type == type)
        {
            p_typedata->p_data   = &p_data[index + 2];
            p_typedata->data_len = field_length - 1;
            return NRF_SUCCESS;
        }
        index += field_length + 1;
    }
    return NRF_ERROR_NOT_FOUND;
}
/**@brief Function for handling database discovery events.
 *
 * @details This function is callback function to handle events from the database discovery module.
 *          Depending on the UUIDs that are discovered, this function should forward the events
 *          to their respective services.
 *
 * @param[in] p_event  Pointer to the database discovery event.
 */
static void db_disc_handler(ble_db_discovery_evt_t * p_evt)
{
  uint8_t i,j;

  if (p_evt->evt_type == BLE_DB_DISCOVERY_COMPLETE)
  {
    j = p_evt->params.discovered_db.char_count;
    for(i=0;(i<j);i++)
    {
        if(p_evt->params.discovered_db.charateristics[i].characteristic.uuid.uuid == LBS_UUID_CMD_CHAR)
        {
            remote_cmd_handle.value_handle = p_evt->params.discovered_db.charateristics[i].characteristic.handle_value;
            remote_cmd_handle.cccd_handle =  p_evt->params.discovered_db.charateristics[i].cccd_handle;
            TxUART("Got the handle\r\n");
            return;
        }
    }
  }
}

/**@brief Database discovery initialization.
 */
static void db_discovery_init(void)
{
    ret_code_t err_code = ble_db_discovery_init(db_disc_handler);
    APP_ERROR_CHECK(err_code);
    ble_db_discovery_evt_register(&ble_uuid_svc);
}

/**@brief Function to start scanning.
 */
static void scan_start(void)
{
    ret_code_t err_code;

    (void) sd_ble_gap_scan_stop();

    err_code = sd_ble_gap_scan_start(&m_scan_params);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling the advertising report BLE event.
 *
 * @param[in] p_ble_evt  Bluetooth stack event.
 */
static void on_adv_report(const ble_evt_t * const p_ble_evt)
{
    ret_code_t err_code;
    data_t     adv_data;
    data_t     dev_name;
    bool       do_connect = false;
    uint8_t    buf[32];

    // For readibility.
    ble_gap_evt_t  const * p_gap_evt  = &p_ble_evt->evt.gap_evt;
    ble_gap_addr_t const * peer_addr  = &p_gap_evt->params.adv_report.peer_addr;

    // Initialize advertisement report for parsing
    adv_data.p_data   = (uint8_t *)p_gap_evt->params.adv_report.data;
    adv_data.data_len = p_gap_evt->params.adv_report.dlen;

    // Search for advertising names.
    bool name_found = false;
    err_code = adv_report_parse(BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, &adv_data, &dev_name);

    if (err_code != NRF_SUCCESS)
    {
        // Look for the short local name if it was not found as complete.
        err_code = adv_report_parse(BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME, &adv_data, &dev_name);
        if (err_code != NRF_SUCCESS)
        {
            // If we can't parse the data, then exit
            TxUART("Cant parse\r\n");
            return;
        }
        else
        {
            name_found = true;
        }
    }
    else
    {
        name_found = true;
    }

    if (name_found)
    {
        if (strlen(m_target_periph_name) != 0)
        {
            if (memcmp(m_target_periph_name, dev_name.p_data, dev_name.data_len )== 0)
            {
                found_skoobot = 1;
                do_connect = true;
            }
        }
    }

    if (do_connect)
    {
        (void) sd_ble_gap_scan_stop();
        TxUART("Try to connect\r\n");
        // Initiate connection.
        err_code = sd_ble_gap_connect(peer_addr,
                                      &m_scan_params,
                                      &m_connection_param,
                                      APP_BLE_CONN_CFG_TAG);
        APP_ERROR_CHECK(err_code);

    }
}

//BLE_PERIPHERHAL CODE
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
                                           &cmd_handle);

}

static uint32_t add_cmd4_characteristic(void)
{
    ble_gatts_char_md_t char_md;          //server characteristic metadata
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          ble_uuid;
    ble_gatts_attr_md_t attr_md;
    uint8_t i;

    memset(&char_md, 0, sizeof(char_md));

    char_md.char_props.read  = 1;
    char_md.char_props.write = 1;
    char_md.p_char_user_desc = NULL;
    char_md.p_char_pf        = NULL;
    char_md.p_user_desc_md   = NULL;
    char_md.p_cccd_md        = NULL;
    char_md.p_sccd_md        = NULL;

    ble_uuid.type = uuid_type;
    ble_uuid.uuid = LBS_UUID_BYTE4_CHAR;

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
    attr_char_value.init_len  = 4;
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = 4;
    attr_char_value.p_value   = NULL;

    return sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &data_4byte_handle);

}

//Have 3 characteristics to do, just doing 20 byte for sound pcm file transfer
static uint32_t add_mult_data_characteristics(void)
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
    char_md.char_props.write  = 1;
    char_md.char_props.notify = 1;
    char_md.p_char_user_desc  = NULL;
    char_md.p_char_pf         = NULL;
    char_md.p_user_desc_md    = NULL;
    char_md.p_cccd_md         = &cccd_md;
    char_md.p_sccd_md         = NULL;

    ble_uuid.type = uuid_type;
    ble_uuid.uuid = LBS_UUID_BYTE128_CHAR;

    memset(&attr_md, 0, sizeof(attr_md));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    attr_md.vloc    = BLE_GATTS_VLOC_STACK;
    attr_md.rd_auth = 1;                      //Have to set this to get an authorize event
    attr_md.wr_auth = 0;
    attr_md.vlen    = 0;

    memset(&attr_char_value, 0, sizeof(attr_char_value));

    attr_char_value.p_uuid    = &ble_uuid;
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = MULTI_LEN;
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = MULTI_LEN;
    attr_char_value.p_value   = NULL;

    return sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &data_128byte_handle);   
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

    return sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &data_handle);   
}

static uint32_t add_data2_characteristic(void)
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
    ble_uuid.uuid = LBS_UUID_BYTE2_CHAR;

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
    attr_char_value.init_len  = 2;
    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = 2;
    attr_char_value.p_value   = NULL;

    return sd_ble_gatts_characteristic_add(svc_handle,
                                           &char_md,
                                           &attr_char_value,
                                           &data_2byte_handle);   
}
/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
    ret_code_t     err_code;
  
  // Add service.
    ble_uuid128_t base_uuid = {LBS_UUID_BASE};
    err_code = sd_ble_uuid_vs_add(&base_uuid, &uuid_type);
    APP_ERROR_CHECK(err_code);

    ble_uuid_svc.uuid = LBS_UUID_SERVICE;
    ble_uuid_svc.type = uuid_type;

    err_code = sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &ble_uuid_svc, &svc_handle);
    APP_ERROR_CHECK(err_code);
       
    err_code = add_cmd_characteristic();
    APP_ERROR_CHECK(err_code);
    
    err_code = add_data_characteristic();
    APP_ERROR_CHECK(err_code);

    err_code = add_data2_characteristic();
    APP_ERROR_CHECK(err_code);
    
    err_code = add_cmd4_characteristic();
    APP_ERROR_CHECK(err_code);
 
    err_code = add_mult_data_characteristics();
    APP_ERROR_CHECK(err_code);
}

static uint32_t update_remote_byte(void)
{
    ble_gatts_hvx_params_t params;
    uint16_t len = 1;

    memset(&params, 0, sizeof(params));
    params.type   = BLE_GATT_HVX_NOTIFICATION;
    params.handle = data_handle.value_handle;
    params.p_data = &data_value;
    params.p_len  = &len;

    return sd_ble_gatts_hvx(m_conn_p_handle, &params);
}

static uint32_t update_remote_2byte(void)
{
    ble_gatts_hvx_params_t params;
    uint16_t len = 2;

    data_2byte_val[0] = (uint8_t)(data2_value>>8)&0x00ff;
    data_2byte_val[1] = (uint8_t)(data2_value&0x00ff);

    memset(&params, 0, sizeof(params));
    params.type   = BLE_GATT_HVX_NOTIFICATION;
    params.handle = data_2byte_handle.value_handle;
    params.p_data = data_2byte_val;
    params.p_len  = &len;

    return sd_ble_gatts_hvx(m_conn_p_handle, &params);
}

//Connection interval set to 50Hz, higher needs better signal strength
//Each 20ms period, we send 128 bytes, it takes .02s*(32k/128)=5.12s
static uint32_t update_remote_multi_byte(uint32_t index)
{
    ble_gatts_hvx_params_t params;
    uint16_t len = MULTI_LEN;
    uint16_t i, j;

    i = j = 0;
    while(i<len)
    {
      data_128byte_val[i+1] = (uint8_t)((p_rx_buffer[index+j]>>8)&0x00ff);
      data_128byte_val[i] = (uint8_t)(p_rx_buffer[index+j]&0x00ff);
      i+=2;
      ++j;
    }
    memset(&params, 0, sizeof(params));
    params.type   = BLE_GATT_HVX_NOTIFICATION;
    params.handle = data_128byte_handle.value_handle;
    params.p_data = data_128byte_val;
    params.p_len  = &len;

    return sd_ble_gatts_hvx(m_conn_p_handle, &params);
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
void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    ret_code_t err_code;

    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        err_code = sd_ble_gap_disconnect(m_conn_p_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}


/**@brief Function for handling a Connection Parameters error.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
void conn_params_error_handler(uint32_t nrf_error)
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

/**@brief Function for handling BLE_GAP_EVT_CONNECTED events.
 * Save the connection handle and GAP role, then discover the peer DB.

static void on_ble_gap_evt_connected(ble_gap_evt_t const * p_gap_evt)
{
    ret_code_t err_code;

 
}
*/
/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t err_code;
    uint16_t i, j;
    uint8_t buf[128];

    ble_gap_evt_t const * p_gap_evt = &p_ble_evt->evt.gap_evt;
    m_gap_role = p_gap_evt->params.connected.role;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
        {
            if (m_gap_role == BLE_GAP_ROLE_PERIPH)
            {
              (void) sd_ble_gap_adv_stop();
              m_conn_p_handle = p_gap_evt->conn_handle;
              TxUART("Connected Peripheral\r\n");
              BLE_P_Connected = 1;
            }
            else
            {
              if (m_gap_role == BLE_GAP_ROLE_CENTRAL)
              {
                m_conn_c_handle = p_gap_evt->conn_handle;
                err_code = ble_db_discovery_start(&m_db_disc, p_gap_evt->conn_handle);
                APP_ERROR_CHECK(err_code);
                BLE_C_Connected = 1;
                TxUART("Connect Central\n");
              }
            }
        }
            break;
        
        case BLE_GAP_EVT_DISCONNECTED:
        {
            if (p_gap_evt->conn_handle == m_conn_p_handle)
            {
              TxUART("Disconnected peripheral\r\n");
              m_conn_p_handle = BLE_CONN_HANDLE_INVALID;
              BLE_P_Connected = 0;
              advertising_start();
            }
            else
            {
              if (p_gap_evt->conn_handle == m_conn_c_handle)
              {
                TxUART("Disconnected central\r\n");
                m_conn_c_handle = BLE_CONN_HANDLE_INVALID;
                BLE_C_Connected = 0;
              }
            }
       }
            break;
       case BLE_GAP_EVT_ADV_REPORT:
            
            on_adv_report(p_ble_evt);
            break;

        case BLE_GAP_EVT_TIMEOUT:
            // We have not specified a timeout for scanning, so only connection attemps can timeout.
            if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN)
            {
                TxUART("Connection request timed out.");
            }
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
        
            // Accept parameters requested by peer.
            err_code = sd_ble_gap_conn_param_update(p_gap_evt->conn_handle,
                                        &p_gap_evt->params.conn_param_update_request.conn_params);
            APP_ERROR_CHECK(err_code);
            break;
        
        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported

            err_code = sd_ble_gap_sec_params_reply(p_ble_evt->evt.gap_evt.conn_handle,
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
            err_code = sd_ble_gatts_sys_attr_set(p_ble_evt->evt.gap_evt.conn_handle, NULL, 0, 0);
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
            static ble_gatts_evt_rw_authorize_request_t  req;
            static ble_gatts_rw_authorize_reply_params_t auth_reply;

            req = p_ble_evt->evt.gatts_evt.params.authorize_request;

            if (req.type == BLE_GATTS_AUTHORIZE_TYPE_READ && req.request.read.handle == data_128byte_handle.value_handle)
            {
              if (pi_reads_active == 1)
              {
                  //This is a little tricky, i increments by 2 and goes to 20, j increments by 1 and goes to 10
                  i = j = 0;
                  while(i<MULTI_LEN)
                  {
                    data_128byte_val[i+1] = (uint8_t)((p_rx_buffer[load_buffer_offset+j]>>8)&0x00ff);
                    data_128byte_val[i] = (uint8_t)(p_rx_buffer[load_buffer_offset+j]&0x00ff);
                    i+=2;
                    ++j;
                  }
                  //sound_value.offset = 0;
                  //sound_value.len = 20;
                  //sound_value.p_value = data_128byte_val;
                  //sd_ble_gatts_value_set(m_conn_handle,data_128byte_handle.value_handle,&sound_value);
                  
                  load_buffer_offset += MULTI_LEN/2; //increment by 10
  
                  auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                  auth_reply.params.read.gatt_status = BLE_GATT_STATUS_SUCCESS;
                  auth_reply.params.read.update = 1;
                  auth_reply.params.read.len = MULTI_LEN;
                  auth_reply.params.read.offset = 0;
                  auth_reply.params.read.p_data = data_128byte_val;
                  err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                               &auth_reply);
                  if (err_code != NRF_SUCCESS)
                  {
                    load_buffer_offset -= MULTI_LEN/2;    //decrement for retry, does NRF do retry after failure?
                  }                 
                  return;
              }
              else  //if it gets here, it is a startup or discovery read, must be authorized.
              {
                  memset(&auth_reply,0,sizeof(ble_gatts_rw_authorize_reply_params_t));
                  auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                  auth_reply.params.read.gatt_status = BLE_GATT_STATUS_SUCCESS;
                  err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                               &auth_reply);
                  return; 
              }
            }
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
                    return;
                }
            }
        }
        break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST

      default:
        // No implementation needed.
        break;
    }
}

void on_write(ble_evt_t const * p_ble_evt)
{    
    ble_gatts_evt_write_t const * p_evt_write = &p_ble_evt->evt.gatts_evt.params.write;

    if ((p_evt_write->handle == cmd_handle.value_handle)
        && (p_evt_write->len == 1))
    {
         cmd_value = p_evt_write->data[0];
         new_cmd = 1;
    }
    else
    {
      if ((p_evt_write->handle == data_handle.value_handle)
          && (p_evt_write->len == 1))
      {
           //data_value = p_evt_write->data[0];
           new_cmd = 2;
      }
    }

}

void ble_skoobot_p_on_ble_evt(ble_evt_t const * p_ble_evt, void * p_context)
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

void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);
}


/*
*@brief Function for the Power Manager.
 */
void power_manage(void)
{
    ret_code_t err_code = sd_app_evt_wait();
    APP_ERROR_CHECK(err_code);
}

/*
*@brief Function for the Timer initialization.
 */
void timers_init(void)
{
    // Initialize timer module, making it use the scheduler
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
}