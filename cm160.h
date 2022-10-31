/*
 * morpork application.
 *
 * Copyright (C) 2013 Ben Jones <ben.jones12@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Based on the eagle-owl application by Philippe Cornet <phil.cornet@gmail.com>
 * https://github.com/cornetp/eagle-owl
 */

#ifndef __CM160_H__
#define __CM160_H__

#define MAX_DEVICES   1 // Only one device supported now

struct cm160_device {
  struct usb_device *usb_dev;
  usb_dev_handle *hdev;
  int epin;  // IN end point address
  int epout; // OUT end point address
};

struct cm160_frame {
  int id;
  int year;
  int month;
  int day;
  int hour;
  int min;
  double cost;
  double amps;
  double watts;
};

/* CP210X device options */
#define CP210X_IFC_ENABLE       0x00
#define CP210X_GET_LINE_CTL     0x04
#define CP210X_SET_MHS          0x07
#define CP210X_GET_MDMSTS       0x08
#define CP210X_GET_FLOW         0x14
#define CP210X_GET_BAUDRATE     0x1D
#define CP210X_SET_BAUDRATE     0x1E

/* CP210X_IFC_ENABLE */
#define UART_ENABLE             0x0001
#define UART_DISABLE            0x0000

/* CM160 protocol */
#define FRAME_ID_CONTROL 		0xA9
#define FRAME_ID_LIVE 			0x51 
#define FRAME_ID_HISTORY   		0x59
#define FRAME_ID_UNKNOWN		0x1D

/* CM160 baud rate */
#define	CM160_BAUD_RATE 		250000

#endif // __CM160_H__

