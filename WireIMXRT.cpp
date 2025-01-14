
#include "Wire.h"


#if defined(__IMXRT1062__)

//#include "debug/printf.h"

#define PINCONFIG (IOMUXC_PAD_ODE | IOMUXC_PAD_SRE | IOMUXC_PAD_DSE(4) | IOMUXC_PAD_SPEED(1) | IOMUXC_PAD_PKE | IOMUXC_PAD_PUE | IOMUXC_PAD_PUS(3) | IOMUXC_PAD_HYS)


//***************************************************
//  Master Mode
//***************************************************

FLASHMEM void TwoWire::begin(void)
{
	// use 24 MHz clock
	CCM_CSCDR2 = (CCM_CSCDR2 & ~CCM_CSCDR2_LPI2C_CLK_PODF(63)) | CCM_CSCDR2_LPI2C_CLK_SEL;
	hardware.clock_gate_register |= hardware.clock_gate_mask;

	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	port->MCR = LPI2C_MCR_RST;
	setClock(100000);
	// setSDA() & setSCL() may be called before or after begin()
	configSDApin(sda_pin_index_); // Setup SDA register
	configSCLpin(scl_pin_index_); // setup SCL register
}

void TwoWire::end()
{
}


size_t TwoWire::write(uint8_t data)
{
	if (transmitting || slave_mode) {
		if (txBufferLength >= BUFFER_LENGTH+1) {
			setWriteError();
			return 0;
		}
		txBuffer[txBufferLength++] = data;
		return 1;
	}
	return 0;
}

size_t TwoWire::write(const uint8_t *data, size_t quantity)
{
	if (transmitting || slave_mode) {
		size_t avail = BUFFER_LENGTH+1 - txBufferLength;
		if (quantity > avail) {
			quantity = avail;
			setWriteError();
		}
		memcpy(txBuffer + txBufferLength, data, quantity);
		txBufferLength += quantity;
		return quantity;
	}
	return 0;
}

// 2      BBF = Bus Busy Flag
// 1      MBF = Master Busy Flag
//   40   DMF = Data Match Flag
//   20   PLTF = Pin Low Timeout Flag
//   10   FEF = FIFO Error Flag
//   08   ALF = Arbitration Lost Flag
//   04   NDF = NACK Detect Flag
//   02   SDF = STOP Detect Flag
//   01   EPF = End Packet Flag
//      2 RDF = Receive Data Flag
//      1 TDF = Transmit Data Flag

bool TwoWire::wait_idle()
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	elapsedMillis timeout = 0;
	while (1) {
		uint32_t status = port->MSR; // pg 2899 & 2892
		if (!(status & LPI2C_MSR_BBF)) break; // bus is available
		if (status & LPI2C_MSR_MBF) break; // we already have bus control
		if (timeout > 16) {
			//Serial.printf("timeout waiting for idle, MSR = %x\n", status);
			if (force_clock()) break;
			//Serial.printf("unable to get control of I2C bus\n");
			return false;
		}
	}
	port->MSR = 0x00007F00; // clear all prior flags
	return true;
}


uint8_t TwoWire::endTransmission(uint8_t sendStop)
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	uint32_t tx_len = txBufferLength;
	if (!tx_len) return 4; // no address for transmit
	if (!wait_idle()) return 4;
	uint32_t tx_index = 0; // 0=start, 1=addr, 2-(N-1)=data, N=stop
	elapsedMillis timeout = 0;
	while (1) {
		// transmit stuff, if we haven't already
		if (tx_index <= tx_len) {
			uint32_t fifo_used = port->MFSR & 0x07; // pg 2914
			while (fifo_used < 4) {
				if (tx_index == 0) {
					port->MTDR = LPI2C_MTDR_CMD_START | txBuffer[0];
					tx_index = 1;
				} else if (tx_index < tx_len) {
					port->MTDR = LPI2C_MTDR_CMD_TRANSMIT | txBuffer[tx_index++];
				} else {
					if (sendStop) port->MTDR = LPI2C_MTDR_CMD_STOP;
					tx_index++;
					break;
				}
				fifo_used++;
			}
		}
		// monitor status
		uint32_t status = port->MSR; // pg 2884 & 2891
		if (status & LPI2C_MSR_ALF) {
			port->MCR |= LPI2C_MCR_RTF | LPI2C_MCR_RRF; // clear FIFOs
			return 4; // we lost bus arbitration to another master
		}
		if (status & LPI2C_MSR_NDF) {
			port->MCR |= LPI2C_MCR_RTF | LPI2C_MCR_RRF; // clear FIFOs
			port->MTDR = LPI2C_MTDR_CMD_STOP;
			return 2; // NACK (assume address, TODO: how to tell address from data)
		}
		if ((status & LPI2C_MSR_PLTF) || timeout > 50) {
			port->MCR |= LPI2C_MCR_RTF | LPI2C_MCR_RRF; // clear FIFOs
			port->MTDR = LPI2C_MTDR_CMD_STOP; // try to send a stop
			return 4; // clock stretched too long or generic timeout
		}
		// are we done yet?
		if (tx_index > tx_len) {
			uint32_t tx_fifo = port->MFSR & 0x07;
			if (tx_fifo == 0 && ((status & LPI2C_MSR_SDF) || !sendStop)) {
				return 0;
			}
		}
		yield();
	}
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t length, uint8_t sendStop)
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	if (!wait_idle()) return 4;
	address = (address & 0x7F) << 1;
	if (length < 1) length = 1;
	if (length > 255) length = 255;
	rxBufferIndex = 0;
	rxBufferLength = 0;
	uint32_t tx_state = 0; // 0=begin, 1=start, 2=data, 3=stop
	elapsedMillis timeout = 0;
	while (1) {
		// transmit stuff, if we haven't already
		if (tx_state < 3) {
			uint32_t tx_fifo = port->MFSR & 0x07; // pg 2914
			while (tx_fifo < 4 && tx_state < 3) {
				if (tx_state == 0) {
					port->MTDR = LPI2C_MTDR_CMD_START | 1 | address;
				} else if (tx_state == 1) {
					port->MTDR = LPI2C_MTDR_CMD_RECEIVE | (length - 1);
				} else {
					if (sendStop) port->MTDR = LPI2C_MTDR_CMD_STOP;
				}
				tx_state++;
				tx_fifo--;
			}
		}
		// receive stuff
		if (rxBufferLength < sizeof(rxBuffer)) {
			uint32_t rx_fifo = (port->MFSR >> 16) & 0x07;
			while (rx_fifo > 0 && rxBufferLength < sizeof(rxBuffer)) {
				rxBuffer[rxBufferLength++] = port->MRDR;
				rx_fifo--;
			}
		}
		// monitor status, check for error conditions
		uint32_t status = port->MSR; // pg 2884 & 2891
		if (status & LPI2C_MSR_ALF) {
			port->MCR |= LPI2C_MCR_RTF | LPI2C_MCR_RRF; // clear FIFOs
			break;
		}
		if ((status & LPI2C_MSR_NDF) || (status & LPI2C_MSR_PLTF) || timeout > 50) {
			port->MCR |= LPI2C_MCR_RTF | LPI2C_MCR_RRF; // clear FIFOs
			port->MTDR = LPI2C_MTDR_CMD_STOP; // try to send a stop
			break;
		}
		// are we done yet?
		if (rxBufferLength >= length && tx_state >= 3) {
			uint32_t tx_fifo = port->MFSR & 0x07;
			if (tx_fifo == 0 && ((status & LPI2C_MSR_SDF) || !sendStop)) {
				break;
			}
		}
		yield();
	}
	uint32_t rx_fifo = (port->MFSR >> 16) & 0x07;
	if (rx_fifo > 0) port->MCR |= LPI2C_MCR_RRF;
	return rxBufferLength;
}

uint8_t TwoWire::requestFrom(uint8_t addr, uint8_t qty, uint32_t iaddr, uint8_t n, uint8_t stop)
{
	if (n > 0) {
		union { uint32_t ul; uint8_t b[4]; } iaddress;
		iaddress.ul = iaddr;
		beginTransmission(addr);
		if (n > 3) n = 3;
		do {
			n = n - 1;
			write(iaddress.b[n]);
		} while (n > 0);
		endTransmission(false);
	}
	if (qty > BUFFER_LENGTH) qty = BUFFER_LENGTH;
	return requestFrom(addr, qty, stop);
}

bool TwoWire::force_clock()
{
	bool ret = false;
	uint32_t sda_pin = hardware.sda_pins[sda_pin_index_].pin;
	uint32_t scl_pin = hardware.scl_pins[scl_pin_index_].pin;
	uint32_t sda_mask = digitalPinToBitMask(sda_pin);
	uint32_t scl_mask = digitalPinToBitMask(scl_pin);
	// take control of pins with GPIO
	*portConfigRegister(sda_pin) = 5 | 0x10;
	*portSetRegister(sda_pin) = sda_mask;
	*portModeRegister(sda_pin) |= sda_mask;
	*portConfigRegister(scl_pin) = 5 | 0x10;
	*portSetRegister(scl_pin) = scl_mask;
	*portModeRegister(scl_pin) |= scl_mask;
	delayMicroseconds(10);
	for (int i=0; i < 9; i++) {
		if ((*portInputRegister(sda_pin) & sda_mask)
		  && (*portInputRegister(scl_pin) & scl_mask)) {
			// success, both pins are high
			ret = true;
			break;
		}
		*portClearRegister(scl_pin) = scl_mask;
		delayMicroseconds(5);
		*portSetRegister(scl_pin) = scl_mask;
		delayMicroseconds(5);
	}
	// return control of pins to I2C
	*(portConfigRegister(sda_pin)) = hardware.sda_pins[sda_pin_index_].mux_val;
	*(portConfigRegister(scl_pin)) = hardware.scl_pins[scl_pin_index_].mux_val;
	return ret;
}



//***************************************************
//  Slave Mode
//***************************************************

// registers start on page 2835

void TwoWire::begin(uint8_t address)
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	CCM_CSCDR2 = (CCM_CSCDR2 & ~CCM_CSCDR2_LPI2C_CLK_PODF(63)) | CCM_CSCDR2_LPI2C_CLK_SEL;
	hardware.clock_gate_register |= hardware.clock_gate_mask;
	// setSDA() & setSCL() may be called before or after begin()
	configSDApin(sda_pin_index_); // Setup SDA register
	configSCLpin(scl_pin_index_); // setup SCL register
	port->SCR = LPI2C_SCR_RST;
	port->SCR = 0;
	port->SCFGR1 = LPI2C_SCFGR1_TXDSTALL | LPI2C_SCFGR1_RXSTALL; // page 2841
	port->SCFGR2 = 0; // page 2843;
	port->SAMR = LPI2C_SAMR_ADDR0(address);
	attachInterruptVector(hardware.irq_number, hardware.irq_function);
	NVIC_SET_PRIORITY(hardware.irq_number, 144);
	NVIC_ENABLE_IRQ(hardware.irq_number);
	port->SIER = LPI2C_SIER_TDIE |  LPI2C_SIER_RDIE | LPI2C_SIER_SDIE;
	transmitting = 0;
	slave_mode = 1;
	port->SCR = LPI2C_SCR_SEN;
}


void TwoWire::isr(void)
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	uint32_t status = port->SSR;
	uint32_t w1c_bits = status & 0xF00;
	if (w1c_bits) port->SSR = w1c_bits;

	//Serial.print("isr ");
	//Serial.println(status, HEX);

	if (status & LPI2C_SSR_RDF) { // Receive Data Flag
		int rx = port->SRDR;
		if (rx & 0x8000) {
			rxBufferIndex = 0;
			rxBufferLength = 0;
		}
		if (rxBufferLength < BUFFER_LENGTH) {
			rxBuffer[rxBufferLength++] = rx & 255;
		}
		//Serial.print("rx = ");
		//Serial.println(rx, HEX);
	}
	if (status & LPI2C_SSR_TDF) { // Transmit Data Flag
		if (!transmitting) {
			if (user_onRequest != nullptr) {
				(*user_onRequest)();
			}
			txBufferIndex = 0;
			transmitting = 1;
		}
		if (txBufferIndex < txBufferLength) {
			port->STDR = txBuffer[txBufferIndex++];
		} else {
			port->STDR = 0;
		}
		//Serial.println("tx");
	}

	if (status & LPI2C_SSR_SDF) { // Stop
		//Serial.println("Stop");
		if (rxBufferLength > 0 && user_onReceive != nullptr) {
			(*user_onReceive)(rxBufferLength);
		}
		rxBufferIndex = 0;
		rxBufferLength = 0;
		txBufferIndex = 0;
		txBufferLength = 0;
		transmitting = 0;
	}
}




//***************************************************
//  Pins Configuration
//***************************************************


FLASHMEM void TwoWire::setSDA(uint8_t pin) {
	if (pin == hardware.sda_pins[sda_pin_index_].pin) return;
	uint32_t newindex=0;
	while (1) {
		uint32_t sda_pin = hardware.sda_pins[newindex].pin;
		if (sda_pin == 255) return;
		if (sda_pin == pin) break;
		if (++newindex >= sizeof(hardware.sda_pins)) return;
	}
	if ((hardware.clock_gate_register & hardware.clock_gate_mask)) {
		// disable old pin, hard to know what to go back to?
		*(portConfigRegister(hardware.sda_pins[sda_pin_index_].pin)) = 5;
		// setup new one...
		configSDApin(newindex);
	}
	sda_pin_index_ = newindex;
}

FLASHMEM void TwoWire::configSDApin(uint8_t i)
{
	*(portControlRegister(hardware.sda_pins[i].pin)) = PINCONFIG;
	*(portConfigRegister(hardware.sda_pins[i].pin)) = hardware.sda_pins[i].mux_val;
	if (hardware.sda_pins[i].select_input_register) {
		*(hardware.sda_pins[i].select_input_register) = hardware.sda_pins[i].select_val;
	}
}

FLASHMEM void TwoWire::setSCL(uint8_t pin) {
	if (pin == hardware.scl_pins[scl_pin_index_].pin) return;
	uint32_t newindex=0;
	while (1) {
		uint32_t scl_pin = hardware.scl_pins[newindex].pin;
		if (scl_pin == 255) return;
		if (scl_pin == pin) break;
		if (++newindex >= sizeof(hardware.scl_pins)) return;
	}
	if ((hardware.clock_gate_register & hardware.clock_gate_mask)) {
		// disable old pin, hard to know what to go back to?
		*(portConfigRegister(hardware.scl_pins[scl_pin_index_].pin)) = 5;
		// setup new one...
		configSCLpin(newindex);
	}
	scl_pin_index_ = newindex;
}

FLASHMEM void TwoWire::configSCLpin(uint8_t i)
{
	*(portControlRegister(hardware.scl_pins[i].pin)) = PINCONFIG;
	*(portConfigRegister(hardware.scl_pins[i].pin)) = hardware.scl_pins[i].mux_val;
	if (hardware.scl_pins[i].select_input_register) {
		*(hardware.scl_pins[i].select_input_register) = hardware.scl_pins[i].select_val;
	}
}



#if defined(ARDUINO_TEENSY_MICROMOD)
void lpi2c1_isr(void) { Wire.isr(); }
void lpi2c3_isr(void) { Wire2.isr(); }
void lpi2c4_isr(void) { Wire1.isr(); }
void lpi2c2_isr(void) { Wire3.isr(); }
#else
void lpi2c1_isr(void) { Wire.isr(); }
void lpi2c3_isr(void) { Wire1.isr(); }
void lpi2c4_isr(void) { Wire2.isr(); }
#endif

PROGMEM
constexpr TwoWire::I2C_Hardware_t TwoWire::i2c1_hardware = {
	CCM_CCGR2, CCM_CCGR2_LPI2C1(CCM_CCGR_ON),
		{{18, 3 | 0x10, &IOMUXC_LPI2C1_SDA_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
		{{19, 3 | 0x10, &IOMUXC_LPI2C1_SCL_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
	IRQ_LPI2C1, &lpi2c1_isr
};
TwoWire Wire(IMXRT_LPI2C1_ADDRESS, TwoWire::i2c1_hardware);

PROGMEM
constexpr TwoWire::I2C_Hardware_t TwoWire::i2c3_hardware = {
	CCM_CCGR2, CCM_CCGR2_LPI2C3(CCM_CCGR_ON),
#if defined(ARDUINO_TEENSY41)
		{{17, 1 | 0x10, &IOMUXC_LPI2C3_SDA_SELECT_INPUT, 2}, {44, 2 | 0x10, &IOMUXC_LPI2C3_SDA_SELECT_INPUT, 1}},
		{{16, 1 | 0x10, &IOMUXC_LPI2C3_SCL_SELECT_INPUT, 2}, {45, 2 | 0x10, &IOMUXC_LPI2C3_SCL_SELECT_INPUT, 1}},
#else  // T4 and ARDUINO_TEENSY_MICROMOD
		{{17, 1 | 0x10, &IOMUXC_LPI2C3_SDA_SELECT_INPUT, 2}, {36, 2 | 0x10, &IOMUXC_LPI2C3_SDA_SELECT_INPUT, 1}},
		{{16, 1 | 0x10, &IOMUXC_LPI2C3_SCL_SELECT_INPUT, 2}, {37, 2 | 0x10, &IOMUXC_LPI2C3_SCL_SELECT_INPUT, 1}},
#endif
	IRQ_LPI2C3, &lpi2c3_isr
};
//TwoWire Wire1(&IMXRT_LPI2C3, TwoWire::i2c3_hardware);

PROGMEM
constexpr TwoWire::I2C_Hardware_t TwoWire::i2c4_hardware = {
	CCM_CCGR6, CCM_CCGR6_LPI2C4_SERIAL(CCM_CCGR_ON),
		{{25, 0 | 0x10, &IOMUXC_LPI2C4_SDA_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
		{{24, 0 | 0x10, &IOMUXC_LPI2C4_SCL_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
	IRQ_LPI2C4, &lpi2c4_isr
};
//TwoWire Wire2(&IMXRT_LPI2C4, TwoWire::i2c4_hardware);

#if defined(ARDUINO_TEENSY_MICROMOD)
	TwoWire Wire2(IMXRT_LPI2C3_ADDRESS, TwoWire::i2c3_hardware);
	TwoWire Wire1(IMXRT_LPI2C4_ADDRESS, TwoWire::i2c4_hardware);
#else
	TwoWire Wire1(IMXRT_LPI2C3_ADDRESS, TwoWire::i2c3_hardware);
	TwoWire Wire2(IMXRT_LPI2C4_ADDRESS, TwoWire::i2c4_hardware);
#endif


#if defined(ARDUINO_TEENSY_MICROMOD)
PROGMEM
constexpr TwoWire::I2C_Hardware_t TwoWire::i2c2_hardware = {
	CCM_CCGR2, CCM_CCGR2_LPI2C2(CCM_CCGR_ON),
		{{41, 2 | 0x10, &IOMUXC_LPI2C2_SDA_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
		{{40, 2 | 0x10, &IOMUXC_LPI2C2_SCL_SELECT_INPUT, 1}, {0xff, 0xff, nullptr, 0}},
	IRQ_LPI2C2, &lpi2c2_isr
};
TwoWire Wire3(IMXRT_LPI2C2_ADDRESS, TwoWire::i2c2_hardware);
#endif





// Timeout if a device stretches SCL this long, in microseconds
#define CLOCK_STRETCH_TIMEOUT 15000


void TwoWire::setClock(uint32_t frequency)
{
	IMXRT_LPI2C_t* port = (IMXRT_LPI2C_t*)portAddr;
	port->MCR = 0;
	if (frequency < 400000) {
		// 100 kHz
		port->MCCR0 = LPI2C_MCCR0_CLKHI(55) | LPI2C_MCCR0_CLKLO(59) |
			LPI2C_MCCR0_DATAVD(25) | LPI2C_MCCR0_SETHOLD(40);
		port->MCFGR1 = LPI2C_MCFGR1_PRESCALE(1);
		port->MCFGR2 = LPI2C_MCFGR2_FILTSDA(5) | LPI2C_MCFGR2_FILTSCL(5) |
			LPI2C_MCFGR2_BUSIDLE(3000); // idle timeout 250 us
		port->MCFGR3 = LPI2C_MCFGR3_PINLOW(CLOCK_STRETCH_TIMEOUT * 12 / 256 + 1);
	} else if (frequency < 1000000) {
		// 400 kHz
		port->MCCR0 = LPI2C_MCCR0_CLKHI(26) | LPI2C_MCCR0_CLKLO(28) |
			LPI2C_MCCR0_DATAVD(12) | LPI2C_MCCR0_SETHOLD(18);
		port->MCFGR1 = LPI2C_MCFGR1_PRESCALE(0);
		port->MCFGR2 = LPI2C_MCFGR2_FILTSDA(2) | LPI2C_MCFGR2_FILTSCL(2) |
			LPI2C_MCFGR2_BUSIDLE(3600); // idle timeout 150 us
		port->MCFGR3 = LPI2C_MCFGR3_PINLOW(CLOCK_STRETCH_TIMEOUT * 24 / 256 + 1);
	} else {
		// 1 MHz
		port->MCCR0 = LPI2C_MCCR0_CLKHI(9) | LPI2C_MCCR0_CLKLO(10) |
			LPI2C_MCCR0_DATAVD(4) | LPI2C_MCCR0_SETHOLD(7);
		port->MCFGR1 = LPI2C_MCFGR1_PRESCALE(0);
		port->MCFGR2 = LPI2C_MCFGR2_FILTSDA(1) | LPI2C_MCFGR2_FILTSCL(1) |
			LPI2C_MCFGR2_BUSIDLE(2400); // idle timeout 100 us
		port->MCFGR3 = LPI2C_MCFGR3_PINLOW(CLOCK_STRETCH_TIMEOUT * 24 / 256 + 1);
	}
	port->MCCR1 = port->MCCR0;
	port->MCFGR0 = 0;
	port->MFCR = LPI2C_MFCR_RXWATER(1) | LPI2C_MFCR_TXWATER(1);
	port->MCR = LPI2C_MCR_MEN;
}

#endif
