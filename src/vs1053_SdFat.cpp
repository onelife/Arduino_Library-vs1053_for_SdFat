/**
\file vs1053.cpp

\brief Code file for the vs1053 library
\remarks comments are implemented with Doxygen Markdown format

*/

#include <vs1053_SdFat.h>
// inslude the SPI library:
#include "SPI.h"
//avr pgmspace library for storing the LUT in program flash instead of sram
#include <avr/pgmspace.h>

#define DEBUG 0

#if DEBUG
  static uint32_t cntIsr;
  static uint32_t cntRead;
  static uint32_t cntWork;
#endif

/**
 * \brief bitrate lookup table
 *
 * This is a table to decode the bitrate as per the MP3 file format,
 * as read by the SdCard
 *
 * <A HREF = "http://www.mp3-tech.org/programmer/frame_header.html" > www.mp3-tech.org </A>
 * \note PROGMEM macro forces to Flash space.
 * \warning This consums 190 bytes of flash
 */
static const uint16_t bitrate_table[15][6] PROGMEM = {
                 { 0,   0,  0,  0,  0,  0}, //0000
                 { 32, 32, 32, 32,  8,  8}, //0001
                 { 64, 48, 40, 48, 16, 16}, //0010
                 { 96, 56, 48, 56, 24, 24}, //0011
                 {128, 64, 56, 64, 32, 32}, //0100
                 {160, 80, 64, 80, 40, 40}, //0101
                 {192, 96, 80, 96, 48, 48}, //0110
                 {224,112, 96,112, 56, 56}, //0111
                 {256,128,112,128, 64, 64}, //1000
                 {288,160,128,144, 80, 80}, //1001
                 {320,192,160,160, 96, 69}, //1010
                 {352,224,192,176,112,112}, //1011
                 {384,256,224,192,128,128}, //1100
                 {416,320,256,224,144,144}, //1101
                 {448,384,320,256,160,160}  //1110
               };

/*
 * Format of a MIDI file into a char arrar. Simply one note on and then off.
*/
// MIDI Event Specifics
#define MIDI_NOTE_ON             9
#define MIDI_NOTE_OFF            8

// MIDI File structure
// Header Chunk
#define MIDI_HDR_CHUNK_ID     0x4D, 0x54, 0x68, 0x64  // const for MIDI
#define MIDI_CHUNKSIZE           0,    0,    0,    6
#define MIDI_FORMAT              0,    0              // VSdsp only support Format 0!
#define MIDI_NUMBER_OF_TRACKS    0,    1              // ergo must be 1 track
#define MIDI_TIME_DIVISION       0,   96
// Track Chunk
#define MIDI_TRACK_CHUNK_ID   0x4D, 0x54, 0x72, 0x6B  // const for MIDI
#define MIDI_CHUNK_SIZE          0,    0,    0, sizeof(MIDI_EVENT_NOTE_ON) + sizeof(MIDI_EVENT_NOTE_OFF) + sizeof(MIDI_END_OF_TRACK) // hard coded with zero padded
// Events
#define MIDI_EVENT_NOTE_ON       0, (MIDI_NOTE_ON<<4) + MIDI_CHANNEL, MIDI_NOTE_NUMBER, MIDI_INTENSITY
#define MIDI_EVENT_NOTE_OFF   MIDI_NOTE_DURATION, (MIDI_NOTE_OFF<<4) + MIDI_CHANNEL, MIDI_NOTE_NUMBER, MIDI_INTENSITY
//
#define MIDI_END_OF_TRACK        0, 0xFF, 0x2F,    0

/**
 * \brief a MIDI File of one Note
 *
 * This is string containing a complete MIDI format 0 file of one Note ON and then Off.
 *
 * <A HREF = "http://www.sonicspot.com/guide/midifiles.html" > Description of MIDI file parsing </A>
 * \note PROGMEM macro forces to Flash space.
 * \warning This should consume 34 bytes of flash
 *
 *
 * An inline equation @f$ e^{\pi i}+1 = 0 @f$
 *
 * A displayed equation: @f[ e^{\pi i}+1 = 0 @f]
 *
 *
 */
PROGMEM const uint8_t SingleMIDInoteFile[] = {MIDI_HDR_CHUNK_ID, MIDI_CHUNKSIZE, MIDI_FORMAT, MIDI_NUMBER_OF_TRACKS, MIDI_TIME_DIVISION, MIDI_TRACK_CHUNK_ID, MIDI_CHUNK_SIZE, MIDI_EVENT_NOTE_ON, MIDI_EVENT_NOTE_OFF, MIDI_END_OF_TRACK};

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
/* Initialize static classes and variables
 */

/**
 * \brief Initializer for the instance of the SdCard's static member.
 */
SdFile vs1053::track;

/**
 * \brief Initializer for the instance of the SdCard's static member.
 */
state_m vs1053::playing_state = uninitialized;

bool vs1053::isPatched;
bool vs1053::isSkipping;

/**
 * \brief Initializer for the instance of the SdCard's static member.
 */
uint16_t vs1053::spi_Read_Rate = SPI_CLOCK_DIV16;
uint16_t vs1053::spi_Write_Rate = SPI_CLOCK_DIV16;

format_m vs1053::trackFormat;
uint16_t vs1053::duration;
uint32_t vs1053::position;
uint16_t vs1053::skipToPosition;
bool vs1053::isRecordingStereo;
uint16_t vs1053::recordingLevel;

uint8_t vs1053::VolL = 0x30;
uint8_t vs1053::VolR = 0x30;

// only needed for specific means of refilling
#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
  SimpleTimer timer;
  int timerId_mp3;
#endif

//buffer for music
uint8_t vs1053::mp3DataBuffer[BUFFER_SIZE];
uint16_t vs1053::bufferOffset;

uint16_t vs1053::registers_backup[3];

//------------------------------------------------------------------------------
/**
 * \brief Initialize the MP3 Player shield.
 *
 * Execute this function before anything else, typically during setup().
 * It will bring the VS10xx out of reset, initialize the connected pins and
 * then ready the VSdsp for playback, with vs_init().
 *
 * \return Any Value other than zero indicates a problem occured.
 * where value indicates specific error
 *
 * \see
 * end() for low power mode
 * \see
 * \ref Error_Codes
 * \warning Will disrupt playback, if issued while playing back.
 * \note The \c SdFat::begin() function is required to be executed prior, as to
 * define the volume for the tracks (aka files) to be operated on.
 */
uint8_t  vs1053::begin() {
/*
 This test is to assist in the migration from versions prior to 1.01.00.
 It is not really needed, simply prints an easy error, to better assist.
 If you are using SdFat objects other than "sd" the below may be omitted.
 or whant to save 222 bytes of Flash space.
 */
#if (1)
  if (int8_t(sd.vol()->fatType()) == 0) {
    Serial.println(F("If you get this error, you likely do not have a sd.begin in the main sketch, See Trouble Shooting Guide!"));
    Serial.println(F("http://mpflaga.github.com/Sparkfun-MP3-Player-Shield-Arduino-Library/#Troubleshooting"));
  }
#endif

  pinMode(MP3_DREQ, INPUT);
  pinMode(MP3_XCS, OUTPUT);
  pinMode(MP3_XDCS, OUTPUT);
  pinMode(MP3_RESET, OUTPUT);
#if PERF_MON_PIN != -1
  pinMode(PERF_MON_PIN, OUTPUT);
  digitalWrite(PERF_MON_PIN,HIGH);
#endif

  cs_high();  //MP3_XCS, Init Control Select to deselected
  dcs_high(); //MP3_XDCS, Init Data Select to deselected
  digitalWrite(MP3_RESET, LOW); //Put VS1053 into hardware reset

  playing_state = initialized;

  uint8_t result = vs_init();
  if (result) return result;

#if defined(OGG_REFILL_USING_TIMER)
  Timer1.initialize();
#endif

#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_Timer1
  Timer1.initialize(MP3_REFILL_PERIOD);
#elif defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
  timerId_mp3 = timer.setInterval(MP3_REFILL_PERIOD, refill);
  timer.disable(timerId_mp3);
#endif

  return 0;
}

/**
 * \brief Disables the MP3 Player shield.
 *
 * Places the VS10xx into low power hard reset, after polity closing files
 * after releasing interrupts and or timers.
 *
 * \warning Will stop any playing tracks. Check isBusy() prior to executing, as not to stop on a track.
 * \note use begin() to reinitialize the VS10xx, for use.
 */
void vs1053::end() {
  stop(); // Stop and CLOSE any open tracks.
  cs_high();  //MP3_XCS, Init Control Select to deselected
  dcs_high(); //MP3_XDCS, Init Data Select to deselected
  digitalWrite(MP3_RESET, LOW); //Put VS1053 into hardware reset

  playing_state = uninitialized;
}

//------------------------------------------------------------------------------
/**
 * \brief Initialize the VS10xx Audio Decoder Chip.
 *
 * Reset and initialize the VS10xx chip's internal registers such as clock
 * for normal operation with the vs1053 class's members.
 * Along with uploading corresponding accumilative patch file, if present.
 *
 * \return Any Value other than zero indicates a problem occured.
 * - 0 indicates that upload was successful.
 * - 1 thru 3 are omitted, as not to overlap with other errors.
 * - 4 indicates other than default values were found in the SCI_MODE register.
 * - 5 indicates SCI_CLOCKF did not read back and verify the configured value.
 *
 * \note returned Error codes are typically passed and therefore need to avoid
 * overlap.
 *
 * \see
 * \ref Error_Codes
 */
uint8_t vs1053::vs_init() {
  /* Initialize VS1053 */
  digitalWrite(MP3_RESET, LOW); // Shut down VS1053
  delay(50);
  digitalWrite(MP3_RESET, HIGH); // Bring up VS1053
  delay(10);
  /* 
  From section 7.4.4 of datasheet (V1.31), the maximum speed for SCI reads is CLKI/7.
  And VS1053's clock multiplier (SCI_CLOCKF:SC_MULT) is 1.0x after reset.
  So when VS1053 CLKI = 12.288MHz and Arduino CLKI = 16.0MHz, the maximum SPI rate is 
  1.8MHz = (CLKI/7) = (12.288/7).

  Warning:
  Note that spi transfers interleave between SdCard and VS10xx.
  Where Sd2Card.cpp sets SPCR & SPSR each and every transfer

  The SDfatlib using SPI_FULL_SPEED results in an 8MHz spi clock rate,
  faster than initial allowed spi rate of 1.8MHz.
  */

  uint16_t sciMode = Mp3ReadRegister(SCI_MODE);
  // Serial.print(F("SCI_Mode (0x4800) = 0x"));
  // Serial.println(sciMode, HEX);
  if (sciMode != (SM_LINE1 | SM_SDINEW)) return 4;

  // uint16_t sciStatus = Mp3ReadRegister(SCI_Status);
  // Serial.print(F("SCI_Status (0x48) = 0x"));
  // Serial.println(sciStatus, HEX);

  // uint16_t sciClock = Mp3ReadRegister(SCI_CLOCKF);
  // Serial.print(F("SCI_ClockF = 0x"));
  // Serial.println(sciClock, HEX);
 
  /* Speed up */
  Mp3WriteRegister(SCI_CLOCKF, 0x6000); // Set multiplier to 3.0x. So max SPI speed is 52MHz
  delay(1);
  if (Mp3ReadRegister(SCI_CLOCKF) != 0x6000) return 5;
  
#if (F_CPU == 16000000 )
  spi_Read_Rate  = SPI_CLOCK_DIV4; //use safe SPI rate of (16MHz / 4 = 4MHz)
  spi_Write_Rate = SPI_CLOCK_DIV2; //use safe SPI rate of (16MHz / 2 = 8MHz)
#else
  // must be 8000000
  spi_Read_Rate  = SPI_CLOCK_DIV2; //use safe SPI rate of (8MHz / 2 = 4MHz)
  spi_Write_Rate = SPI_CLOCK_DIV2; //use safe SPI rate of (8MHz / 2 = 4MHz)
#endif
  
  /* Apply patch */
  if(VSLoadUserCode("patches.053")) return 6;
  delay(1); // just a good idea to let settle.
  isPatched = true;
  
  /* Set default volume */
  Mp3WriteRegister(SCI_VOL, VolL, VolR);

  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief load VS1xxx with patch or plugin from file on SDcard.
 *
 * \param[out] fileName pointer of a char array (aka string), contianing the filename
 *
 * Loads the VX10xx with filename of the specified patch, if present.
 * This can be used to load various VSdsp apps, patches and plug-in's.
 * Providing many new features and updates not present on the default firmware.
 *
 * The file format of the plugin is raw binary, in VLSI's interleaved and RLE
 * compressed format, as extracted from the source plugin file (.plg).
 * A perl script \c vs_plg_to_bin.pl is provided to convert the .plg
 * file in to the binary filename.053. Where the extension of .053 is a
 * convention to indicate the VSdsp chip version.
 *
 * \note by default all plug-ins are expected to be in the root of the SdCard.
 *
 * \return Any Value other than zero indicates a problem occured.
 * - 0 indicates that upload was successful.
 * - 1 indicates the upload can not be performed while currently streaming music.
 * - 2 indicates that desired file was not found.
 * - 3 indicates that the VSdsp is in reset.
 *
 * \see
 * - \ref Error_Codes
 * - \ref Plug_Ins
 */
uint8_t vs1053::VSLoadUserCode(const char* fileName){

  union twobyte val;
  union twobyte addr;
  union twobyte n;

  if(!digitalRead(MP3_RESET)) return 3;
  if(isBusy()) return 1;
  if(!digitalRead(MP3_RESET)) return 3;
  if (!track.open(fileName, O_READ)) return 2;
  
  while(1) {
    if (!track.read(addr.byte, 2)) break;
    if (!track.read(n.byte, 2)) break;
    if (n.word & 0x8000U) { 
      /* RLE run, replicate n samples */
      n.word &= 0x7FFF;
      if (!track.read(val.byte, 2)) break;
      while (n.word--) {
        Mp3WriteRegister(addr.word, val.word);
      }
    } else {
      /* Copy run, copy n samples */
      while(n.word--) {
        if (!track.read(val.byte, 2)) break;
        Mp3WriteRegister(addr.word, val.word);
      }
    }
  }
  track.close(); 
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief load VS1xxx image with patch or plugin from file on SDcard.
 *
 * \param[in] fileName pointer of a char array (aka string), contianing the filename
 * \param[out] address pointer of a 16-bit address
 *
 * Loads the VX10xx image with filename of the specified patch, if present.
 * This can be used to load various VSdsp apps, patches and plug-in's.
 * Providing many new features and updates not present on the default firmware.
 *
 * The file format of the image is raw binary, provided by VLSI.
 *
 * \note by default all images are expected to be in the root of the SdCard.
 *
 * \return Any Value other than zero indicates a problem occured.
 * - 0 indicates that upload was successful.
 * - 1 indicates the upload can not be performed while currently streaming music.
 * - 2 indicates that there is error when accessing desired file.
 * - 3 indicates that the VSdsp is in reset.
 *
 * \see
 * - \ref Error_Codes
 * - \ref Plug_Ins
 */
uint8_t vs1053::VSLoadImage(const char *fileName, uint16_t *address) {
  *address = 0xFFFF;
  if (!digitalRead(MP3_RESET)) return 3;
  if (isBusy()) return 1;
  if (!digitalRead(MP3_RESET)) return 3;
  if (!track.open(fileName, O_READ)) return 2;
  
  uint16_t offsets[] = {0x8000UL, 0x0, 0x4000UL};
  uint8_t result = 2;
  uint8_t temp[5];
  uint16_t n, addr;
  
  if (!track.read(temp, 3) || (temp[0] != 'P') || (temp[1] != '&') || (temp[2] != 'H')) {
    track.close();
    return result;
  }
  
  while (1) {
    if (!track.read(temp, 5)) break; // read error
    n = ((temp[1] << 8) | temp[2]) >> 1;
    addr = (temp[3] << 8) | temp[4];
    if (temp[0] >= 4) break; // Wrong type
    else if (temp[0] == 3) { // Execute, done
      *address = addr;
      result = 0;
      break;
    }
    
    Mp3WriteRegister(SCI_WRAMADDR, addr + offsets[temp[0]]); // Address
    while (n > 0) {
      if(!track.read(temp, 2)) break; // Read error
      Mp3WriteRegister(SCI_WRAM, (temp[0] << 8) | temp[1]); // Data
      n--;
    }
    if (n) break;
  }

  track.close(); 
  return result;
}

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// SelfTest_Group

//------------------------------------------------------------------------------
/**
 * \brief Generate Test Sine wave
 *
 * \param[in] freq specifies the output frequency sine wave.
 *
 * Enable and/or report the generation of Test Sine Wave as per specified.
 * As specified by Data Sheet Section 9.12.1
 *
 * \return
 * - -1 indicates the test can not be performed while currently streaming music
 *      or chip is reset.
 * - 1 indicates that test has begun successfully.
 * - 2 indicates that test is already in progress.
 *
 * \see
 * \ref Error_Codes
 * \note 9.12.5 New Sine and Sweep Tests was not implemented.
 */
uint8_t vs1053::enableTestSineWave(uint8_t freq) {

  if(isBusy()) {
    Serial.println(F("Warning Tests are not available."));
    return -1;
  }

  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);
  if(MP3SCI_MODE & SM_TESTS) {
    return 2;
  }

  Mp3WriteRegister(SCI_MODE, MP3SCI_MODE | SM_TESTS);

  for(int y = 0 ; y <= 1 ; y++) { // need to do it twice if it was already done once before
    //Wait for DREQ to go high indicating IC is available
    while(!digitalRead(MP3_DREQ)) ;
    //Select control
    dcs_low();
    //SCI consists of instruction byte, address byte, and 16-bit data word.
    SPI.transfer(0x53);
    SPI.transfer(0xEF);
    SPI.transfer(0x6E);
    SPI.transfer(freq);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    while(!digitalRead(MP3_DREQ)) ; //Wait for DREQ to go high indicating command is complete
    dcs_high(); //Deselect Control
  }

  playing_state = testing_sinewave;
  return 1;
}

//------------------------------------------------------------------------------
/**
 * \brief Disable Test Sine wave
 *
 * Disable and report the generation of Test Sine Wave as per specified.
 * As specified by Data Sheet Section 9.12.1
 * \return
 * - -1 indicates the test can not be performed while currently streaming music
 *      or chip is reset.
 * - 0 indicates the test is not previously enabled and skipping disable.
 * - 1 indicates that test was disabled.
 *
 * \see
 * \ref Error_Codes
 */
uint8_t vs1053::disableTestSineWave() {

  if(isBusy()) {
    Serial.println(F("Warning Tests are not available."));
    return -1;
  }

  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);
  if(!(MP3SCI_MODE & SM_TESTS)) {
    return 0;
  }

  //Wait for DREQ to go high indicating IC is available
  while(!digitalRead(MP3_DREQ)) ;

  //Select SPI Control channel
  dcs_low();
  //SDI consists of instruction byte, address byte, and 16-bit data word.
  SPI.transfer(0x45);
  SPI.transfer(0x78);
  SPI.transfer(0x69);
  SPI.transfer(0x74);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  SPI.transfer(0x00);
  while(!digitalRead(MP3_DREQ)) ; //Wait for DREQ to go high indicating command is complete
  //Deselect SPI Control channel
  dcs_high();

  // turn test mode bit off
  Mp3WriteRegister(SCI_MODE, Mp3ReadRegister(SCI_MODE) & ~SM_TESTS);

  playing_state = ready;
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Perform Memory Test
 *
 * Perform the internal memory test of the VSdsp core processor and resources.
 * As specified by Data Sheet Section 9.12.4
 *
 * \return
 * - -1 indicates the test can not be performed while currently streaming music
 *      or chip is reset.
 * - 1 indicates that test has begun successfully.
 * - 2 indicates that test is already in progress.
 *
 * \see
 * \ref Error_Codes
 */
uint16_t vs1053::memoryTest() {

  if(isBusy()) {
    Serial.println(F("Warning Tests are not available."));
    return -1;
  }

  playing_state = testing_memory;

  vs_init();

  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);
  if(MP3SCI_MODE & SM_TESTS) {
    playing_state = ready;
    return 2;
  }

  Mp3WriteRegister(SCI_MODE, MP3SCI_MODE | SM_TESTS);

//  for(int y = 0 ; y <= 1 ; y++) { // need to do it twice if it was already done once before
    //Wait for DREQ to go high indicating IC is available
    while(!digitalRead(MP3_DREQ)) ;

    //Select SPI Control channel
    dcs_low();
    //SCI consists of instruction byte, address byte, and 16-bit data word.
    SPI.transfer(0x4D);
    SPI.transfer(0xEA);
    SPI.transfer(0x6D);
    SPI.transfer(0x54);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    while(!digitalRead(MP3_DREQ)) ; //Wait for DREQ to go high indicating command is complete
    //Deselect SPI Control channel
    dcs_high();
//  }
  delay(250);

  uint16_t MP3SCI_HDAT0 = Mp3ReadRegister(SCI_HDAT0);

  Mp3WriteRegister(SCI_MODE, Mp3ReadRegister(SCI_MODE) & ~SM_TESTS);

  vs_init();

  playing_state = ready;
  return MP3SCI_HDAT0;
}
// @}
// SelfTest_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Volume_Group

//------------------------------------------------------------------------------
/**
 * \brief Overload function of vs1053::setVolume(leftchannel, rightchannel)
 *
 * \param[in] data packed with left and right master volume
 *
 * calls vs1053::setVolume expecting the left channel in the upper byte
 * and right channel in the lower byte.
 *
 * As specified by Data Sheet Section 8.7.11
 */
void vs1053::setVolume(uint16_t data) {
  union twobyte val = {.word = data};
  setVolume(val.byte[1], val.byte[0]);
}

//------------------------------------------------------------------------------
/**
 * \brief Overload function of vs1053::setVolume(leftchannel, rightchannel)
 *
 * \param[in] data byte to be placed into both Left and Right
 *
 * calls vs1053::setVolume placing the input into both the left channel
 * and right channels.
 *
 * As specified by Data Sheet Section 8.7.11
 */
void vs1053::setVolume(uint8_t data) {
  setVolume(data, data);
}

//------------------------------------------------------------------------------
/**
 * \brief Store and Push member volume to VS10xx chip
 *
 * \param[in] leftchannel writes the left channel master volume
 * \param[in] rightchannel writes the right channel master volume
 *
 * Updates the VS10xx SCI_VOL register's left and right master volume level in
 * -0.5 dB Steps. Where maximum volume is 0x0000 and total silence is 0xFEFE.
 * As specified by Data Sheet Section 8.7.11
 *
 * \note input values are -1/2dB. e.g. 40 results in -20dB.
 */
void vs1053::setVolume(uint8_t leftchannel, uint8_t rightchannel){
  VolL = leftchannel;
  VolR = rightchannel;
  Mp3WriteRegister(SCI_VOL, leftchannel, rightchannel);
}

//------------------------------------------------------------------------------
/**
 * \brief Get the current volume from the VS10xx chip
 *
 * Read the VS10xx SC_VOL register and return its results
 * As specified by Data Sheet Section 8.7.11
 *
 * \return uint16_t of both channels of master volume.
 * Where the left channel is in the upper byte and right channel is in the lower
 * byte.
 *
 * \note Input values are -1/2dB. e.g. 40 results in -20dB.
 * \note Cast the output to the union of twobyte.word to access individual
 * channels, with twobyte.byte[1] and [0].
 */
uint16_t vs1053::getVolume() {
  return Mp3ReadRegister(SCI_VOL);
}
// @}
// Volume_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Base_Treble_Group

//------------------------------------------------------------------------------
/**
 * \brief Get the current Treble Frequency limit from the VS10xx chip
 *
 * \return int16_t of frequency limit in Hertz.
 *
 */
uint16_t vs1053::getTrebleFrequency() {
  union sci_bass_m sci_base_value;
  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  return (sci_base_value.nibble.Treble_Freqlimt * 1000);
}

//------------------------------------------------------------------------------
/**
 * \brief Get the current Treble Amplitude from the VS10xx chip
 *
 * \return int16_t of amplitude (from -8 to 7).
 *
 */
int8_t vs1053::getTrebleAmplitude() {
  union sci_bass_m sci_base_value;
  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  return (sci_base_value.nibble.Treble_Amplitude);
}
//------------------------------------------------------------------------------
/**
 * \brief Get the current Bass Frequency limit from the VS10xx chip
 *
 * \return int16_t of bass frequency limit in Hertz.
 *
 */
uint16_t vs1053::getBassFrequency() {
  union sci_bass_m sci_base_value;
  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  return (sci_base_value.nibble.Bass_Freqlimt * 10);
}

//------------------------------------------------------------------------------
/**
 * \brief Get the current Bass boost amplitude from the VS10xx chip
 *
 * \return int16_t of bass bost amplitude in dB.
 *
 * \note Any value greater then zero enables the Bass Enhancer VSBE is a 
 * powerful bass boosting DSP algorithm, which tries to take the most out 
 * of the users earphones without causing clipping.
 *
 */
int8_t vs1053::getBassAmplitude() {
  union sci_bass_m sci_base_value;
  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  return (sci_base_value.nibble.Bass_Amplitude);
}
//------------------------------------------------------------------------------
/**
 * \brief Set the current treble frequency limit in VS10xx chip
 *
 * \param[in] frequency Treble cutoff limit in Hertz.
 *
 * \note The upper and lower limits of this parameter is checked.
 */
void vs1053::setTrebleFrequency(uint16_t frequency) {
  union sci_bass_m sci_base_value;

  frequency /= 1000;

  if(frequency < 1)
  {
      frequency = 1;
  }
  else if(frequency > 15)
  {
      frequency = 15;
  }
  
  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  sci_base_value.nibble.Treble_Freqlimt = frequency;
  Mp3WriteRegister(SCI_BASS, sci_base_value.word); 
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current Treble Amplitude in VS10xx chip
 *
 * \param[in] amplitude Treble in dB from -8 to 7.
 *
 * \note The upper and lower limits of this parameter is checked. 
 */
void vs1053::setTrebleAmplitude(int8_t amplitude) {
  union sci_bass_m sci_base_value;

  if(amplitude < -8)
  {
      amplitude = -8;
  }
  else if(amplitude > 7)
  {
      amplitude = 7;
  }

  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  sci_base_value.nibble.Treble_Amplitude = amplitude;
  Mp3WriteRegister(SCI_BASS, sci_base_value.word); 
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current Bass Boost Frequency limit cutoff in VS10xx chip
 *
 * \param[in] frequency of Bass Boost frequency cutoff limit in Hertz (20Hz to 150Hz).
 *
 * \note The upper and lower limits of this parameter is checked. 
 */
void vs1053::setBassFrequency(uint16_t frequency) {
  union sci_bass_m sci_base_value;

  frequency /= 10;

  if(frequency < 2)
  {
      frequency = 2;
  }
  else if(frequency > 15)
  {
      frequency = 15;
  }

  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  sci_base_value.nibble.Bass_Freqlimt = frequency;
  Mp3WriteRegister(SCI_BASS, sci_base_value.word); 
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current Bass Boost amplitude in VS10xx chip
 *
 * \param[in] amplitude to Bass Boost amplitude in dB (0dB to 15dB).
 *
 * \note Any value greater then zero enables the Bass Enhancer VSBE is a 
 * powerful bass boosting DSP algorithm, which tries to take the most out 
 * of the users earphones without causing clipping.
 *
 * \note The upper and lower limits of this parameter is checked. 
 */
void vs1053::setBassAmplitude(uint8_t amplitude) {
  union sci_bass_m sci_base_value;

  if(amplitude > 15)
  {
      amplitude = 15;
  }

  sci_base_value.word = Mp3ReadRegister(SCI_BASS);
  sci_base_value.nibble.Bass_Amplitude = amplitude;
  Mp3WriteRegister(SCI_BASS, sci_base_value.word); 
}
// @}
// Base_Treble_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// PlaySpeed_Group

//------------------------------------------------------------------------------
/**
 * \brief Get the current playSpeed from the VS10xx chip
 *
 * Read the VS10xx extra parameter memory for playSpeed register and return its
 * results.
 * As specified by Data Sheet Section 9.11.1
 *
 * \return multipler of current playspeed versus normal speed.
 * Where 0 and/or 1 are normal 1x speed.
 * e.g. 4 to playSpeed will play the song four times as fast as normal,
 * if you are able to feed the data with that speed.
 *
 * \warning Excessive playspeed beyond the ability to stream data between the
 * SdCard, Arduino and VS10xx may result in erratic behavior.
 */
uint16_t vs1053::getPlaySpeed() {
  return Mp3ReadWRAM(para_playSpeed);
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current playSpeed of the VS10xx chip
 *
 * Write the VS10xx extra parameter memory for playSpeed register with the
 * desired multipler.
 *
 * Where 0 and/or 1 are normal 1x speed.
 * e.g. 4 to playSpeed will play the song four times as fast as normal,
 * if you are able to feed the data with that speed.
 * As specified by Data Sheet Section 9.11.1
 *
 * \warning Excessive playspeed beyond the ability to stream data between the
 * SdCard, Arduino and VS10xx may result in erratic behavior.
 */
void vs1053::setPlaySpeed(uint16_t data) {
  Mp3WriteWRAM(para_playSpeed, data);
}
// @}
//PlaySpeed_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// EarSpeaker_Group

//------------------------------------------------------------------------------
/**
 * \brief Get the current Spatial EarSpeaker setting from the VS10xx chip
 *
 * Read the VS10xx SCI_MODE register bits SM_EARSPEAKER_LO and SM_EARSPEAKER_HIGH
 * for current EarSpeaker and return its results as a composite integer.
 * As specified by Data Sheet Section 8.7.1 and 8.4
 *
 * \return result between 0 and 3. Where 0 is OFF and 3 is maximum.
 */
uint8_t vs1053::getEarSpeaker() {
  uint8_t result = 0;
  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);

  // SM_EARSPEAKER bits are not adjacent hence need to add them together
  if(MP3SCI_MODE & SM_EARSPEAKER_LO) {
    result += 0b01;
  }
  if(MP3SCI_MODE & SM_EARSPEAKER_HI) {
    result += 0b10;
  }
  return result;
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current Spatial EarSpeaker setting of the VS10xx chip
 *
 * \param[in] EarSpeaker integer value between 0 and 3. Where 0 is OFF and 3 is maximum.
 *
 * The input value is mapped onto SM_EARSPEAKER_LO and SM_EARSPEAKER_HIGH bits
 * and written the VS10xx SCI_MODE register, preserving the remainder of SCI_MODE.
 * As specified by Data Sheet Section 8.7.1 and 8.4
 */
void vs1053::setEarSpeaker(uint16_t EarSpeaker) {
  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);

  // SM_EARSPEAKER bits are not adjacent hence need to add them individually
  if(EarSpeaker & 0b01) {
    MP3SCI_MODE |=  SM_EARSPEAKER_LO;
  } else {
    MP3SCI_MODE &= ~SM_EARSPEAKER_LO;
  }

  if(EarSpeaker & 0b10) {
    MP3SCI_MODE |=  SM_EARSPEAKER_HI;
  } else {
    MP3SCI_MODE &= ~SM_EARSPEAKER_HI;
  }
  Mp3WriteRegister(SCI_MODE, MP3SCI_MODE);
}
// @}
// EarSpeaker_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Differential_Output_Mode_Group

//------------------------------------------------------------------------------
/**
 * \brief Get the current SM_DIFF setting from the VS10xx chip
 *
 * Read the VS10xx SCI_MODE register bits SM_DIFF
 * for current SM_DIFF and return its results as a composite integer.
 * To indicate if the Left Channel is either normal or differential output.
 * As specified by Data Sheet Section 8.7.1
 *
 * \return 0 for Normal and 1 is Differential Output.
 * \return
 * - 0 Normal in-phase audio output of left and right speaker signals.
 * - 1 Left channel output is the invert of the right channel.
 *
 * \see setDifferentialOutput()
 */
uint8_t vs1053::getDifferentialOutput() {
  uint8_t result = 0;
  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);

  if(MP3SCI_MODE & SM_DIFF) {
    result = 1;
  }
  return result;
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current SM_DIFF setting of the VS10xx chip
 *
 * \param[in] DiffMode integer value between 0 and 1.
 *
 * The input value is mapped onto the SM_DIFF of the SCI_MODE register,
 *  preserving the remainder of SCI_MODE. For stereo playback streams this
 * creates a virtual sound, and for mono streams this creates a differential
 * left/right output with a maximum output of 3V.

 * As specified by Data Sheet Section 8.7.1
 * \see getDifferentialOutput()
 */
void vs1053::setDifferentialOutput(uint16_t DiffMode) {
  uint16_t MP3SCI_MODE = Mp3ReadRegister(SCI_MODE);

  if(DiffMode) {
    MP3SCI_MODE |=  SM_DIFF;
  } else {
    MP3SCI_MODE &= ~SM_DIFF;
  }
  Mp3WriteRegister(SCI_MODE, MP3SCI_MODE);
}
// @}
// Differential_Output_Mode_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Stereo_Group

//------------------------------------------------------------------------------
/**
 * \brief Get the current Stereo/Mono setting of the VS10xx output
 *
 * Read the VS10xx WRAMADDR bit 0 of para_MonoOutput] for the current Stereo/Mono and
 * return its results as a byte. As specified by VS1053B PATCHES AND FLAC
 * DECODER Data Sheet Section 1.2 Mono output mode.
 *
 * \return result between 0 and 3. Where 0 is OFF and 3 is maximum.
 *
 * \warning This feature is only available when composite patch 1.7 or higher
 * is loaded into the VSdsp.
 */
uint16_t vs1053::getMonoMode() {
  return (Mp3ReadWRAM(para_MonoOutput) & 0x0001);
}

//------------------------------------------------------------------------------
/**
 * \brief Set the current Stereo/Mono setting of the VS10xx output
 *
 * Write the VS10xx WRAMADDR para_MonoOutput bit 0 to configure the current
 * Stereo/Mono. As specified by VS1053B PATCHES AND FLAC DECODER Data Sheet
 * Section 1.2 Mono output mode. While preserving the other bits.
 *
 * \warning This feature is only available when composite patch 1.7 or higher
 * is loaded into the VSdsp.
 */
void vs1053::setMonoMode(bool mono) {
  uint16_t data = Mp3ReadWRAM(para_MonoOutput);
  data = mono ? (data | 0x0001) : (data & ~0x0001);
  Mp3WriteWRAM(para_MonoOutput, data);
}
// @}
// Stereo_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Play_Control_Group

//------------------------------------------------------------------------------
/**
 * \brief Begin playing a mp3 file, just with a number
 *
 * \param[in] trackNo integer value between 0 and 9, corresponding to the track to play.
 *
 * Formats the input number into a corresponding filename,
 * from track001.mp3 through track009.mp3. Then executes the track by
 * calling vs1053::play(char* fileName)
 *
 * \return Any Value other than zero indicates a problem occured.
 * where value indicates specific error
 *
 * \see
 * \ref Error_Codes
 */
uint8_t vs1053::playTrack(uint8_t trackNo) {
  //a storage place for track names
  char trackName[15];
  //tack the number onto the rest of the filename
  sprintf(trackName, "track%03d.mp3", trackNo);
  //play the file
  return play(trackName);
}

//------------------------------------------------------------------------------
/**
 * \brief Begin playing a mp3 file by its filename.
 *
 * \param[out] fileName pointer of a char array (aka string), contianing the filename
 * \param[in] timecode (optional) milliseconds from the begining of the file.
 *  Only works with mp3 files, otherwise do nothing.
 *
 * Skip, if already playing. Otherwise initialize the SdCard track to desired filehandle.
 * Reset the ByteRate and Play position and set playing to indicate such.
 * If the filename extension is MP3, then pre-read the byterate from the file.
 * And initially fill the VSDsp's buffer, then enable refilling.
 *
 * \return Any Value other than zero indicates a problem occured.
 * where value indicates specific error
 *
 * \see
 * \ref Error_Codes
 *
 * \note
 * - Currently bitrate to calculate time offset is determined by either
 *   playing files or by reading MP3 headers. Hence only the later is doable
 *   without actually playing files. Hence other formats are not available, yet.
 * - enableRefill() will enable the appropiate interrupt to match the
 *   corresponding means selected.
 * - use \c SdFat::chvol() command prior, to select desired SdCard volume, if
 *   multiple cards are used.
 */
uint8_t vs1053::play(char* fileName, uint32_t timecode) {
  if(isBusy()) return 1;
  
  if (!isPatched) {
    VSLoadUserCode("patches.053");
    delay(1);
    isPatched = true;
  }
  
  /* Initialize track */
  if(!track.open(fileName, O_READ)) return 2;
  trackFormat = getTrackFormat(fileName);
  bufferOffset = sizeof(mp3DataBuffer);
  isSkipping = false;
  start_of_music = 0;
  duration = 0;
  if (trackFormat == ogg) {
    /* OGG format support */
    getOggInfo();
  }
  else if (trackFormat == mp3) {
    /* 
    Only know how to read bitrate from MP3 file. ignore the rest.
    Note bitrate may get updated later by getAudioInfo() */
    getBitRateFromMP3File();
    if (timecode > 0) {
      track.seekSet(timecode * bitrate + start_of_music); // skip to X ms.
    }
  }

  Mp3WriteRegister(SCI_DECODE_TIME, 0); // Reset the decode time
  Mp3WriteRegister(SCI_DECODE_TIME, 0);
  playing_state = playback;
  refill();
  enableRefill();

  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Gracefully close track and cancel refill
 *
 * Skip if already not playing. Otherwise Disable the refill means,
 * then set playing to false, close the filehandle track instance.
 * And finally flush the VSdsp's stream buffer.
 */
void vs1053::stop(){
  if (isBusy() != 0x01) return;

  bool isPaused = playing_state == paused_playback;
  playing_state = cancelling;
  if (isPaused) enableRefill(); // Stopping is processed by refill()
  Serial.println(F("Stopping track"));
}

uint16_t vs1053::getRecordingLevel() {
  return recordingLevel;
}

//------------------------------------------------------------------------------
/**
 * \brief Begin recording an OGG file
 *
 * \param[in] fileName output file name.
 * \param[in] profileName profile name.
 * \param[in] isStereo stereo (true, 2 channels) or mono (false, 1 channel).
 *
 * 1. Backup and set registers
 * 2. Soft reset and disable interrupt except SCI
 * 3. Load profile (053 or img)
 * 4. Set recording related registers
 * 5. Open output file for writing
 * 6. Start recording
 *
 * \return Any Value other than zero indicates a problem occurred.
 * where value indicates specific error
 *
 * \see
 * \ref Error_Codes
 */
uint8_t vs1053::recordOgg(const char* fileName, const char* profileName, bool isStereo) {
  if (isBusy()) return 1;
  
  playing_state = loading;
  registers_backup[0] = Mp3ReadRegister(SCI_CLOCKF);
  registers_backup[1] = Mp3ReadRegister(SCI_BASS);
  registers_backup[2] = Mp3ReadRegister(SCI_MODE);
  
  Mp3WriteRegister(SCI_CLOCKF, 0xC000); // Set multiplier to 4.5x
  delay(1);
  Mp3WriteRegister(SCI_BASS, 0); // Clear bass
  Mp3WriteRegister(SCI_MODE, ((registers_backup[2] | SM_RESET) & ~SM_ADPCM)); // Soft reset
  delay(10);
  isPatched = false;
  Mp3WriteRegister(SCI_AIADDR, 0); 
  Mp3WriteWRAM(para_interrupt, 0x02); // Disable all interrupts except SCI
  
#if defined(PROFILE_LOADER) && PROFILE_LOADER == IMG_LOADER
  uint16_t addr;
  if (VSLoadImage(profileName, &addr)) {
#else
  if (VSLoadUserCode(profileName)) {
#endif
    playing_state = ready;
    Serial.print(F("Error: Load ")); Serial.print(profileName); Serial.println(F(" failed!"));
    return 2;
  }
#if defined(PROFILE_LOADER) && PROFILE_LOADER == IMG_LOADER
  Serial.print(F("Image at: $")); Serial.println(addr, HEX);
#endif

  int sciMODE;
  if (isStereo) {
    sciMODE = Mp3ReadRegister(SCI_MODE) | SM_ADPCM | SM_LAYER12;
  } else {
    sciMODE = Mp3ReadRegister(SCI_MODE) | SM_ADPCM;
  }
  isRecordingStereo = isStereo;
  /* Set Input Mode to either Line1 or Microphone. */
#if defined(VS_LINE1_MODE)
  Mp3WriteRegister(SCI_MODE, (sciMODE | SM_LINE1));
#else
  Mp3WriteRegister(SCI_MODE, (sciMODE & ~SM_LINE1));
#endif
  Mp3WriteRegister(SCI_AICTRL1, 1024); // Recording gain 1x
  Mp3WriteRegister(SCI_AICTRL2, 0); // Maximum AGC
  Mp3WriteRegister(SCI_AICTRL3, 0);
  
  if(!track.open(fileName, O_CREAT | O_WRITE)) return 2; // Open the file in write mode.

  Mp3WriteRegister(SCI_AIADDR, 0x34); // Start recording
  delay(1);
  while(!digitalRead(MP3_DREQ));
  
  playing_state = recording;
  enableRefill(true);
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Write recording data to file which should be called periodically  
 * during recording (e.g. in main loop) 
 *
 * 1. Check how many 16-bit words there are waiting in the VS1053 buffer 
 * 2. Write to file if #waiting >= (sizeof(mp3DataBuffer) >> 1)
 * 3. When stop recording is requested,
 *    a. Do 1 and 2  
 *    b. Check if VS1053 stopped
 *    c. When VS1053 stopped, check if last word is in full or half
 * 4. When writing data finished
 *    a. Close the file  
 *    b. Soft reset and restore registers 
 *
 * \return Any Value other than zero indicates a problem occurred.
 * where value indicates specific error
 *
 * \see
 * \ref Error_Codes
 */
uint8_t vs1053::writeOggInLoop() {
  uint8_t busy = isBusy();
  if ((busy != 0x02) && (busy != 0x04)) return 1;
  return oggRefill();
}

uint8_t vs1053::oggRefill() {
#if DEBUG
  uint32_t position2 = position;
  cntIsr++;
#endif
  uint8_t result = 0;
  const uint16_t mask = isRecordingStereo ? 0x8080 : 0x8000;
  static uint8_t readRecordingLevel = 0;
  static unsigned long millis_prv;
  bool finished = false;
  uint16_t written = 0;
  uint16_t waiting = Mp3ReadRegister(SCI_HDAT1);
  // Serial.print("waiting: "); Serial.println(waiting);
  
  while (waiting >= (sizeof(mp3DataBuffer) >> 1)) {
    for (uint16_t addr = 0; addr < sizeof(mp3DataBuffer); addr += 2) {
      union twobyte sciHDAT0;
      sciHDAT0.word = Mp3ReadRegister(SCI_HDAT0);
      mp3DataBuffer[addr] = sciHDAT0.byte[1]; 
      mp3DataBuffer[addr + 1] = sciHDAT0.byte[0];
    }
    if (!track.write(mp3DataBuffer, sizeof(mp3DataBuffer))) {
      Serial.println(F("Error: write OGG failed when recording"));
      finished = true; 
      result = 2;
      break;
    }
    written += sizeof(mp3DataBuffer);
    waiting -= (sizeof(mp3DataBuffer) >> 1);
#if DEBUG
    cntWork++;
#endif
  }
  
  if (playing_state == finishing) {
    Mp3WriteRegister(SCI_AICTRL3, Mp3ReadRegister(SCI_AICTRL3) | _BV(0)); // Stop
    while (!finished) {
      if (Mp3ReadRegister(SCI_AICTRL3) & _BV(1)) {
        finished = true;
        Serial.println("VS1053 stopped"); 
      }
      waiting = Mp3ReadRegister(SCI_HDAT1);
      // Serial.print("waiting2: "); Serial.println(waiting);
      /* wrapping up the recording! */
      while (waiting > 0) {
        uint16_t size, addr;
        if (waiting > (sizeof(mp3DataBuffer) >> 1)) {
          size = sizeof(mp3DataBuffer);
        } else if (finished) {
          size = (waiting - 1) << 1; // last word process later
        } else {
          size = waiting << 1;
        }
        
        for (addr = 0; addr < size; addr += 2) {
          union twobyte sciHDAT0;
          sciHDAT0.word = Mp3ReadRegister(SCI_HDAT0);
          mp3DataBuffer[addr] = sciHDAT0.byte[1]; 
          mp3DataBuffer[addr + 1] = sciHDAT0.byte[0];
        }
        /* last word */
        if (finished && waiting <= (sizeof(mp3DataBuffer) >> 1)) {
          union twobyte sciHDAT0;
          sciHDAT0.word = Mp3ReadRegister(SCI_HDAT0);
          mp3DataBuffer[addr] = sciHDAT0.byte[1]; 
          Mp3ReadRegister(SCI_AICTRL3);
          size++;
          if (!(Mp3ReadRegister(SCI_AICTRL3) & _BV(2))) {
            mp3DataBuffer[addr + 1] = sciHDAT0.byte[0];
            size++;
            Serial.println("Full last word"); 
          }
          // Serial.print("waiting3: "); Serial.println(waiting);
          // Serial.print("size: "); Serial.println(size);
        }
        if (!track.write(mp3DataBuffer, size)) {
          Serial.println(F("Error: write OGG failed when finishing"));
          finished = true; 
          result = 2;
          break;
        }
        written += size++;
        waiting -= (size >> 1);
        // Serial.print("written1: "); Serial.println(written);
        // Serial.print("waiting4: "); Serial.println(waiting);
#if DEBUG
        cntWork++;
#endif
      }
    }
  }
  
  /* Get position */
  position = Mp3ReadWRAM(para_recordingTime_0, true);
  /* Get recording level */
  switch (readRecordingLevel) {
    default:
    case 0:
      Mp3WriteRegister(SCI_AICTRL0, mask);
      millis_prv = millis();
      readRecordingLevel = 1;
      break;
    case 1:
      {
        unsigned long millis_cur = millis();
        if ((millis_cur - millis_prv) >= 25) { // Waiting at least 1/50 second
          uint16_t temp = Mp3ReadRegister(SCI_AICTRL0);
          if (!(temp & mask)) {
            recordingLevel = temp;
          }
          millis_prv = millis_cur;
          readRecordingLevel = 2;
        }
      }
      break;
    case 2:
      {
        unsigned long millis_cur = millis();
        if ((millis_cur - millis_prv) >= 500) { // Waiting at least 1/2 second
          readRecordingLevel = 0;
        }
      }
      break;
  }
  
  if (finished) {
    disableRefill(true);
    track.close(); // Close out this track
    
    Mp3WriteRegister(SCI_CLOCKF, registers_backup[0]); // Restore multiplier
    delay(1);
    Mp3WriteRegister(SCI_BASS, registers_backup[1]); // Restore bass
    Mp3WriteRegister(SCI_MODE, ((Mp3ReadRegister(SCI_MODE) & ~SM_ADPCM) | SM_RESET)); // Soft reset
    delay(1);
    isPatched = false;
    Mp3WriteRegister(SCI_MODE, registers_backup[2]); // Restore mode
    
    playing_state = ready;
    Serial.println(F("recording done"));
  }
  
  // Serial.print("written: "); Serial.println(written);
#if DEBUG
  if(position2 != position) {
    Serial.print(F("position ")); Serial.println(position);
    Serial.print(F("recordingLevel ")); Serial.println(recordingLevel);
    Serial.print(F("cntIsr ")); Serial.println(cntIsr);
    Serial.print(F("cntWork ")); Serial.println(cntWork);
    Serial.print(F("rate ")); Serial.println(1.0 * cntWork / cntIsr * 100);
    cntIsr = 0;
    cntWork = 0;
  }
#endif
  return result;
}

/**
 * \brief Gracefully stop recording
 *
 * Skip if already not recording. 
 */
void vs1053::stopRecord(){
  if (isBusy() != 0x02) return;

  playing_state = finishing;
  Serial.println(F("Recording is finishing!"));
}

//------------------------------------------------------------------------------
/**
 * \brief Indicate if a track is playing or recording
 *
 * Public method for determining if a file is streaming to the VSdsp or VSdsp is
 * doing encoding.
 *
 * \return
 * - 0 indicates \b NO file is currently being streamed to the VSdsp.
 * - 1 indicates that a file is currently being streamed to the VSdsp.
 * - 2 indicates that VSdsp is doing encoding.
 * - 3 indicates that the VSdsp is in reset.
 */
uint8_t vs1053::isBusy(){
  uint8_t result;
  if(!digitalRead(MP3_RESET))
    result = 0xFF;
  else {
    switch (getState()) {
      case playback:
      case paused_playback:
        result = 0x01;
        break;
      case recording:
        result = 0x02;
        break;
      case cancelling:
      case skipping:
        result = 0x03;
        break;
      case finishing:
        result = 0x04;
        break;
      default:
        result = 0x00;
        break;
    }
  }
  return result;
}

/**
 * \brief Get the current state of the device
 *
 * Reports the current operational status of the device from the list of possible
 * states enumerated by state_m
 *
 * \return the value held by vs1053::playing_state
 */
state_m vs1053::getState(){
 return playing_state;
}

//------------------------------------------------------------------------------
/**
 * \brief Pause music.
 *
 * Public method for pausing the play of music.
 *
 * \note This is currently equal to pauseDataStream() and is a place holder to
 * pausing the VSdsp's playing and DREQ's.
 */
void vs1053::pauseMusic() {
  if ((playing_state != playback) || !digitalRead(MP3_RESET)) return;
  
  disableRefill();
  playing_state = paused_playback;
}

//------------------------------------------------------------------------------
/**
 * \brief Resume music from pause at new location.
 *
 * \param[in] timecode (optional) milliseconds from the begining of the file.
 *
 * Public method for resuming the play of music from a specific file location.
 *
 * \return
 * - 0 indicates the position was changed.
 * - 1 indicates no action, in lieu of any current file stream.
 * - 2 indicates failure to skip to new file location.
 *
 * \note This is effectively equal to resumeDataStream() and is a place holder to
 * resuming the VSdsp's playing and DREQ's.
 */
uint8_t vs1053::resumeMusic(uint32_t timecode) {
  if ((playing_state != paused_playback) || !digitalRead(MP3_RESET)) return 1;

  if (timecode != 0xFFFFFFFF) {
    if (!track.seekSet(timecode * Mp3ReadWRAM(para_byteRate) / 1000 + start_of_music)) return 2;
  }

  enableRefill();
  playing_state = playback;
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Skips to a duration in the track
 *
 * \param[in] timecode offset milliseconds from the current location of the file.
 *
 * Repositions the filehandles track location to the requested offset.
 * As calculated by the bitrate multiplied by the desired ms offset.
 *
 * \return
 * - 0 indicates the position was changed.
 * - 1 indicates no action, in lieu of any current file stream.
 * - 2 indicates failure to skip to new file location.
 *
 * \warning Limited to +/- 32768ms, since SdFile::seekCur(int32_t);
 */
uint8_t vs1053::skip(int32_t seconds) {
  if ((isBusy() != 0x01) || seconds == 0) return 1;
  
  uint32_t targetPosition;
  if (trackFormat == ogg) {
    if ((seconds < 0) && ((uint32_t)(-seconds) > position)) {
      targetPosition = 0;
    } else if ((position + seconds) > duration) {
      targetPosition = duration;
    } else {
      targetPosition = position + seconds;
    }
  } else {
    targetPosition = (track.curPosition() - start_of_music) / Mp3ReadWRAM(para_byteRate) + seconds;
  }
  return skipTo(targetPosition);
}

//------------------------------------------------------------------------------
/**
 * \brief Skips to a certain point in the track
 *
 * \param[in] timecode offset milliseconds from the begining of the file.
 *
 * Repositions the filehandles track location to the requested offset.
 * As calculated by the bitrate multiplied by the desired ms offset.
 *
 * \return
 * - 0 indicates the position was changed.
 * - 1 indicates no action, in lieu of any current file stream.
 * - 2 indicates failure to skip to new file location.
 *
 * \warning Limited to first 65535ms, since SdFile::seekSet(int32_t);
 */
uint8_t vs1053::skipTo(uint32_t seconds) {
  if (isBusy() != 0x01) return 1;
  
  if (trackFormat == ogg) {
    bool isPaused = playing_state == paused_playback;
    if (seconds >= duration) {
      skipToPosition = duration;
      playing_state = cancelling;
    } else {
      skipToPosition = seconds;
      playing_state = skipping;
    }
    Serial.print(F("skipping to ")); Serial.println(skipToPosition);
    if (isPaused) enableRefill(); // Skipping is processed by refill()
  } else {
    //stop interupt for now
    disableRefill();
    playing_state = paused_playback;

    // try to set the files position to current position + offset(in bytes)
    // as calculated from current byte rate, as per VSdsp.
    if(!track.seekSet(seconds * Mp3ReadWRAM(para_byteRate) + start_of_music)) return 2; // skip to X ms.

    Mp3WriteRegister(SCI_VOL, 0xFE, 0xFE);
    //seeked successfully

    flush_cancel(pre); //possible mode of "none" for faster response.

    Mp3WriteRegister(SCI_DECODE_TIME, seconds); 
    Mp3WriteRegister(SCI_DECODE_TIME, seconds); 
      
    //gotta start feeding that hungry mp3 chip
    refill();

    //again, I'm being bad and not following the spec sheet.
    //I already turned the volume down, so when the MP3 chip gets upset at me
    //for just slammin in new bits of the file, you won't hear it.
    //so we'll wait a bit, and restore the volume to previous level
    delay(50);

    //one of these days I'll come back and try to do it the right way.
    setVolume(VolL,VolR);

    playing_state = playback;
    //attach refill interrupt off DREQ line, pin 2
    enableRefill();
  }
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Current timecode in ms
 *
 * Reads the currenty position from the VSdsp's decode time
 * and converts the value to milliseconds.
 *
 * \return the milliseconds offset of stream played.
 *
 * \note SCI_DECODE_TIME is cleared during vs1053::play, as to restart
 * the position for each file stream. Erasing prior streams weight.
 *
 * \warning Not very accurate, rounded off to second. And Variable Bit-Rates
 * are completely inaccurate.
 */
uint32_t vs1053::currentPosition(){
  return position;
}

// @}
// Play_Control_Group

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// @{
// Audio_Information_Group

//------------------------------------------------------------------------------
/**
 * \brief Get Track's Artist
 *
 * \param[out] infobuffer pointer char array to be updated with result
 *
 * Extract the Artist from the current filehandles track ID3 tag information.
 *
 * \warning ID3 Tag information may not be present on all source files.
 * Otherwise may result in non-sense.
 * It is possible to add it with common tools outside of this project.
 */
void vs1053::trackArtist(char* infobuffer){
  getTrackInfo(TRACK_ARTIST, infobuffer);
}

//------------------------------------------------------------------------------
/**
 * \brief Get Track's Title
 *
 * \param[out] infobuffer pointer char array to be updated with result
 *
 * Extract the Title from the current filehandles track ID3 tag information.
 *
 * \warning ID3 Tag information may not be present on all source files.
 * Otherwise may result in non-sense.
 * It is possible to add it with common tools outside of this project.
 */
void vs1053::trackTitle(char* infobuffer){
  getTrackInfo(TRACK_TITLE, infobuffer);
}

//------------------------------------------------------------------------------
/**
 * \brief Get Track's Album
 *
 * \param[out] infobuffer pointer char array to be updated with result
 *
 * Extract the Album from the current filehandles track ID3 tag information.
 *
 * \warning ID3 Tag information may not be present on all source files.
 * Otherwise may result in non-sense.
 * It is possible to add it with common tools outside of this project.
 */
void vs1053::trackAlbum(char* infobuffer){
  getTrackInfo(TRACK_ALBUM, infobuffer);
}

//------------------------------------------------------------------------------
/**
 * \brief Fetch ID3 Tag information
 *
 * \param[in] offset for the desired information desired.
 * \param[out] infobuffer pointer char array of filename to be read.
 *
 * Read current filehandles offset of track ID3 tag information. Then strip
 * all non readible (ascii) characters.
 *
 * \note this suspends currently playing streams and returns afterwards.
 * Restoring the file position to where it left off, before resuming.
 */
void vs1053::getTrackInfo(uint8_t offset, char* infobuffer){

  //disable interupts
  if(playing_state == playback) {
    disableRefill();
  }

  //record current file position
  uint32_t currentPos = track.curPosition();

  //skip to end
  track.seekEnd((-128 + offset));

  //read 30 bytes of tag informat at -128 + offset
  track.read(infobuffer, 30);
  infobuffer = strip_nonalpha_inplace(infobuffer);

  //seek back to saved file position
  track.seekSet(currentPos);

  //renable interupt
  if(playing_state == playback) {
    enableRefill();
  }

}

//------------------------------------------------------------------------------
/**
 * \brief Playable duration in second
 */
uint32_t vs1053::getDuration(){
  return duration;
}

//------------------------------------------------------------------------------
/**
 * \brief Display various Audio information from the VSdsp.
 *
 * Read numerous attributes from the VSdsp's registers about either the currently
 * or prior played stream and display in a column format for easy reviewing.
 *
 * This may be called while playing a current stream.
 *
 * \note this suspends currently playing streams and returns afterwards.
 * Restoring the file position to where it left off, before resuming.
 */
void vs1053::getAudioInfo() {

  //disable interupts
  // already disabled in Mp3ReadRegister function
  //pauseDataStream();

  Serial.print(F("HDAT1"));
  Serial.print(F("\tHDAT0"));
  Serial.print(F("\tVOL"));
  Serial.print(F("\tMode"));
  Serial.print(F("\tStatus"));
  Serial.print(F("\tClockF"));
  Serial.print(F("\tpversion"));
  Serial.print(F("\t[Bytes/S]"));
  Serial.print(F("\t[KBits/S]"));
  Serial.print(F("\tPlaySpeed"));
  Serial.print(F("\tDECODE_TIME"));
  Serial.print(F("\tCurrentPos"));
  Serial.println();


  uint16_t MP3HDAT1 = Mp3ReadRegister(SCI_HDAT1);
  Serial.print(F("0x"));
  Serial.print(MP3HDAT1, HEX);

  uint16_t MP3HDAT0 = Mp3ReadRegister(SCI_HDAT0);
  Serial.print(F("\t0x"));
  Serial.print(MP3HDAT0, HEX);

  uint16_t MP3SCI_VOL = Mp3ReadRegister(SCI_VOL);
  Serial.print(F("\t0x"));
  Serial.print(MP3SCI_VOL, HEX);

  uint16_t MP3Mode = Mp3ReadRegister(SCI_MODE);
  Serial.print(F("\t0x"));
  Serial.print(MP3Mode, HEX);

  uint16_t MP3Status = Mp3ReadRegister(SCI_STATUS);
  Serial.print(F("\t0x"));
  Serial.print(MP3Status, HEX);

  uint16_t MP3Clock = Mp3ReadRegister(SCI_CLOCKF);
  Serial.print(F("\t0x"));
  Serial.print(MP3Clock, HEX);

  uint16_t MP3para_version = Mp3ReadWRAM(para_version);
  Serial.print(F("\t0x"));
  Serial.print(MP3para_version, HEX);

  uint16_t MP3ByteRate = Mp3ReadWRAM(para_byteRate);
  Serial.print(F("\t\t"));
  Serial.print(MP3ByteRate, HEX);

  Serial.print(F("\t\t"));
  Serial.print((MP3ByteRate>>7), DEC); // shift 7 is the same as *8/1024, and easier math.

  uint16_t MP3playSpeed = Mp3ReadWRAM(para_playSpeed);
  Serial.print(F("\t\t"));
  Serial.print(MP3playSpeed, HEX);

  uint16_t MP3SCI_DECODE_TIME = Mp3ReadRegister(SCI_DECODE_TIME);
  Serial.print(F("\t\t"));
  Serial.print(MP3SCI_DECODE_TIME, DEC);

  Serial.print(F("\t\t"));
  Serial.print(currentPosition(), DEC);

  Serial.println();
}

//------------------------------------------------------------------------------
/**
 * \brief Read the Bit-Rate from the current track's filehandle.
 *
 * \param[out] fileName pointer of a char array (aka string), contianing the filename
 *
 * locate the MP3 header in the current file and from there determine the
 * Bit-Rate, using bitrate_table located in flash. And return the position
 * to the prior location.
 *
 * \note the bitrate will be updated, as read back from the VS10xx when needed.
 *
 * \warning This feature only works on MP3 files.
 * It will \b LOCK-UP on other file formats, looking for the MP3 header.
 */
void vs1053::getBitRateFromMP3File() {
  //look for first MP3 frame (11 1's)
  bitrate = 0;
  uint8_t temp = 0;
  uint8_t row_num =0;

    for(uint16_t i = 0; i<65535; i++) {
      if(track.read() == 0xFF) {

        temp = track.read();

        if(((temp & 0b11100000) == 0b11100000) && ((temp & 0b00000110) != 0b00000000)) {

          //found the 11 1's
          //parse version, layer and bitrate out and save bitrate
          if(!(temp & 0b00001000)) { //!true if Version 1, !false version 2 and 2.5
            row_num = 3;
          }
          else if((temp & 0b00000110) == 0b00000100) { //true if layer 2, false if layer 1 or 3
            row_num += 1;
          }
          else if((temp & 0b00000110) == 0b00000010) { //true if layer 3, false if layer 2 or 1
            row_num += 2;
          } else {
            continue; // Not found, need to skip the rest and continue looking.
                      // \warning But this can lead to a dead end and file end of file.
          }

          //parse bitrate code from next byte
          temp = track.read();
          temp = temp>>4;

          //lookup bitrate
          bitrate = pgm_read_word_near ( &(bitrate_table[temp][row_num]) );

          //convert kbps to Bytes per mS
          bitrate /= 8;

          //record file position
          track.seekCur(-3);
          start_of_music = track.curPosition();

//          Serial.print(F("POS: "));
//          Serial.println(start_of_music);

//          Serial.print(F("Bitrate: "));
//          Serial.println(bitrate);

          //break out of for loop
          break;

        }
      }
    }
  }

//------------------------------------------------------------------------------
/**
 * \brief Read info from OGG file
 *
 * 1. Looking for OGG headers 
 * 2. Get the sample numbers, sample rate
 * 3. Calculate duration
 *
 * Ref: https://xiph.org/vorbis/doc/Vorbis_I_spec.html#x1-620004.2.1
 * Ref: https://xiph.org/ogg/doc/framing.html
 *
 * \warning This feature only works on OGG files.
 */
void vs1053::getOggInfo() {
  char header1[7] = {0x01, 'v', 'o', 'r', 'b', 'i', 's'};
  char header2[6] = {'O', 'g', 'g', 'S', 0x00, 0x04};
  uint8_t channelNumber = 0;
  uint32_t sampleRate = 0;
  uint64_t sampleNumber = 0;

  /* {0x01, 'v', 'o', 'r', 'b', 'i', 's'} */
  track.seekSet(0);
  while (track.read(mp3DataBuffer, sizeof(mp3DataBuffer))) {
    int16_t offset = -1;
    for (uint16_t i = 0; i < sizeof(mp3DataBuffer); i++) {
      if (mp3DataBuffer[i] == 'v') {
        bool doneForLoop = false;
        if (i + sizeof(header1) - 1 > sizeof(mp3DataBuffer)) {
          track.seekSet(track.curPosition() - sizeof(mp3DataBuffer) + i - 1);
          track.read(mp3DataBuffer, 16);
          i = 1;
          doneForLoop = true;
        }
        if (!memcmp(header1, &mp3DataBuffer[i - 1], sizeof(header1))) {
          offset = i + 10;
          break;
        }
        if (doneForLoop) break;
      }
    }
    if (offset < 0) continue;
    if ((uint16_t)(offset + 4) >= sizeof(mp3DataBuffer)) {
      track.seekSet(track.curPosition() - sizeof(mp3DataBuffer) + offset);
      track.read(mp3DataBuffer, 5);
      offset = 0;
    }
    channelNumber = mp3DataBuffer[offset];
    sampleRate = ((uint32_t)mp3DataBuffer[offset + 4]) << 24 | \
                 ((uint32_t)mp3DataBuffer[offset + 3]) << 16 | \
                 ((uint32_t)mp3DataBuffer[offset + 2]) << 8 | \
                 ((uint32_t)mp3DataBuffer[offset + 1]);
    // Serial.print("channelNumber: "); Serial.println(channelNumber);
    // Serial.print("sampleRate: "); Serial.println(sampleRate);
    break;
  }
  
  /* {'O', 'g', 'g', 'S', 0x00, 0x04} */
  track.seekEnd();
  track.seekSet(track.curPosition() - sizeof(mp3DataBuffer));
  while(track.curPosition() >= sizeof(mp3DataBuffer)) {
    int16_t offset = -1;
    uint32_t readOffset = track.curPosition();
    track.read(mp3DataBuffer, sizeof(mp3DataBuffer));
    for (uint16_t i = 0; i < sizeof(mp3DataBuffer); i++) {
      if (mp3DataBuffer[i] == 'O') {
        bool doneForLoop = false;
        if (i + sizeof(header2) > sizeof(mp3DataBuffer)) {
          track.seekSet(readOffset + i);
          track.read(mp3DataBuffer, 14);
          i = 0;
          doneForLoop = true;
        }
        if (!memcmp(header2, &mp3DataBuffer[i], sizeof(header2))) {
          offset = i + 6;
          break;
        }
        if (doneForLoop) break;
      }
    }
    if (offset < 0) {
      track.seekSet(readOffset - sizeof(mp3DataBuffer));
      continue;
    }
    if ((uint16_t)(offset + 7) >= sizeof(mp3DataBuffer)) {
      track.seekSet(track.curPosition() - sizeof(mp3DataBuffer) + offset);
      track.read(mp3DataBuffer, 8);
      offset = 0;
    }
    sampleNumber = ((uint64_t)mp3DataBuffer[offset + 7]) << 56 | \
                   ((uint64_t)mp3DataBuffer[offset + 6]) << 48 | \
                   ((uint64_t)mp3DataBuffer[offset + 5]) << 40 | \
                   ((uint64_t)mp3DataBuffer[offset + 4]) << 32 | \
                   ((uint64_t)mp3DataBuffer[offset + 3]) << 24 | \
                   ((uint64_t)mp3DataBuffer[offset + 2]) << 16 | \
                   ((uint64_t)mp3DataBuffer[offset + 1]) << 8 | \
                   ((uint64_t)mp3DataBuffer[offset + 0]);
    // Serial.print("sampleNumber: "); Serial.println((uint32_t)sampleNumber);
    break;
  }
  duration = (uint16_t)(sampleNumber / channelNumber / sampleRate);
  Serial.print("duration: "); Serial.println(duration);
  track.seekSet(0);
}



//------------------------------------------------------------------------------
/**
 * \brief get the status of the VSdsp VU Meter
 *
 * \return responds with the current value of the SS_VU_ENABLE bit of the
 * SCI_STATUS register indicating if the VU meter is enabled.
 *
 * See data patches data sheet VU meter for details.
 * \warning This feature is only available with patches that support VU meter.
 */
int8_t vs1053::getVUmeter() {
  if(Mp3ReadRegister(SCI_STATUS) & SS_VU_ENABLE) {
    return 1;
  }
  return 0;
 }

//------------------------------------------------------------------------------
/**
 * \brief enable VSdsp VU Meter
 *
 * \param[in] enable when set will enable the VU meter
 *
 * Writes the SS_VU_ENABLE bit of the SCI_STATUS register to enable VU meter on
 * board to the VSdsp.
 *
 * See data patches data sheet VU meter for details.
 * \warning This feature is only available with patches that support VU meter.
 * \n The VU meter takes about 0.2MHz of processing power with 48 kHz samplerate.
 */
int8_t vs1053::setVUmeter(int8_t enable) {
  uint16_t MP3Status = Mp3ReadRegister(SCI_STATUS);

  if(enable) {
    Mp3WriteRegister(SCI_STATUS, MP3Status | SS_VU_ENABLE);
  } else {
    Mp3WriteRegister(SCI_STATUS, MP3Status & ~SS_VU_ENABLE);
  }
  return 1; // in future return if not available, if patch not applied.
 }

//------------------------------------------------------------------------------
/**
 * \brief get current measured VU Meter
 *
 * Returns the calculated peak sample values from both channels in 3 dB
 * increaments through. Where the high byte represent the left channel,
 * and the low bytes the right channel.
 *
 * Values from 0 to 31 are valid for both channels.
 *
 * \warning This feature is only available with patches that support VU meter.
 */
int16_t vs1053::getVUlevel() {
  return Mp3ReadRegister(SCI_AICTRL3);
}

// @}
// Audio_Information_Group

//------------------------------------------------------------------------------
/**
 * \brief Force bit rate
 *
 * \param[in] bitr new bit-rate
 *
 * Public method for forcing the percieved bit-rate to a desired value.
 * Useful if auto-detect failed
 */
void vs1053::setBitRate(uint16_t bitr){

  bitrate = bitr;
  return;
}

//------------------------------------------------------------------------------
/**
 * \brief Initialize the SPI for VS10xx use.
 *
 * Primative function to configure the SPI's BitOrder, DataMode and ClockDivider to that of
 * the current VX10xx.
 *
 * \warning This sets the rate fast for write, too fast for reading. In the case of a subsequent SPI.transfer that is reading back data followup with a SPI.setClockDivider(spi_Read_Rate); as not to get gibberish.
*/
void vs1053::spiInit(bool toWrite=true) {
  //Set SPI bus for write
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  if (toWrite) {
    SPI.setClockDivider(spi_Write_Rate);
  } else {
    SPI.setClockDivider(spi_Read_Rate);
  }
}

//------------------------------------------------------------------------------
/**
 * \brief Select Control Channel
 *
 * Primative function to configure the SPI's Mode and rate control to that of
 * the current VX10xx. Then select the VS10xx's Control Chip Select as per
 * defined by MP3_XCS.
 *
 * \warning This uses spiInit() which sets the rate fast for write, too fast for reading. In the case of a subsequent SPI.transfer that is reading back data followup with a SPI.setClockDivider(spi_Read_Rate); as not to get gibberish.
 */
void vs1053::cs_low(bool toWrite) {
  spiInit(toWrite);
  digitalWrite(MP3_XCS, LOW);
}

//------------------------------------------------------------------------------
/**
 * \brief Deselect Control Channel
 *
 * Primative function to Deselect the VS10xx's Control Chip Select as per
 * defined by MP3_XCS.
 */
void vs1053::cs_high() {
  digitalWrite(MP3_XCS, HIGH);
}

//------------------------------------------------------------------------------
/**
 * \brief Select Data Channel
 *
 * Primative function to configure the SPI's Mode and rate control to that of
 * the current VX10xx. Then select the VS10xx's Data Chip Select as per
 * defined by MP3_XDCS.
 *
 * \warning This uses spiInit() which sets the rate fast for write, too fast for reading. In the case of a subsequent SPI.transfer that is reading back data followup with a SPI.setClockDivider(spi_Read_Rate); as not to get gibberish.
 */
void vs1053::dcs_low(bool toWrite) {
  spiInit(toWrite);
  digitalWrite(MP3_XDCS, LOW);
}

//------------------------------------------------------------------------------
/**
 * \brief Deselect Data Channel
 *
 * Primative function to Deselect the VS10xx's Control Data Select as per
 * defined by MP3_XDCS.
 */
void vs1053::dcs_high() {
  digitalWrite(MP3_XDCS, HIGH);
}

//------------------------------------------------------------------------------
/**
 * \brief uint16_t Overload of vs1053::Mp3WriteRegister
 *
 * \param[in] address of the VSdsp's register to be written
 * \param[in] data to writen to the register
 *
 * Forces the input value into the Big Endian Corresponding positions of
 * Mp3WriteRegister as to be written to the addressed VSdsp's registers.
 */
void vs1053::Mp3WriteRegister(uint8_t address, uint16_t data) {
  union twobyte val = {.word = data};
  Mp3WriteRegister(address, val.byte[1], val.byte[0]);
}

//------------------------------------------------------------------------------
/**
 * \brief Write a value a VSDsp's register.
 *
 * \param[in] address of the VSdsp's register to be written
 * \param[in] highbyte to writen to the register
 * \param[in] lowbyte to writen to the register
 *
 * Primative function to suspend playing and directly communicate over the SPI
 * to the VSdsp's registers. Where the value write is Big Endian (MSB first).
 */
void vs1053::Mp3WriteRegister(uint8_t address, uint8_t msb, uint8_t lsb) {
  if (!digitalRead(MP3_RESET)) return;

  /* Pause data */
  if(playing_state == playback) {
    disableRefill();
  }

  while(!digitalRead(MP3_DREQ));
  cs_low();
  SPI.transfer(0x02); // Write instruction
  SPI.transfer(address);
  SPI.transfer(msb);
  SPI.transfer(lsb);
  cs_high();

  /* Resume data */
  if(playing_state == playback) {
    refill();
    enableRefill();
  }
}

//------------------------------------------------------------------------------
/**
 * \brief Read a VS10xx register
 *
 * \param[in] addressbyte of the VSdsp's register to be read
 * \return result read from the register
 *
 * Primative function to suspend playing and directly communicate over the SPI
 * to the VSdsp's registers.
 */
uint16_t vs1053::Mp3ReadRegister(uint8_t address){
  if(!digitalRead(MP3_RESET)) return 0;

  union twobyte val;
  /* Pause data */
  if(playing_state == playback) {
    disableRefill();
  }

  while(!digitalRead(MP3_DREQ)); 
  cs_low(false); 
  SPI.transfer(0x03); // Read instruction
  SPI.transfer(address);
  val.byte[1] = SPI.transfer(0xFF); // MSB
  val.byte[0] = SPI.transfer(0xFF); // LSB
  cs_high();

  /* Resume data */
  if(playing_state == playback) {
    refill();
    enableRefill();
  }
  return val.word;
}

//------------------------------------------------------------------------------
/**
 * \brief Read a VS10xx WRAM Location
 *
 * \param[in] address of the VSdsp's WRAM to be read
 * \return result read from the WRAM
 *
 * Function to communicate to the VSdsp's registers, indirectly accessing the WRAM.
 * As per data sheet the result is read back twice to verify. As it is not buffered.
 */
uint32_t vs1053::Mp3ReadWRAM(uint16_t address, bool is32bit) {
  if (!is32bit) {
    Mp3WriteRegister(SCI_WRAMADDR, address);
    return Mp3ReadRegister(SCI_WRAM);
  } else {
    Mp3WriteRegister(SCI_WRAMADDR, address + 1);
    uint16_t msb = Mp3ReadRegister(SCI_WRAM);
    Mp3WriteRegister(SCI_WRAMADDR, address);
    uint16_t lsb = Mp3ReadRegister(SCI_WRAM);
    uint16_t msb2 = Mp3ReadRegister(SCI_WRAM);
    if (lsb < 0x8000) {
      msb = msb2;
    }
    return ((uint32_t)msb << 16) | lsb;
  }
}

//------------------------------------------------------------------------------
/**
 * \brief Write a VS10xx WRAM Location
 *
 * \param[in] address of the VSdsp's WRAM to be read
 * \param[in] data written to the VSdsp's WRAM
 *
 * Function to communicate to the VSdsp's registers, indirectly accessing the WRAM.
 */
//Write the 16-bit value of a VS10xx WRAM location
void vs1053::Mp3WriteWRAM(uint16_t address, uint32_t data, bool is32bit) {
  Mp3WriteRegister(SCI_WRAMADDR, address);
  if (!is32bit) {
     Mp3WriteRegister(SCI_WRAM, data);
  } else {
    Mp3WriteRegister(SCI_WRAM, (uint16_t)(data & 0x0000FFFF));
    Mp3WriteRegister(SCI_WRAM, (uint16_t)(data >> 16));
  }
}

//------------------------------------------------------------------------------
/**
 * \brief Public interface of refill.
 *
 * Serves as a helper as to correspondingly run either the timer service or run
 * the refill() direclty, depending upon the configured means for refilling.
 */
void vs1053::available() {
#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
  timer.run();
#elif defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_Polled
  refill();
#endif
}

//------------------------------------------------------------------------------
/**
 * \brief Refill the VS10xx buffer with new data
 *
 * This the primative function to refilling the VSdsp's buffers. And is
 * typically called as an interrupt to the rising edge of the VS10xx's DREQ.
 * Where if the DREQ is indicating not full, it will read 32 bytes from the
 * filehandle's track and send them via SPI to the VSdsp's data stream buffer.
 * Repeating until the DREQ indicates it is full.
 *
 * When the filehandle's track indicates it is at the end of file. The track is
 * closed, the playing indicator is set to false, interrupts for refilling are
 * disabled and the VSdsp's data stream buffer is flushed appropiately.
 */
void vs1053::refill() {
#if PERF_MON_PIN != -1
  digitalWrite(PERF_MON_PIN, LOW);
#endif
#if DEBUG
  uint32_t position2 = position;
  bool iswork = false;
  cntIsr++;
#endif
  // Serial.println(F("filling"));

  while(digitalRead(MP3_DREQ)) {
#if DEBUG
    iswork = true;
#endif
    /* Read data */
    if (bufferOffset == sizeof(mp3DataBuffer)) {
      if(!track.read(mp3DataBuffer, sizeof(mp3DataBuffer))) {
        position = duration;
        playing_state = cancelling;
        /* track end */
        disableRefill();
        uint16_t data = Mp3ReadWRAM(para_endFillByte);
        fillEnd((uint8_t)(data & 0x00FF));
        cancelDecoding(false, (uint8_t)(data & 0x00FF));
        track.close();
        playing_state = ready;
        Serial.println(F("Track end"));
        //Oh no! There is no data left to read!
        //Time to exit
        break;
      }
#if DEBUG
      cntRead++;
#endif
      bufferOffset = 0;
    }
    
    if (playing_state == cancelling) {
      /* Cancelling when track is on going */
      disableRefill();
      cancelDecoding(true);
      uint16_t data = Mp3ReadWRAM(para_endFillByte);
      fillEnd((uint8_t)(data & 0x00FF));
      track.close();
      playing_state = ready;
      break;
    } else if (playing_state == skipping) {
      if (!isSkipping) {
        if (position > skipToPosition) {
          Serial.println(F("Rewind"));
          /* Rewind: cancel then start */
          cancelDecoding(true);
          uint16_t data = Mp3ReadWRAM(para_endFillByte);
          fillEnd((uint8_t)(data & 0x00FF));
          track.seekSet(0);
          bufferOffset = sizeof(mp3DataBuffer);
          Mp3WriteRegister(SCI_DECODE_TIME, 0); // Reset decode time
          Mp3WriteRegister(SCI_DECODE_TIME, 0);
          position = 0;
          continue;
        }
        /* Skipping */
        Mp3WriteRegister(SCI_VOL, 0xFE, 0xFE); // Mute
        Mp3WriteWRAM(para_playSpeed, SKIPPING_SPEED); // Speed up
        isSkipping = true;
        Serial.println(F("skipping start"));
      }
    }
    
    /* Feed 32 bytes */
    dcs_low(); //Select Data
    for(uint8_t i = 0; i < 32; i++) {
      SPI.transfer(mp3DataBuffer[bufferOffset + i]); 
    }
    bufferOffset += 32;
    dcs_high(); 
    
    /* Get position */
    if (bufferOffset == sizeof(mp3DataBuffer)) {
      cs_low(false); // Select control to read
      //SCI consists of instruction byte, address byte, and 16-bit data word.
      SPI.transfer(0x03);  //Read instruction
      SPI.transfer(SCI_DECODE_TIME);
      position = (((uint16_t)SPI.transfer(0xFF)) << 8) | SPI.transfer(0xFF); //Read the first byte
      cs_high(); //Deselect Control
    }
  }
  
  /* Check if skipping done */
  if (isSkipping && (position >= skipToPosition)) {
    Mp3WriteWRAM(para_playSpeed, 0x0001); // Normal speed
    Mp3WriteRegister(SCI_VOL, VolL, VolR); // Unmute
    isSkipping = false;
    Serial.println(F("skipping done"));
    playing_state = playback;
  }
  
#if DEBUG
  if (iswork) cntWork++;

  if(position2 != position) {
    Serial.print(F("position ")); Serial.println(position);
    Serial.print(F("cntIsr ")); Serial.println(cntIsr);
    Serial.print(F("cntWork ")); Serial.println(cntWork);
    Serial.print(F("cntRead ")); Serial.println(cntRead);
    Serial.print(F("rate ")); Serial.println(1.0 * cntWork / cntIsr * 100);
    cntIsr = 0;
    cntWork = 0;
    cntRead = 0;
  }
#endif
#if PERF_MON_PIN != -1
  digitalWrite(PERF_MON_PIN,HIGH);
#endif
}

//------------------------------------------------------------------------------
/**
 * \brief Play hardcoded MIDI file
 *
 * This the primative function to fill the VSdsp's buffers quicly. The intention
 * is to send a quick MIDI file of a single note on and off. This can be used 
 * responses to buttons and such. Where the MIDI file is short enough to be 
 * stored into an array that can be delivered via SPI to the VSdsp's data stream 
 * buffer. Waiting for DREQ every 32 bytes.
 */
void vs1053::SendSingleMIDInote() {

  if(!digitalRead(MP3_RESET))
    return;

  //cancel and store current state to restore after
  disableRefill();
  state_m prv_state = playing_state;
  playing_state = playMIDIbeep;
  
  // need to quickly purge the exiting formate of decoder.
  flush_cancel(none);

  // wait for VS1053 to be available.
  while(!digitalRead(MP3_DREQ)); 

#if !defined(USE_MP3_REFILL_MEANS) || USE_MP3_REFILL_MEANS == USE_MP3_INTx
  cli(); // allow transfer to occur with out interruption.
#endif

  dcs_low(); //Select Data
  for(uint8_t y = 0 ; y < sizeof(SingleMIDInoteFile) ; y++) { // sizeof(mp3DataBuffer)
    // Every 32 check if not ready for next buffer chunk.
    if ( !(y % 32) ) {
      while(!digitalRead(MP3_DREQ));
    }
    SPI.transfer( pgm_read_byte_near( &(SingleMIDInoteFile[y]))); // Send next byte
  }
  dcs_high(); //Deselect Data

#if !defined(USE_MP3_REFILL_MEANS) || USE_MP3_REFILL_MEANS == USE_MP3_INTx
  sei();  // renable interrupts for other processes
#endif

  flush_cancel(none); // need to quickly purge the exiting format of decoder.
  playing_state = prv_state;
  enableRefill();
}

//------------------------------------------------------------------------------
/**
 * \brief Enable the Interrupts for refill.
 *
 * Depending upon the means selected to request refill of the VSdsp's data
 * stream buffer, this routine will enable the corresponding service.
 */
void vs1053::enableRefill(bool isRecording) {
  if (isRecording) {
#if defined(OGG_REFILL_USING_TIMER)
    Timer1.setPeriod(OGG_REFILL_PERIOD);
    Timer1.attachInterrupt(oggRefill);
#endif
  } else {
#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_Timer1
    Timer1.attachInterrupt( refill );
#elif defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
    timer.enable(timerId_mp3);
#elif !defined(USE_MP3_REFILL_MEANS) || USE_MP3_REFILL_MEANS == USE_MP3_INTx
    attachInterrupt(MP3_DREQINT, refill, RISING);
#endif
  }
}

//------------------------------------------------------------------------------
/**
 * \brief Disable the Interrupts for refill.
 *
 * Depending upon the means selected to request refill of the VSdsp's data
 * stream buffer, this routine will disable the corresponding service.
 */
void vs1053::disableRefill(bool isRecording) {
  if (isRecording) {
#if defined(OGG_REFILL_USING_TIMER)
    Timer1.detachInterrupt();
#endif
  } else {
#if defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_Timer1
    Timer1.detachInterrupt();
#elif defined(USE_MP3_REFILL_MEANS) && USE_MP3_REFILL_MEANS == USE_MP3_SimpleTimer
    timer.disable(timerId_mp3);
#elif !defined(USE_MP3_REFILL_MEANS) || USE_MP3_REFILL_MEANS == USE_MP3_INTx
    detachInterrupt(MP3_DREQINT);
#endif
  }
}

//------------------------------------------------------------------------------
/**
 * \brief flush the VSdsp buffer and cancel
 *
 * \param[in] mode is an enumerated value of flush_m
 *
 * Typically called after a filehandlers' stream has been stopped, as to
 * gracefully flush any buffer contents still playing. Along with issueing a
 * SM_CANCEL to the VSdsp's SCI_MODE register.

 * - post - will flush vx10xx's 2K buffer after cancelled, typically with stopping a file, to have immediate affect.
 * - pre  - will flush buffer prior to issuing cancel, typically to allow completion of file
 * - both - will flush before and after issuing cancel
 * - none - will just issue cancel. Not sure if this should be used. Such as in skipTo().
 *
 * \note if cancel fails the vs10xx will be reset and initialized to current values.
 */
void vs1053::cancelDecoding(bool fillTrack, uint8_t fillingByte) {
  /* Cancel */
  Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) | SM_CANCEL));
  /* Wait cancel clear */
  bool isCancelled = false;
  bool getFilling = false;

  for (uint8_t i = 0; i < 64; i++) {
    while(!digitalRead(MP3_DREQ)); 
    if (fillTrack) {
      /* Read data */
      if (bufferOffset == sizeof(mp3DataBuffer)) {
        if(!track.read(mp3DataBuffer, sizeof(mp3DataBuffer))) {
          /* track end */
          if (!getFilling) {
            uint16_t data = Mp3ReadWRAM(para_endFillByte);
            fillingByte = (uint8_t)(data & 0x00FF);
          }
          memset(mp3DataBuffer, fillingByte, sizeof(mp3DataBuffer));
        }
        bufferOffset = 0;
      }
      dcs_low(); 
      for(uint8_t j = 0; j < 32; j++) {
        SPI.transfer(mp3DataBuffer[bufferOffset + j]); 
      }
      dcs_high(); 
      bufferOffset += 32;
    } else {
      dcs_low(); 
      for(uint8_t j = 0 ; j < 32; j++) {
        SPI.transfer(fillingByte); 
      }
      dcs_high(); 
    }

    isCancelled = !(Mp3ReadRegister(SCI_MODE) & SM_CANCEL);
    if (isCancelled) break;
  }
  
  if (!isCancelled) {
    Serial.println(F("Cancelling failed, reset!"));
    Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) | SM_RESET));
    delay(1);
    isPatched = false;
  }
}
 
void vs1053::fillEnd(uint8_t fillingByte) {
  /* Send 2052 bytes */
  dcs_low();
  for (uint16_t i = 0; i < 2052; i ++) {
    while(!digitalRead(MP3_DREQ));
    for (uint16_t j = 0; j < 32; j++) {
      SPI.transfer(fillingByte);
    }
  }
  dcs_high();
  // Serial.println(Mp3ReadRegister(SCI_HDAT0));
  // Serial.println(Mp3ReadRegister(SCI_HDAT1));
}
 
void vs1053::flush_cancel(flush_m mode) {
  uint16_t data = Mp3ReadWRAM(para_endFillByte);
  uint8_t endFillByte = data & 0x00FF;

  if((mode == post) || (mode == both)) {

    dcs_low(); //Select Data
    for(int y = 0 ; y < 2052 ; y++) {
      while(!digitalRead(MP3_DREQ)); // wait until DREQ is or goes high
      SPI.transfer(endFillByte); // Send SPI byte
    }
    dcs_high(); //Deselect Data
  }

  for (int n = 0; n < 64 ; n++)
  {
//    Mp3WriteRegister(SCI_MODE, SM_LINE1 | SM_SDINEW | SM_CANCEL); // old way of SCI_MODE WRITE.
    Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) | SM_CANCEL));

    dcs_low(); //Select Data
    for(int y = 0 ; y < 32 ; y++) {
      while(!digitalRead(MP3_DREQ)); // wait until DREQ is or goes high
      SPI.transfer(endFillByte); // Send SPI byte
    }
    dcs_high(); //Deselect Data

    int cancel = Mp3ReadRegister(SCI_MODE) & SM_CANCEL;
    if(cancel == 0) {
      // Cancel has succeeded.
      if((mode == pre) || (mode == both)) {
        dcs_low(); //Select Data
        for(int y = 0 ; y < 2052 ; y++) {
          while(!digitalRead(MP3_DREQ)); // wait until DREQ is or goes high
          SPI.transfer(endFillByte); // Send SPI byte
        }
        dcs_high(); //Deselect Data
      }
      return;
    }
  }
  // Cancel has not succeeded.
  //Serial.println(F("Warning: VS10XX chip did not cancel, reseting chip!"));
//  Mp3WriteRegister(SCI_MODE, SM_LINE1 | SM_SDINEW | SM_RESET); // old way of SCI_MODE WRITE.
  Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) | SM_RESET));  // software reset. but vs_init will HW reset anyways.
  isPatched = false;
//  vs_init(); // perform hardware reset followed by re-initializing.
  //vs_init(); // however, vs1053::begin() is member function that does not exist statically.
}


//------------------------------------------------------------------------------
/**
 * \brief Initially load ADMixer patch and configure line/mic mode
 *
 * \param[out] fileName pointer of a char array (aka string), contianing the filename
 *
 * Loads a patch file of Analog to Digital Mixer. Current available options are
 * as follows:
 * - "admxster.053" Takes both ADC channels and routes them to left and right outputs.
 * - "admxswap.053" Swaps both Left and Right ADC channels and routes them to left and right outputs.
 * - "admxleft.053" MIC/LINE1 routed to both left and right output
 * - "admxrght.053" LINE2 routed to both left and right output
 * - "admxmono.053" mono version mixes both left and right inputs and plays them using both left and right outputs.
 *
 * And subsequently returns the following result codes.
 *
 * \return Any Value other than zero indicates a problem occured.
 * - 0 indicates that upload was successful.
 * - 1 indicates the upload can not be performed while currently streaming music.
 * - 2 indicates that desired file was not found.
 * - 3 indicates that the VSdsp is in reset.
 */
uint8_t vs1053::ADMixerLoad(char* fileName){
  if(isBusy()) return 1;

  playing_state = loading;
  if(VSLoadUserCode(fileName)) {
    playing_state = ready;
    return 2;
    // Serial.print(F("Error: ")); Serial.print(fileName); Serial.println(F(", file not found, skipping."));
  }

  // Set Input Mode to either Line1 or Microphone.
#if defined(VS_LINE1_MODE)
    Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) | SM_LINE1));
#else
    Mp3WriteRegister(SCI_MODE, (Mp3ReadRegister(SCI_MODE) & ~SM_LINE1));
#endif
  playing_state = ready;
  return 0;
}

//------------------------------------------------------------------------------
/**
 * \brief Set ADMixer's attenuation of input to between -3 and -31 dB otherwise
 * disable.
 *
 * \param[in] ADM_volume -3 through -31 dB of attentuation.
 *
 * Will range check the requested value and for values out of range the VSdsp's
 * ADMixer will be disabled. While valid ranges will write to VSdsp's current
 * operating volume and enable the the ADMixer.
 *
 * \warning If file patch not applied this call will lock up the VS10xx.
 * need to add interlock to avoid.
 */
void vs1053::ADMixerVol(int8_t ADM_volume){
  union twobyte MP3AIADDR;
  union twobyte MP3AICTRL0;

  MP3AIADDR.word = Mp3ReadRegister(SCI_AIADDR);

  if((ADM_volume > -3) || (-31 > ADM_volume)) {
    // Disable Mixer Patch
    MP3AIADDR.word = 0x0F01;
    Mp3WriteRegister(SCI_AIADDR, MP3AIADDR.word);
  } else {
    // Set Volume
    //MP3AICTRL0.word = Mp3ReadRegister(SCI_AICTRL0);
    MP3AICTRL0.byte[1] = (uint8_t) ADM_volume; // upper byte appears to have no affect
    MP3AICTRL0.byte[0] = (uint8_t) ADM_volume;
    Mp3WriteRegister(SCI_AICTRL0, MP3AICTRL0.word);

    // Enable Mixer Patch
    MP3AIADDR.word = 0x0F00;
    Mp3WriteRegister(SCI_AIADDR, MP3AIADDR.word);
  }
}


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
// Global Function

/**
 * \brief chomp non printable characters out of string.
 *
 * \param[out] s pointer of a char array (aka string)
 *
 * \return char array (aka string) with out whitespaces
 */
char* strip_nonalpha_inplace(char *s) {
  for ( ; *s && !isalpha(*s); ++s)
    ; // skip leading non-alpha chars
  if(*s == '\0')
    return s; // there are no alpha characters

  char *tail = s + strlen(s);
  for ( ; !isalpha(*tail); --tail)
    ; // skip trailing non-alpha chars
  *++tail = '\0'; // truncate after the last alpha

  return s;
}

/**
 * \brief is the filename music
 *
 * \param[in] filename inspects the end of the filename to be of the extension types
 *            that VS10xx can decode.
 *
 * \return boolean true indicating that it is music
 */
format_m getTrackFormat(char *filename) {
  uint8_t len = strlen(filename);
  
  if (strstr(strlwr(filename + (len - 4)), ".mp3")) return mp3;
  else if (strstr(strlwr(filename + (len - 4)), ".aac")) return aac;
  else if (strstr(strlwr(filename + (len - 4)), ".wma")) return wma;
  else if (strstr(strlwr(filename + (len - 4)), ".wav")) return wav;
  else if (strstr(strlwr(filename + (len - 4)), ".fla")) return fla;
  else if (strstr(strlwr(filename + (len - 4)), ".mid")) return mid;
  else if (strstr(strlwr(filename + (len - 4)), ".ogg")) return ogg;
  else return unknownFormat;
}

bool isFormat(format_m targetFormat, char *filename) {
  format_m format = getTrackFormat(filename);
  if ((format == targetFormat) || ((targetFormat == supportedFormat) && (format != unknownFormat)))
    return true;
  return false;
}
