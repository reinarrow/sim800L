#include <SoftwareSerial.h> // For serial communication with the SIM800L module
#include <EEPROM.h> // For persistent data storage into the memory
#include <DHT.h> // For DHT, humidity and temperature sensor

// ------------ Debug mode configuration ------------//
# define DEBUG_MODE true
# define SMS_SIMULATION false // Simulate Sim800 serial in local Arduino serial. Example message: +CMT: "+34605521505","","22/02/04,01:47:51+04" RIEGO,MEASUREMENTS

// ------------ Pins definition ------------//
// Valve: 2 pins for H-bridge (A-, A+)
#define PIN_VALVE_A1 12
#define PIN_VALVE_A2 11

// Water flow sensor
#define PIN_FLOW_SENSOR 2

// Voltage sensor
#define PIN_VOLTAGE_SENSOR A0

// Humidity and temperature sensor
#define PIN_DHT_SENSOR A1

// Light sensor
#define PIN_LIGHT_SENSOR A2

// Safety relay
#define PIN_SAFETY_RELAY 10

// Input buttons
#define PIN_INPUT_BUTTON 7

// Serial TX and RX for communication with SIM800L
#define PIN_RX 8
#define PIN_TX 9

// Create object for serial communication with SIM800
SoftwareSerial SerialSIM800(PIN_RX, PIN_TX);

// Create object for interaction with DHT sensor. DHT_22 is the sensor model
DHT dht(PIN_DHT_SENSOR, DHT22);

// ------------ Constants definition ------------//
// Sample time for measurements in seconds
const int SAMPLE_TIME = 1;

// Minimum time between consecutive irrigations in seconds
const int TIME_BETWEEN_IRRIGATIONS = 2;

// Safety time to detect that a button is stuck on the pushed position and
// consider that it is broken in seconds
const int BUTTON_SAFETY_TIME = 60;

// Effective irrigation time in seconds
const int EFFECTIVE_IRRIGATION_TIME = 7;

// Conversion factor from Hz to L/min flow for the water flow sensor
const float FLOW_CONVERSION_FACTOR = 7.11;

// Humidity threshold for irrigation (only irrigate below this value)
const int HUMIDITY_THRESHOLD = 720;

// Temperature threshold for irrigation (only irrigate over this value)
const int TEMPERATURE_THRESHOLD = 980;

// Light threshold for irrigation (only irrigate below this value) 
const int LIGHT_THRESHOLD = 1000;

// Voltage threshold for irrigation (only irrigate over this value) and SMS alert
const byte VOLTAGE_THRESHOLD = 6;

// Phone number for the SMS alerts
const char PHONE_NUMBER[14] = "+34605521505";

// Size of buffer for incoming data from SIM800
const int BUFFER_SIZE = 127;

// Command to request the status of the measurements and total water volume
const char* STATUS_COMMAND = "ESTADO";

// Commands to request a manual irrigation for valves 1 and 2 (similar to pressing the button)
const char* IRRIGATION_COMMAND = "RIEGO";

// Command to request a manual close of the relay
const char* CLOSE_COMMAND = "CERRAR";

// Time between writes of total volume to the EEPROM in minutes
const int EEPROM_WRITE_SAMPLE_TIME = 1440; //24h

// EEPROM memory adress to store the data to
const int EEPROM_ADDRESS = 0;

// ------------ Global variables ------------//
// Boolean for the irrigation valve to be open
bool valveOpen = false;

// Boolean for relay status
bool relayOn = false;

//Reference time for previous measurement
unsigned long prevMeasurementTime = 0;

// Reference time when the irrigation started
unsigned long irrigationStartTime = 0 ;

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
bool remoteIrrigationPending = false;

// Flag to detect if the button was pressed on the previous loop
bool wasButtonPressed = false;

// Variable to store the initial timestamp when the button was pressed (in milliseconds since program start time)
unsigned long buttonPressedStartTime = 0;

// Flag set to true when an irrigation ends and the button is still pressed. If this happens then the relay will be closed for safety and an SMS alert will be sent
bool buttonBrokenFlag = false;

// Flag to mark that a relay closure has been requested by SMS
bool closeRelayRequested = false;

void setup()
{
  // Start serial connection
  if (DEBUG_MODE) Serial.begin(9600); Serial.println ("Enviar 'r' para restablecer el volumen a 0 Litros");

  // Define pin modes
  pinMode(PIN_FLOW_SENSOR, INPUT);
  pinMode(PIN_INPUT_BUTTON, INPUT_PULLUP); // Configura pin 7 interno pull-up resistor  Estado en 1, requiere 0 para activarse
  pinMode(PIN_VALVE_A1, OUTPUT); // Pin valvula 1 A-
  pinMode(PIN_VALVE_A2, OUTPUT); // Pin valvula 1 A+
  pinMode(PIN_SAFETY_RELAY, OUTPUT); // Salida rele para apagado voltage seguridad

  // Define interruption for flow sensor
  attachInterrupt(0, countPulses , RISING); //(Interrupción 0(Pin2),función,Flanco de subida)
  interrupts();

  // Initialize DHT sensor
  dht.begin();

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
  unsigned long currentMillis = millis();
  unsigned int dt = (unsigned int) ((currentMillis - prevMeasurementTime)/1000); //Elapsed time since last measurement in seconds

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
    else totalWaterVolume += (waterFlow_L_min / 60) * dt; // volumen(L)=caudal(L/s)*tiempo(s)

    // Persist the totalVolume to the EEPROM when the configured time has passed
    if ((unsigned int)((currentMillis - prevEepromWriteTime) / 60000) >= EEPROM_WRITE_SAMPLE_TIME)
    {
      prevEepromWriteTime = currentMillis;
      sendMeasurements = true; // Set flag to true to send the measurements with the new stored water volume to the phone

      // Write to EEPROM
      EEPROM.put(EEPROM_ADDRESS, totalWaterVolume);
      if (DEBUG_MODE) Serial.print("Wrote to EEPROM value: "); Serial.println(totalWaterVolume, 3);
    }

    // Read sensors
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();
    float light = analogRead(PIN_LIGHT_SENSOR);
    float voltage = analogRead(PIN_VOLTAGE_SENSOR) * (5.01 / 1023.00) * 1.24;

    // SMS alert if voltage is under threshold
    if (voltage < VOLTAGE_THRESHOLD && !lowVoltageSmsSent)
    {
      lowVoltageSmsSent = true;
      sendLowVoltageSMSAlert(voltage);
    }

    // Check button status. If it is low, buttonPresed = true. If the button is broken, this will always give false to ignore it and stop irrgating
    bool buttonPressed;
    if (buttonBrokenFlag)
    {
      // The button has been detected to have a failure, ignore its current status.
      buttonPressed = false;
    }
    else
    {
      // The button is not in failure state, check its status
      buttonPressed = checkButtonStatus();
    }

    // Send measurements when requested through SMS
    if (sendMeasurements)
    {
      sendMeasurements = false; // Setting it to false to avoid send in loop
      sendMeasurementsSMS(humidity, temperature, light, voltage, totalWaterVolume);
    }

    // Print measurements only in debug mode
    if (DEBUG_MODE) printMeasurements(humidity, temperature, light, voltage, waterFlow_L_min, totalWaterVolume, buttonPressed);

    // If batteries are running out, temperature is too low or humidity is too high or the button is broken, turn off the system for safety and to save power
    if (humidity > HUMIDITY_THRESHOLD || voltage < VOLTAGE_THRESHOLD || temperature < TEMPERATURE_THRESHOLD || light > LIGHT_THRESHOLD || closeRelayRequested || buttonBrokenFlag)
    {
      // Ensure valves stay closed
      if (valveOpen)
      {
        closeValve();
        valveOpen = false;
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
      if (buttonPressed || remoteIrrigationPending)
      {
        // Only irrigate if enough time has passed since last irrigation
        if ((((unsigned int)((currentMillis - irrigationStartTime)/1000) >= (EFFECTIVE_IRRIGATION_TIME + TIME_BETWEEN_IRRIGATIONS)) || irrigationStartTime == 0) && !valveOpen)
        {
          irrigationStartTime = currentMillis;
          valveOpen = true;
          openValve();

          // If the request was remote, clear the flag and notify the user that the irrigation is starting
          if (remoteIrrigationPending)
          {
            remoteIrrigationPending = false;
            sendIrrigationConfirmationSMS();
          }
        }
      }

      // If the valve 1 has been open for the configured EFFECTIVE_IRRIGATION_TIME1, turn it off
      if ((unsigned int)((currentMillis - irrigationStartTime)/1000) >= EFFECTIVE_IRRIGATION_TIME && valveOpen)
      {
        valveOpen = false;
        closeValve();
      }
    }
  }
  delay(100);
}

  // --- Function to check if a button is in failure state (pushed for too much time)
  // The argument 'buttonId' identifies the button in case of multiple ones.
  bool checkButtonStatus()
  {
    bool buttonPressed = !digitalRead(PIN_INPUT_BUTTON);
    if (buttonPressed)
    {
      // Read current time
      unsigned long currentMillis =  millis();
      
      // The button is pressed, check if it was already pressed in the last loop
      if (!wasButtonPressed)
      {
        // The button was not pressed, store the time when the button was initially pressed
        wasButtonPressed = true;
        buttonPressedStartTime = currentMillis;
      }
      else
      {
        // The button was already pressed, check if it has been pressed for too long to detect a failure        
        if ((unsigned int) ((currentMillis - buttonPressedStartTime)/1000) >= BUTTON_SAFETY_TIME)
        {
          if(DEBUG_MODE) Serial.println("Failure in button, ignoring status and sending SMS alert");
          
          // Button pressed overtime -> Set the buttonBrokenFlag to true
          buttonBrokenFlag = true;

          // Set button pressed to false to avoid irrigation.
          buttonPressed = false;
  
          // Send SMS to nofity that the button is broken
          sendBrokenButtonSMS();
        }
      }
    }
    else
    {
      // The button is not pressed, clear the pressed status for the next loop
      wasButtonPressed = false;
    }
    return buttonPressed;
  }

  // -------- Function to open valve ---------- //
  void openValve()
  {
    digitalWrite(PIN_VALVE_A1, HIGH);
    digitalWrite(PIN_VALVE_A2, LOW);
    delay(300);
    digitalWrite(PIN_VALVE_A1, LOW);      

    if (DEBUG_MODE) Serial.println("OPEN RIEGO");
  }

  // -------- Function to close valve ---------- //
  void closeValve()
  {
    digitalWrite(PIN_VALVE_A2, HIGH);
    digitalWrite(PIN_VALVE_A1, LOW);
    delay(300);
    digitalWrite(PIN_VALVE_A2, LOW);     

    if (DEBUG_MODE) Serial.println("CLOSED RIEGO");
  }

  //---Función que se ejecuta en interrupción (ISR) ---------------//
  void countPulses ()
  {
    pulsesCount++;  //incrementamos la variable de pulsos
  }

  //--- Function for pretty-printing the measurements--------//
  void printMeasurements(float hum, float temp, float light, float voltage, float waterFlow, float totalVolume, bool buttonPressed)
  {
    if (!DEBUG_MODE) return;
    Serial.print("Humedad: ");
    Serial.println(hum);
    Serial.print("Temperatura: ");
    Serial.println(temp);
    Serial.print("Luz: ");
    Serial.println(light);
    Serial.print("Voltage: ");
    Serial.println(voltage);
    Serial.print("Caudal: ");
    Serial.print(waterFlow, 3);
    Serial.print("L/min\tVolumen: ");
    Serial.print(totalVolume, 3);
    Serial.println(" L");
    Serial.print("Boton pulsado: ");
    Serial.println(buttonPressed);
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
  }

  //--- Function to send an SMS to the given phone number and with the given text
  void sendSMS(char* text, char* phone_number)
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

    if (strstr(message, IRRIGATION_COMMAND))
    {
      remoteIrrigationPending = true;
      if (DEBUG_MODE) Serial.println("Remote irrigation request received for valve 1!");
    }

    if (strstr(message, CLOSE_COMMAND))
    {
      closeRelayRequested = true;
      if (DEBUG_MODE) Serial.println("Relay closure request received!");
      sendCloseRelayConfirmationSMS();
    }
  }

  //--- Function to format and send the SMS with the sensor measurements. The used phone number is the one read from the received request ---//
  void sendMeasurementsSMS(float hum, float temp, float light, float voltage, float totalVolume)
  {
    char payload[100];

    // Convert floatt to char arrays. Sprintf does not work with floats
    char voltageStr[10];
    char volumeStr[10];
    char humStr[10];
    char tempStr[10];
    char lightStr[10];
    dtostrf(voltage, 3, 2, voltageStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision
    dtostrf(hum, 3, 2, humStr);
    dtostrf(temp, 3, 2, tempStr);
    dtostrf(light, 3, 2, lightStr);
    
    sprintf(payload, "HUM:%s,TEMP:%s,LIGHT:%s,VOLT:%s,LITROS:%s,RIEGO:%s,RELE:%s", humStr, tempStr, lightStr, voltageStr, volumeStr, valveOpen ? "SI" : "NO", relayOn ? "ABIERTO" : "CERRADO");
    
    sendSMS(payload, senderNum);
  }

  //--- Function to send SMS alert when the voltage is too low. The used SMS is the one that is configured by default in the application ---//
  void sendLowVoltageSMSAlert(float voltage)
  {
    char payload[100];

    // Convert floatt to char arrays. Sprintf does not work with floats
    char voltageStr[10];
    dtostrf(voltage, 3, 2, voltageStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision

    char waterVolumeStr[10];
    dtostrf(totalWaterVolume, 3, 2, waterVolumeStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision

    sprintf(payload, "ALERTA, voltage demasiado bajo: %s V. Litros: %s", voltageStr, waterVolumeStr);

    sendSMS(payload, PHONE_NUMBER);
  }

  //--- Function to inform the user that the requested irrigation is being executed ---//
  void sendIrrigationConfirmationSMS()
  {
     sendSMS("OK, Riego en marcha", senderNum);
  }

  //--- Function to inform the user that the button is broken ---//
  void sendBrokenButtonSMS()
  {
    char waterVolumeStr[10];
    dtostrf(totalWaterVolume, 3, 2, waterVolumeStr); // Minimum 3 digits (with the decimal point) and 2 decimals of precision

    char payload[100];
    sprintf(payload, "ALERTA, el boton de riego esta bloqueado. Litros: %s", waterVolumeStr);
    sendSMS(payload, PHONE_NUMBER);      
  }

  //--- Function to inform the user that the requested relay closure is being executed ---//
  void sendCloseRelayConfirmationSMS()
  {
    sendSMS("OK, cerrando rele", senderNum);
  }
