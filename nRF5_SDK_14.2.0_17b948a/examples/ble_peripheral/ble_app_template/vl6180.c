#include "vl6180.h"
#include "nrf_delay.h"

extern volatile bool m_xfer_done;
extern const nrf_drv_twi_t m_twi;

void VL6180xInit(void)
{
  uint8_t data; //for temp data storage

  data = VL6180x_getRegister(VL6180X_SYSTEM_FRESH_OUT_OF_RESET);

  if(data != 1) return VL6180x_FAILURE_RESET;

  //Required by datasheet
  //http://www.st.com/st-web-ui/static/active/en/resource/technical/document/application_note/DM00122600.pdf
  VL6180x_setRegister(0x0207, 0x01);
  VL6180x_setRegister(0x0208, 0x01);
  VL6180x_setRegister(0x0096, 0x00);
  VL6180x_setRegister(0x0097, 0xfd);
  VL6180x_setRegister(0x00e3, 0x00);
  VL6180x_setRegister(0x00e4, 0x04);
  VL6180x_setRegister(0x00e5, 0x02);
  VL6180x_setRegister(0x00e6, 0x01);
  VL6180x_setRegister(0x00e7, 0x03);
  VL6180x_setRegister(0x00f5, 0x02);
  VL6180x_setRegister(0x00d9, 0x05);
  VL6180x_setRegister(0x00db, 0xce);
  VL6180x_setRegister(0x00dc, 0x03);
  VL6180x_setRegister(0x00dd, 0xf8);
  VL6180x_setRegister(0x009f, 0x00);
  VL6180x_setRegister(0x00a3, 0x3c);
  VL6180x_setRegister(0x00b7, 0x00);
  VL6180x_setRegister(0x00bb, 0x3c);
  VL6180x_setRegister(0x00b2, 0x09);
  VL6180x_setRegister(0x00ca, 0x09);  
  VL6180x_setRegister(0x0198, 0x01);
  VL6180x_setRegister(0x01b0, 0x17);
  VL6180x_setRegister(0x01ad, 0x00);
  VL6180x_setRegister(0x00ff, 0x05);
  VL6180x_setRegister(0x0100, 0x05);
  VL6180x_setRegister(0x0199, 0x05);
  VL6180x_setRegister(0x01a6, 0x1b);
  VL6180x_setRegister(0x01ac, 0x3e);
  VL6180x_setRegister(0x01a7, 0x1f);
  VL6180x_setRegister(0x0030, 0x00);
}

void VL6180xDefautSettings(void)
{
  //Enable Interrupts on Conversion Complete (any source)
  VL6180x_setRegister(VL6180X_SYSTEM_INTERRUPT_CONFIG_GPIO, (4 << 3)|(4) ); // Set GPIO1 high when sample complete


  VL6180x_setRegister(VL6180X_SYSTEM_MODE_GPIO1, 0x10); // Set GPIO1 high when sample complete
  VL6180x_setRegister(VL6180X_READOUT_AVERAGING_SAMPLE_PERIOD, 0x30); //Set Avg sample period
  VL6180x_setRegister(VL6180X_SYSALS_ANALOGUE_GAIN, 0x46); // Set the ALS gain
  VL6180x_setRegister(VL6180X_SYSRANGE_VHV_REPEAT_RATE, 0xFF); // Set auto calibration period (Max = 255)/(OFF = 0)
  VL6180x_setRegister(VL6180X_SYSALS_INTEGRATION_PERIOD, 0x63); // Set ALS integration time to 100ms
  VL6180x_setRegister(VL6180X_SYSRANGE_VHV_RECALIBRATE, 0x01); // perform a single temperature calibration
  //Optional settings from datasheet
  //http://www.st.com/st-web-ui/static/active/en/resource/technical/document/application_note/DM00122600.pdf
  VL6180x_setRegister(VL6180X_SYSRANGE_INTERMEASUREMENT_PERIOD, 0x09); // Set default ranging inter-measurement period to 100ms
  VL6180x_setRegister(VL6180X_SYSALS_INTERMEASUREMENT_PERIOD, 0x0A); // Set default ALS inter-measurement period to 100ms
  VL6180x_setRegister(VL6180X_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x24); // Configures interrupt on ‘New Sample Ready threshold event’ 
  //Additional settings defaults from community
  VL6180x_setRegister(VL6180X_SYSRANGE_MAX_CONVERGENCE_TIME, 0x32);
  VL6180x_setRegister(VL6180X_SYSRANGE_RANGE_CHECK_ENABLES, 0x10 | 0x01);
  VL6180x_setRegister16bit(VL6180X_SYSRANGE_EARLY_CONVERGENCE_ESTIMATE, 0x7B );
  VL6180x_setRegister16bit(VL6180X_SYSALS_INTEGRATION_PERIOD, 0x64);

  VL6180x_setRegister(VL6180X_READOUT_AVERAGING_SAMPLE_PERIOD,0x30);
  VL6180x_setRegister(VL6180X_SYSALS_ANALOGUE_GAIN,0x40);
  VL6180x_setRegister(VL6180X_FIRMWARE_RESULT_SCALER,0x01);
}

uint8_t getDistance(void)
{
  VL6180x_setRegister(VL6180X_SYSRANGE_START, 0x01); //Start Single shot mode
  nrf_delay_ms(10);
  VL6180x_setRegister(VL6180X_SYSTEM_INTERRUPT_CLEAR, 0x07);
  //	return distance;
  return VL6180x_getRegister(VL6180X_RESULT_RANGE_VAL);
}

float32_t getAmbientLight(uint8_t VL6180X_ALS_GAIN)
{
  //First load in Gain we are using, do it every time in case someone changes it on us.
  //Note: Upper nibble shoudl be set to 0x4 i.e. for ALS gain of 1.0 write 0x46
  VL6180x_setRegister(VL6180X_SYSALS_ANALOGUE_GAIN, (0x40 | VL6180X_ALS_GAIN)); // Set the ALS gain

  //Start ALS Measurement 
  VL6180x_setRegister(VL6180X_SYSALS_START, 0x01);

  nrf_delay_ms(100); //give it time... 

  VL6180x_setRegister(VL6180X_SYSTEM_INTERRUPT_CLEAR, 0x07);

  //Retrieve the Raw ALS value from the sensoe
  unsigned int alsRaw = VL6180x_getRegister16bit(VL6180X_RESULT_ALS_VAL);
  
  //Get Integration Period for calculation, we do this every time in case someone changes it on us.
  unsigned int alsIntegrationPeriodRaw = VL6180x_getRegister16bit(VL6180X_SYSALS_INTEGRATION_PERIOD);
  
  float32_t alsIntegrationPeriod = 100.0 / alsIntegrationPeriodRaw ;

  //Calculate actual LUX from Appnotes

  float32_t alsGain = 0.0;
  
  switch (VL6180X_ALS_GAIN){
    case GAIN_20: alsGain = 20.0; break;
    case GAIN_10: alsGain = 10.32; break;
    case GAIN_5: alsGain = 5.21; break;
    case GAIN_2_5: alsGain = 2.60; break;
    case GAIN_1_67: alsGain = 1.72; break;
    case GAIN_1_25: alsGain = 1.28; break;
    case GAIN_1: alsGain = 1.01; break;
    case GAIN_40: alsGain = 40.0; break;
  }

  //Calculate LUX from formula in AppNotes
  
  float32_t alsCalculated = (float32_t)0.32 * ((float32_t)alsRaw / alsGain) * alsIntegrationPeriod;

  return alsCalculated;
}

void VL6180x_setRegister(uint16_t registerAddr, uint8_t data)
{
    uint8_t wrdata[3];
    ret_code_t err_code;

    wrdata[0] = (registerAddr>>8)&0xff;
    wrdata[1] = registerAddr&0xff;
    wrdata[2] = data;

   // Writing to register
    err_code = nrf_drv_twi_tx(&m_twi, VL6180X_ADDRESS, wrdata, 3, false);
 
    return;
}

void VL6180x_setRegister16bit(uint16_t registerAddr, uint16_t data)
{

    uint8_t wrdata[4];
    ret_code_t err_code;

    wrdata[0] = (registerAddr>>8)&0xff;
    wrdata[1] = registerAddr&0xff;
    wrdata[2] = (data>>8)&0xff;
    wrdata[3] = data&0xff;

   // Writing to register
    err_code = nrf_drv_twi_tx(&m_twi, VL6180X_ADDRESS, wrdata, 4, false);
 
    return;
}


uint8_t VL6180x_getRegister(uint16_t registerAddr)
{

    uint8_t wrdata[2], rddata[2];
    ret_code_t ret;

    wrdata[0] = (registerAddr>>8)&0xff;
    wrdata[1] = registerAddr&0xff;

    ret = nrf_drv_twi_tx(&m_twi, VL6180X_ADDRESS, wrdata, 2, true);
    if (NRF_SUCCESS != ret)
    {
       return 0;
    }
    ret = nrf_drv_twi_rx(&m_twi, VL6180X_ADDRESS, rddata, 1);

    return rddata[0];

}

uint16_t VL6180x_getRegister16bit(uint16_t registerAddr)
{
    uint8_t wrdata[2], rddata[2];
    uint16_t ret_value;
    ret_code_t err_code;

    wrdata[0] = (registerAddr>>8)&0xff;
    wrdata[1] = registerAddr&0xff;


    nrf_drv_twi_xfer_desc_t xfer = NRF_DRV_TWI_XFER_DESC_TXRX(VL6180X_ADDRESS, wrdata, 2, rddata, 2);
    // Reading register
    err_code = nrf_drv_twi_xfer(&m_twi, &xfer, NRF_DRV_TWI_FLAG_TX_NO_STOP);

    ret_value = (rddata[1]<<8)|rddata[0];

    return ret_value;

}
