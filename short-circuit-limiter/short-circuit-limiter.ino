#include <U8g2lib.h>
#include "CTSensor.h"

#define CURRENT_ENABLE_THRESHOLD 0.23
#define CURRENT_MCB_CUT 16

#define PIN_LED 26
#define PIN_CT A0
#define PIN_BUTTON_ENTER 24
#define PIN_TEMP A11
#define PIN_BUZZER 4

#define PIN_RELAY_MCB 5
#define PIN_RELAY_LIMITER 7

#define BUZZER_BEEP_ON_TIME 20
#define BUZZER_BEEP_OFF_TIME 200
#define BUZZER_LONG_BEEP_TIME 2000

#define CT_CALIBRATION_VALUE 60.6  // (100A / 0.05A) / 33 ohms
#define INITIAL_SETTLE_TIME 20000 //20 seconds

#define DELAY_BEFORE_LIMITER_RELAY_ENABLE 1000

//Number of temperature reads to average over
#define TEMP_READS_SET 50

//The MCU can take 5500 samples/s.
//0.05s seconds will require 275 samples/set
#define NUM_CT_SAMPLES 275

//Delay between LCD Print to avoid slowing down data collection
#define INTERVAL_PRINT 500

#define BACKLIGHT_BLINK_RATE 200

typedef enum {
  STATE_MCB_TRIPPED, STATE_WINDOW_BEFORE, STATE_WINDOW_WITHIN, STATE_WINDOW_EXITED
} STATE;

//This is to prepare to transition to the STATE_WINDOW_BEFORE at the start
STATE currentState = STATE_MCB_TRIPPED;
STATE nextState = STATE_WINDOW_BEFORE;

U8G2_UC1701_MINI12864_F_4W_SW_SPI u8g2(U8G2_R2, 21, 20, 19, 22);
CTSensor clamp(PIN_CT, CT_CALIBRATION_VALUE);

double mcbTrippedCurrent;

int beepsLeft = 0;

void setup() {

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  pinMode(PIN_RELAY_LIMITER, OUTPUT);
  pinMode(PIN_RELAY_MCB, OUTPUT);
  pinMode(PIN_BUTTON_ENTER, INPUT);
  pinMode(PIN_TEMP, INPUT);

  changeDisplayBacklight(true);
  passFullCurrentThrough(false);
  changeMCBRelayState(false);

  u8g2.begin();

  unsigned long initialTime = millis();
  unsigned long timeElapsedSinceStart;

  while((timeElapsedSinceStart = (millis() - initialTime)) < INITIAL_SETTLE_TIME){
    getCurrentMeasurement();
    
    unsigned long timeLeft = INITIAL_SETTLE_TIME - timeElapsedSinceStart;
    unsigned int timeLeftSeconds = timeLeft / 1000;
  
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(0,10, "Short Circuit Limiter\n");
    u8g2.drawStr(0,35, "Starting in: ");

    u8g2.setFont(u8g2_font_9x18_tr);
    char secondsBuffer[10];
    sprintf(secondsBuffer, "%ds", timeLeftSeconds);
    
    u8g2.drawStr(80, 35, secondsBuffer);
    u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(0,63, "Designer: Yeo Kheng Meng");

    u8g2.sendBuffer();
   
  }

  Serial.begin(115200);
  Serial.println("Setup complete");

  enterWindowBeforeMode(0);
}

double getCurrentMeasurement(){
  
  for(int sampleIndex = 0; sampleIndex < NUM_CT_SAMPLES; sampleIndex++){
    clamp.doIncrementalMeasurement();
  }

  double currentValue = clamp.getIrmsFromIncrementalMeasurement();
  return currentValue;
}



void loop() {

  double currentValue = getCurrentMeasurement();
  
  if(currentValue >= CURRENT_MCB_CUT){
    nextState = enterMCBTrippedMode(currentValue);
  }

  switch(nextState){
    case STATE_MCB_TRIPPED:
      nextState = enterMCBTrippedMode(currentValue);
      break;
    case STATE_WINDOW_BEFORE:
      nextState = enterWindowBeforeMode(currentValue);
      break;
    case STATE_WINDOW_WITHIN:
      nextState = enterWindowWithinMode(currentValue);
      break;
    case STATE_WINDOW_EXITED:
      nextState = enterWindowExitedMode(currentValue);
      break;
    default:
      Serial.println("Something is really wrong here, this mode should not exist!!!");
      break;
   }

  shortBeepRunner();
  displayToScreen(currentValue);


}

void displayToScreen(double currentValue){
  long long currentTime = millis();

  if(currentState == STATE_MCB_TRIPPED){
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x13_tr);
    u8g2.drawStr(0,10, "Overcurrent!!!");

    u8g2.drawStr(0,31, "Reset MCB >>>>>>>>");

    char valueBuff[10];
    char fullBuff[30];
    
    dtostrf(mcbTrippedCurrent, 4, 1, valueBuff);
    sprintf(fullBuff,"Tripped at: %sA", valueBuff);
    u8g2.drawStr(0,50, fullBuff);

    u8g2.setFont(u8g2_font_5x8_tr);
    dtostrf(CURRENT_MCB_CUT, 4, 1, valueBuff);
    sprintf(fullBuff,"Trip threshold: %sA", valueBuff);
    u8g2.drawStr(0,63, fullBuff);
    
    u8g2.sendBuffer();

  } else {

    static double runningTotal = 0;
    static int samplesTaken = 0;

    runningTotal += currentValue;
    samplesTaken++;
    
    static unsigned long lastDisplayTime;
    //We don't want to keep printing to screen as it is slow so we just do a running average
    if((currentTime - lastDisplayTime) >= INTERVAL_PRINT){
      float temperature = getTemperature();
  
      lastDisplayTime = currentTime;
      double average = runningTotal / samplesTaken;
  
      runningTotal = 0;
      samplesTaken = 0;

      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_7x13_tr);

      
      switch(nextState){
        case STATE_WINDOW_BEFORE:
          u8g2.drawStr(0,10, "Limiter: In Effect");
          break;
        case STATE_WINDOW_WITHIN:
          u8g2.drawStr(0,10, "Limiter:");
          u8g2.drawStr(0,20, "Ready to Bypass...");
          break;
        case STATE_WINDOW_EXITED:
          u8g2.drawStr(0,10, "Limiter: Bypassed");
          break;
        case STATE_MCB_TRIPPED:
          //Fallthrough 
        default:
          Serial.println("LCD Printing shouldn't come to this mode!");
          break;
       }

       char valueBuff[10];
       char fullBuff[30];
        
       dtostrf(currentValue, 4, 2, valueBuff);
       sprintf(fullBuff,"Current  : %sA", valueBuff);
       u8g2.drawStr(0,33, fullBuff);

       dtostrf(temperature, 2, 0, valueBuff);
       sprintf(fullBuff,"Int. Temp: %sC", valueBuff);
       u8g2.drawStr(0,47, fullBuff);

       u8g2.setFont(u8g2_font_5x8_tr);
       dtostrf(CURRENT_ENABLE_THRESHOLD, 4, 2, valueBuff);
       sprintf(fullBuff,"Enable Current: %sA", valueBuff);
       u8g2.drawStr(0,63, fullBuff);


   
       u8g2.sendBuffer();
    }
  }
  
}

STATE enterMCBTrippedMode(double currentValue){

  static bool initialWarningTriggered = false;
  static unsigned long initialWarningStart = 0;

  unsigned long currentTime = millis();

  if(currentState != STATE_MCB_TRIPPED){
    changeMCBRelayState(false);
    passFullCurrentThrough(false);

    currentState = STATE_MCB_TRIPPED;
    mcbTrippedCurrent = currentValue;
    Serial.print("MCB tripped at: ");
    Serial.println(mcbTrippedCurrent);

    initialWarningTriggered = true;
    initialWarningStart = currentTime;
    digitalWrite(PIN_BUZZER, HIGH);
  }

  //This is to sound the buzzer and blink the Backlight for BACKLIGHT_BLINK_RATE duration 
  if(initialWarningTriggered){

    static bool displayBacklightCurrentlyOn = false;
    static unsigned long displayBacklightLastChanged = 0;

    if((currentTime - displayBacklightLastChanged) > BACKLIGHT_BLINK_RATE){
      displayBacklightLastChanged = currentTime;
      displayBacklightCurrentlyOn = !displayBacklightCurrentlyOn;
      changeDisplayBacklight(displayBacklightCurrentlyOn);
    }


    if((currentTime - initialWarningStart) > BUZZER_LONG_BEEP_TIME){
      initialWarningTriggered = false;
      digitalWrite(PIN_BUZZER, LOW);
      changeDisplayBacklight(true);
       
    }
    
  }

 

  int buttonPressed = digitalRead(PIN_BUTTON_ENTER);

  if(buttonPressed == LOW){
    return STATE_WINDOW_BEFORE;
  }

  return STATE_MCB_TRIPPED; 
}

STATE enterWindowBeforeMode(double currentValue){
  if(currentState != STATE_WINDOW_BEFORE){

    passFullCurrentThrough(false);
    changeMCBRelayState(true);
    changeDisplayBacklight(false);

    currentState = STATE_WINDOW_BEFORE;
    Serial.println("Window Before");
    shortBeepXTimesNoDelay(2);
  }


  if(currentValue >= CURRENT_ENABLE_THRESHOLD){
    return STATE_WINDOW_WITHIN;
  }

  return STATE_WINDOW_BEFORE;
   
}

STATE enterWindowWithinMode(double currentValue){
  unsigned long currentTime = millis();

  static unsigned long enableWindowTime = 0;
  
  if(currentState != STATE_WINDOW_WITHIN){
    currentState = STATE_WINDOW_WITHIN;
    enableWindowTime = currentTime;
    Serial.println("Enter window");
  }

  if(currentValue < CURRENT_ENABLE_THRESHOLD){
    return STATE_WINDOW_BEFORE;
  } else if((currentTime - enableWindowTime) >= DELAY_BEFORE_LIMITER_RELAY_ENABLE){
    //Exit window if no overcurrent is detected in this time
    return STATE_WINDOW_EXITED;
  } else {
    return STATE_WINDOW_WITHIN;
  } 
}

STATE enterWindowExitedMode(double currentValue){

  if(currentState != STATE_WINDOW_EXITED){
    currentState = STATE_WINDOW_EXITED;
    changeDisplayBacklight(true);
    Serial.println("Exited window, bypass resistor");
    shortBeepXTimesNoDelay(1);
  }

  passFullCurrentThrough(true);

  if(currentValue < CURRENT_ENABLE_THRESHOLD){
    return STATE_WINDOW_BEFORE;
  } else {
    return STATE_WINDOW_EXITED;
  } 

}

void changeMCBRelayState(bool state){
  if(state){
    digitalWrite(PIN_RELAY_MCB, HIGH);
  } else {
    digitalWrite(PIN_RELAY_MCB, LOW);
  }
}

void passFullCurrentThrough(bool state){
  if(state){
    digitalWrite(PIN_RELAY_LIMITER, HIGH);
  } else {
    digitalWrite(PIN_RELAY_LIMITER, LOW);
  }
}

void changeDisplayBacklight(bool state){

  //State is flipped for some reason
  if(state){
    digitalWrite(PIN_LED, LOW);
  } else {
    digitalWrite(PIN_LED, HIGH);
  }
}

//Obtained from https://learn.adafruit.com/tmp36-temperature-sensor/using-a-temp-sensor
float getTemperature(){

  long total = 0; 
  
  //First value is usually off
  analogRead(PIN_TEMP);
  
  for(int i = 0; i < TEMP_READS_SET; i++){
    total += analogRead(PIN_TEMP);
  }
  
  int reading = total / TEMP_READS_SET; 
  
  // converting that reading to voltage, for 3.3v arduino use 3.3
  float voltage = reading * 5.0;
  voltage /= 1024.0; 
  
  // now print out the temperature
  float temperatureC = (voltage - 0.5) * 100 ;
  
  return temperatureC;
}


void shortBeepXTimesNoDelay(int times){
  beepsLeft = times;  
}

//We don't use delay from beeps to avoid holding up the controller
void shortBeepRunner(){

  static bool beepCurrentlyOn = false;
  static unsigned long beepChangedTime = 0;

  unsigned long currentTime = millis();

  if(beepsLeft > 0){

    if(beepCurrentlyOn){

      if((currentTime - beepChangedTime) > BUZZER_BEEP_ON_TIME){
          beepChangedTime = currentTime;
          digitalWrite(PIN_BUZZER, LOW);
          beepCurrentlyOn = false;
          beepsLeft--;
      }
      
    } else {

      if((currentTime - beepChangedTime) > BUZZER_BEEP_OFF_TIME){
          beepChangedTime = currentTime;
          digitalWrite(PIN_BUZZER, HIGH);
          beepCurrentlyOn = true;
      }
    }
    
  }
  
  
}

