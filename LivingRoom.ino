#include <IRremote.h>
#include <XBee.h>
#include <DHT.h>

#define PIR_PIN 2
#define DHT_PIN 5
volatile boolean motionDetected;
volatile long motionMillis;

long dhtReadFreqMillis = 10000;
DHT dht;

IRsend irsend;
XBee xbee = XBee();
XBeeResponse response = XBeeResponse();
// create reusable response objects for responses we expect to handle 
ZBRxResponse rx = ZBRxResponse();
ModemStatusResponse msr = ModemStatusResponse();

// SH + SL Address of receiving XBee
XBeeAddress64 addr64 = XBeeAddress64(0, 0);
XBeeAddress64 addr64_bcast = XBeeAddress64(0, 0xffff);
ZBTxStatusResponse txStatus = ZBTxStatusResponse();

int statusLed = 13;
int errorLed = 13;
int dataLed = 13;
int toggle = 0;
char txMsg[30];
unsigned long lasttime=0;
unsigned long diff;

uint16_t command16 = 0;
uint64_t command64 = 0; 

short tempF;
short tempC;
short relHumid;
        
void flashLed(int pin, int times, int wait) {
    
    for (int i = 0; i < times; i++) {
      digitalWrite(pin, HIGH);
      delay(wait);
      digitalWrite(pin, LOW);
      
      if (i + 1 < times) {
        delay(wait);
      }
    }
}

void doDht() {
  diff = millis() - lasttime;
  if (diff < 0) {
    lasttime = 0;
  }
  else {
    if (lasttime == 0 || (diff > dht.getMinimumSamplingPeriod() && diff > dhtReadFreqMillis)) {
    // Send the DHT data as a text string
    // Note that arduino's sprintf doesn't support floats
    // To keep 1 digit of decimal precision, I multiply by 10, then use modulo to get the mantissa
    float c = dht.getTemperature();
    tempF = (short) (dht.toFahrenheit(c)*10);
    tempC = (short) (c*10);
    relHumid = (short) (dht.getHumidity()*10);
    snprintf(txMsg, sizeof(txMsg), "M:%s F:%d.%d C:%d.%d RH:%d.%d", 
      dht.getStatusString(),
      tempF/10, tempF%10, tempC/10, tempC%10, relHumid/10, relHumid%10); 
     
      if (*txMsg) {
        ZBTxRequest zbTx = ZBTxRequest(addr64, 
          (uint8_t*)txMsg, strnlen(txMsg, sizeof(txMsg)));
        xbee.send(zbTx); // after sending a tx request, we expect a status response
        // wait up to half second for the status response
        if (xbee.readPacket(500)) {
          // got a response!  
          // should be a znet tx status            	
          if (xbee.getResponse().getApiId() == ZB_TX_STATUS_RESPONSE) {
            xbee.getResponse().getZBTxStatusResponse(txStatus);
      
            // get the delivery status, the fifth byte
            if (txStatus.getDeliveryStatus() == SUCCESS) {
              // success.  time to celebrate
              flashLed(statusLed, 5, 50);
            } else {
              // the remote XBee did not receive our packet. is it powered on?
              flashLed(errorLed, 3, 500);
            }
          }
        } else if (xbee.getResponse().isError()) {
          //nss.print("Error reading packet.  Error code: ");  
          //nss.println(xbee.getResponse().getErrorCode());
        } else {
          // local XBee did not provide a timely TX Status Response -- should not happen
          flashLed(errorLed, 2, 50);
        }
      }
      *txMsg = 0;     
     lasttime = millis();
    }
  }
}

long motionReportedMillis = 0;
void reportMotion() {  
  if (motionDetected && millis() - motionReportedMillis > 5000) { //rate-limit the motion reports
    snprintf(txMsg, sizeof(txMsg), "LR MOTION %ld", motionMillis);
     
    if (*txMsg) {
      uint8_t broadcastRadius = 0;
      uint8_t frameId = 0; // no response required
      uint8_t option = 0;
      ZBTxRequest zbTx = ZBTxRequest(addr64_bcast, 0xfffe, broadcastRadius, option, (uint8_t *)txMsg, strnlen(txMsg, sizeof(txMsg)), frameId);
      xbee.send(zbTx); 
      motionReportedMillis = millis();
    }
    *txMsg = 0;
    motionDetected = false;
  }
}


void setup()
{
  pinMode(statusLed, OUTPUT);
  pinMode(errorLed, OUTPUT);
  pinMode(dataLed,  OUTPUT);
  pinMode(PIR_PIN, INPUT);
  
  attachInterrupt(digitalPinToInterrupt(PIR_PIN),pirPinRisingISR,RISING);
  motionDetected = false;
  Serial.begin(9600);
  xbee.begin(Serial);
  
  flashLed(statusLed, 3, 50);
  dht.setup(DHT_PIN); 
}

void loop() {
      
    doDht();
    reportMotion();
    
    if (Serial.available() > 5) {
      xbee.readPacket();      
      if (xbee.getResponse().isAvailable()) {
        // got something        
        if (xbee.getResponse().getApiId() == ZB_RX_RESPONSE) {
          xbee.getResponse().getZBRxResponse(rx);              
          if (rx.getOption() == ZB_PACKET_ACKNOWLEDGED) {
              // the sender got an ACK
              flashLed(statusLed, 10, 10);
          } else {
              // we got it (obviously) but sender didn't get an ACK
              flashLed(errorLed, 2, 20);
          }
          int d = 0;
          int nbits;
          int i=0;
          *txMsg = 0;
          switch(rx.getData(d++)) {
            case 'S': // SONY
              command16 = 0;
              nbits = rx.getData(d++); // should be 12 (0x0c)            
              for (int shiftbytes = 1; shiftbytes >=0; shiftbytes--) {
                command16 |= rx.getData(d++) << (8 * shiftbytes);
              }
              if (command16 > 0) {
                for (int i = 0; i < 3; i++) {
                  irsend.sendSony(command16, nbits); // Sony TV power code
                  delay(40);
                }              
              }
              snprintf(txMsg, sizeof(txMsg), "S:%x", command16);            
              break;
              
            case 'X': // XBOX (RC6)            
              command64 = 0;
              nbits = rx.getData(d++); // should be 36 (0x24)              
              for (int shiftbytes = 7; shiftbytes >=0; shiftbytes--) {
                command64 |= rx.getData(d++) << (8 * shiftbytes);
              }
              
              command64 = toggle ? command64 ^ 0x8000LL : command64;
              irsend.sendRC6(command64, nbits);
              snprintf(txMsg, sizeof(txMsg), "X:%x", command64);
              
              toggle = 1 - toggle;
              break;   
              
            case 'D': // reset dht timeout millis
              // Message is 'D' followed by decimal digits
              // any data following decimal digits is ignored
              // if the first byte after 'D' is not a decimal digit, the whole message is ignored
              i=0;
              for (char c = rx.getData(d++); i < sizeof(txMsg)-1 && isdigit(c); c = rx.getData(d++)) {
                txMsg[i++] = c;
              }
              txMsg[i] = 0;
              if (strlen(txMsg) > 0) {
                dhtReadFreqMillis = strtol(txMsg, NULL, 10);
              }
              break;
              
            default:
              break;
          }
          
        } else if (xbee.getResponse().getApiId() == MODEM_STATUS_RESPONSE) {
          xbee.getResponse().getModemStatusResponse(msr);
          // the local XBee sends this response on certain events, like association/dissociation
          
          if (msr.getStatus() == ASSOCIATED) {
            // yay this is great.  flash led
            flashLed(statusLed, 10, 10);
          } else if (msr.getStatus() == DISASSOCIATED) {
            // this is awful.. flash led to show our discontent
            flashLed(errorLed, 10, 10);
          } else {
            // another status
            flashLed(statusLed, 5, 10);
          }
        } else {
        	// not something we were expecting
          flashLed(errorLed, 1, 25);    
        }
      } else if (xbee.getResponse().isError()) {
        //nss.print("Error reading packet.  Error code: ");  
        //nss.println(xbee.getResponse().getErrorCode());
      }
    }    
}



void answer(char* ans, byte val) {
	byte frame[100];
	int f=0;
	frame[f++] = 0x7e;	// start byte

	frame[f++] = 0x0;	// length MSB (always 0)
	frame[f++] = 0x0;	// length LSB (set it later)

	frame[f++] = 0x10;	// frame ID for TX request
	frame[f++] = 0x1;	// frame ID (0=no reply needed, 1=pls reply)

	frame[f++] = 0x0;	// 64-bit destination address (0x0 = coord)
	frame[f++] = 0x0;
	frame[f++] = 0x0;
	frame[f++] = 0x0;
	frame[f++] = 0x0;
	frame[f++] = 0x0;
        frame[f++] = 0x00;
        frame[f++] = 0x00;
        
	frame[f++] = 0x00; // destination network addr 0 == coord (0xfffe if unknown)
	frame[f++] = 0x00;

        frame[f++] = 0x00; // radius
        frame[f++] = 0x00; // options

        // RF Data
	for (char *c = ans; *c; c++) {
		frame[f++] = *c;
	}

	// set LSB of length
	frame[2] = f-3; // length does not include start byte or the 
                        // 2 length bytes or the checksum

	long checksum = 0;
	for (int i=3; i < f; i++) {
		checksum += frame[i];
	}
	checksum = 0xff - (checksum & 0xff);
	frame[f++] = checksum;
          
	// Output the frame to serial
	for (int i=0; i <=f; i++) {
		Serial.write(frame[i]);
	}
}

 
// interrupt handler
 
void pirPinRisingISR() {
  motionDetected = true;
  motionMillis = millis();
}

