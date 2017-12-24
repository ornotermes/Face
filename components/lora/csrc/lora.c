// Copyright (c) Sandeep Mistry. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
// Reorganised to run under the ESP32 OLED LoRA board by Heltec by Simon Waite.

#include <lora.h>
#include <esp_log.h>

// registers
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_LNA                  0x0c
#define REG_FIFO_ADDR_PTR        0x0d
#define REG_FIFO_TX_BASE_ADDR    0x0e
#define REG_FIFO_RX_BASE_ADDR    0x0f
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1a
#define REG_MODEM_CONFIG_1       0x1d
#define REG_MODEM_CONFIG_2       0x1e
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_RSSI_WIDEBAND        0x2c
#define REG_DETECTION_OPTIMIZE   0x31
#define REG_DETECTION_THRESHOLD  0x37
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42

// modes
#define MODE_LONG_RANGE_MODE     0x80
#define MODE_SLEEP               0x00
#define MODE_STDBY               0x01
#define MODE_TX                  0x03
#define MODE_RX_CONTINUOUS       0x05
#define MODE_RX_SINGLE           0x06

// PA config
#define PA_BOOST                 0x80

// IRQ masks
#define IRQ_TX_DONE_MASK           0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK           0x40

#define MAX_PKT_LENGTH           255

//LoRaClass::LoRaClass() :
LoRa lora;
const char *TAG = "lora";

void lora_init()
{
  ESP_LOGD(TAG,"lora_init");
  //TODO: FIXME: _spiSettings(8E6, MSBFIRST, SPI_MODE0),
  lora.ss=LORA_DEFAULT_SS_PIN ;
  lora.reset=LORA_DEFAULT_RESET_PIN;
  lora.dio0=LORA_DEFAULT_DIO0_PIN;
  lora.frequency=0;
  lora.packetIndex=0;
  lora.implicitHeaderMode=0;
  lora_onReceive(NULL)
  // overide Stream timeout value
  //setTimeout(0);
}

#define PIN_NUM_MISO (19)
#define PIN_NUM_MOSI (27)
#define PIN_NUM_CLK  (5)
#define PIN_NUM_SS   LORA_DEFAULT_SS_PIN

int lora_begin(long frequency)
{
    ESP_LOGD(TAG,"lora_begin %ld",frequency);

  // set things up...
  //void begin(int8_t sck=-1, int8_t miso=-1, int8_t mosi=-1, int8_t ss=-1);
  //SPI.begin(5, 19, 27, 18);

  lora.buscfg={
        .miso_io_num=PIN_NUM_MISO,
        .mosi_io_num=PIN_NUM_MOSI,
        .sclk_io_num=PIN_NUM_CLK,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1
    };
    //Initialize the SPI bus
      ESP_LOGD(TAG,"lora_begin - init SPI");

    esp_err_t ret=spi_bus_initialize(HSPI_HOST, &lora.buscfg, 1);
    assert(ret==ESP_OK);
    //Attach the radio to the SPI bus
    ret=spi_bus_add_device(HSPI_HOST, &lora.devcfg, &lora.spi);
    assert(ret==ESP_OK);
/* ARDUINO STUFF
  // setup pins
  pinMode(lora.ss, OUTPUT);
  // set SS high
  digitalWrite(_ss, HIGH);

  if (lora.reset != -1) {
    pinMode(_reset, OUTPUT);

    // perform reset
    digitalWrite(_reset, LOW);
    delay(10);
    digitalWrite(_reset, HIGH);
    delay(10);
  }
*/
  // start SPI
  // ARDUINO STUFF: SPI.begin();

  // check version
  uint8_t version = lora_readRegister(REG_VERSION);
  if (version != 0x12) {
    return 0;
  }

  // put in sleep mode
  lora_sleep();

  // set frequency
  lora_setFrequency(frequency);

  // set base addresses
  lora_writeRegister(REG_FIFO_TX_BASE_ADDR, 0);
  lora_writeRegister(REG_FIFO_RX_BASE_ADDR, 0);

  // set LNA boost
  lora_writeRegister(REG_LNA, readRegister(REG_LNA) | 0x03);

  // set auto AGC
  lora_writeRegister(REG_MODEM_CONFIG_3, 0x04);

  // set output power to 17 dBm
  lora_setTxPower_default(17);

  // put in standby mode
  lora_idle();

  return 1;
}

void lora_end()
{
  // put in sleep mode
  lora_sleep();

  // stop SPI
  //ARDUINO STUFF SPI.end();
}

int lora_beginPacket(int implicitHeader)
{
  // put in standby mode
  lora_idle();

  if (implicitHeader) {
    lora_implicitHeaderMode();
  } else {
    lora_explicitHeaderMode();
  }

  // reset FIFO address and paload length
  lora_writeRegister(REG_FIFO_ADDR_PTR, 0);
  lora_writeRegister(REG_PAYLOAD_LENGTH, 0);

  return 1;
}

int lora_endPacket()
{
  // put in TX mode
  lora_writeRegister(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_TX);

  // wait for TX done
  while ((readRegister(REG_IRQ_FLAGS) & IRQ_TX_DONE_MASK) == 0) {
    yield();
  }

  // clear IRQ's
  lora_writeRegister(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);

  return 1;
}

int lora_parsePacket(int size)
{
  int packetLength = 0;
  int irqFlags = readRegister(REG_IRQ_FLAGS);

  if (size > 0) {
    lora_implicitHeaderMode();

    writeRegister(REG_PAYLOAD_LENGTH, size & 0xff);
  } else {
    lora_explicitHeaderMode();
  }

  // clear IRQ's
  writeRegister(REG_IRQ_FLAGS, irqFlags);

  if ((irqFlags & IRQ_RX_DONE_MASK) && (irqFlags & IRQ_PAYLOAD_CRC_ERROR_MASK) == 0) {
    // received a packet
    lora.packetIndex = 0;

    // read packet length
    if (lora.implicitHeaderMode) {
      packetLength = readRegister(REG_PAYLOAD_LENGTH);
    } else {
      packetLength = readRegister(REG_RX_NB_BYTES);
    }

    // set FIFO address to current RX address
    lora_writeRegister(REG_FIFO_ADDR_PTR, lora_readRegister(REG_FIFO_RX_CURRENT_ADDR));

    // put in standby mode
    idle();
  } else if (lora_readRegister(REG_OP_MODE) != (MODE_LONG_RANGE_MODE | MODE_RX_SINGLE)) {
    // not currently in RX mode

    // reset FIFO address
    lora_writeRegister(REG_FIFO_ADDR_PTR, 0);

    // put in single RX mode
    lora_writeRegister(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_SINGLE);
  }

  return packetLength;
}

int lora_packetRssi()
{
  return (lora_readRegister(REG_PKT_RSSI_VALUE) - (lora.frequency < 868E6 ? 164 : 157));
}

float lora_packetSnr()
{
  return ((int8_t)lora_readRegister(REG_PKT_SNR_VALUE)) * 0.25;
}

size_t lora_write_byte(uint8_t byte)
{
  return lora_write_buffer(&byte, sizeof(byte));
}

size_t lora_write_buffer(const uint8_t *buffer, size_t size)
{
  int currentLength = readRegister(REG_PAYLOAD_LENGTH);

  // check size
  if ((currentLength + size) > MAX_PKT_LENGTH) {
    size = MAX_PKT_LENGTH - currentLength;
  }

  // write data
  for (size_t i = 0; i < size; i++) {
    lora_writeRegister(REG_FIFO, buffer[i]);
  }

  // update length
  lora_writeRegister(REG_PAYLOAD_LENGTH, currentLength + size);

  return size;
}

int lora_available()
{
  return (lora_readRegister(REG_RX_NB_BYTES) - lora.packetIndex);
}

int lora_read()
{
  if (!lora_available()) {
    return -1;
  }

  lora.packetIndex++;

  return lora_readRegister(REG_FIFO);
}

int lora_peek()
{
  if (!lora_available()) {
    return -1;
  }

  // store current FIFO address
  int currentAddress = lora_readRegister(REG_FIFO_ADDR_PTR);

  // read
  uint8_t b = lora_readRegister(REG_FIFO);

  // restore FIFO address
  lora_writeRegister(REG_FIFO_ADDR_PTR, currentAddress);

  return b;
}

void lora_flush()
{
}

void lora_onReceive(void(*callback)(int))
{
  lora.onReceive = callback;

  if (callback) {
    lora_writeRegister(REG_DIO_MAPPING_1, 0x00);
// TODO: ARDUINO SPECIFIC
    attachInterrupt(digitalPinToInterrupt(lora.dio0), lora_onDio0Rise, RISING);
  } else {
    detachInterrupt(digitalPinToInterrupt(lora.dio0));
  }
}

void lora_receive(int size)
{
  if (size > 0) {
    lora_implicitHeaderMode();

    lora_writeRegister(REG_PAYLOAD_LENGTH, size & 0xff);
  } else {
    lora_explicitHeaderMode();
  }

  lora_writeRegister(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_RX_CONTINUOUS);
}

void lora_idle()
{
  lora_writeRegister(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_STDBY);
}

void lora_sleep()
{
  lora_writeRegister(REG_OP_MODE, MODE_LONG_RANGE_MODE | MODE_SLEEP);
}
void lora_setTxPower_default(int level)
{
  lora_setTxPower(level, PA_OUTPUT_PA_BOOST_PIN);
}
void lora_setTxPower(int level, int outputPin)
{
  if (PA_OUTPUT_RFO_PIN == outputPin) {
    // RFO
    if (level < 0) {
      level = 0;
    } else if (level > 14) {
      level = 14;
    }

    writeRegister(REG_PA_CONFIG, 0x70 | level);
  } else {
    // PA BOOST
    if (level < 2) {
      level = 2;
    } else if (level > 17) {
      level = 17;
    }

    lora_writeRegister(REG_PA_CONFIG, PA_BOOST | (level - 2));
  }
}

void lora_setFrequency(long frequency)
{
  lora.frequency = frequency;

  uint64_t frf = ((uint64_t)frequency << 19) / 32000000;

  lora_writeRegister(REG_FRF_MSB, (uint8_t)(frf >> 16));
  lora_writeRegister(REG_FRF_MID, (uint8_t)(frf >> 8));
  lora_writeRegister(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

void lora_setSpreadingFactor(int sf)
{
  if (sf < 6) {
    sf = 6;
  } else if (sf > 12) {
    sf = 12;
  }

  if (sf == 6) {
    lora_writeRegister(REG_DETECTION_OPTIMIZE, 0xc5);
    lora_writeRegister(REG_DETECTION_THRESHOLD, 0x0c);
  } else {
    lora_writeRegister(REG_DETECTION_OPTIMIZE, 0xc3);
    lora_writeRegister(REG_DETECTION_THRESHOLD, 0x0a);
  }

  lora_writeRegister(REG_MODEM_CONFIG_2, (lora_readRegister(REG_MODEM_CONFIG_2) & 0x0f) | ((sf << 4) & 0xf0));
}

void lora_setSignalBandwidth(long sbw)
{
  int bw;

  if (sbw <= 7.8E3) {
    bw = 0;
  } else if (sbw <= 10.4E3) {
    bw = 1;
  } else if (sbw <= 15.6E3) {
    bw = 2;
  } else if (sbw <= 20.8E3) {
    bw = 3;
  } else if (sbw <= 31.25E3) {
    bw = 4;
  } else if (sbw <= 41.7E3) {
    bw = 5;
  } else if (sbw <= 62.5E3) {
    bw = 6;
  } else if (sbw <= 125E3) {
    bw = 7;
  } else if (sbw <= 250E3) {
    bw = 8;
  } else /*if (sbw <= 250E3)*/ {
    bw = 9;
  }

  lora_writeRegister(REG_MODEM_CONFIG_1, (lora_readRegister(REG_MODEM_CONFIG_1) & 0x0f) | (bw << 4));
}

void lora_setCodingRate4(int denominator)
{
  if (denominator < 5) {
    denominator = 5;
  } else if (denominator > 8) {
    denominator = 8;
  }

  int cr = denominator - 4;

  lora_writeRegister(REG_MODEM_CONFIG_1, (lora_readRegister(REG_MODEM_CONFIG_1) & 0xf1) | (cr << 1));
}

void lora_setPreambleLength(long length)
{
  lora_writeRegister(REG_PREAMBLE_MSB, (uint8_t)(length >> 8));
  lora_writeRegister(REG_PREAMBLE_LSB, (uint8_t)(length >> 0));
}

void lora_setSyncWord(int sw)
{
  lora_writeRegister(REG_SYNC_WORD, sw);
}

void lora_enableCrc()
{
  lora_writeRegister(REG_MODEM_CONFIG_2, lora_readRegister(REG_MODEM_CONFIG_2) | 0x04);
}

void lora_disableCrc()
{
  lora_writeRegister(REG_MODEM_CONFIG_2, lora_readRegister(REG_MODEM_CONFIG_2) & 0xfb);
}

uint8_t lora_random()
{
  return lora_readRegister(REG_RSSI_WIDEBAND);
}

void lora_setPins(int ss, int reset, int dio0)
{
  lora.ss = ss;
  lora.reset = reset;
  lora.dio0 = dio0;
}

void lora_setSPIFrequency(uint32_t frequency)
{
  _spiSettings = SPISettings(frequency, MSBFIRST, SPI_MODE0);
}

void lora_dumpRegisters()
{
  for (int i = 0; i < 128; i++) {
    out.print("0x");
    out.print(i, HEX);
    out.print(": 0x");
    out.println(lora_readRegister(i), HEX);
  }
}

void lora_explicitHeaderMode()
{
  lora.implicitHeaderMode = 0;

  lora_writeRegister(REG_MODEM_CONFIG_1, lora_readRegister(REG_MODEM_CONFIG_1) & 0xfe);
}

void lora_implicitHeaderMode()
{
  lora.implicitHeaderMode = 1;

  lora_writeRegister(REG_MODEM_CONFIG_1, lora_readRegister(REG_MODEM_CONFIG_1) | 0x01);
}

void lora_handleDio0Rise()
{
  int irqFlags = lora_readRegister(REG_IRQ_FLAGS);

  // clear IRQ's
  writeRegister(REG_IRQ_FLAGS, irqFlags);

  if ((irqFlags & IRQ_PAYLOAD_CRC_ERROR_MASK) == 0) {
    // received a packet
    lora.packetIndex = 0;

    // read packet length
    int packetLength = lora.implicitHeaderMode ? lora_readRegister(REG_PAYLOAD_LENGTH) : lora_readRegister(REG_RX_NB_BYTES);

    // set FIFO address to current RX address
    lora_writeRegister(REG_FIFO_ADDR_PTR, lora_readRegister(REG_FIFO_RX_CURRENT_ADDR));

    if (lora.onReceive) {
      lora.onReceive(packetLength);
    }

    // reset FIFO address
    lora_writeRegister(REG_FIFO_ADDR_PTR, 0);
  }
}

uint8_t lora_readRegister(uint8_t address)
{
  return lora_singleTransfer(address & 0x7f, 0x00);
}

void lora_writeRegister(uint8_t address, uint8_t value)
{
  lora_singleTransfer(address | 0x80, value);
}

uint8_t lora_singleTransfer(uint8_t address, uint8_t value)
{
  //TODO: to fix...
  uint8_t response;

  digitalWrite(_ss, LOW);

  SPI.beginTransaction(_spiSettings);
  SPI.transfer(address);
  response = SPI.transfer(value);
  SPI.endTransaction();

  digitalWrite(_ss, HIGH);

  return response;
}

void lora_onDio0Rise()
{
  lora_handleDio0Rise();
}
