/*
 * RF24.c
 *
 *  Created on: Apr 28, 2019
 *      Author: jackxu
 */

#include <stdint.h>
#include <stdbool.h>

#include "common.h"
#include "nrf24l01.h"
#include "RF24.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define LOW   0
#define HIGH  1

// M4_SPI
#define RF24_LPSPI_MASTER_IRQN (LPSPI0_IRQn)
#define RF24_LPSPI_MASTER_CLOCK_NAME (kCLOCK_Lpspi0)
#define RF24_LPSPI_MASTER_CLOCK_SOURCE (kCLOCK_IpSrcFircAsync)

#define RF24_MAX_TRANSFER_SIZE      64U         /*! Transfer dataSize */
#define RF24_SPI_TRANSFER_BAUDRATE  2000000U /*! Transfer baudrate - RF modules support 10 Mhz SPI bus speed */
#define MAX_SPI_MSG_SIZE_IN_BYTE    32U

// Macro - Helper
#define rf24_max(a,b) (a>b?a:b)
#define rf24_min(a,b) (a<b?a:b)
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define _BV(bit) (1<<(bit))
/*******************************************************************************
 * typedef
 ******************************************************************************/
typedef struct{
  // M4_SPI
  pin_t     ce_pin;
  lpspi_t   spi;
  uint8_t   addr_width; /**< The address width to use - 3,4 or 5 bytes. */
  bool      dynamic_payloads_enabled; /**< Whether dynamic payloads are enabled. */
  uint8_t   pipe0_reading_address[5]; /**< Last address set on pipe 0 for reading. */
  uint32_t  txDelay;
  uint8_t   payload_size;
  bool      configed;
  bool      p_variant;
  // pre-allocated storage for spi msgs
  uint8_t   buf_rx[MAX_SPI_MSG_SIZE_IN_BYTE + 1];
  uint8_t   buf_tx[MAX_SPI_MSG_SIZE_IN_BYTE + 1];
}RF24_S;

/*******************************************************************************
 * private variables
 ******************************************************************************/
static RF24_S m_rf24;
static const uint8_t child_pipe[6] =
{
  RX_ADDR_P0, RX_ADDR_P1, RX_ADDR_P2, RX_ADDR_P3, RX_ADDR_P4, RX_ADDR_P5
};
static const uint8_t child_payload_size[6] =
{
  RX_PW_P0, RX_PW_P1, RX_PW_P2, RX_PW_P3, RX_PW_P4, RX_PW_P5
};
static const uint8_t child_pipe_enable[6] =
{
  ERX_P0, ERX_P1, ERX_P2, ERX_P3, ERX_P4, ERX_P5
};
/*******************************************************************************
 * private function prototypes
 ******************************************************************************/
//static void delay_ms(uint16_t time);
static void ce(bool level);
static void init_spi_rf24(void);
static uint8_t spi_transfer(uint8_t data);
static uint8_t write_register_buf(uint8_t reg, const uint8_t* buf, uint8_t len);
static uint8_t write_register(uint8_t reg, uint8_t value);
static uint8_t read_register(uint8_t reg);
static uint8_t spiTrans(uint8_t cmd);
static uint8_t flush_rx(void);
static uint8_t flush_tx(void);
static uint8_t get_status(void);
static void setChannel(uint8_t channel);
static uint8_t available(uint8_t* pipe_num);
static uint8_t read_payload(void* buf, uint8_t data_len);
static void powerUp(void);
static void closeReadingPipe( uint8_t pipe );
static inline void RF24_LPSPI_RTOS_transfer(void);
/*******************************************************************************
 * Private Functions
 ******************************************************************************/
static inline void RF24_LPSPI_RTOS_setup(uint8_t len){
  uint8_t size = rf24_min(len, MAX_SPI_MSG_SIZE_IN_BYTE);
  memset(m_rf24.spi.spi0_transfer.txData, 0, size+1);
  memset(m_rf24.spi.spi0_transfer.rxData, 0, size+1);
  m_rf24.spi.spi0_transfer.dataSize = sizeof(uint8_t);
}

static inline void RF24_LPSPI_RTOS_transfer(void){
  LPSPI_RTOS_Transfer(&(m_rf24.spi.spi0_handle), &(m_rf24.spi.spi0_transfer));
}

static uint8_t spi_transfer(uint8_t data){
  uint8_t result = 0;
  RF24_LPSPI_RTOS_setup(sizeof(uint8_t));
  m_rf24.buf_tx[0] = data;
  RF24_LPSPI_RTOS_transfer();
  result = m_rf24.buf_rx[0];
  return result;
}

//static void delay_ms(uint16_t time){
// for (uint8_t i = 0; i < SystemCoreClock / 1000U*time;)
// {
//     i++;
// }
//}

static void ce(bool level)
{
  GPIO_PinWrite(m_rf24.ce_pin.port, m_rf24.ce_pin.pin, level);
}

static void init_spi_rf24(void){
  // Minimum ideal SPI bus speed is 2x data rate
  // If we assume 2Mbs data rate and 16Mhz clock, a
  // divider of 4 is the minimum we want.
  // CLK:BUS 8Mhz:2Mhz, 16Mhz:4Mhz, or 20Mhz:5Mhz

   uint32_t sourceClock = kCLOCK_Lpspi0;

   /*LPSPI master init*/
   //initialize the SPI0 configuration
  LPSPI_MasterGetDefaultConfig(&m_rf24.spi.spi0_master_config);
   m_rf24.spi.spi0_master_config.pcsActiveHighOrLow = kLPSPI_PcsActiveHigh; //because it is csn
   m_rf24.spi.spi0_master_config.baudRate = RF24_SPI_TRANSFER_BAUDRATE;
   m_rf24.spi.spi0_master_config.pcsToSckDelayInNanoSec = 1000000000 / m_rf24.spi.spi0_master_config.baudRate * 2;
   m_rf24.spi.spi0_master_config.lastSckToPcsDelayInNanoSec = 1000000000 / m_rf24.spi.spi0_master_config.baudRate * 2;
   m_rf24.spi.spi0_master_config.betweenTransferDelayInNanoSec = 1000000000 / m_rf24.spi.spi0_master_config.baudRate * 2;
   m_rf24.spi.spi0_master_config.whichPcs = kLPSPI_Pcs0;
   m_rf24.spi.spi0_master_config.direction = kLPSPI_MsbFirst;
   m_rf24.spi.spi0_master_config.pinCfg = kLPSPI_SdiInSdoOut;
//   masterConfig.cpol = kLPSPI_ClockPolarityActiveHigh;
//   masterConfig.cpha = kLPSPI_ClockPhaseFirstEdge;
   LPSPI_RTOS_Init(&(m_rf24.spi.spi0_handle), LPSPI0, &(m_rf24.spi.spi0_master_config), sourceClock);
}

static uint8_t write_register_buf(uint8_t reg, const uint8_t* buf, uint8_t len)
{
  uint8_t status;
  uint8_t* buf_ptr = m_rf24.buf_tx;
  RF24_LPSPI_RTOS_setup(len);
  buf_ptr[0] = W_REGISTER | ( REGISTER_MASK & reg );
  while ( len-- )
  {
    buf_ptr++;
    *buf_ptr = (uint8_t)(*buf);
    buf++;
  }
  // - Send
  RF24_LPSPI_RTOS_transfer();
  status = m_rf24.buf_rx[0];
  return status;
}

static uint8_t write_register(uint8_t reg, uint8_t value)
{
  uint8_t status = write_register_buf(reg, &value, 1);
  return status;
}

static uint8_t read_register(uint8_t reg)
{
  uint8_t result;
  uint8_t* buf_ptr = m_rf24.buf_tx;
  RF24_LPSPI_RTOS_setup(sizeof(uint8_t));
  buf_ptr[0] = W_REGISTER | ( REGISTER_MASK & reg );
  buf_ptr[1] = 0xff;
  // - Send
  RF24_LPSPI_RTOS_transfer();
  result = m_rf24.buf_rx[0];
  return result;
}

static uint8_t spiTrans(uint8_t cmd)
{
  return spi_transfer( cmd );
}

static uint8_t flush_rx(void)
{
  return spiTrans( FLUSH_RX );
}


static uint8_t flush_tx(void)
{
  return spiTrans( FLUSH_TX );
}

static uint8_t get_status(void)
{
  return spiTrans( RF24_NOP );
}

static void setChannel(uint8_t channel)
{
  const uint8_t max_channel = 125;
  write_register(RF_CH,rf24_min(channel,max_channel));
}

static uint8_t available(uint8_t* pipe_num)
{
  if (!( read_register(FIFO_STATUS) & _BV(RX_EMPTY) )){

    // If the caller wants the pipe number, include that
    if ( pipe_num ){
	  uint8_t status = get_status();
      *pipe_num = ( status >> RX_P_NO ) & 0x07;
  	}
  	return 1;
  }
  return 0;
}

////////////////////- READ -////////////
static uint8_t read_payload(void* buf, uint8_t data_len)
{
  uint8_t status;
  uint8_t* current = (uint8_t*)(buf);

  if(data_len > m_rf24.payload_size) data_len = m_rf24.payload_size;
  uint8_t blank_len = (m_rf24.dynamic_payloads_enabled) ? 0 : (m_rf24.payload_size - data_len);
  
  /*
  uint8_t result;
  uint8_t* buf_ptr = m_rf24.buf_tx[0];
  RF24_LPSPI_RTOS_setupr(sizeof(uint8_t));
  buf_ptr[0] = W_REGISTER | ( REGISTER_MASK & reg );
  buf_ptr[1] = 0xff;
  // - Send
  RF24_LPSPI_RTOS_transfer();
  result = m_rf24.buf_rx[0];
  */
	uint8_t * prx = m_rf24.buf_rx;
	uint8_t * ptx = m_rf24.buf_tx;

  uint8_t size;
  size = data_len + blank_len + 1; // Add register value to transmit buffer

  RF24_LPSPI_RTOS_setup(size);

	*ptx++ =  R_RX_PAYLOAD;
	while(--size) 
		*ptx++ = RF24_NOP;

	RF24_LPSPI_RTOS_transfer();
	
	status = *prx++; // 1st byte is status	
    
  if (data_len > 0) {
    while ( --data_len ) // Decrement before to skip 1st status byte
        *current++ = *prx++;
  
    *current = *prx;
  }

  return status;
}


static void powerUp(void)
{
   uint8_t cfg = read_register(NRF_CONFIG);

   // if not powered up then power up and wait for the radio to initialize
   if (!(cfg & _BV(PWR_UP))){
      write_register(NRF_CONFIG, cfg | _BV(PWR_UP));

      // For nRF24L01+ to go from power down mode to TX or RX mode it must first pass through stand-by mode.
    // There must be a delay of Tpd2stby (see Table 16.) after the nRF24L01+ leaves power down mode before
    // the CEis set high. - Tpd2stby can be up to 5ms per the 1.0 datasheet
    //  delay_ms(5U);
   }
}


static void closeReadingPipe( uint8_t pipe )
{
  write_register(EN_RXADDR,read_register(EN_RXADDR) & ~_BV(pgm_read_byte(&child_pipe_enable[pipe])));
}

/*******************************************************************************
 * Public Functions
 ******************************************************************************/
void RF24_toggle_features(void)
{
  spi_transfer( ACTIVATE );
  spi_transfer( 0x73 );
}

void RF24_config(pin_t* _cepin)
{
  memset(&m_rf24, 0, sizeof(m_rf24));
  m_rf24.configed = true; //set_flag to true
  m_rf24.ce_pin = *_cepin;
  m_rf24.addr_width = (5);
  m_rf24.dynamic_payloads_enabled = (false);
  m_rf24.payload_size = (MAX_SPI_MSG_SIZE_IN_BYTE);
  // link tx/rx buffer to a static array, dont use malloc
  m_rf24.spi.spi0_transfer.txData = (m_rf24.buf_tx);
  m_rf24.spi.spi0_transfer.rxData = (m_rf24.buf_rx);
  m_rf24.spi.spi0_transfer.configFlags = kLPSPI_Pcs0 | kLPSPI_MasterPcsContinuous;
}

void RF24_setPayloadSize(uint8_t size)
{
  m_rf24.payload_size = rf24_min(size, MAX_SPI_MSG_SIZE_IN_BYTE);
}

bool RF24_isChipConnected(void)
{
  uint8_t setup = read_register(SETUP_AW);
  if(setup >= 1 && setup <= 3)
  {
    return true;
  }

  return false;
}

uint8_t RF24_getChannel(void)
{
  return read_register(RF_CH);
}

void RF24_setRetries(uint8_t delay, uint8_t count)
{
 write_register(SETUP_RETR,(delay&0xf)<<ARD | (count&0xf)<<ARC);
}

void RF24_powerDown(void)
{
  ce(LOW); // Guarantee CE is low on powerDown
  write_register(NRF_CONFIG,read_register(NRF_CONFIG) & ~_BV(PWR_UP));
}

//Power up now. Radio will not power down unless instructed by MCU for config changes etc.
void RF24_powerUp(void)
{
   uint8_t cfg = read_register(NRF_CONFIG);

   // if not powered up then power up and wait for the radio to initialize
   if (!(cfg & _BV(PWR_UP))){
      write_register(NRF_CONFIG, cfg | _BV(PWR_UP));

      // For nRF24L01+ to go from power down mode to TX or RX mode it must first pass through stand-by mode.
	  // There must be a delay of Tpd2stby (see Table 16.) after the nRF24L01+ leaves power down mode before
	  // the CEis set high. - Tpd2stby can be up to 5ms per the 1.0 datasheet
      //delay_ms(5U);
   }
}

void RF24_enableAckPayload(void)
{
  //
  // enable ack payload and dynamic payload features
  //

    write_register(FEATURE,read_register(FEATURE) | _BV(EN_ACK_PAY) | _BV(EN_DPL) );
  //
  // Enable dynamic payload on pipes 0 & 1
  //

  write_register(DYNPD,read_register(DYNPD) | _BV(DPL_P1) | _BV(DPL_P0));
  m_rf24.dynamic_payloads_enabled = true;
}


void RF24_openReadingPipe(uint8_t child, const uint8_t *address)
{
  // If this is pipe 0, cache the address.  This is needed because
  // openWritingPipe() will overwrite the pipe 0 address, so
  // startListening() will have to restore it.
  if (child == 0){
    memcpy(m_rf24.pipe0_reading_address,&address,m_rf24.addr_width);
  }

  if (child <= 6)
  {
    // For pipes 2-5, only write the LSB
    write_register_buf(pgm_read_byte(&child_pipe[child]), (address), ((child < 2)?m_rf24.addr_width:1));
    write_register(pgm_read_byte(&child_payload_size[child]), m_rf24.payload_size);

    // Note it would be more efficient to set all of the bits for all open
    // pipes at once.  However, I thought it would make the calling code
    // more simple to do it this way.
    write_register(EN_RXADDR, read_register(EN_RXADDR) | _BV(pgm_read_byte(&child_pipe_enable[child])));
  }
}

void RF24_setPALevel(uint8_t level)
{

  uint8_t setup = read_register(RF_SETUP) & 0xF8;

  if(level > 3){  						// If invalid level, go to max PA
	  level = (RF24_PA_MAX << 1) + 1;		// +1 to support the SI24R1 chip extra bit
  }else{
	  level = (level << 1) + 1;	 		// Else set level as requested
  }


  write_register( RF_SETUP, setup |= level ) ;	// Write it to the chip
}

void RF24_startListening(void)
{
  powerUp();
  write_register(NRF_CONFIG, read_register(NRF_CONFIG) | _BV(PRIM_RX));
  write_register(NRF_STATUS, _BV(RX_DR) | _BV(TX_DS) | _BV(MAX_RT) );
  ce(HIGH);
  // Restore the pipe0 adddress, if exists
  if (m_rf24.pipe0_reading_address[0] > 0){
    write_register_buf(RX_ADDR_P0, m_rf24.pipe0_reading_address, m_rf24.addr_width);
  }else{
	closeReadingPipe(0);
  }

  // Flush buffers
  //flush_rx();
  if(read_register(FEATURE) & _BV(EN_ACK_PAY)){
	flush_tx();
  }

  // Go!
  //delayMicroseconds(100);
}

bool RF24_available(void)
{
  return available(NULL);
}


/****************************************************************************/
bool RF24_setDataRate(rf24_datarate_e speed)
{
  bool result = false;
  uint8_t setup = read_register(RF_SETUP) ;

  // HIGH and LOW '00' is 1Mbs - our default
  setup &= ~(_BV(RF_DR_LOW) | _BV(RF_DR_HIGH)) ;

  m_rf24.txDelay=250;

  if( speed == RF24_250KBPS )
  {
    // Must set the RF_DR_LOW to 1; RF_DR_HIGH (used to be RF_DR) is already 0
    // Making it '10'.
    setup |= _BV( RF_DR_LOW ) ;
    m_rf24.txDelay=450;

  }
  else
  {
    // Set 2Mbs, RF_DR (RF_DR_HIGH) is set 1
    // Making it '01'
    if ( speed == RF24_2MBPS )
    {
      setup |= _BV(RF_DR_HIGH);
      m_rf24.txDelay=190;
    }
  }
  write_register(RF_SETUP,setup);

  // Verify our result
  if ( read_register(RF_SETUP) == setup )
  {
    result = true;
  }
  return result;
}


RF24_INIT_STATUS_E RF24_init(void)
{
  if(m_rf24.configed)
  {
    uint8_t setup=0;
    /* Define the init structure for the output LED pin*/
    gpio_pin_config_t gpio_out_config = {
        kGPIO_DigitalOutput, 0,
    };
    /* Init output CE GPIO. */
    GPIO_PinInit(m_rf24.ce_pin.port, m_rf24.ce_pin.pin, &gpio_out_config);

    init_spi_rf24();

    ce(LOW);
    
    // Delay 5ms (for the chip to settle)
   // delay_ms( 5U ) ;

    // Reset NRF_CONFIG and enable 16-bit CRC.
    write_register( NRF_CONFIG, 0x0C ) ;

    // Set 1500uS (minimum for 32B payload in ESB@250KBPS) timeouts, to make testing a little easier
    // WARNING: If this is ever lowered, either 250KBS mode with AA is broken or maximum packet
    // sizes must never be used. See documentation for a more complete explanation.
    RF24_setRetries(5,15);

    // check for connected module and if this is a p nRF24l01 variant
    if(RF24_setDataRate( RF24_250KBPS ) )
    {
      m_rf24.p_variant = true ;
    }
    setup = read_register(RF_SETUP);
    /*if( setup == 0b00001110 )     // register default for nRF24L01P
    {
      p_variant = true ;
    }*/
    
    // Then set the data rate to the slowest (and most reliable) speed supported by all
    // hardware.
    RF24_setDataRate( RF24_1MBPS ) ;

    // Initialize CRC and request 2-byte (16bit) CRC
    //setCRCLength( RF24_CRC_16 ) ;

    // Disable dynamic payloads, to match dynamic_payloads_enabled setting - Reset value is 0
    RF24_toggle_features();
    write_register(FEATURE,0 );
    write_register(DYNPD,0);
    m_rf24.dynamic_payloads_enabled = false;

    // Reset current status
    // Notice reset and flush is the last thing we do
    write_register(NRF_STATUS,_BV(RX_DR) | _BV(TX_DS) | _BV(MAX_RT) );

    // Set up default configuration.  Callers can always change it later.
    // This channel should be universally safe and not bleed over into adjacent
    // spectrum.
    setChannel(76);

    // Flush buffers
    flush_rx();
    flush_tx();

    powerUp(); //Power up by default when begin() is called

    // Enable PTX, do not write CE high so radio will remain in standby I mode ( 130us max to transition to RX or TX instead of 1500us from powerUp )
    // PTX should use only 22uA of power
    write_register(NRF_CONFIG, ( read_register(NRF_CONFIG) ) & ~_BV(PRIM_RX) );

    // if setup is 0 or ff then there was no response from module
    return (( setup != 0 && setup != 0xff )?RF24_INIT_STATUS_SUCCESS:RF24_INIT_STATUS_SETUP_ERR);
  }
  else
  {
    return RF24_INIT_STATUS_CONFIG_ERR;
  }
}


void RF24_read( void* buf, uint8_t len ){

  // Fetch the payload
  read_payload( buf, len );

  //Clear the two possible interrupt flags with one command
  write_register(NRF_STATUS,_BV(RX_DR) | _BV(MAX_RT) | _BV(TX_DS) );

}
