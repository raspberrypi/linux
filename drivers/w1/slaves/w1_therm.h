/*
 * Copyright (c) 2020 Akira Corp. <akira215corp@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __W1_THERM_H
#define __W1_THERM_H

#include <asm/types.h>

#include <linux/device.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/w1.h>

/*----------------------------------Defines---------------------------------*/

#define W1_RECALL_EEPROM	0xB8    /* This command should be in public header w1.h but is not */

#define W1_THERM_MAX_TRY		    5	/* Nb of try for an operation */
#define W1_THERM_RETRY_DELAY	    20	/* Delay in ms to retry to acquire bus mutex */
#define W1_THERM_EEPROM_WRITE_DELAY	10	/* Delay in ms to write in EEPROM */

#define EEPROM_CMD_WRITE    "save" 		/* command to be written in the eeprom sysfs */
#define EEPROM_CMD_READ     "restore"  	/* to trigger device EEPROM operations */
#define BULK_TRIGGER_CMD    "trigger"   /* to trigger a bulk read on the bus */

#define MIN_TEMP	-55	/* min temperature that can be mesured */
#define MAX_TEMP	125	/* max temperature that can be mesured */

/* Counter for devices supporting bulk reading */
static u16 bulk_read_device_counter = 0;

/*----------------------------------Structs---------------------------------*/

/*
 * struct w1_therm_family_converter
 * Used to bind standard function call
 * to device specific function
 * it could be routed to NULL if device don't support feature
 * see helper : device_family() 
 */
struct w1_therm_family_converter {
	u8					broken;
	u16					reserved;
	struct w1_family	*f;
	int					(*convert)(u8 rom[9]);
	int					(*get_conversion_time)(struct w1_slave *sl);
	int					(*set_resolution)(struct w1_slave *sl, int val);
	int					(*get_resolution)(struct w1_slave *sl);
	int					(*write_data)(struct w1_slave *sl, const u8 *data);
	bool				bulk_read;
};

/*
 * struct w1_therm_family_data 
 * rom : data
 * refcnt : ref count
 * external_powered : 1 - device powered externally, 
 *					 0 - device parasite powered, 
 *					-x - error or undefined
 * resolution : resolution in bit of the device, negative value are error code
*/
struct w1_therm_family_data {
	uint8_t rom[9];
	atomic_t refcnt;
	int external_powered;
	int resolution;
	int convert_triggered;
	struct w1_therm_family_converter *specific_functions;
};

/*
 * struct therm_info
 * Only used to store temperature reading
 * rom : RAM device data
 * crc : computed crc from rom
 * verdict: 1 crc checked, 0 crc not matching
*/
struct therm_info {
	u8 rom[9];
	u8 crc;
	u8 verdict;
};

/*-----------------------Device specific functions-------------------------*/

static inline int w1_DS18B20_convert_temp(u8 rom[9]);
static inline int w1_DS18S20_convert_temp(u8 rom[9]);

static inline int w1_DS18B20_convert_time(struct w1_slave *sl);
static inline int w1_DS18S20_convert_time(struct w1_slave *sl);

static inline int w1_DS18B20_write_data(struct w1_slave *sl,const u8 *data);
static inline int w1_DS18S20_write_data(struct w1_slave *sl,const u8 *data);											

static inline int w1_DS18B20_set_resolution(struct w1_slave *sl, int val);
static inline int w1_DS18B20_get_resolution(struct w1_slave *sl);

/*-------------------------------Macros--------------------------------------*/

/* return a pointer on the slave w1_therm_family_converter struct: 
	always test family data existance before*/
#define SLAVE_SPECIFIC_FUNC(sl) \
	(((struct w1_therm_family_data *)(sl->family_data))->specific_functions)

/* return the power mode of the sl slave : 1-ext, 0-parasite, <0 unknown 
	always test family data existance before*/
#define SLAVE_POWERMODE(sl) \
	(((struct w1_therm_family_data *)(sl->family_data))->external_powered)

/* return the resolution in bit of the sl slave : <0 unknown 
	always test family data existance before*/
#define SLAVE_RESOLUTION(sl) \
	(((struct w1_therm_family_data *)(sl->family_data))->resolution)

/*  return wether or not a converT command has been issued to the slave
 *  0: no bulk read is pending
 * -1: conversion is in progress
 *  1: conversion done, result to be read
*/
#define SLAVE_CONVERT_TRIGGERED(sl) \
	(((struct w1_therm_family_data *)(sl->family_data))->convert_triggered)

/* return the address of the refcnt in the family data */
#define THERM_REFCNT(family_data) \
	(&((struct w1_therm_family_data *)family_data)->refcnt)

/*-------------------------- Helpers Functions------------------------------*/

/*  device_family() 
 *  @brief Helper function that provide a pointer on the w1_therm_family_converter struct
 *  @param sl represents the device 
 *  @return pointer to the slaves's family converter, NULL if not known
*/
static struct w1_therm_family_converter *device_family(struct w1_slave *sl);

/* bus_mutex_lock() get the mutex & retry
 * @param: lock: w1 bus mutex to get
 * return value : true is mutex is acquired and lock, false otherwise
*/
static inline bool bus_mutex_lock(struct mutex *lock);

/* support_bulk_read() check is device is supporting bulk read
 * @param: sl: device to get the conversion time
 * return value : true : bulk read support, false : no support or error
*/
static inline bool bulk_read_support(struct w1_slave *sl);

/* conversion_time() get the Tconv fo the device
 * @param: sl: device to get the conversion time
 * return value : positive value is conversion time in ms, negative values kernel error code otherwise
*/
static inline int conversion_time(struct w1_slave *sl);

/* temperature_from_RAM() return the temperature in 1/100°
 * Device dependant, it will select the correct computation method
 * @param: sl: device that sent the RAM data
 * @param: rom: ram read value
 * return value : positive value istemperature in 1/1000°, negative values kernel error code otherwise
*/
static inline int temperature_from_RAM(struct w1_slave *sl, u8 rom[9]);

/* int_to_short() safe casting of int to short
 * min/max values are defined by Macro
 * @param: i integer to be converted to short
 * return value : a short in the range of min/max value
*/
static inline s8 int_to_short(int i);

/*---------------------------Hardware Functions-----------------------------*/

/**
 * reset_select_slave() - reset and select a slave
 * @sl:		the slave to select
 *
 * Resets the bus and then selects the slave by sending either a ROM MATCH.  
 * w1_reset_select_slave() from w1_io.c could not be used
 * here because a SKIP ROM command is sent if only one device is on the line.
 * At the beginning of the such process, sl->master->slave_count is 1 even if 
 * more devices are on the line, causing collision on the line.
 * The w1 master lock must be held.
 *
 * Return:	0 if success, negative kernel error code otherwise
 */
static int reset_select_slave(struct w1_slave *sl);

/* convert_t()
 * @sl: 				pointer to the slave to read
 * @info: 	pointer to a structure to store the read results
 * return value: 0 if success, -kernel error code otherwise
 */
static int convert_t(struct w1_slave *sl, struct therm_info *info);

/* read_scratchpad()
 * @sl: 	pointer to the slave to read
 * @info: 	pointer to a structure to store the read results
 * return value: 0 if success, -kernel error code otherwise
 */
static int read_scratchpad(struct w1_slave *sl, struct therm_info *info);

/* write_data()
 * @sl: 		pointer to the slave to read
 * @data: 		pointer to an array of 3 bytes, as 3 bytes MUST be written
 * @nb_bytes: 	Nb bytes to be written (2 for DS18S20, 3 for other devices)
 * return value: 0 if success, -kernel error code otherwise
 */
static int write_scratchpad(struct w1_slave *sl, const u8 *data, u8 nb_bytes);

/* copy_scratchpad() - Copy the content of scratchpad in device EEPROM
 * @sl:		slave involved
 * return value : 0 if success, -kernel error code otherwise
 */
static int copy_scratchpad(struct w1_slave *sl);

/* recall_eeprom() - retrieve EEPROM data to device RAM 
 * @sl:		slave involved
 * return value : 0 if success, -kernel error code otherwise
 */
static int recall_eeprom(struct w1_slave *sl);

/* read_powermode() - Ask the device to get its power mode {external, parasite}
 * @sl:		slave to be interrogated
 * return value :
 * 0 - parasite powered device
 * 1 - externally powered device
 * <0 - kernel error code
 */
static int read_powermode(struct w1_slave *sl);

/* trigger_bulk_read() - send a SKIP ROM follow by a CONVERT T commmand
 * on the bus. It also set a flag in each slave struct to signal
 * @dev_master		the device master of the bus
 * return value : 0 if success, -kernel error code otherwise
 */
static int trigger_bulk_read(struct w1_master *dev_master);


/*-----------------------------Interface sysfs--------------------------------*/

/* @brief A callback function to output the temperature Old way
 * read temperature and return the result in the sys file 
 * This has been kept for compatibility
 */
static ssize_t w1_slave_show(struct device *device,
	struct device_attribute *attr, char *buf);

/* @brief A callback function to set the resolution Old way
 * If value is 0, it write config in the EEPROM
 * If value is 9..12, it set the resolution in the RAM 
 * This has been kept for compatibility
 */
static ssize_t w1_slave_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t size);

static ssize_t w1_seq_show(struct device *device,
	struct device_attribute *attr, char *buf);

/* @brief A callback function to output the temperature (1/1000°)
* temperature_show
* read temperature and return the result in the sys file 
* Main differences with w1_slave :
*	- No hardware check ()
*/
static ssize_t temperature_show(struct device *device,
	struct device_attribute *attr, char *buf);

/** @brief A callback function to output the power mode of the device
 *	Ask the device to get its powering mode
 * 	Once done, it is stored in the sl->family_data to avoid doing the test
 * 	during data read. Negative results are kernel error code
 *  @param device represents the device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer to which to write the resolution
 *  @return return the total number of characters written to the buffer (excluding null)
 */
static ssize_t ext_power_show(struct device *device,
	struct device_attribute *attr, char *buf);

/** @brief A callback function to output the resolution of the device
 *  @param device represents the device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer to which to write the resolution
 *  @return return the total number of characters written to the buffer (excluding null)
 */
static ssize_t resolution_show(struct device *device,
	struct device_attribute *attr, char *buf);

/** @brief A callback function to store the user resolution in the device RAM
 *  @param device represents the device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which to read resolution to be set
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t resolution_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t size);

/** @brief A callback function to let the user read/write device EEPROM
 *  @param device represents the device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which the instruction (direction) will be read
 *       EEPROM_CMD_WRITE 'write' -> device write RAM to EEPROM, 
 *       EEPROM_CMD_READ  'read' -> device read EEPROM and put to RAM
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t eeprom_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t size);

/** @brief A callback function to set the alarms level
 *  @param device represents the master device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which the TH and TL will be read
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t alarms_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t size);

/** @brief A callback function to get the alarms level
 *  @param device represents the master device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which the TH and TL will be sent
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t alarms_show(struct device *device,
	struct device_attribute *attr, char *buf);

/** @brief A callback function to trigger bulk read on the bus
 *  @param device represents the master device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which the instruction BULK_TRIGGER_CMD will be read
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t therm_bulk_read_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t size);


/** @brief A callback function to check if bulk read is on progress
 *  @param device represents the master device
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which the instruction BULK_TRIGGER_CMD will be read
 *  @param size the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t therm_bulk_read_show(struct device *device,
	struct device_attribute *attr, char *buf);

	

/*-----------------------------Attributes declarations-------------------------------*/

static DEVICE_ATTR_RW(w1_slave);
static DEVICE_ATTR_RO(w1_seq);
static DEVICE_ATTR_RO(temperature);
static DEVICE_ATTR_RO(ext_power);
static DEVICE_ATTR_RW(resolution);
static DEVICE_ATTR_WO(eeprom);
static DEVICE_ATTR_RW(alarms);

static DEVICE_ATTR_RW(therm_bulk_read); /* attribut at master level */

/*-----------------------------Interface Functions------------------------------------*/

/* w1_therm_add_slave() - Called each time a search discover a new device
 * used to initialized slave (family datas)
 * @sl:	slave just discovered
 * return value : 0 - If success, negative kernel code otherwise
 */
static int w1_therm_add_slave(struct w1_slave *sl);

/* w1_therm_remove_slave() - Called each time a slave is removed
 * used to free memory
 * @sl:	slave to be removed
 */
static void w1_therm_remove_slave(struct w1_slave *sl);

/* w1_DS18B20_set_resolution() write new resolution to the RAM device
 * @param: slave: device to set the resolution
 * @param: val: new resolution in bit [9..12]
 * return value : 0 if success, negative kernel error code otherwise
*/
static inline int w1_DS18B20_set_resolution(struct w1_slave *sl, int val);

/* w1_DS18B20_get_resolution() read the device RAM to get its resolution setting
 * @param: device: device to get the resolution form
 * return value : resolution in bit [9..12] or negative kernel error code
*/
static inline int w1_DS18B20_get_resolution(struct w1_slave *sl);

#endif  /* __W1_THERM_H */
