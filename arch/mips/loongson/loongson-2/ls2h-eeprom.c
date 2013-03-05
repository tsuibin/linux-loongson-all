 /*
  * This file is for AT24C64 eeprom.
  * Author: liushaozong
  */
#include <asm/addrspace.h>
#include <linux/kernel.h>
#include <ls2h/ls2h.h>
#include <ls2h/ls2h-eeprom.h>

#define	DEBUG_IIC
#ifdef	DEBUG_IIC
#define EEP_DBG(fmt, args...)	printk(fmt, ## args)
#else
#define EEP_DBG(fmt, args...) 	do { } while (0)
#endif

#define	AT24C64_ADDR	0xa0
static inline void ls2h_i2c_stop(void)
{
again:
	ls2h_writeb(CR_STOP, LS2H_I2C1_CR_REG);
	ls2h_readb(LS2H_I2C1_SR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_BUSY)
		goto again;
}

void ls2h_i2c1_init(void)
{
	ls2h_writeb(0x0, LS2H_I2C1_CTR_REG);
	ls2h_writeb(0xc8, LS2H_I2C1_PRER_LO_REG);
	ls2h_writeb(0, LS2H_I2C1_PRER_HI_REG);
	ls2h_writeb(0x80,LS2H_I2C1_CTR_REG);
}

static int i2c_send_addr(int data_addr)
{
	unsigned char ee_dev_addr = AT24C64_ADDR;
	int i = (data_addr >> 8) & 0x1f;

	ls2h_writeb(ee_dev_addr, LS2H_I2C1_TXR_REG);
	ls2h_writeb((CR_START | CR_WRITE), LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	ls2h_writeb((i & 0xff), LS2H_I2C1_TXR_REG);
	ls2h_writeb(CR_WRITE, LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	ls2h_writeb((data_addr & 0xff), LS2H_I2C1_TXR_REG);
	ls2h_writeb(CR_WRITE, LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	return 1;
}

 /*
  * BYTE WRITE: A write operation requires two 8-bit data word addresses following
  * the device address word and acknowledgment. Upon receipt of this address, 
  * the EEPROM will again respond with a zero and then clock in the first 8-bit
  * data word. Following receipt of the 8-bit data word, the EEPROM will output 
  * a zero and the addressing device, such as a microcontroller, must terminate 
  * the write sequence with a stop condition.
  *
  **/
int eeprom_write_byte(int data_addr, unsigned char *buf)
{
	int i;
	i = i2c_send_addr(data_addr);
	if (!i) {
		EEP_DBG("%s:%d send addr failed!\n", __func__, __LINE__);
		return 0;
	}
	ls2h_writeb((*buf & 0xff), LS2H_I2C1_TXR_REG);
	ls2h_writeb(CR_WRITE, LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	ls2h_i2c_stop();
	return 1;
}

 /*
  * PAGE WRITE: The 34K/64K EEPROM is capable of an 32-byte page writes.
  * A page write is initiated the same as a byte write, but the microcontroller 
  * does not send a stop condition after the first data word is clocked in. Insted,
  * after the EEPROM acknowledges receipt of the first data word, the microcontroller
  * can transmit up to 31 more data words. The EEPROM will respond with a zero after
  * each data word received. The microcontroller must terminate the page write 
  * sequence with a stop condition. If more than 32 data words are transmitted 
  * to the EEPROM, the data word address will "roll over" and previous data will be
  * overwritten.
  **/
int eeprom_write_page(int data_addr, unsigned char *buf, int count)
{
	int i;
	i = i2c_send_addr(data_addr);
	if (!i) {
		EEP_DBG("%s:%d send addr error!\n", __func__, __LINE__);
		return 0;
	}

	for (i = 0; i < count; i++) {
		ls2h_writeb((buf[i] & 0xff), LS2H_I2C1_TXR_REG);
		ls2h_writeb(CR_WRITE, LS2H_I2C1_CR_REG);
		while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

		if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
			EEP_DBG("%s:%d  eeprom has no ack\n", __func__,
				__LINE__);
			ls2h_i2c_stop();
			return 0;
		}
	}

	ls2h_i2c_stop();
	return i;
}

 /* 
  * The internal data word address counter maintains the last address
  * accessed during the lase read or write operation, incremented by one.
  * This address stays valid between operations as longs as the chip 
  * power is maintained. The address "roll over" during read is from the 
  * last byte if the last memory pate to the first byte of the first page.
  * The address "roll over" during write is from the last byte of the 
  * same page.
  *
  **/
int eeprom_read_cur(unsigned char *buf)
{
	unsigned char ee_dev_addr = AT24C64_ADDR | 0x1;

	ls2h_writeb(ee_dev_addr, LS2H_I2C1_TXR_REG);
	ls2h_writeb((CR_START | CR_WRITE), LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	ls2h_writeb(CR_READ, LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	*buf = ls2h_readb(LS2H_I2C1_RXR_REG);
	ls2h_i2c_stop();

	return 1;
}

 /*
  * A random read requires a "dummy" byte write sequence to load in the 
  * data word address. Once the device address word and data word address
  * are clocked in and acknoeledged by the EEPROM, the microcontroller must
  * generate another start condition. The microcontroller now initiates a
  * current address read by sending a device address and serially clocks 
  * out the data word. The microcontroller does not respond with a zero but
  * does generate a following stop condition.
  *
  **/
int eeprom_read_rand(int data_addr, unsigned char *buf)
{
	int i;
	i = i2c_send_addr(data_addr);
	if (!i) {
		EEP_DBG("%s:%d eeprpm random send addr error!\n", __func__,
			__LINE__);
		return 0;
	}

	i = eeprom_read_cur(buf);
	if (!i) {
		EEP_DBG("%s:%d eeprom random read failed!\n", __func__,
			__LINE__);
		return 0;
	}

	return 1;
}

 /*
  * Sequential reads are initiated by either a current address read or a random 
  * address read. After the microcontroller receives a data word, it responds with 
  * acknowledge. As longs as the EEPROM receives an acknowledge, it will continue
  * to increment the data word address and serially cloke out sequential data words.
  * When the memory addrsss limit is reached, the data word address will "roll over"
  * and the sequential read will continue. The sequential read operation is terminated
  * when the microcontroller does not respond with a zero but does generate a following
  * stop condition.
  *
  */
static int i2c_read_seq_cur(unsigned char *buf, int count)
{
	int i;
	unsigned char ee_dev_addr = AT24C64_ADDR | 0x1;

	ls2h_writeb(ee_dev_addr, LS2H_I2C1_TXR_REG);
	ls2h_writeb((CR_START | CR_WRITE), LS2H_I2C1_CR_REG);
	while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;

	if (ls2h_readb(LS2H_I2C1_SR_REG) & SR_NOACK) {
		EEP_DBG("%s:%d  eeprom has no ack\n", __func__, __LINE__);
		ls2h_i2c_stop();
		return 0;
	}

	for (i = 0; i < count; i++) {
		ls2h_writeb(((i == count - 1) ? (CR_READ | CR_ACK) : CR_READ),
			LS2H_I2C1_CR_REG);
		while (ls2h_readb(LS2H_I2C1_SR_REG) & SR_TIP) ;
		buf[i] = ls2h_readb(LS2H_I2C1_RXR_REG);
	}
	ls2h_i2c_stop();
	return i;
}

static int i2c_read_seq_rand(int data_addr, unsigned char *buf, int count)
{
	int i;
	i = i2c_send_addr(data_addr);
	if (!i) {
		EEP_DBG("%s:%d send addr failed!\n", __func__, __LINE__);
		return i;
	}

	i = i2c_read_seq_cur(buf, count);

	if (!i) {
		EEP_DBG("%s:%d eeprom random read failed!\n", __func__,
			__LINE__);
		return 0;
	}

	return i;
}

int eeprom_read_seq(int data_addr, unsigned char *buf, int count)
{
	int i;
	i = i2c_read_seq_rand(data_addr, buf, count);
	return i;
}
