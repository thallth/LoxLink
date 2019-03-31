//
//  LoxLegacyModbusExtension.cpp
//
//  Created by Markus Fritze on 03.03.19.
//  Copyright (c) 2019 Markus Fritze. All rights reserved.
//

#include "LoxLegacyModbusExtension.hpp"
#if EXTENSION_MODBUS
#include "global_functions.hpp"
#include "stm32f1xx_hal.h"
#include "stream_buffer.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#define RS485_TX_PIN_Pin GPIO_PIN_10
#define RS485_TX_PIN_GPIO_Port GPIOB
#define RS485_RX_PIN_Pin GPIO_PIN_11
#define RS485_RX_PIN_GPIO_Port GPIOB
#define RS485_RX_ENABLE_Pin GPIO_PIN_4
#define RS485_RX_ENABLE_GPIO_Port GPIOC
#define RS485_TX_ENABLE_Pin GPIO_PIN_5
#define RS485_TX_ENABLE_GPIO_Port GPIOC

static UART_HandleTypeDef huart3;
static uint8_t gModbusChar;
static volatile uint8_t gModbus_RX_Buffer[Modbus_RX_BUFFERSIZE];
static volatile int gModbus_RX_Buffer_count;

/***
 *  Constructor
 ***/
LoxLegacyModbusExtension::LoxLegacyModbusExtension(LoxCANBaseDriver &driver, uint32_t serial)
  : LoxLegacyExtension(driver, (serial & 0xFFFFFF) | (eDeviceType_t_ModbusExtension << 24), eDeviceType_t_ModbusExtension, 0, 10020326, &config, sizeof(config)) {
  assert(sizeof(sModbusConfig) == 0x810);
}

/***
 *  RS485 requires to switch between TX/RX mode. Default is RX.
 ***/
void LoxLegacyModbusExtension::set_tx_mode(bool txMode) {
  // The board strangely has independed pins to control RX/TX enable, but they are mutally exclusive
  // Also: RX is negated in the MAX3485, so both pins always have to have the same state, which leads
  // to the valid question: why does the board have two pins for it anyway?
  GPIO_PinState pinState = txMode ? GPIO_PIN_SET : GPIO_PIN_RESET;
  HAL_GPIO_WritePin(RS485_RX_ENABLE_GPIO_Port, RS485_RX_ENABLE_Pin, pinState);
  HAL_GPIO_WritePin(RS485_TX_ENABLE_GPIO_Port, RS485_TX_ENABLE_Pin, pinState);
}

/***
 *
 ***/
void LoxLegacyModbusExtension::rs485_setup(void) {
  // configure the UART
  huart3.Instance = USART3;
  huart3.Init.BaudRate = this->config.baudrate;
  huart3.Init.WordLength = (this->config.wordLength == 8) ? UART_WORDLENGTH_8B : UART_WORDLENGTH_9B;
  huart3.Init.StopBits = this->config.twoStopBits == 0 ? UART_STOPBITS_1 : UART_STOPBITS_2;
  huart3.Init.Parity = this->config.parity == 0 ? UART_PARITY_NONE : ((this->config.parity == 1) ? UART_PARITY_EVEN : UART_PARITY_ODD);
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK) {
#if DEBUG
    printf("### RS232 HAL_UART_Init ERROR\n");
#endif
  }
  // RXNE Interrupt Enable
  SET_BIT(huart3.Instance->CR1, USART_CR1_RXNEIE);

  HAL_UART_Receive_IT(&huart3, &gModbusChar, 1);
}

/***
 *  New configuration was loaded
 ***/
void LoxLegacyModbusExtension::config_load(void) {
  printf("RS485 config : %ld ", this->config.baudrate);
  printf("%ld", this->config.wordLength);
  switch (this->config.parity) {
  case 0:
    printf("N");
    break;
  case 1:
    printf("E");
    break;
  case 2:
    printf("O");
    break;
  case 3:
    printf("0");
    break;
  case 4:
    printf("1");
    break;
  }
  printf("%ld\n", this->config.twoStopBits + 1);

  if (this->config.manualTimingFlag) {
    this->timePause = this->config.timingPause;
    this->timeTimeout = this->config.timingTimeout;
  } else {
    // automatic mode
    this->timePause = (1 + 8 + 1) * 1000 / this->config.baudrate * 35 / 10; // RS485 are using 10 bits per character, the delay is 3.5 times a character
    this->timeTimeout = 1000;                                               // wait up to 1s for a reply
  }
  // minimum time: 5ms, maximum 10s
  if (this->timePause < 5)
    this->timePause = 5;
  else if (this->timePause > 10000)
    this->timePause = 10000;
  if (this->timeTimeout < 5)
    this->timeTimeout = 5;
  else if (this->timeTimeout > 10000)
    this->timeTimeout = 10000;

  for (int i = 0; i < this->config.entryCount; ++i) {
    const sModbusDeviceConfig *d = &this->config.devices[i];
    printf("config device #%d: ", i);
    switch (d->functionCode) {
    case 1:
      printf("Read coil status");
      break;
    case 2:
      printf("Read input status");
      break;
    case 3:
      printf("Read holding register");
      break;
    case 4:
      printf("Read input register");
      break;
      // these are allowed for actors, but not used in the config
      //        case 5: printf("Write single coil"); break;
      //        case 6: printf("Write single register"); break;
      //        case 15: printf("Force multiple coils"); break;
      //        case 16: printf("Preset multiple registers"); break;
    default: // should not happen with Loxone Config
      printf("functionCode(%d)", d->functionCode);
      break;
    }
    printf(" slave address:0x%02x ", d->address);
    printf(" register:0x%02x", d->regNumber);
    printf(" options:");
    uint16_t flags = d->pollingCycle >> 16;
    if (flags & 0x4000)
      printf("reg order HL,");
    else
      printf("reg order LH,");
    if (flags & 0x8000)
      printf("Little Endian,");
    else
      printf("Big Endian,");
    if (flags & 0x2000)
      printf("2 regs for 32-bit");
    uint32_t cycle = d->pollingCycle & 0xFFF;
    if (cycle && flags & tModbusFlags_1000ms) { // in seconds
      cycle = (cycle & 0xFFF) * 1000;
    } else { // in ms
      cycle = cycle * 100;
    }
    printf(" %.1fs\n", cycle * 0.001);
  }

  memset(this->deviceTimeout, 0, sizeof(this->deviceTimeout));

  HAL_UART_DeInit(&huart3);
  rs485_setup();
}

/***
 *  Transmit buffer via RS485 and wait for reply
 ***/
bool LoxLegacyModbusExtension::_transmitBuffer(int devIndex, const uint8_t *txBuffer, size_t txBufferCount) {
  debug_print_buffer((void *)txBuffer, txBufferCount, "### TX DATA:");
  this->set_tx_mode(true);
  for (int i = 0; i < txBufferCount; ++i) {
    uint8_t byte = txBuffer[i];
    HAL_StatusTypeDef status = HAL_UART_Transmit(&huart3, &byte, sizeof(byte), 50);
    if (status != HAL_OK) {
#if DEBUG
      printf("### Modbus TX error %d\n", status);
#endif
    }
  }
  gModbus_RX_Buffer_count = 0; // reset the RX buffer for our transmission
  this->set_tx_mode(false);
  for (uint32_t i = 0; i < this->timeTimeout; i += 100) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (gModbus_RX_Buffer_count)
      break;
  }
  debug_print_buffer((void *)gModbus_RX_Buffer, gModbus_RX_Buffer_count, "### RX DATA:");
  if (!gModbus_RX_Buffer_count) {
    printf("tModbusError_NoResponse\n");
    return false;
  }
  if (gModbus_RX_Buffer_count <= 4) {
    printf("tModbusError_InvalidReceiveLength\n");
    return false;
  }
  uint16_t crc = crc16_Modus((void *)gModbus_RX_Buffer, gModbus_RX_Buffer_count - 2);
  if (gModbus_RX_Buffer[gModbus_RX_Buffer_count - 2] != (crc & 0xFF) or gModbus_RX_Buffer[gModbus_RX_Buffer_count - 1] != (crc >> 8)) {
    printf("tModbusError_CRC_Error\n");
    return false;
  }
  if (gModbus_RX_Buffer[0] != txBuffer[0] or gModbus_RX_Buffer[1] != txBuffer[1]) {
    if (txBuffer[1] == gModbus_RX_Buffer[0]) {
      //
    } else {
      //
    }
    printf("tModbusError_InvalidResponse\n");
    return false;
  }
  debug_print_buffer((void *)gModbus_RX_Buffer, gModbus_RX_Buffer_count, "### CMD:");
  int count = gModbus_RX_Buffer_count - 5;
  uint32_t value = *(uint32_t *)&gModbus_RX_Buffer[3];
  const sModbusDeviceConfig *dc = &this->config.devices[devIndex];
  switch (gModbus_RX_Buffer[1]) {
  case tModbusCode_ReadCoils:
  case tModbusCode_ReadDiscreteInputs:
    if (count < 1) {
      value = gModbus_RX_Buffer[3];
      sendCommandWithValues(Modbus_485_SensorValue, devIndex, 0, value);
    } else {
      sendCommandWithValues(debug, gModbus_RX_Buffer[1], tModbusError_InvalidReceiveLength, value);
    }
    break;
  case tModbusCode_ReadHoldingRegisters:
  case tModbusCode_ReadInputRegister:
    if (dc->pollingCycle & tModbusFlags_combineTwoRegs) { // two 16-bit registers = 32-bit value
      if (count <= 4) {
        if (dc->pollingCycle & tModbusFlags_regOrderHighLow)
          value = (value >> 16) | (value << 16);
        if (dc->pollingCycle & tModbusFlags_littleEndian)
          value = ((value >> 8) & 0x00FF00FF) | ((value << 8) & 0xFF00FF00);
        sendCommandWithValues(Modbus_485_SensorValue, devIndex, 0, value);
      } else {
        sendCommandWithValues(debug, gModbus_RX_Buffer[1], tModbusError_InvalidReceiveLength, value);
      }
    } else {
      if (count <= 2) {
        if (dc->pollingCycle & tModbusFlags_littleEndian)
          value = ((value >> 8) & 0x00FF) | ((value << 8) & 0xFF00);
        sendCommandWithValues(Modbus_485_SensorValue, devIndex, 0, value);
      } else {
        sendCommandWithValues(debug, gModbus_RX_Buffer[1], tModbusError_InvalidReceiveLength, value);
      }
    }
    break;
  case tModbusCode_WriteSingleCoil:
  case tModbusCode_WriteSingleRegister:
  case tModbusCode_WriteMultipleCoils:
  case tModbusCode_WriteMultipleRegisters:
    sendCommandWithValues(debug, gModbus_RX_Buffer[0], tModbusError_ActorResponse, *(uint32_t *)&gModbus_RX_Buffer[2]);
    break;
  case tModbusCode_ReadExceptionStatus:
    sendCommandWithValues(Modbus_485_SensorValue, devIndex, 0, gModbus_RX_Buffer[2]);
    break;
  default:
    sendCommandWithValues(debug, gModbus_RX_Buffer[1], tModbusError_UnexpectedError, value);
    break;
  }
  return true;
}

bool LoxLegacyModbusExtension::transmitBuffer(int devIndex, const uint8_t *txBuffer, size_t txBufferCount) {
  bool result = _transmitBuffer(devIndex, txBuffer, txBufferCount);
  vTaskDelay(pdMS_TO_TICKS(this->timePause)); // a little pause after a transmission
  gModbus_RX_Buffer_count = 0;                // reset the RX buffer for our transmission
  return result;
}

/***
 *  Modbus TX Task
 ***/
void LoxLegacyModbusExtension::vModbusTXTask(void *pvParameters) {
  LoxLegacyModbusExtension *_this = (LoxLegacyModbusExtension *)pvParameters;
  static uint8_t txBuffer[32]; // static to avoid stack usage
  while (1) {
    // poll devices
    for (int devIndex = 0; devIndex < _this->config.entryCount; ++devIndex) {
      const sModbusDeviceConfig *dc = &_this->config.devices[devIndex];
      if (HAL_GetTick() >= _this->deviceTimeout[devIndex]) {
        uint32_t pollingCycle = dc->pollingCycle;
        uint32_t ticks = (pollingCycle & 0xFFF) * 100; // default unit: ticks in 100ms
        if (pollingCycle & tModbusFlags_1000ms)        // value too large for it? Then the ticks are in seconds
          ticks *= 10;
        //if (ticks < 5000)
        //  ticks = 5000; // Loxone throttles the requests to 5s
        _this->deviceTimeout[devIndex] = HAL_GetTick() + pdMS_TO_TICKS(ticks);
        size_t txBufferCount = 0;
        txBuffer[txBufferCount++] = dc->address; // Modbus address
        txBuffer[txBufferCount++] = dc->functionCode & 0x1F;
        txBuffer[txBufferCount++] = dc->regNumber >> 8;
        txBuffer[txBufferCount++] = dc->regNumber & 0xFF;
        switch (dc->functionCode & 0x1F) {
        case tModbusCode_ReadCoils:
        case tModbusCode_ReadDiscreteInputs:
          txBuffer[txBufferCount++] = 0;
          txBuffer[txBufferCount++] = 1;
          break;
        case tModbusCode_ReadHoldingRegisters:
        case tModbusCode_ReadInputRegister:
          txBuffer[txBufferCount++] = 0;
          txBuffer[txBufferCount++] = (pollingCycle & tModbusFlags_combineTwoRegs) ? 2 : 1; // combine two registers?
          break;
        }
        uint16_t crc = crc16_Modus(txBuffer, txBufferCount);
        txBuffer[txBufferCount++] = crc & 0xFF;
        txBuffer[txBufferCount++] = crc >> 8;
        if (!_this->transmitBuffer(devIndex, txBuffer, txBufferCount)) {
          // give it a second try, if the first transmission failed
          _this->transmitBuffer(devIndex, txBuffer, txBufferCount);
        }
      }
    }
    // forward a Modbus command coming from the Miniserver
    uint8_t txBufferCount;
    while (xQueueReceive(&_this->txQueue, &txBufferCount, 10)) {
      for (int i = 0; i < txBufferCount; ++i) {
        uint8_t byte;
        while (!xQueueReceive(&_this->txQueue, &byte, 1)) {
        }
        txBuffer[i] = byte;
      }
      if (!_this->transmitBuffer(0, txBuffer, txBufferCount)) {
        // give it a second try, if the first transmission failed
        _this->transmitBuffer(0, txBuffer, txBufferCount);
      }
    }
  }
}

/***
 *  Setup GPIOs
 ***/
void LoxLegacyModbusExtension::Startup(void) {
  __HAL_RCC_USART3_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  static uint8_t sModbusTXBuffer[Modbus_TX_BUFFERSIZE];
  xQueueCreateStatic(Modbus_TX_BUFFERSIZE, 1, sModbusTXBuffer, &this->txQueue);

  static StackType_t sModbusTXTaskStack[configMINIMAL_STACK_SIZE];
  static StaticTask_t sModbusTXTask;
  xTaskCreateStatic(LoxLegacyModbusExtension::vModbusTXTask, "ModbusTXTask", configMINIMAL_STACK_SIZE, this, 2, sModbusTXTaskStack, &sModbusTXTask);

  this->config.manualTimingFlag = false;
  this->config.baudrate = 9600;
  this->config.wordLength = 8;
  this->config.twoStopBits = 0; // 1 stop bit
  this->config.parity = 0;      // no parity
  this->config.entryCount = 0;  // no devices
  rs485_setup();
}

/***
 *  CAN packet received
 ***/
void LoxLegacyModbusExtension::PacketToExtension(LoxCanMessage &message) {
  switch (message.commandLegacy) {
  case Modbus_485_WriteSingleCoil:
  case Modbus_485_WriteSingleRegister:
  case Modbus_485_WriteMultipleRegisters:
  case Modbus_485_WriteMultipleRegisters2:
  case Modbus_485_WriteSingleRegister4:
  case Modbus_485_WriteMultipleRegisters4: {
    int byteCount = 2;
    int regCount = 1;
    size_t txBufferCount = 0;
    static uint8_t txBuffer[16];
    txBuffer[txBufferCount++] = message.data[0]; // Modbus address
    switch (message.commandLegacy) {
    case Modbus_485_WriteSingleCoil:
      txBuffer[txBufferCount++] = tModbusCode_WriteSingleCoil;
      break;
    case Modbus_485_WriteSingleRegister:
      txBuffer[txBufferCount++] = tModbusCode_WriteSingleRegister;
      break;
    case Modbus_485_WriteMultipleRegisters:
      txBuffer[txBufferCount++] = tModbusCode_WriteMultipleRegisters;
      regCount = 2;
      break;
    case Modbus_485_WriteMultipleRegisters2:
      txBuffer[txBufferCount++] = tModbusCode_WriteMultipleRegisters;
      byteCount = 4;
      break;
    case Modbus_485_WriteSingleRegister4:
      txBuffer[txBufferCount++] = tModbusCode_WriteSingleRegister;
      byteCount = 4;
      break;
    case Modbus_485_WriteMultipleRegisters4:
      txBuffer[txBufferCount++] = tModbusCode_WriteMultipleRegisters;
      break;
    default: // should never happen
      break;
    }
    uint16_t reg = *(uint16_t *)&message.data[1]; // Modbus IO-address
    txBuffer[txBufferCount++] = reg >> 8;
    txBuffer[txBufferCount++] = reg & 0xFF;
    switch (message.commandLegacy) {
    case Modbus_485_WriteMultipleRegisters:
    case Modbus_485_WriteMultipleRegisters2:
    case Modbus_485_WriteMultipleRegisters4:
      txBuffer[txBufferCount++] = regCount >> 8; // number of registers
      txBuffer[txBufferCount++] = regCount & 0xFF;
      txBuffer[txBufferCount++] = byteCount; // number of transferred bytes
      break;
    default:
      break;
    }
    memcpy(&txBuffer[txBufferCount], &message.data[3], byteCount);
    txBufferCount += byteCount;
    uint16_t crc = crc16_Modus(txBuffer, txBufferCount);
    txBuffer[txBufferCount++] = crc & 0xFF;
    txBuffer[txBufferCount++] = crc >> 8;
    // send Modbus command comming from the Miniserver
    uint8_t size = txBufferCount;
    xQueueSendToBack(&this->txQueue, &size, 0);
    for (int i = 0; i < txBufferCount; ++i) {
      xQueueSendToBack(&this->txQueue, &txBuffer[i], 0);
    }
    break;
  }
  default:
    LoxLegacyExtension::PacketToExtension(message);
    break;
  }
}

/***
 *  Only configuration package is send via fragmented packets
 ***/
void LoxLegacyModbusExtension::FragmentedPacketToExtension(LoxMsgLegacyFragmentedCommand_t fragCommand, const void *fragData, int size) {
  switch (fragCommand) {
  case FragCmd_Modbus_config: {
    const sModbusConfig *config = (const sModbusConfig *)fragData;
    if (size <= sizeof(sModbusConfig) and config->version == 1) { // valid config?
      config_load();
    }
    break;
  }
  default:
    break;
  }
}

/***
 *  After a start request some extensions have to send additional messages
 ***/
void LoxLegacyModbusExtension::StartRequest() {
  sendCommandWithValues(config_check_CRC, 0, 1 /* config version */, 0); // required, otherwise it is considered offline
}

/**
* @brief UART MSP Initialization
* This function configures the hardware resources used in this example
* @param huart: UART handle pointer
* @retval None
*/
extern "C" void HAL_UART_MspInit(UART_HandleTypeDef *huart) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (huart->Instance == USART3) {
    /* Peripheral clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(RS485_RX_ENABLE_GPIO_Port, RS485_RX_ENABLE_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RS485_TX_ENABLE_GPIO_Port, RS485_TX_ENABLE_Pin, GPIO_PIN_RESET);

    /*Configure GPIO pins : PCPin PCPin */
    GPIO_InitStruct.Pin = RS485_RX_ENABLE_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RS485_RX_ENABLE_GPIO_Port, &GPIO_InitStruct);
    GPIO_InitStruct.Pin = RS485_TX_ENABLE_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(RS485_TX_ENABLE_GPIO_Port, &GPIO_InitStruct);

    /**USART3 GPIO Configuration    
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX 
    */
    GPIO_InitStruct.Pin = RS485_TX_PIN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RS485_TX_PIN_GPIO_Port, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = RS485_RX_PIN_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(RS485_RX_PIN_GPIO_Port, &GPIO_InitStruct);

    /* USART3 interrupt Init */
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
  }
}

/**
* @brief UART MSP De-Initialization
* This function freeze the hardware resources used in this example
* @param huart: UART handle pointer
* @retval None
*/
extern "C" void HAL_UART_MspDeInit(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART3) {
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    /**USART3 GPIO Configuration    
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX 
    */
    HAL_GPIO_DeInit(RS485_TX_PIN_GPIO_Port, RS485_TX_PIN_Pin);
    HAL_GPIO_DeInit(RS485_RX_PIN_GPIO_Port, RS485_RX_PIN_Pin);

    /* USART3 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART3_IRQn);
  }
}

/**
  * @brief  Rx Transfer completed callbacks.
  * @param  huart: pointer to a UART_HandleTypeDef structure that contains
  *                the configuration information for the specified UART module.
  * @retval None
  */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  printf("{%02x}", gModbusChar);
  if (gModbus_RX_Buffer_count < sizeof(gModbus_RX_Buffer)) {
    gModbus_RX_Buffer[gModbus_RX_Buffer_count++] = gModbusChar;
  }
  HAL_UART_Receive_IT(huart, &gModbusChar, 1);
}

extern "C" void USART3_IRQHandler(void) {
  HAL_UART_IRQHandler(&huart3);
}

#endif