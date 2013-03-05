#ifndef __LS2H_EEPEOM_H_
#define __LS2H_EEPEOM_H_
void ls2h_i2c1_init(void);
int eeprom_read_cur(unsigned char *buf);
int eeprom_read_rand(int data_addr, unsigned char *buf);
int eeprom_read_seq(int data_addr, unsigned char *buf, int count);
int eeprom_write_byte(int data_addr, unsigned char *buf);
int eeprom_write_page(int data_addr, unsigned char *buf, int count);
#endif
