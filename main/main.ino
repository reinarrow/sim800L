#include <SoftwareSerial.h> // For serial communication with the SIM800L module
#include <EEPROM.h> // For persistent data storage into the memory

// ------------ Debug mode configuration ------------//
# define DEBUG_MODE true
# define SMS_SIMULATION false // Simulate Sim800 serial in local Arduino serial. Example message: +CMT: "+34605521505","","22/02/04,01:47:51+04" RIEGO,MEASUREMENTS

// ------------ Pins definition ------------//
// Valve 1: 2 pins for H-bridge (A-, A+)
#define PIN_VALVE1_A1 12
#define PIN_VALVE1_A2 11

// Valve 2: 2 pins for H-bridge (A-, A+)
#define PIN_VALVE2_A1 6
#define PIN_VALVE2_A2 5

// Water flow sensor
#define PIN_FLOW_SENSOR 2

// Voltage sensor
#define PIN_VOLTAGE_SENSOR A0

// Humidity sensor
#define PIN_HUMIDITY_SENSOR A1

// Temperature sensor
#define PIN_TEMPERATURE_SENSOR A2

// Safety relay
#define PIN_SAFETY_RELAY 10

// Input buttons
#define PIN_INPUT_BUTTON1 7
#define PIN_INPUT_BUTTON2 4

// Serial TX and RX for communication with SIM800L
#define PIN_RX 8
#define PIN_TX 9

// Create object for serial communication with SIM800
SoftwareSerial SerialSIM800(PIN_RX, PIN_TX);

// ------------ Constants definition ------------//
// Sample time for measurements in milliseconds
const int SAMPLE_TIME = 1000;

// Minimum time between consecutive irrigations in milliseconds for valves 1 and 2
const int TIME_BETWEEN_IRRIGATIONS1 = 2000;
const int TIME_BETWEEN_IRRIGATIONS2 = 2000;

// Safety time to detect that a button is stuck on the pushed position and
// consider that it is broken in milliseconds
const int BUTTON_SAFETY_TIME1 = 10000;
const int BUTTON_SAFETY_TIME2 = 10000;

// Effective irrigation time in milliseconds for valves 1 and 2
const int EFFECTIVE_IRRIGATION_TIME1 = 7000;
const int EFFECTIVE_IRRIGATION_TIME2 = 7000;

// Conversion factor from Hz to L/min flow for the water flow sensor
const float FLOW_CONVERSION_FACTOR = 7.11;

// Humidity threshold for irrigation (only irrigate below this value)
const int HUMIDITY_THRESHOLD = 720;

// Temperature threshold for irrigation (only irrigate over this value)
const int TEMPERATURE_THRESHOLD = 980;

// Voltage threshold for irrigation (only irrigate over this value) and SMS alert
const byte VOLTAGE_THRESHOLD = 6;

// Phone number for the SMS alerts
const String PHONE_NUMBER = "+34605521505";

// Size of buffer for incoming data from SIM800
const int BUFFER_SIZE = 127;

// Command to request the status of the measurements and total water volume
const char* STATUS_COMMAND = "ESTADO";

// Commands to request a manual irrigation for valves 1 and 2 (similar to pressing the button)
const char* IRRIGATION_COMMAND1 = "RIEGO1";
const char* IRRIGATION_COMMAND2 = "RIEGO2";

// Command to request a manual close of the relay
const char* CLOSE_COMMAND = "CERRAR";

// Time between writes of total volume to the EEPROM in minutes
const int EEPROM_WRITE_SAMPLE_TIME = 1440; //24h

// EEPROM memory adress to store the data to
const int EEPROM_ADDRESS = 0;

// ------------ Global variables ------------//
// Boolean for the irrigation valve to be open
bool valveOpen1 = false;
bool valveOpen2 = false;

// Boolean for relay status
bool relayOn = false;

//Reference time for previous measurement
unsigned long prevMeasurementTime = 0;

// Reference time when the irrigation started
unsigned long irrigationStartTime1 = 0 ;
unsigned long irrigationStartTime2 = 0 ;

// Counter for flow sensor pulses
volatile unsigned int pulsesCount;

// Storage of total water volume since the program start
float totalWaterVolume = 0;

// Variables for parsing the incoming SMS
char bufferData[BUFFER_SIZE]; // Buffer for incoming data from SIM800
char bufferIndex; // Index for writing the data into the buffer

bool cmtOk = false; // Boolean to signal that sender SMS parsing is on going
byte cmtIndex = 0; // Current position of index in the parsing of the sender number

bool msgOk = false; // Boolean to signal that the message payload parsing is on going
byte msgIndex = 0; // Current position of index in the parsing of the message

// Create the char arrays to store the SMS sender and payload
char senderNum[14]; // Holds the phone number of the SMS sender (example "+34601234567")
char message[100]; // Holds the message payload

// Variables for commands execution
bool sendMeasurements = false; // When this is true, the Arduino will send an SMS with the measurements in the next iteration

// EEPROM persistance
unsigned long prevEepromWriteTime = 0;

// Flag to only send the SMS alert once. When the battery is charged, the program will be reset and the flag will be false again
bool lowVoltageSmsSent = false;

// Flag to mark that an irrigation has been requested by SMS
bool remoteIrrigationPending1 = false;
bool remoteIrrigationPending2 = false;

// Flag to detect if the button was pressed on the previous loop
bool wasButtonPressed1 = false;
bool wasButtonPressed2 = false;

// Variable to store the initial timestamp when the button was pressed (in milliseconds since program start time)
unsigned long buttonPressedStartTime1 = 0;
unsigned long buttonPressedStartTime2 = 0;

// Flag set to true when an irrigation ends and the button is still pressed. If this happens then the relay will be closed for safety and an SMS alert will be sent
bool buttonBrokenFlag1 = false;
bool buttonBrokenFlag2 = false;

// Flag to mark that a relay closure has been requested by SMS
bool closeRelayRequested = false;

void setup()
{
  // Start serial connection
  if (DEBUG_MODE) Serial.begin(9600); Serial.println ("Enviar 'r' para restablecer el volumen a 0 Litros");

  // Define pin modes
  pinMode(PIN_FLOW_SENSOR, INPUT);
  pinMode(PIN_INPUT_BUTTON1, INPUT_PULLUP); // Configura pin 7 interno pull-up resistor  Estado en 1, requiere 0 para activarse
  pinMode(PIN_INPUT_BUTTON2, INPUT_PULLUP); // Configura pin 7 interno pull-up resistor  Estado en 1, requiere 0 para activarse
  pinMode(PIN_VALVE1_A1, OUTPUT); // Pin valvula 1 A-
  pinMode(PIN_VALVE1_A2, OUTPUT); // Pin valvula 1 A+
  pinMode(PIN_VALVE2_A1, OUTPUT); // Pin valvula 2 A-
  pinMode(PIN_VALVE2_A2, OUTPUT); // Pin valvula 2 A+
  pinMode(PIN_SAFETY_RELAY, OUTPUT); // Salida rele para apagado voltage seguridad

  // Define interruption for flow sensor
  attachInterrupt(0, countPulses , RISING); //(Interrupción 0(Pin2),función,Flanco de subida)
  interrupts();

  // Initialise SIM800
  initializeSIM800();

  // Get total water volume from EEPROM
  EEPROM.get(EEPROM_ADDRESS, totalWaterVolume);
  if (isnan(totalWaterVolume)) totalWaterVolume = 0; //First time EEPROM is empty, so populate it with 0 as initial value
  if (DEBUG_MODE) Serial.print("Last Water Volume value from EEPROM: "); Serial.println(totalWaterVolume, 3);
}

void loop()
{
  // Read incoming data in SIM800
  readSIM800Data(false); 

  // Additionally, if SMS simulation is on, read Serial input and process it as if it was an SMS.
  if(SMS_SIMULATION) readSIM800Data(true); 

  // Time measurement
  long currentMillis = millis();
  int dt = currentMillis - prevMeasurementTime; //Elapsed time since last measurement

  if (dt >= SAMPLE_TIME || currentMillis == 0) //Run also on first iteration
  {
    prevMeasurementTime = currentMillis;

    // Calculate water flow
    float waterFlow_L_min = (pulsesCount / FLOW_CONVERSION_FACTOR);
    // Reset pulses count to start new water flow measurement
    pulsesCount = 0;

    // Read serial input to flush volume storage or sum to the previous total volume instead
    if (Serial.available() && Serial.read() == 'r')
    {
      if (DEBUG_MODE) Serial.print("Clearing total water volume and EEPROM ");

      //Flush local variable
      totalWaterVolume = 0;

      //Clear EEPROM
      for (int idx = 0 ; idx < EEPROM.length() ; idx++) {
        EEPROM.write(idx, 0);
      }
    }
    else totalWaterVolume += (waterFlow_L_min / 60) * (dt / 1000); // volumen(L)=caudal(L/s)*tiempo(s)

    // Persist the totalVolume to the EEPROM when the configured time has passed
    if ((currentMillis - prevEepromWriteTime) / 60000 >= EEPROM_WRITE_SAMPLE_TIME)
    {
      prevEepromWriteTime = currentMillis;
      sendMeasurements = true; // Set flag to true to send the measurements with the new stored water volume to the phone

      // Write to EEPROM
      EEPROM.put(EEPROM_ADDRESS, totalWaterVolume);
      if (DEBUG_MODE) Serial.print("Wrote to EEPROM value: "); Serial.println(totalWaterVolume, 3);
    }

    // Read sensors
    int humidity = analogRead(PIN_HUMIDITY_SENSOR);
    int temperature = analogRead(PIN_TEMPERATURE_SENSOR);
    float voltage = analogRead(PIN_VOLTAGE_SENSOR) * (5.01 / 1023.00) * 1.24;

    // SMS alert if voltage is under threshold
    if (voltage < VOLTAGE_THRESHOLD && !lowVoltageSmsSent)
    {
      lowVoltageSmsSent = true;
      sendLowVoltageSMSAlert(voltage);
    }

    // Check button status. If it is low, buttonPresed = true. If the button is broken, this will always give false to ignore it and stop irrgating
    bool buttonPressed1;
    if (buttonBrokenFlag1)
    {
      // The button has been detected to have a failure, ignore its current status.
      buttonPressed1 = false;
    }
    else
    {
      // The button is not in failure state, check its status
      buttonPressed1 = checkButtonStatus(1);
    }

    bool buttonPressed2;
    if (buttonBrokenFlag2)
    {
      // The button has been detected to have a failure, ignore its current status.
      buttonPressed2 = false;
    }
    else
    {
      // The button is not in failure state, check its status
      buttonPressed2 = checkButtonStatus(2);
    }

    // Send measurements when requested through SMS
    if (sendMeasurements)
    {
      sendMeasurements = false; // Setting it to false to avoid send in loop
      sendMeasurementsSMS(humidity, temperature, voltage, totalWaterVolume);
    }

    // Print measurements only in debug mode
    if (DEBUG_MODE) printMeasurements(humidity, temperature, voltage, waterFlow_L_min, totalWaterVolume, buttonPressed1, buttonPressed2);

    // If batteries are running out, temperature is too low or humidity is too high or the button is broken, turn off the system for safety and to save power
    if (humidity > HUMIDITY_THRESHOLD || voltage < VOLTAGE_THRESHOLD || temperature < TEMPERATURE_THRESHOLD  || closeRelayRequested || buttonBrokenFlag1 || buttonBrokenFlag2)
    {
      // Ensure valves stay closed
      if (valveOpen1)
      {
        closeValve(1);
        valveOpen1 = false;
      }
      
      if (valveOpen2)
      {
        closeValve(2);
        valveOpen2 = false;
      }

      if (relayOn)
      {
        // Turn off relay to remove power from the valve
        digitalWrite(PIN_SAFETY_RELAY, LOW);

        relayOn = false;
      }
    }
    else
    {
      if (!relayOn)
      {
        // Turn on relay to give power to the valve if it was off
        digitalWrite(PIN_SAFETY_RELAY, HIGH);

        relayOn = true;
      }

      // If irrigation is requested for valve 1
      if (buttonPressed1 || remoteIrrigationPending1)
      {
        // Only irrigate if enough time has passed since last irrigation
        if ((currentMillis - irrigationStartTime1) >= (EFFECTIVE_IRRIGATION_TIME1 + TIME_BETWEEN_IRRIGATIONS1) && !valveOpen1)
        {
          irrigationStartTime1 = currentMillis;
          valveOpen1 = true;
          openValve(1);

          // If the request was remote, clear the flag and notify the user that the irrigation is starting
          if (remoteIrrigationPending1)
          {
            remoteIrrigationPending1 = false;
            sendIrrigationConfirmationSMS(1);
          }
        }
      }

      // If the valve 1 has been open for the configured EFFECTIVE_IRRIGATION_TIME1, turn it off
      if (currentMillis - irrigationStartTime1 >= EFFECTIVE_IRRIGATION_TIME1 && valveOpen1)
      {
        valveOpen1 = false;
        closeValve(1);
      }

      // If irrigation is requested for valve 2
      if (buttonPressed2 || remoteIrrigationPending2)
      {
        // Only irrigate if enough time has passed since last irrigation
        if ((currentMillis - irrigationStartTime2) >= (EFFECTIVE_IRRIGATION_TIME2 + TIME_BETWEEN_IRRIGATIONS2) && !valveOpen2)
        {
          irrigationStartTime2 = currentMillis;
          valveOpen2 = true;
          openValve(2);

          // If the request was remote, clear the flag and notify the user that the irrigation is starting
          if (remoteIrrigationPending2)
          {
            remoteIrrigationPending2 = false;
            sendIrrigationConfirmationSMS(2);
          }
        }
      }

      // If the valve 2 has been open for the configured EFFECTIVE_IRRIGATION_TIME2, turn it off
      if (currentMillis - irrigationStartTime2 >= EFFECTIVE_IRRIGATION_TIME2 && valveOpen2)
      {
        valveOpen2 = false;
        closeValve(2);
      }
    }
  }
  delay(100);
}

  // --- Function to check if a button is in failure state (pushed for too much time)
  // The argument 'buttonId' identifies the button in case of multiple ones.
  bool checkButtonStatus(int buttonId)
  {
    // Make pointers for the global variables of the selected buttonId
    int buttonPin;
    bool * wasButtonPressedPtr;
    unsigned long * buttonPressedStartTimePtr;
    bool * buttonBrokenFlagPtr;
    int buttonSafetyTime;
    
    switch (buttonId)
    {
      case 1:
        buttonPin = PIN_INPUT_BUTTON1;
        wasButtonPressedPtr = &wasButtonPressed1;
        buttonPressedStartTimePtr = &buttonPressedStartTime1;
        buttonBrokenFlagPtr = &buttonBrokenFlag1;
        buttonSafetyTime = BUTTON_SAFETY_TIME1;
        break;
      case 2:
        buttonPin = PIN_INPUT_BUTTON2;
        wasButtonPressedPtr = &wasButtonPressed2;
        buttonPressedStartTimePtr = &buttonPressedStartTime2;
        buttonBrokenFlagPtr = &buttonBrokenFlag2;
        buttonSafetyTime = BUTTON_SAFETY_TIME2;
        break;
      default:
        // Unconfigured button selected, just exit the function and trigger a debug message
        if(DEBUG_MODE) Serial.println("Error while reading button status, unknown button selected.");
        return false;  
    }

    bool buttonPressed = !digitalRead(buttonPin);
    if (buttonPressed)
    {
      // Read current time
      int currentMillis =  millis();
      
      // The button is pressed, check if it was already pressed in the last loop
      if (!(*wasButtonPressedPtr))
      {
        // The button was not pressed, store the time when the button was initially pressed
        *wasButtonPressedPtr = true;
        *buttonPressedStartTimePtr = currentMillis;
      }
      else
      {
        // The button was already pressed, check if it has been pressed for too long to detect a failure
        int buttonPressedElapsedTime = currentMillis - *buttonPressedStartTimePtr;
        if (buttonPressedElapsedTime >= buttonSafetyTime)
        {
          if(DEBUG_MODE) Serial.println("Failure in button, ignoring status and sending SMS alert");
          
          // Button pressed overtime -> Set the buttonBrokenFlag to true
          *buttonBrokenFlagPtr = true;

          // Set button pressed to false to avoid irrigation.
          buttonPressed = false;
  
          // Send SMS to nofity that the button is broken
          sendBrokenButtonSMS(buttonId);
        }
      }
    }
    else
    {
      // The button is not pressed, clear the pressed status for the next loop
      *wasButtonPressedPtr = false;
    }
    return buttonPressed;
  }

  // -------- Function to open valve ---------- //
  void openValve(int valveId)
  {
    if(valveId == 1)
    {
      digitalWrite(PIN_VALVE1_A1, HIGH);
      digitalWrite(PIN_VALVE1_A2, LOW);
      delay(300);
      digitalWrite(PIN_VALVE1_A1, LOW);      
    }
    else if(valveId == 2)
    {
      digitalWrite(PIN_VALVE2_A1, HIGH);
      digitalWrite(PIN_VALVE2_A2, LOW);
      delay(300);
      digitalWrite(PIN_VALVE2_A1, LOW);
    }
    if (DEBUG_MODE) Serial.println("OPEN RIEGO");
  }

  // -------- Function to close valve ---------- //
  void closeValve(int valveId)
  {
    if(valveId == 1)
    {
      digitalWrite(PIN_VALVE1_A2, HIGH);
      digitalWrite(PIN_VALVE1_A1, LOW);
      delay(300);
      digitalWrite(PIN_VALVE1_A2, LOW);     
    }
    else if(valveId == 2)
    {
      digitalWrite(PIN_VALVE2_A2, HIGH);
      digitalWrite(PIN_VALVE2_A1, LOW);  
      delay(300);
      digitalWrite(PIN_VALVE2_A2, LOW);
    }
    if (DEBUG_MODE) Serial.println("CLOSED RIEGO");
  }

  //---Función que se ejecuta en interrupción (ISR) ---------------//
  void countPulses ()
  {
    pulsesCount++;  //incrementamos la variable de pulsos
  }

  //--- Function for pretty-printing the measurements--------//
  void printMeasurements(int hum, int temp, float voltage, float waterFlow, float totalVolume, bool buttonPressed1, bool buttonPressed2)
  {
    if (!DEBUG_MODE) return;
    Serial.print("Humedad: ");
    Serial.println(hum);
    Serial.print("Temperatura: ");
    Serial.println(temp);
    Serial.print("Voltage: ");
    Serial.println(voltage);
    Serial.print("Caudal: ");
    Serial.print(waterFlow, 3);
    Serial.print("L/min\tVolumen: ");
    Serial.print(totalVolume, 3);
    Serial.println(" L");
    Serial.print("Boton 1 pulsado: ");
    Serial.println(buttonPressed1);
    Serial.print("Boton 2 pulsado: ");
    Serial.println(buttonPressed2);
  }

  //--- Function to initialize and configure the module. Input buffer is read after every command to avoid buffer overflow
  void initializeSIM800()
  {
    SerialSIM800.begin(9600);

    SerialSIM800.println("AT");                  // Sends an ATTENTION command for initial handshake, reply should be OK
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT+CCID");             //Read SIM information to confirm whether the SIM is plugged
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT+CMGF=1");           // Configuration for sending SMS
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT+CNMI=1,2,0,0,0");   // Configuration for receiving SMS
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT+CSQ");              // Check signal strength
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT+CREG?");            // Network registration test
    delay(100);
    readSIM800Data(false);

    SerialSIM800.println("AT&W");                // Save the configuration settings
    delay(100);
    readSIM800Data(false);

    // Initialize the buffer for reading the incoming Serial data from SIM800
    memset(bufferData, 0, sizeof(bufferData));
    bufferIndex = 0;

    // Initially store the PHONE_NUMBER as senderNum for the periodic measurements send.
    PHONE_NUMBER.toCharArray(senderNum, 14);
    Serial.println(senderNum);
  }

  //--- Function to send an SMS to the given phone number and with the given text
  void sendSMS(char* text, String phone_number)
  {
    if (DEBUG_MODE) 
    {
      Serial.print("Sending SMS: ");
      Serial.print(text);
      Serial.print(" to number: ");
      Serial.println(phone_number);
    }

    //Your phone number don't forget to include your country code, example +212123456789"
    SerialSIM800.print("AT+CMGS=\"");
    SerialSIM800.print(phone_number);
    SerialSIM800.println("\"\r");
    delay(100);
    readSIM800Data(false);

    //This is the text to send
    SerialSIM800.print(text);
    delay(100);
    readSIM800Data(false);

    //Required according to the datasheet)
    SerialSIM800.print((char)26);
    delay(1000);
    readSIM800Data(false);
  }

  //--- Function that checks if something has been received by SIM800 and parses it
  //    It also prints the text if DEBUG is enabled
  //    The sender number is stored in variable senderNum
  //    The message is stored in variable message
  // ---
  void readSIM800Data(bool simulationMode)
  {
    // If SMS_SIMULATION is ON, data will be get from the user input in the console
    if (simulationMode ? (Serial.available() > 0) : (SerialSIM800.available() > 0))
    {
      // Stop for 1 second to ensure that the full SMS is received
      delay(1000);
      while (simulationMode ? (Serial.available() > 0) : (SerialSIM800.available() > 0))
      {
        bufferData[bufferIndex] = simulationMode ? Serial.read() : SerialSIM800.read();

        // Finds the string "CMT:"
        // if found, reset the senderNum buffer
        if ( !cmtOk &&
             (bufferData[bufferIndex - 3] == 'C') &&
             (bufferData[bufferIndex - 2] == 'M') &&
             (bufferData[bufferIndex - 1] == 'T') &&
             (bufferData[bufferIndex] == ':')      )  {
          cmtOk = true;
          memset(senderNum, 0, sizeof(senderNum));
          cmtIndex = 0;            // reset pos counter
        }

        // String "CMT:" is found,
        // parse the sender number for the reply
        // +CMT: "+34601234567"
        if ( cmtOk )
        {
          if (bufferData[bufferIndex] != ' ' && bufferData[bufferIndex] != '"' && bufferData[bufferIndex] != ':')
          {
            senderNum[cmtIndex] =  bufferData[bufferIndex];
            cmtIndex++;
          }
          else
          {
            if ( cmtIndex > 0 ) // The number has been actually parsed -> The detected " was the end of the number and not the start. Otherwise, don't do anything
              cmtOk = false; // done
          }
        }

        // CMT has been already parsed, the rest is the message
        if (cmtOk == false && cmtIndex > 0 && msgOk == false)
        {
          msgOk = true;
          memset(message, 0, sizeof(message));
          msgIndex = 0;            // reset pos counter
        }

        // msgOk means that the message is in the process of being parsed
        if ( msgOk )
        {
          message[msgIndex] = bufferData[bufferIndex];
          msgIndex++;
        }

        bufferIndex++;
      }

      if (DEBUG_MODE)
      {
        if(simulationMode) Serial.println("------------------> New (simulated) message: ");
        else Serial.println("------------------> New message: ");
        Serial.println(bufferData);
      }

      // Clean buffer
      memset(bufferData, 0, sizeof(bufferData)); // Initialize the string
      bufferIndex = 0;
      msgOk = false;
      cmtOk = false;

      // Only evaluate SMS command if a CMT number was received
      if (cmtIndex > 0)
      {
        cmtIndex = 0;
        evaluateSmsCommand();
      }
    }
  }

  //--- Function that checks whether the received SMS matches any of the predefined commands and executes it ---//
  void evaluateSmsCommand()
  {
    //First check for the status command
    if (strstr(message, STATUS_COMMAND))
    {
      sendMeasurements = true;
      if (DEBUG_MODE) Serial.println("Measurements request received!");
    }

    if (strstr(message, IRRIGATION_COMMAND1))
    {
      remoteIrrigationPending1 = true;
      if (DEBUG_MODE) Serial.println("Remote irrigation request received for valve 1!");
    }

    if (strstr(message, IRRIGATION_COMMAND2))
    {
      remoteIrrigationPending2 = true;
      if (DEBUG_MODE) Serial.println("Remote irrigation request received for valve 2!");
    }

    if (strstr(message, CLOSE_COMMAND))
    {
      closeRelayRequested = true;
      if (DEBUG_MODE) Serial.println("Relay closure request received!");
      sendCloseRelayConfirmationSMS();
    }
  }

  //--- Function to format and send the SMS with the sensor measurements. The used phone number is the one read from the received request ---//
  void sendMeasurementsSMS(int hum, int temp, float voltage, float totalVolume)
  {
    char payload[100];

    // Convert floatt to char arrays. Sprintf does not work with floats
    char voltageStr[10];
    char volumeStr[10];
    dtostrf(voltage, 3, 2, voltageStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision
    dtostrf(totalVolume, 3, 2, volumeStr);
    
    sprintf(payload, "HUM:%d,TEMP:%d,VOLT:%s,LITROS:%s,RIEGO1:%s,RIEGO2:%s,RELE:%s", hum, temp, voltageStr, volumeStr, valveOpen1 ? "SI" : "NO", valveOpen2 ? "SI" : "NO", relayOn ? "ABIERTO" : "CERRADO");
    
    sendSMS(payload, senderNum);
  }

  //--- Function to send SMS alert when the voltage is too low. The used SMS is the one that is configured by default in the application ---//
  void sendLowVoltageSMSAlert(float voltage)
  {
    char payload[100];

    // Convert floatt to char arrays. Sprintf does not work with floats
    char voltageStr[10];
    dtostrf(voltage, 3, 2, voltageStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision

    sprintf(payload, "ALERTA, voltage demasiado bajo: %s V", voltageStr);

    sendSMS(payload, PHONE_NUMBER);
  }

  //--- Function to inform the user that the requested irrigation is being executed ---//
  void sendIrrigationConfirmationSMS(int valveId)
  {
    if(valveId == 1) sendSMS("OK, Riego en marcha para valvula 1", senderNum);
    else if(valveId == 2) sendSMS("OK, Riego en marcha para valvula 2", senderNum);
  }

  //--- Function to inform the user that the button is broken ---//
  void sendBrokenButtonSMS(int buttonId)
  {
    if(buttonId == 1) sendSMS("ALERTA, el boton de riego 1 esta bloqueado.", PHONE_NUMBER);
    else if(buttonId == 2) sendSMS("ALERTA, el boton de riego 2 esta bloqueado.", PHONE_NUMBER);
    
  }

  //--- Function to inform the user that the requested relay closure is being executed ---//
  void sendCloseRelayConfirmationSMS()
  {
    sendSMS("OK, cerrando rele", senderNum);
  }
