/*
 * Copyright (C) 2011 - 2012 Skrilax_CZ
 * Based on Motorola Usb daemon
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>

#include <cutils/properties.h>
#include <cutils/sockets.h>

/* for LOGI, LOGE, etc. */
#define LOG_TAG "usbd"
#include <cutils/log.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

/* Usbd version */
#define USBD_VER "1.0_CM"

/* The following defines should be general for all Motorola phones */

#define PROPERTY_ADB_ENABLED                "persist.service.adb.enable"

/* usb status */
#define USB_MODEL_NAME_PATH                 "/sys/devices/platform/cpcap_battery/power_supply/usb/model_name"
#define USB_ONLINE_PATH                     "/sys/devices/platform/cpcap_battery/power_supply/usb/online"

/* input from model_name */
#define USB_INPUT_CABLE_NORMAL              "usb"
#define USB_INPUT_CABLE_FACTORY             "factory"

/* power supply events */
#define POWER_SUPPLY_TYPE_EVENT              "POWER_SUPPLY_TYPE="
#define POWER_SUPPLY_ONLINE_EVENT            "POWER_SUPPLY_ONLINE="
#define POWER_SUPPLY_MODEL_NAME_EVENT        "POWER_SUPPLY_MODEL_NAME="

/* cable events */
#define USBD_EVENT_CABLE_DISCONNECTED       "cable_disconnected"
#define USBD_EVENT_CABLE_CONNECTED          "cable_connected"
#define USBD_EVENT_GET_DESCRIPTOR           "get_descriptor"
#define USBD_EVENT_USB_ENUMERATED           "usb_enumerated"

#define USBD_DEV_EVENT_ADB_ENABLE           "adb_enable"
#define USBD_DEV_EVENT_ADB_DISABLE          "adb_disable"
#define USBD_DEV_EVENT_GET_DESCRIPTOR       "get_desc"
#define USBD_DEV_EVENT_USB_ENUMERATED       "enumerated"

#define USBD_UEVENT_CABLE_ATTACH            "usb_cable_attach"
#define USBD_UEVENT_CABLE_DETACH            "usb_cable_detach"

/* adb status */
#define USBD_ADB_STATUS_ON                  "usbd_adb_status_on"
#define USBD_ADB_STATUS_OFF                 "usbd_adb_status_off"

/* event prefixes */
#define USBD_START_PREFIX                   "usbd_start_"
#define USBD_REQ_SWITCH_PREFIX              "usbd_req_switch_"
#define USB_MODE_PREFIX                     "usb_mode_"

/* response suffix */
#define USBD_RESP_OK                        ":ok"
#define USBD_RESP_FAIL                      ":fail"

/* adb suffix */
#define USB_MODE_ADB_SUFFIX                 "_adb"

/* unload event */
#define USB_UNLOAD_DRIVER                   "usb_unload_driver"

/* structure */
struct usb_mode_info
{
	const char* apk_mode;
	const char* apk_mode_adb;
	const char* apk_start;
	const char* apk_start_adb;
	const char* apk_req_switch;
	
	const char* kern_mode;
	const char* kern_mode_adb;
};

#define USB_MODE_NONE \
{ \
	.apk_mode =         USB_UNLOAD_DRIVER,  \
	.apk_mode_adb =     USB_UNLOAD_DRIVER,  \
	.apk_start =        "",                 \
	.apk_start_adb =    "",                 \
	.apk_req_switch =   "",                 \
	.kern_mode =        "",                 \
	.kern_mode_adb =    "",                 \
}

#define USB_MODE_INFO(apk,kern) \
{ \
	.apk_mode =         USB_MODE_PREFIX        apk, \
	.apk_mode_adb =     USB_MODE_PREFIX        apk   USB_MODE_ADB_SUFFIX, \
	.apk_start =        USBD_START_PREFIX      apk, \
	.apk_start_adb =    USBD_START_PREFIX      apk   USB_MODE_ADB_SUFFIX, \
	.apk_req_switch =   USBD_REQ_SWITCH_PREFIX apk, \
	.kern_mode =                               kern, \
	.kern_mode_adb =                           kern  USB_MODE_ADB_SUFFIX, \
}

/* usb get mode namespace */
enum usb_mode_get_t
{
	USBMOD_APK,
	USBMOD_APK_START,
	USBMOD_APK_REQ_SWITCH,
	USBMOD_KERN
};

/* usb state */
enum usbd_state_t
{
	USBDSTAT_CABLE_DISCONNECTED,
	USBDSTAT_CABLE_CONNECTED,
	USBDSTAT_GET_DESCRIPTOR,
	USBDSTAT_USB_ENUMERATED
};

/* The following defines have matching equivalents in usb.apk
 * and in kernel g_mot_android module (see mot_android.c)
 * if you change them here, don't forget to update them there
 * or you will break usb.
 *
 * On Motorola Milestone, the configuration is altered
 * in a module mot_usb.ko.
 */

/* usb modes for Usb.apk */
#define USB_APK_MODE_NGP              "ngp"
#define USB_APK_MODE_NGP_MTP          "ngp_mtp"
#define USB_APK_MODE_MTP              "mtp"
#define USB_APK_MODE_MODEM            "acm"
#define USB_APK_MODE_MSC              "msc"
#define USB_APK_MODE_RNDIS            "rndis"
#define USB_APK_MODE_CHARGE_ONLY      "charge_only"

/* usb modes for kernel */
#define USB_KERN_MODE_NET             "eth"
#define USB_KERN_MODE_NGP             "acm_eth"
#define USB_KERN_MODE_NGP_MTP         "acm_eth_mtp"
#define USB_KERN_MODE_MTP             "mtp"
#define USB_KERN_MODE_MODEM           "acm"
#define USB_KERN_MODE_MSC             "msc"
#define USB_KERN_MODE_RNDIS           "rndis"
#define USB_KERN_MODE_CHARGE_ONLY     "charge_only"

/* available modes */
static struct usb_mode_info usb_modes[] = 
{
	USB_MODE_NONE,
	USB_MODE_INFO(USB_APK_MODE_NGP,         USB_KERN_MODE_NGP),
	USB_MODE_INFO(USB_APK_MODE_NGP_MTP,     USB_KERN_MODE_NGP_MTP),
	USB_MODE_INFO(USB_APK_MODE_MTP,         USB_KERN_MODE_MTP),
	USB_MODE_INFO(USB_APK_MODE_MODEM,       USB_KERN_MODE_MODEM),
	USB_MODE_INFO(USB_APK_MODE_MSC,         USB_KERN_MODE_MSC),
	USB_MODE_INFO(USB_APK_MODE_RNDIS,       USB_KERN_MODE_RNDIS),
	USB_MODE_INFO(USB_APK_MODE_CHARGE_ONLY, USB_KERN_MODE_CHARGE_ONLY),
};

/* File descriptors */
static int uevent_fd = -1;
static int usbd_app_fd = -1;
static int usbd_socket_fd = -1;
static int usb_device_fd = -1;

/* Status variables */
static int usb_current_mode = 0;
static int usb_factory_cable = 0;
static int usb_got_descriptor = 0;
static enum usbd_state_t usb_state = USBDSTAT_CABLE_DISCONNECTED;
static int usb_online = 0;
static int last_sent_usb_online = 0;

/* Opens uevent socked for usbd */
static int open_uevent_socket(void)
{
	struct sockaddr_nl addr;
	int sz = 64*1024;
	
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = getpid();
	addr.nl_groups = 0xFFFFFFFF;
	
	uevent_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (uevent_fd < 0)
	{
		LOGE("%s(): Unable to create uevent socket '%s'\n", __func__, strerror(errno));
		return -1;
	}
	
	if (setsockopt(uevent_fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) < 0) 
	{
		LOGE("%s(): Unable to set uevent socket options '%s'\n", __func__, strerror(errno));
		return -1;
	}
	
	if (bind(uevent_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		LOGE("%s(): Unable to bind uevent socket '%s'\n", __func__, strerror(errno));
		return -1;
	}
	
	return 0;
}

/* initialize usbd socket */
static int init_usdb_socket()
{
	usbd_socket_fd = android_get_control_socket("usbd");
	
	if (usbd_socket_fd < 0)
	{
		LOGE("%s(): Obtaining file descriptor socket 'usbd' failed: %s\n", __func__, strerror(errno));
		return 1;
	}
	
	if (listen(usbd_socket_fd, 4) < 0)
	{
		LOGE("%s(): Unable to listen on fd '%d' for socket 'usbd': %s", __func__, usbd_socket_fd, strerror(errno));
		return 1;
	}
	
	return 0;
}

/* Return highest socket fd for select */
static int get_highest_socket_fd(void)
{
	int fd;
	
	if (usbd_app_fd >= 0)
		fd = usbd_app_fd;
	
	if (usbd_socket_fd > fd)
		fd = usbd_socket_fd;
	
	if (usb_device_fd > fd)
		fd = usb_device_fd;
	
	if (uevent_fd > fd)
		fd = uevent_fd;
	
	return fd;
}

/* Gets adb status */
static int get_adb_enabled_status(void)
{
	char buff[PROPERTY_VALUE_MAX];
	int ret;
	
	ret = property_get(PROPERTY_ADB_ENABLED, buff, "0");
	if (!ret)
		return -1;
	
	return (!strcmp(buff, "1"));
}

/* Sends adb status to usb.apk */
static int usbd_send_adb_status(int sockfd, int status)
{
	int ret;
	
	if (status == 1)
	{
		LOGI("%s(): Send ADB Enable message\n", __func__);
		ret = write(sockfd, USBD_ADB_STATUS_ON, strlen(USBD_ADB_STATUS_ON) + 1);
		
	}
	else
	{
		LOGI("%s(): Send ADB Disable message\n", __func__);
		ret = write(sockfd, USBD_ADB_STATUS_OFF, strlen(USBD_ADB_STATUS_OFF) + 1);
	}
	
	return ret <= 0; /*1 = fail */
	
}

/* Get usb mode index */
static int usbd_get_mode_index(const char* mode, enum usb_mode_get_t usbmod)
{
	size_t i;
	
	for (i = 0; i < ARRAY_SIZE(usb_modes); i++)
	{
		switch (usbmod)
		{
			case USBMOD_APK:
				if (!strncmp(mode, usb_modes[i].apk_mode, strlen(usb_modes[i].apk_mode)))
					return i;
				break;
				
			case USBMOD_APK_START:
				if (!strncmp(mode, usb_modes[i].apk_start, strlen(usb_modes[i].apk_start)))
					return i;
				break;
				
			case USBMOD_APK_REQ_SWITCH:
				if (!strcmp(mode, usb_modes[i].apk_req_switch))
					return i;		
				break;
				
			case USBMOD_KERN:
				if (!strncmp(mode, usb_modes[i].kern_mode, strlen(usb_modes[i].kern_mode)))
					return i;
				
				break;
				
			default:
				LOGE("%s(): %d is not valid usb mode type\n", __func__, usbmod);
				return -1;
		}
	}
	
	return -1;
}

/* Sets usb mode */
static int usbd_set_usb_mode(int new_mode)
{
	int adb_sts;
	const char* mode_str;
	
	if (new_mode > 0 && new_mode < ((int) ARRAY_SIZE(usb_modes)))
	{		
		/* Moto gadget driver expects us to append "_adb" for adb on */
		if (get_adb_enabled_status() == 1)
			mode_str = usb_modes[new_mode].kern_mode_adb;
		else
			mode_str = usb_modes[new_mode].kern_mode;
		
		if (write(usb_device_fd, mode_str, strlen(mode_str) + 1) < 0)
			return 1;
		
		usb_current_mode = new_mode;
		return 0;
	}
	else if (new_mode == 0)
	{
		/* Unloaded */
		usb_current_mode = 0;
		return 0;
	}
	else
	{
		LOGE("%s(): Cannot set usb mode to '%d'\n", __func__, new_mode);
		return 1;
	}
}

/* Get cable status */
static int usbd_get_cable_status(void)
{
	char buffer[256];
	FILE* f;
	
	/* get cable type */
	f = fopen(USB_MODEL_NAME_PATH, "r");
	
	if (!f)
	{
		LOGE("%s: Unable to open power_supply model_name file '%s'\n", __func__, strerror(errno));
		return -errno;
	}
	
	/* Read it */
	if (!fgets(buffer, ARRAY_SIZE(buffer), f))
	{
		fclose(f);
		LOGE("%s: Unable to read power supply model_name for cable type\n", __func__);
		return -EIO;
	}
	
	/* Remove linefeed */
	buffer[strlen(buffer) - 1] = '\0';
	LOGI("%s(): cable_type = %s\n", __func__, buffer);
	
	if (!strcmp(buffer, USB_INPUT_CABLE_NORMAL))
		usb_factory_cable = 0;
	else if (!strcmp(buffer, USB_INPUT_CABLE_FACTORY))
	{
		usb_factory_cable = 1;
		usbd_set_usb_mode(usbd_get_mode_index(USB_KERN_MODE_NET, USBMOD_KERN));
	}
	
	fclose(f);
	
	/* get online status */
	f = fopen(USB_ONLINE_PATH, "r");
	
	if (!f)
	{
		LOGE("%s: Unable to open power_supply online file '%s'\n", __func__, strerror(errno));
		return -errno;
	}
	
	/* Read it */
	if (!fgets(buffer, ARRAY_SIZE(buffer), f))
	{
		fclose(f);
		LOGE("%s: Unable to read power supply online stat\n", __func__);
		return -EIO;
	}
	
	/* Remove linefeed */
	buffer[strlen(buffer) - 1] = '\0';
	if (!strcmp(buffer, "1"))
	{
		usb_online = 1;
		usb_state = USBDSTAT_CABLE_CONNECTED;
	}
	else
	{
		usb_online = 0;
		usb_state = USBDSTAT_CABLE_DISCONNECTED;
	}
	
	LOGI("%s(): current usb_online = %d\n", __func__, usb_online);
	fclose(f);
	
	return 0;
}

/* notify Usb.apk our current status */
static int usbd_notify_current_status(int sockfd)
{
	const char* event_msg = NULL;
	
	if (usb_factory_cable)
		return 0;
	
	switch(usb_state)
	{
		case USBDSTAT_CABLE_DISCONNECTED:
			event_msg = USBD_EVENT_CABLE_DISCONNECTED;
			break;
			
		case USBDSTAT_CABLE_CONNECTED:
			event_msg = USBD_EVENT_CABLE_CONNECTED;
			break;
			
		case USBDSTAT_GET_DESCRIPTOR:
			event_msg = USBD_EVENT_GET_DESCRIPTOR;
			break;
			
		case USBDSTAT_USB_ENUMERATED:
			event_msg = USBD_EVENT_USB_ENUMERATED;
			break;
	}
	
	if (event_msg)
	{
		LOGI("%s(): Notifying App with Current Status : %s\n", __func__, event_msg);
		if (write(sockfd, event_msg, strlen(event_msg) + 1) < 0)
		{
			LOGE("%s(): Write Error : Notifying App with Current Status\n", __func__);
			return 1;
		}
	}
	
	last_sent_usb_online = usb_online;
	return 0;
}

/* send usb mode to the Usb.apk */
static int usbd_enum_process(int sockfd)
{
	const char* mode;
	
	LOGI("%s(): current usb mode = %d\n", __func__, usb_current_mode);
	
	if (get_adb_enabled_status() == 1)
		mode = usb_modes[usb_current_mode].apk_start_adb;
	else
		mode = usb_modes[usb_current_mode].apk_start;
	
	if (write(sockfd, mode, strlen(mode) + 1) < 0)
	{
		LOGE("%s(): Socket Write Failure: %s\n", __func__, strerror(errno));
		return 1;
	}
	else
	{
		LOGE("%s(): enum done\n", __func__);
		return 0;
	}
	
}

/* socket event */
static int usbd_socket_event(int sockfd)
{
	char buffer[1024];
	const char* suffix;
	const char* start_msg;
	int res, new_mode;
	
	memset(buffer, 0, sizeof(buffer));
	res = read(sockfd, buffer, ARRAY_SIZE(buffer));
	
	if (res < 0)
	{
		LOGE("%s(): Socket Read Failure: %s", __func__, strerror(errno));
		return 1;
	}
	else if (res)
	{
		LOGI("%s(): recieved %s\n", __func__, buffer);
		new_mode = usbd_get_mode_index(buffer, USBMOD_APK);
		
		if (new_mode < 0)
		{
			LOGE("%s(): %s is not valid usb mode\n", __func__, buffer);
			return 1;
		}
		
		LOGI("%s(): Matched new usb mode = %d , current mode = %d\n", __func__, usb_current_mode, new_mode);
		
		if (!new_mode)
		{
			usbd_set_usb_mode(0);
			return 0;
		}
		
		if (new_mode == -1)
			suffix = USBD_RESP_FAIL;
		else
			suffix = USBD_RESP_OK;
		
		sprintf(buffer, "%s%s", usb_modes[new_mode].apk_mode, suffix);
		
		if (write(sockfd, buffer, strlen(buffer) + 1) < 0)
		{
			LOGE("%s(): Socket Write Failure: %s\n", __func__, strerror(errno));
			return 1;
		}
			
		if (new_mode != usb_current_mode)
		{
			usbd_set_usb_mode(new_mode);
			
			if (get_adb_enabled_status() == 1)
				start_msg = usb_modes[usb_current_mode].apk_start_adb;
			else
				start_msg = usb_modes[usb_current_mode].apk_start;
			
			if (write(sockfd, start_msg, strlen(start_msg) + 1) < 0)
			{
				LOGE("%s(): Socket Write Failure: %s\n", __func__, strerror(errno));
				return 1;
			}
			
		}
		
		return 0;
	}
	else
	{
		LOGI("%s(): Socket Connection Closed\n", __func__);
		return 1;
	}
}

/* Process USB message */
static int process_usb_uevent_message()
{
	char buffer[1024];
	char* power_supply_type = NULL;
	char* power_supply_online = NULL;
	char* power_supply_model_name = NULL;
	char* ptr;
	char* end;
	
	int res = recv(uevent_fd, buffer, ARRAY_SIZE(buffer), 0);
	
	if (res <= 0)
		return 0;
	
	ptr = buffer;
	end = &(buffer[res]);
	
	if (ptr > end)
		return 0;	
	
	do
	{
		if (!strncmp(POWER_SUPPLY_TYPE_EVENT, ptr, strlen(POWER_SUPPLY_TYPE_EVENT)))
			power_supply_type = ptr + strlen(POWER_SUPPLY_TYPE_EVENT);
		else if (!strncmp(POWER_SUPPLY_ONLINE_EVENT, ptr, strlen(POWER_SUPPLY_ONLINE_EVENT)))
			power_supply_online = ptr + strlen(POWER_SUPPLY_ONLINE_EVENT);
		else if (!strncmp(POWER_SUPPLY_MODEL_NAME_EVENT, ptr, strlen(POWER_SUPPLY_MODEL_NAME_EVENT)))
			power_supply_model_name = ptr + strlen(POWER_SUPPLY_MODEL_NAME_EVENT);
		
		
		ptr += strlen(ptr) + 1;
		
	} while (end > ptr);
	
	/* Now check what we got */
	
	if (!power_supply_type || strcmp(power_supply_type, "USB"))
		return 0;
	
	if (power_supply_model_name)
	{
		if (strcmp(power_supply_model_name, "usb"))
		{
			if (!strcmp(power_supply_model_name, "factory"))
				usb_factory_cable = 1;
		}
		else
			usb_factory_cable = 0;
		
		LOGI("%s(): cable type: %s\n", __func__, power_supply_model_name);
	}
	else
		LOGI("%s(): cable type not specified in USB Event\n", __func__);
	
	if (power_supply_online)
	{
		if (!strcmp(power_supply_online, "1"))
		{
			LOGI("%s(): USB online\n", __func__);
			usb_state = USBDSTAT_CABLE_CONNECTED;
			usb_online = 1;
			return 0;
		}
		else if (!strcmp(power_supply_online, "0"))
		{
			LOGI("%s(): USB offline\n", __func__);
			usb_got_descriptor = 0;
			usb_state = USBDSTAT_CABLE_DISCONNECTED;
			usb_online = 0;
			write(usb_device_fd, USBD_UEVENT_CABLE_DETACH, strlen(USBD_UEVENT_CABLE_DETACH) + 1);
			return 0;
		}
		else
		{
			LOGE("%s(): Unknown USB State\n", __func__);
			return 1;
		}
	}
	else
	{
		LOGE("%s(): Did not receive USB state\n", __func__);
		return 1;
	}
}

/* Request mode switch */
static int usb_req_mode_switch(const char* new_mode)
{
	int adb_enabled;
	int new_mode_index;
	
	adb_enabled = get_adb_enabled_status();
	
	if (adb_enabled < 0)
		return 1;
	
	new_mode_index = usbd_get_mode_index(new_mode, USBMOD_APK_REQ_SWITCH);
	
	if (new_mode_index < 0)
		return 1;
	
	LOGI("%s(): switch_req=%s\n", __func__, new_mode);
	
	if (usbd_app_fd >= 0)
	{
		LOGI("%s(): usb switch to %s\n", __func__, new_mode);
		
		if (write(usbd_app_fd, usb_modes[new_mode_index].apk_req_switch, strlen(usb_modes[new_mode_index].apk_req_switch) + 1) < 0)
		{
			LOGE("%s(): Socket Write Failure: %s\n", __func__, strerror(errno));
			close(usbd_app_fd);
			usbd_app_fd = -1;
			return 1;
		}
	}
	
	return 0;
}

/* Usbd main */
int main(int argc, char **argv)
{
	char pc_switch_buf[32];
	char adb_enable_buf[32];
	char enum_buf[32];
	char buffer[64];
	const char* cable_msg;
	const char* pch;
	struct sockaddr addr;
	int addr_len;
	int sockfd;
	fd_set socks;
	
	LOGI("%s(): Start usbd - version " USBD_VER "\n", __func__);
	
	/* init uevent */
	LOGI("%s(): Initializing uevent_socket \n", __func__);
	if (open_uevent_socket())
		return 1;
	
	/* open device mode */
	LOGI("%s(): Initializing usb_device_mode \n", __func__);
	usb_device_fd = open("/dev/usb_device_mode", O_RDWR);
	
	if (usb_device_fd < 0)
	{
		LOGE("%s(): Unable to open usb_device_mode '%s'\n", __func__, strerror(errno));
		return 1;
	}
	
	/* init usdb socket */
	LOGI("%s(): Initializing usbd socket \n", __func__);
	if (init_usdb_socket())
	{
		LOGE("%s(): failed to create socket server '%s'\n", __func__, strerror(errno));
		return 1;
	}
	
	/* init cable status */
	if (usbd_get_cable_status() < 0)
	{
		LOGE("%s(): failed to get cable status\n", __func__);
		return 1;
	}
	
	if (usb_online)
		cable_msg = "Cable Attached";
	else
		cable_msg = "Cable Detached";
	
	LOGI("%s(): Initial Cable State = %s\n", __func__, cable_msg);
	
	while (1)
	{
		/* wait for sockets */
		FD_ZERO(&socks);
		
		FD_SET(uevent_fd, &socks);
		FD_SET(usb_device_fd, &socks);
		FD_SET(usbd_socket_fd, &socks);
		
		if (usbd_app_fd >= 0)
			FD_SET(usbd_app_fd, &socks);
		
		select(get_highest_socket_fd() + 1, &socks, NULL, NULL, NULL);
		
		/* Check received data */
		
		/* uevent socket */
		if (FD_ISSET(uevent_fd, &socks) && !process_usb_uevent_message())
		{
			/* Factory cable is handled directly in process_usb_uevent_message */
			
			if (!usb_factory_cable)
			{
				if (last_sent_usb_online == usb_online)
					LOGI("%s(): Spurious Cable Event, Ignore \n", __func__);
				else
				{
					LOGI("%s(): Cable Status Changed, need to notify Cable Status to App \n", __func__);
					
					if (usbd_app_fd < 0)
						last_sent_usb_online = usb_online;
					else if (usbd_notify_current_status(usbd_app_fd) < 0)
					{
						close(usbd_app_fd);
						usbd_app_fd = -1;
						continue;
					}
					
					/* Set to no mode if we're disconnected */
					if (usb_state == USBDSTAT_CABLE_DISCONNECTED)
						usbd_set_usb_mode(0);
					
				}
			}
		}
		
		/* usb device fd */
		if (FD_ISSET(usb_device_fd, &socks))
		{
			LOGI("%s(): get event from usb_device_fd\n", __func__);
			memset(buffer, 0, sizeof(buffer));
			
			if (read(usb_device_fd, buffer, ARRAY_SIZE(buffer)) > 0 && !usb_factory_cable)
			{
				LOGI("%s(): devbuf: %s\n"
				"rc: %d usbd_curr_cable_status: %d\n", __func__, buffer, strlen(buffer), usb_state);
				
				/* PC switch buffer */
				pch = strtok(buffer, ":");
				
				if (pch != NULL)
					strcpy(pc_switch_buf, pch);
				else
					memset(pc_switch_buf, 0, sizeof(pc_switch_buf)); 
				
				LOGI("%s(): pc_switch_buf = %s\n", __func__, pc_switch_buf);
				
				/* ADB enable buffer */
				pch = strtok(NULL, ":");
				
				if (pch != NULL)
					strcpy(adb_enable_buf, pch);
				else
					memset(adb_enable_buf, 0, sizeof(adb_enable_buf)); 
				
				LOGI("%s(): adb_enable_buf = %s\n", __func__, adb_enable_buf);
				
				/* Remainder */
				pch = strtok(NULL, ":");
				
				if (pch != NULL)
				{
					strcpy(enum_buf, pch);
					LOGI("%s(): enum_buf: %s\n", __func__, enum_buf);
				}
				else
					memset(enum_buf, 0, sizeof(enum_buf));
				
				/* Evaluate adb status */
				if (usbd_app_fd >= 0)
				{
					if (!strcmp(adb_enable_buf, USBD_DEV_EVENT_ADB_ENABLE))
						usbd_send_adb_status(usbd_app_fd, 1);
					else if (!strcmp(adb_enable_buf, USBD_DEV_EVENT_ADB_DISABLE))
						usbd_send_adb_status(usbd_app_fd, 0);
				}
				
				if (pc_switch_buf[0] != '\0' && strcmp(pc_switch_buf, "none"))
					usb_req_mode_switch(pc_switch_buf);
				
				if (!strncmp(enum_buf, USBD_DEV_EVENT_GET_DESCRIPTOR, strlen(USBD_DEV_EVENT_GET_DESCRIPTOR)))
				{
					/* Make sure it is not spurious */
					usb_got_descriptor++;
					
					if (usb_got_descriptor == 1)
					{
						usb_state = USBDSTAT_GET_DESCRIPTOR;
						LOGI("%s(): received get_descriptor, enum in progress\n", __func__);
						
						if (usbd_app_fd >= 0)
						{
							LOGI("%s(): Notifying Apps that Get_Descriptor was called...\n", __func__);
							if (write(usbd_app_fd, USBD_EVENT_GET_DESCRIPTOR, strlen(USBD_EVENT_GET_DESCRIPTOR)) < 0)
							{
								close(usbd_app_fd);
								usbd_app_fd = -1;
							}
						}
					}
				}
				else if (!strncmp(enum_buf, USBD_DEV_EVENT_USB_ENUMERATED, strlen(USBD_DEV_EVENT_USB_ENUMERATED)))
				{
					LOGI("%s(): recieved enumerated\n", __func__);
					LOGI("%s(): usbd_app_fd  = %d\n", __func__, usbd_app_fd);
					usb_state = USBDSTAT_USB_ENUMERATED;
					
					if (usbd_app_fd >= 0 && usbd_enum_process(usbd_app_fd) < 0)
					{
						close(usbd_app_fd);
						usbd_app_fd = -1;
					}
					
				}
			}
		}
		
		/* socket */
		if (FD_ISSET(usbd_socket_fd, &socks))
		{
			LOGI("%s(): get event from usbd server fd\n", __func__);
			
			sockfd = accept(usbd_socket_fd, &addr, &addr_len);
			if (sockfd >= 0)
			{
				if (usbd_app_fd >= 0)
				{
					LOGI("%s(): New socket connection is not supported\n", __func__);
					close(sockfd);
				}
				else
				{
					LOGI("%s(): Estabilished socket connection with the App\n", __func__);
					
					usbd_app_fd = sockfd;
					usbd_send_adb_status(usbd_app_fd, get_adb_enabled_status());
					usbd_notify_current_status(usbd_app_fd);
				}
			}
		}
		
		if (usbd_app_fd >= 0 && FD_ISSET(usbd_app_fd, &socks))
		{
			LOGI("%s(): Read and handle a pending message from the App\n", __func__);
			
			if (usbd_socket_event(usbd_app_fd) < 0)
			{
				close(usbd_app_fd);
				usbd_app_fd = -1;
			}
		}
	}
} 
