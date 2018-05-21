/**
 * \file oggplayer.ino
 *
 * \brief Example sketch of using the "MP3 shield board with TF card" with modified 
 * "Arduino_Library-vs1053_for_SdFat" to record and play ogg files
 * \remarks Comments are implemented with Doxygen Markdown format
 *
 * \author Bill Porter
 * \author Michael P. Flaga
 * \author onelife
 *
 * This sketch listens for commands from a serial terminal (like the Serial
 * Monitor in the Arduino IDE). 
 *
 */

#include <SPI.h>

//Add the SdFat Libraries
#include <SdFat.h>
#include <FreeStack.h>

//and the MP3 Shield Library
#include <vs1053_SdFat.h>

// Below is not needed if interrupt driven. Safe to remove if not using.
#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_Timer1
  #include <TimerOne.h>
#elif defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
  #include <SimpleTimer.h>
#endif

/**
 * \brief Object instancing the SdFat library.
 *
 * principal object for handling all SdCard functions.
 */
SdFat sd;

/**
 * \brief Object instancing the vs1053 library.
 *
 * principal object for handling all the attributes, members and functions for the library.
 */
vs1053 OGGplayer;
int16_t last_ms_char; // milliseconds of last recieved character from Serial port.
int8_t buffer_pos; // next position to recieve character from Serial port.

/**
 * \brief Boolean instancing the recording is in progress.
 */
bool in_recording = false;

//------------------------------------------------------------------------------
/**
 * \brief Setup the Arduino Chip's feature for our use.
 *
 * After Arduino's kernel has booted initialize basic features for this
 * application, such as Serial port and OGGplayer objects with .begin.
 * Along with displaying the Help Menu.
 *
 * \note returned Error codes are typically passed up from OGGplayer.
 * Whicn in turns creates and initializes the SdCard objects.
 *
 * \see
 * \ref Error_Codes
 */
  char buffer[6]; // 0-35K+null

void setup() {

  uint8_t result; //result code from some function as to be tested at later time.

  Serial.begin(115200);

  Serial.print(F("F_CPU = "));
  Serial.println(F_CPU);
  Serial.print(F("Free RAM = ")); // available in Version 1.0 F() bases the string to into Flash, to use less SRAM.
  Serial.print(FreeStack(), DEC);  // FreeRam() is provided by SdFatUtil.h
  Serial.println(F(" Should be a base line of 1017, on ATmega328 when using INTx"));


  //Initialize the SdCard.
  if(!sd.begin(SD_SEL, SPI_FULL_SPEED)) sd.initErrorHalt();
  // depending upon your SdCard environment, SPI_HAVE_SPEED may work better.
  if(!sd.chdir("/")) sd.errorHalt("sd.chdir");

  //Initialize the MP3 Player Shield
  result = OGGplayer.begin();
  //check result, see readme for error codes.
  if(result != 0) {
    Serial.print(F("Error code: "));
    Serial.print(result);
    Serial.println(F(" when trying to start OGG player"));
    if( result == 6 ) {
      Serial.println(F("Warning: patch file not found, skipping.")); // can be removed for space, if needed.
      Serial.println(F("Use the \"d\" command to verify SdCard can be read")); // can be removed for space, if needed.
    }
  }

#if (0)
  // Typically not used by most shields, hence commented out.
  Serial.println(F("Applying ADMixer patch."));
  if(OGGplayer.ADMixerLoad("admxster.053") == 0) {
    Serial.println(F("Setting ADMixer Volume."));
    OGGplayer.ADMixerVol(-3);
  }
#endif

  help();
  last_ms_char = millis(); // stroke the inter character timeout.
  buffer_pos = 0; // start the command string at zero length.
  parse_menu('l'); // display the list of files to play

}

//------------------------------------------------------------------------------
/**
 * \brief Main Loop the Arduino Chip
 *
 * This is called at the end of Arduino kernel's main loop before recycling.
 * And is where the user's serial input of bytes are read and analysed by
 * parsed_menu.
 *
 * Additionally, if the means of refilling is not interrupt based then the
 * OGGplayer object is serviced with the available function.
 *
 * \note Actual examples of the libraries public functions are implemented in
 * the parse_menu() function.
 */
void loop() {

// Below is only needed if not interrupt driven. Safe to remove if not using.
#if defined(USE_MP3_REFILL_MEANS) \
    && ( (USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer) \
    ||   (USE_MP3_REFILL_MEANS == USE_MP3_Polled)      )

  OGGplayer.available();
#endif

  char inByte;
  if (Serial.available() > 0) {
    inByte = Serial.read();
    if ((0x20 <= inByte) && (inByte <= 0x126)) { // strip off non-ASCII, such as CR or LF
      if (isDigit(inByte)) { // macro for ((inByte >= '0') && (inByte <= '9'))
        // else if it is a number, add it to the string
        buffer[buffer_pos++] = inByte;
      } else {
        // input char is a letter command
        buffer_pos = 0;
        parse_menu(inByte);
      }
      buffer[buffer_pos] = 0; // update end of line
      last_ms_char = millis(); // stroke the inter character timeout.
    }
  } else if ((millis() - last_ms_char) > 500 && ( buffer_pos > 0 )) {
    // ICT expired and have something
    if (buffer_pos == 1) {
      // look for single byte (non-number) menu commands
      parse_menu(buffer[buffer_pos - 1]);

    } else if (buffer_pos > 5) {
      // dump if entered command is greater then uint16_t
      Serial.println(F("Ignored, Number is Too Big!"));

    } else {
      // otherwise its a number, scan through files looking for matching index.
      int16_t fn_index = atoi(buffer);
      SdFile file;
      char filename[13];
      sd.chdir("/",true);
      uint16_t count = 1;
      while (file.openNext(sd.vwd(),O_READ))
      {
        file.getName(filename, sizeof(filename));
        if ( isFormat(supportedFormat, filename) ) {

          if (count == fn_index) {
            Serial.print(F("Index "));
            SerialPrintPaddedNumber(count, 5 );
            Serial.print(F(": "));
            Serial.println(filename);
            Serial.print(F("Playing filename: "));
            Serial.println(filename);
            int8_t result = OGGplayer.play(filename);
            //check result, see readme for error codes.
            if(result != 0) {
              Serial.print(F("Error code: "));
              Serial.print(result);
              Serial.println(F(" when trying to play track"));
            }
            break;
          }
          count++;
        }
        file.close();
      }

    }

    //reset buffer to start over
    buffer_pos = 0;
    buffer[buffer_pos] = 0; // delimit
  }

  if (in_recording) {
    in_recording = !OGGplayer.writeOggInLoop();
  } else {
    delay(100);
  }
}

uint32_t  millis_prv;

//------------------------------------------------------------------------------
/**
 * \brief Decode the Menu.
 *
 * Parses through the characters of the users input, executing corresponding
 * OGGplayer library functions and features then displaying a brief menu and
 * prompting for next input command.
 */
void parse_menu(byte key_command) {

  uint8_t result; // result code from some function as to be tested at later time.

  Serial.print(F("Received command: "));
  Serial.write(key_command);
  Serial.println(F(" "));

  //if s, stop the current track
  if(key_command == 's') {
    Serial.println(F("Stopping"));
    switch (OGGplayer.getState()) {
    case playback:
    case paused_playback:
      OGGplayer.stop();
      break;
    case recording:
      OGGplayer.stop();
      break;
    }

  //if +/- to change volume
  } else if((key_command == '-') || (key_command == '+')) {
    union twobyte mp3_vol; // create key_command existing variable that can be both word and double byte of left and right.
    mp3_vol.word = OGGplayer.getVolume(); // returns a double uint8_t of Left and Right packed into int16_t

    if(key_command == '-') { // note dB is negative
      // assume equal balance and use byte[1] for math
      if(mp3_vol.byte[1] >= 254) { // range check
        mp3_vol.byte[1] = 254;
      } else {
        mp3_vol.byte[1] += 2; // keep it simpler with whole dB's
      }
    } else {
      if(mp3_vol.byte[1] <= 2) { // range check
        mp3_vol.byte[1] = 2;
      } else {
        mp3_vol.byte[1] -= 2;
      }
    }
    // push byte[1] into both left and right assuming equal balance.
    OGGplayer.setVolume(mp3_vol.byte[1], mp3_vol.byte[1]); // commit new volume
    Serial.print(F("Volume changed to -"));
    Serial.print(mp3_vol.byte[1]>>1, 1);
    Serial.println(F("[dB]"));

  /* Display the file on the SdCard */
  } else if(key_command == 'd') {
    if(!OGGplayer.isBusy()) {
      // prevent root.ls when playing, something locks the dump. but keeps playing.
      // yes, I have tried another unique instance with same results.
      // something about SdFat and its 500byte cache.
      Serial.println(F("Files found (name date time size):"));
      sd.ls(LS_R | LS_DATE | LS_SIZE);
    } else {
      Serial.println(F("Busy, try again later."));
    }

  /* Get and Display the Audio Information */
  } else if(key_command == 'i') {
    OGGplayer.getAudioInfo();

  } else if(key_command == 'p') {
    if( OGGplayer.getState() == playback) {
      OGGplayer.pauseMusic();
      Serial.println(F("Pausing"));
    } else if( OGGplayer.getState() == paused_playback) {
      OGGplayer.resumeMusic();
      Serial.println(F("Resuming"));
    } else {
      Serial.println(F("Not Playing!"));
    }

  } else if(key_command == 'S') {
    Serial.println(F("Current State of VS10xx is."));
    Serial.print(F("isBusy() = "));
    Serial.println(OGGplayer.isBusy());

    Serial.print(F("getState() = "));
    switch (OGGplayer.getState()) {
    case uninitialized:
      Serial.print(F("uninitialized"));
      break;
    case initialized:
      Serial.print(F("initialized"));
      break;
    case loading:
      Serial.print(F("loading"));
      break;
    case ready:
      Serial.print(F("ready"));
      break;
    case playback:
      Serial.print(F("playback"));
      break;
    case paused_playback:
      Serial.print(F("paused_playback"));
      break;
    case cancelling:
      Serial.print(F("cancelling"));
      break;
    case skipping:
      Serial.print(F("skipping"));
      break;
    case recording:
      Serial.print(F("recording"));
      break;
    case finishing:
      Serial.print(F("finishing"));
      break;
    case testing_memory:
      Serial.print(F("testing_memory"));
      break;
    case testing_sinewave:
      Serial.print(F("testing_sinewave"));
      break;
    }
    Serial.println();

#if !defined(__AVR_ATmega32U4__)
  } else if(key_command == 'c') {
    // Check if the file exists already
    sd.chdir("/",true);
    SdFile *file = sd.vwd();
    char filename[15];
    strcpy(filename, "record01.ogg");
    for (uint8_t i = 1; i < 100; i++) {
      filename[6] = '0' + i/10;
      filename[7] = '0' + i%10;
      // create if does not exist, do not open existing, write, sync after write
      if (!file->exists(filename)) {
        break;
      }
    }
    Serial.print("Recording to "); Serial.println(filename);
    in_recording = !OGGplayer.recordOgg(filename, "en44k1q5.053", false);

  } else if(key_command == 'r') {
    OGGplayer.resumeMusic(2000);

  } else if(key_command == 'R') {
    OGGplayer.stop();
    OGGplayer.vs_init();
    Serial.println(F("Resetting VS10xx chip"));

  } else if(key_command == 'g') {
    int32_t offset_ms = 20000; // Note this is just an example, try your own number.
    Serial.print(F("jumping to "));
    Serial.print(offset_ms, DEC);
    Serial.println(F("[milliseconds]"));
    result = OGGplayer.skipTo(offset_ms);
    if(result != 0) {
      Serial.print(F("Error code: "));
      Serial.print(result);
      Serial.println(F(" when trying to skip track"));
    }

  } else if(key_command == 'k') {
    int32_t offset_ms = -1000; // Note this is just an example, try your own number.
    Serial.print(F("moving = "));
    Serial.print(offset_ms, DEC);
    Serial.println(F("[milliseconds]"));
    result = OGGplayer.skip(offset_ms);
    if(result != 0) {
      Serial.print(F("Error code: "));
      Serial.print(result);
      Serial.println(F(" when trying to skip track"));
    }

  } else if(key_command == 'O') {
    OGGplayer.end();
    Serial.println(F("VS10xx placed into low power reset mode."));

  } else if(key_command == 'o') {
    OGGplayer.begin();
    Serial.println(F("VS10xx restored from low power reset mode."));

#endif

  /* List out music files on the SdCard */
  } else if(key_command == 'l') {
    if(!OGGplayer.isBusy()) {
      Serial.println(F("Music Files found :"));
      SdFile file;
      char filename[13];
      sd.chdir("/",true);
      uint16_t count = 1;
      while (file.openNext(sd.vwd(),O_READ))
      {
        file.getName(filename, sizeof(filename));
        if ( isFormat(supportedFormat, filename) ) {
          SerialPrintPaddedNumber(count, 5 );
          Serial.print(F(": "));
          Serial.println(filename);
          count++;
        }
        file.close();
      }
      Serial.println(F("Enter Index of File to play"));

    } else {
      Serial.println(F("Busy Playing Files, try again later."));
    }

  } else if(key_command == 'h') {
    help();
  }

  // print prompt after key stroke has been processed.
  Serial.print(F("Time since last command: "));  
  Serial.println((float) (millis() -  millis_prv)/1000, 2);  
  millis_prv = millis();
  Serial.print(F("Enter s,+,-,>,<,d,i,p,t,S,b"));
#if !defined(__AVR_ATmega32U4__)
  Serial.print(F(",c,m,e,r,R,g,k,O,o,D,V,B,C,T,E,M:"));
#endif
  Serial.println(F(",l,h :"));
}

//------------------------------------------------------------------------------
/**
 * \brief Print Help Menu.
 *
 * Prints a full menu of the commands available along with descriptions.
 */
void help() {
  Serial.println(F("COMMANDS:"));
  Serial.println(F(" [s] to stop playing"));
  Serial.println(F(" [d] display directory of SdCard"));
  Serial.println(F(" [+ or -] to change volume"));
  Serial.println(F(" [> or <] to increment or decrement play speed by 1 factor"));
  Serial.println(F(" [i] retrieve current audio information (partial list)"));
  Serial.println(F(" [p] to pause."));
  Serial.println(F(" [t] to toggle sine wave test"));
  Serial.println(F(" [S] Show State of Device."));
  Serial.println(F(" [b] Play a MIDI File Beep"));
#if !defined(__AVR_ATmega32U4__)
  Serial.println(F(" [c] Start recording"));
  Serial.println(F(" [e] increment Spatial EarSpeaker, default is 0, wraps after 4"));
  Serial.println(F(" [m] perform memory test. reset is needed after to recover."));
  Serial.println(F(" [M] Toggle between Mono and Stereo Output."));
  Serial.println(F(" [g] Skip to a predetermined offset of ms in current track."));
  Serial.println(F(" [k] Skip a predetermined number of ms in current track."));
  Serial.println(F(" [r] resumes play from 2s from begin of file"));
  Serial.println(F(" [R] Resets and initializes VS10xx chip."));
  Serial.println(F(" [O] turns OFF the VS10xx into low power reset."));
  Serial.println(F(" [o] turns ON the VS10xx out of low power reset."));
  Serial.println(F(" [D] to toggle SM_DIFF between inphase and differential output"));
  Serial.println(F(" [V] Enable VU meter Test."));
  Serial.println(F(" [B] Increament bass frequency by 10Hz"));
  Serial.println(F(" [C] Increament bass amplitude by 1dB"));
  Serial.println(F(" [T] Increament treble frequency by 1000Hz"));
  Serial.println(F(" [E] Increament treble amplitude by 1dB"));
#endif
  Serial.println(F(" [l] Display list of music files"));
  Serial.println(F(" [0####] Enter index of file to play, zero pad! e.g. 01-65534"));
  Serial.println(F(" [h] this help"));
}

void SerialPrintPaddedNumber(int16_t value, int8_t digits ) {
  int currentMax = 10;
  for (byte i=1; i<digits; i++){
    if (value < currentMax) {
      Serial.print("0");
    }
    currentMax *= 10;
  }
  Serial.print(value);
}
