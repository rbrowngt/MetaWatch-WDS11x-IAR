//==============================================================================
//  Copyright 2011 Meta Watch Ltd. - http://www.MetaWatch.org/
// 
//  Licensed under the Meta Watch License, Version 1.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//  
//      http://www.MetaWatch.org/licenses/license-1.0.html
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//==============================================================================

/******************************************************************************/
/*! \file Adc.c
*
*/
/******************************************************************************/

#include "hal_board_type.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include "hal_clock_control.h"
#include "hal_battery.h"
#include "hal_calibration.h"

#include "Messages.h"
#include "MessageQueues.h"
#include "BufferPool.h"
#include "DebugUart.h"
#include "Utilities.h"
#include "Adc.h"
#include "Display.h"

#include "OSAL_Nv.h"
#include "NvIds.h"

#define HARDWARE_CFG_INPUT_CHANNEL  ( ADC12INCH_13 )
#define BATTERY_SENSE_INPUT_CHANNEL ( ADC12INCH_15 )
#define LIGHT_SENSE_INPUT_CHANNEL   ( ADC12INCH_1 )

#define ENABLE_REFERENCE()  { }
#define DISABLE_REFERENCE() { }
  
// start conversion
#define ENABLE_ADC()       { ADC12CTL0 |= ADC12ON; ADC12CTL0 |= ADC12ENC + ADC12SC; }
#define DISABLE_ADC()      { ADC12CTL0 &= ~ADC12ENC; ADC12CTL0 &= ~ADC12ON; }
#define CLEAR_START_ADDR() { ADC12CTL1 &= 0x0FFF; }

static xSemaphoreHandle AdcHardwareMutex;

#define MAX_SAMPLES ( 10 )
static unsigned int HardwareConfiguration = 0;
static unsigned int BatterySense = 0;
static unsigned int LightSense = 0;
static unsigned int BatterySenseSamples[MAX_SAMPLES];
static unsigned int LightSenseSamples[MAX_SAMPLES];
static unsigned char BatterySenseSampleIndex;
static unsigned char LightSenseSampleIndex;
static unsigned char BatterySenseAverageReady = 0;
static unsigned char LightSenseAverageReady = 0;

static unsigned char LowBatteryWarningMessageSent;
static unsigned char LowBatteryBtOffMessageSent;
static tHostMsg* pMsg;
    

#define DEFAULT_LOW_BATTERY_WARNING_LEVEL    ( 3500 )
#define DEFAULT_LOW_BATTERY_BTOFF_LEVEL      ( 3300 )

static unsigned int LowBatteryWarningLevel;
static unsigned int LowBatteryBtOffLevel;

static void AdcCheck(void);
static void VoltageReferenceInit(void);

static void StartHardwareCfgConversion(void);
static void FinishHardwareCfgCycle(void);

static void StartBatterySenseConversion(void);
static void FinishBatterySenseCycle(void);

static void StartLightSenseConversion(void);
static void FinishLightSenseCycle(void);

static void WaitForAdcBusy(void);
static void EndAdcCycle(void);

static void InitializeLowBatteryLevels(void);

/*! the voltage from the battery is divided
 * before it goes to ADC (so that it is less than
 * 2.5 volt reference)
 * 
 * The output of this function is Voltage * 1000
 */
const double CONVERSION_FACTOR_BATTERY = 
  ((24300.0+38300.0)*2.5*1000.0)/(4095.0*24300.0);

/*! Convert the ADC count for the battery input into a voltage 
 *
 * \param Counts Battery Voltage in ADC counts
 * \return Battery voltage in millivolts
 */
unsigned int AdcCountsToBatteryVoltage(unsigned int Counts)
{
  return ((unsigned int)(CONVERSION_FACTOR_BATTERY*(double)Counts));
}

/*! Light sensor conversion factor */
const double CONVERSION_FACTOR =  2.5*10000.0/4096.0;

/*! Convert ADC counts to a voltage (truncates)
 *
 * \param Counts Voltage in ADC counts
 * \return Voltage in millivolts
 */
unsigned int AdcCountsToVoltage(unsigned int Counts)
{
  return ((unsigned int)(CONVERSION_FACTOR*(double)Counts));
}


/* check the the adc is ready */
static void AdcCheck(void)
{
#if 0
  _assert_((ADC12CTL1 & ADC12BUSY) == 0);
  _assert_((ADC12CTL0 & ADC12ON) == 0);
  _assert_((ADC12CTL0 & ADC12ENC) == 0);
#endif
}

static void VoltageReferenceInit(void)
{
  /* 
   * slaug208 it says the voltage reference is not available in this part
   */  
}

void InitializeAdc(void)
{
  VoltageReferenceInit();

  LIGHT_SENSE_INIT();
  BATTERY_SENSE_INIT();
  HARDWARE_CFG_SENSE_INIT();
 
  /* enable the 2.5V reference */
  ADC12CTL0 = ADC12REFON + ADC12REF2_5V;

  /* select ADC12SC bit as sample and hold source (00) 
   * and use pulse mode
   * use ACLK so that ADCCLK < 2.7 MHz and so that SMCLK does not have to be used
  */
  ADC12CTL1 = ADC12CSTARTADD_0 + ADC12SHP + ADC12SSEL_1;

  /* 12 bit resolution, only use reference when doing a conversion, 
   * use low power mode because sample rate is < 50 ksps 
  */
  ADC12CTL2 = ADC12TCOFF + ADC12RES_2 + ADC12REFBURST + ADC12SR;

  /* setup input channels */
  ADC12MCTL0 = HARDWARE_CFG_INPUT_CHANNEL;
  ADC12MCTL1 = BATTERY_SENSE_INPUT_CHANNEL;
  ADC12MCTL2 = LIGHT_SENSE_INPUT_CHANNEL;

  BatterySenseSampleIndex = 0;
  LightSenseSampleIndex = 0;
  HardwareConfiguration = 0;
  BatterySense = 0;
  LightSense = 0;
  BatterySenseAverageReady = 0;
  LightSenseAverageReady = 0;

  /* control access to adc peripheral */
  AdcHardwareMutex = xSemaphoreCreateMutex();
  xSemaphoreGive(AdcHardwareMutex);
  
  InitializeLowBatteryLevels();
  LowBatteryWarningMessageSent = 0;
  LowBatteryBtOffMessageSent = 0;
  
}

/* switch context if we are waiting */
static void WaitForAdcBusy(void)
{
  TaskDelayLpmDisable();
  while(ADC12CTL1 & ADC12BUSY)
  {
    vTaskDelay(0);  
  }
  TaskDelayLpmEnable();
}

/*
 * A voltage divider on the board is populated differently
 * for each revision of the board.  This may be deprecated.
 */
void HardwareCfgCycle(void)
{
  xSemaphoreTake(AdcHardwareMutex,portMAX_DELAY);
  
  HARDWARE_CFG_SENSE_ENABLE();
  ENABLE_REFERENCE();
  
  StartHardwareCfgConversion();
  WaitForAdcBusy();
  FinishHardwareCfgCycle();
  
}

static void StartHardwareCfgConversion(void)
{
  AdcCheck();
  
  /* setup the ADC channel */
  CLEAR_START_ADDR();
  ADC12CTL1 |= ADC12CSTARTADD_0;

  /* enable the ADC to start sampling and perform a conversion */
  ENABLE_ADC();

}

static void FinishHardwareCfgCycle(void)
{
  HardwareConfiguration = AdcCountsToVoltage(ADC12MEM0);
  HARDWARE_CFG_SENSE_DISABLE();
  DISABLE_ADC();
  DISABLE_REFERENCE();
  
  EndAdcCycle();

}

void BatterySenseCycle(void)
{
  xSemaphoreTake(AdcHardwareMutex,portMAX_DELAY);
  
  BATTERY_SENSE_ENABLE();
  ENABLE_REFERENCE();

  StartBatterySenseConversion();
  WaitForAdcBusy();
  FinishBatterySenseCycle();
  
}

/* battery sense cycle requires 630 us using ACLK */
static void StartBatterySenseConversion(void)
{
  AdcCheck();
  
  CLEAR_START_ADDR();
  ADC12CTL1 |= ADC12CSTARTADD_1;

  ENABLE_ADC();
}

static void FinishBatterySenseCycle(void)
{
  BatterySense = AdcCountsToBatteryVoltage(ADC12MEM1);

  if ( QueryCalibrationValid() )
  {
    BatterySense += GetBatteryCalibrationValue();
  }
  BatterySenseSamples[BatterySenseSampleIndex++] = BatterySense;
  
  if ( BatterySenseSampleIndex >= MAX_SAMPLES )
  {
    BatterySenseSampleIndex = 0;
    BatterySenseAverageReady = 1;
  }
  
  BATTERY_SENSE_DISABLE();

  EndAdcCycle();

}


void LowBatteryMonitor(void)
{
  
  unsigned int BatteryAverage = ReadBatterySenseAverage();
  

  if ( QueryBatteryDebug() )
  {    
    /* it was not possible to get a reading below 2.8V 
     *
     * the radio does not initialize when the battery voltage (from a supply)
     * is below 2.8 V
     *
     * if the battery is not present then the readings are meaningless
    */  
    PrintStringAndTwoDecimals("Batt Inst: ",BatterySense,
                              " Batt Avg: ",BatteryAverage);
    
  }
  
  /* if the battery is charging then ignore the measured voltage
   * and clear the flags
  */
  if ( QueryPowerGood() )
  {
    /* what about case where someone charges battery on an airplane? */
    if ( LowBatteryBtOffMessageSent )
    {
      BPL_AllocMessageBuffer(&pMsg);
      pMsg->Type = TurnRadioOnMsg;
      RouteMsg(&pMsg);
    }
    
    LowBatteryWarningMessageSent = 0;  
    LowBatteryBtOffMessageSent = 0;
  }
  else
  {
    /* 
     * do this check first so the bt off message will get sent first
     * if startup occurs when voltage is below thresholds
     */
    if (   BatteryAverage < LowBatteryBtOffLevel
        && LowBatteryBtOffMessageSent == 0 )
    {
      LowBatteryBtOffMessageSent = 1;
      
      BPL_AllocMessageBuffer(&pMsg);
      UTL_BuildHstMsg(pMsg,LowBatteryBtOffMsgHost,NO_MSG_OPTIONS,
                      (unsigned char*)BatteryAverage,2);
      RouteMsg(&pMsg);
    
      /* send the same message to the display task */
      BPL_AllocMessageBuffer(&pMsg);
      pMsg->Type = LowBatteryBtOffMsg;
      RouteMsg(&pMsg);
      
      /* now send a vibration to the wearer */
      BPL_AllocMessageBuffer(&pMsg);
      pMsg->Type = SetVibrateMode;

      tSetVibrateModePayload* pMsgData;
      pMsgData = (tSetVibrateModePayload*) pMsg->pPayload;
      
      pMsgData->Enable = 1;
      pMsgData->OnDurationLsb = 0x00;
      pMsgData->OnDurationMsb = 0x01;
      pMsgData->OffDurationLsb = 0x00;
      pMsgData->OffDurationMsb = 0x01;
      pMsgData->NumberOfCycles = 5;
      
      RouteMsg(&pMsg);

    }
    
    if (   BatteryAverage < LowBatteryWarningLevel 
        && LowBatteryWarningMessageSent == 0 )
    {
      LowBatteryWarningMessageSent = 1;

      BPL_AllocMessageBuffer(&pMsg);
      UTL_BuildHstMsg(pMsg,LowBatteryWarningMsgHost,NO_MSG_OPTIONS,
                      (unsigned char*)BatteryAverage,2);
      RouteMsg(&pMsg);
    
      /* send the same message to the display task */
      BPL_AllocMessageBuffer(&pMsg);
      pMsg->Type = LowBatteryWarningMsg;
      RouteMsg(&pMsg);
      
      /* now send a vibration to the wearer */
      BPL_AllocMessageBuffer(&pMsg);
      pMsg->Type = SetVibrateMode;

      tSetVibrateModePayload* pMsgData;
      pMsgData = (tSetVibrateModePayload*) pMsg->pPayload;

      pMsgData->Enable = 1;
      pMsgData->OnDurationLsb = 0x00;
      pMsgData->OnDurationMsb = 0x02;
      pMsgData->OffDurationLsb = 0x00;
      pMsgData->OffDurationMsb = 0x02;
      pMsgData->NumberOfCycles = 5;
      
      RouteMsg(&pMsg);
      
    }
  

  }
  
}



void LightSenseCycle(void)
{
  xSemaphoreTake(AdcHardwareMutex,portMAX_DELAY);

  LIGHT_SENSOR_L_GAIN();
  ENABLE_REFERENCE();
  
  /* light sensor requires 1 ms to wake up in the dark */
  TaskDelayLpmDisable();
  vTaskDelay(10);
  TaskDelayLpmEnable();
  
  StartLightSenseConversion();
  WaitForAdcBusy();
  FinishLightSenseCycle();

}

void StartLightSenseConversion(void)
{
  AdcCheck();
  
  CLEAR_START_ADDR();
  ADC12CTL1 |= ADC12CSTARTADD_2;

  ENABLE_ADC();

}

/* obtained reading of 91 (or 85) in office 
 * obtained readings from 2000-12000 with droid light in different positions
 */
void FinishLightSenseCycle(void)
{
  LightSense = AdcCountsToVoltage(ADC12MEM2);

  LightSenseSamples[LightSenseSampleIndex++] = LightSense;
  
  if ( LightSenseSampleIndex >= MAX_SAMPLES )
  {
    LightSenseSampleIndex = 0;
    LightSenseAverageReady = 1;
  }
  
  LIGHT_SENSOR_SHUTDOWN();
 
  EndAdcCycle();

#if 0
  PrintStringAndDecimal("LightSenseInstant: ",LightSense);
#endif
  
}

static void EndAdcCycle(void)
{
  DISABLE_ADC();
  DISABLE_REFERENCE();
 
  /* release the mutex */
  xSemaphoreGive(AdcHardwareMutex);

}

unsigned int ReadBatterySense(void)
{
  return BatterySense;
}

unsigned int ReadBatterySenseAverage(void)
{
  unsigned int SampleTotal = 0;
  unsigned int Result = 0;
  
  if ( BatterySenseAverageReady )
  {
    for (unsigned char i = 0; i < MAX_SAMPLES; i++)
    {
      SampleTotal += BatterySenseSamples[i];
    }
    
    Result = SampleTotal/MAX_SAMPLES;
  }
  else
  {
    Result = BatterySense;  
  }
  
  return Result;
  
}

unsigned int ReadLightSense(void)
{
  return LightSense;  
}


unsigned int ReadLightSenseAverage(void)
{
  unsigned int SampleTotal = 0;
  unsigned int Result = 0;
  
  if ( LightSenseAverageReady )
  {
    for (unsigned char i = 0; i < MAX_SAMPLES; i++)
    {
      SampleTotal += LightSenseSamples[i];
    }
    
    Result = SampleTotal/MAX_SAMPLES;
  }
  else
  {
    Result = LightSense;  
  }
  
  return Result;
}

unsigned int ReadHardwareConfiguration(void)
{
  return HardwareConfiguration;
}

/* Set new low battery levels and save them to flash */
void SetBatteryLevels(unsigned char * pData)
{
  LowBatteryWarningLevel = pData[0] * (unsigned int)100;
  LowBatteryBtOffLevel = pData[1] * (unsigned int)100;
  
  OsalNvWrite(NVID_LOW_BATTERY_WARNING_LEVEL,
              NV_ZERO_OFFSET,
              sizeof(LowBatteryWarningLevel),
              &LowBatteryWarningLevel); 
      
  OsalNvWrite(NVID_LOW_BATTERY_BTOFF_LEVEL,
              NV_ZERO_OFFSET,
              sizeof(LowBatteryBtOffLevel),
              &LowBatteryBtOffLevel);    
}

/* Initialize the low battery levels and read them from flash if they exist */
static void InitializeLowBatteryLevels(void)
{
  LowBatteryWarningLevel = DEFAULT_LOW_BATTERY_WARNING_LEVEL;
  LowBatteryBtOffLevel = DEFAULT_LOW_BATTERY_BTOFF_LEVEL;
    
  OsalNvItemInit(NVID_LOW_BATTERY_WARNING_LEVEL,
                 sizeof(LowBatteryWarningLevel), 
                 &LowBatteryWarningLevel);
   
  OsalNvItemInit(NVID_LOW_BATTERY_BTOFF_LEVEL, 
                 sizeof(LowBatteryBtOffLevel), 
                 &LowBatteryBtOffLevel);
  
}