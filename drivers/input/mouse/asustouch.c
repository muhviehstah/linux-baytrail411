/*
 * Driver for Asus T100 multitouch touchpad
 *
 * Copyright (c) 2016 Helder Filho <heldinho@gmail.com>
 * Copyright (c) 2017 Jonas Aaberg <cja@gmx.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input/mt.h>

#define USB_VENDOR_ID_ASUSTEK		0x0b05
#define USB_DEVICE_ID_ASUSTEK_TOUCHPAD_T100TA 0x17e0
#define USB_DEVICE_ID_ASUSTEK_TOUCHPAD_T100HA 0x1807
#define ASUS_T100_TOUCHPAD_INTERFACEID 2

#define PACKAGE_LEN 0x1c
#define MAX_TOUCHES 5

static const struct usb_device_id asus_devices[] = {
	 { USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		      USB_DEVICE_ID_ASUSTEK_TOUCHPAD_T100TA) },
	 { USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		      USB_DEVICE_ID_ASUSTEK_TOUCHPAD_T100HA) },
	 { }
};

MODULE_DEVICE_TABLE(usb, asus_devices);

#define ASUS_CONFIG_LEN 5
static u8 touchpad_configuration0[] = {0x0d, 0x00, 0x03, 0x01, 0x00};
static u8 touchpad_configuration1[] = {0x0d, 0x05, 0x03, 0x06, 0x01};
static u8 touchpad_configuration2[] = {0x0d, 0x05, 0x03, 0x07, 0x01};
static u8 touchpad_configuration3[] = {0x0d, 0x00, 0x03, 0x01, 0x00};

struct usb_asus {
	char			phys[64];
	struct urb		*int_in_urb;
	struct usb_interface	*intf;
	struct usb_device	*udev;
	bool			open;
	u8			*int_in_buffer;
	struct input_dev	*input;
	size_t			alloc_len;
	bool			button_pressed;
	int			button_val;
};

struct xy {
	u8 yh:4;
	u8 xh:4;
	u8 xl;
	u8 yl;
	/*
	 * The following u16 definitely means something. It is set only
	 * when a touch is detected,  and the values don't seem random.
	 */
	u16 unknown;
} __packed;

struct touchpad_package {
	u8 magic; /* 0x5d */
	u8 button_press:1;
	u8 reserved:2;
	u8 touches:MAX_TOUCHES;
	struct xy xy[MAX_TOUCHES];
} __packed;

static void asus_touchpad_irq(struct urb *urb)
{
	struct usb_asus *asus = urb->context;
	int i;
	int error;
	struct touchpad_package *tp =
		(struct touchpad_package *)asus->int_in_buffer;

	switch (urb->status) {
	case 0:
		break;

	/* Device went away so don't keep trying to read from it. */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;

	default:
		goto resubmit;
	}

	if (tp->button_press && !asus->button_pressed) {
		int x, y;
		int button_pressed_idx = __ffs(tp->touches);

		asus->button_pressed = true;

		x = (tp->xy[button_pressed_idx].xh << 8) |
			tp->xy[button_pressed_idx].xl;
		y = (tp->xy[button_pressed_idx].yh << 8) |
			tp->xy[button_pressed_idx].yl;

		if (x >= 1024 && y < 256)
			asus->button_val = BTN_RIGHT;
		else
			asus->button_val = BTN_LEFT;

		input_report_key(asus->input, asus->button_val, 1);
	}

	for (i = 0; i < MAX_TOUCHES; i++) {
		input_mt_slot(asus->input, i);
		input_mt_report_slot_state(asus->input, MT_TOOL_FINGER,
					   (tp->touches & (1 << i)) != 0);
		if (tp->touches & (1 << i)) {
			input_report_abs(asus->input, ABS_MT_POSITION_X,
					 (tp->xy[i].xh << 8) | tp->xy[i].xl);
			input_report_abs(asus->input, ABS_MT_POSITION_Y,
					 1024 - ((tp->xy[i].yh << 8) | tp->xy[i].yl));
		}
	}
	if (!tp->button_press && asus->button_pressed) {
		asus->button_pressed = false;
		input_report_key(asus->input, asus->button_val, 0);
	}

	input_report_key(asus->input, BTN_TOUCH, hweight_long(tp->touches) > 0);
	input_mt_sync_frame(asus->input);
	input_mt_report_pointer_emulation(asus->input, false);
	input_sync(asus->input);

resubmit:
	error = usb_submit_urb(asus->int_in_urb, GFP_ATOMIC);
	if (error && error != -EPERM)
		dev_err(&asus->intf->dev,
			"usb_submit_urb failed with result: %d",
			error);
}


static int hw_write(struct usb_asus *asus,
		    struct usb_interface_descriptor *desc, u8 *buf,
		    u8 *data, int size)
{
	int s;

	memcpy(buf, data, size);

	s = usb_control_msg(asus->udev, usb_sndctrlpipe(asus->udev, 0),
			    USB_REQ_SET_CONFIGURATION,
			    USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			    0x30d,
			    desc->bInterfaceNumber, buf, size,
			    USB_CTRL_SET_TIMEOUT);
	if (s != size) {
		dev_err(&asus->intf->dev, "Failed to write to device.\n");
		return -EIO;
	}
	return 0;
}

static int hw_read(struct usb_asus *asus,
		   struct usb_interface_descriptor *desc, u8 *data, int size)
{
	int s = usb_control_msg(asus->udev, usb_rcvctrlpipe(asus->udev, 0),
				USB_REQ_CLEAR_FEATURE,
				USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				0x30d,
				desc->bInterfaceNumber, data, size,
				USB_CTRL_SET_TIMEOUT);
	if (s != size) {
		dev_err(&asus->intf->dev, "Failed to read from device.\n");
		return -EIO;
	}
	return 0;
}

static int asus_touchpad_hw_cfg(struct usb_asus *asus,
				struct usb_interface_descriptor *desc)
{
	u8 buf[ASUS_CONFIG_LEN];

	if (hw_write(asus, desc, buf, touchpad_configuration0,
		     ARRAY_SIZE(touchpad_configuration0)))
		return -EIO;

	if (hw_write(asus, desc, buf, touchpad_configuration1,
		     ARRAY_SIZE(touchpad_configuration1)))
		return -EIO;

	if (hw_read(asus, desc, buf, ARRAY_SIZE(buf)))
		return -EIO;

	if (hw_write(asus, desc, buf, touchpad_configuration2,
		     ARRAY_SIZE(touchpad_configuration2)))
		return -EIO;

	if (hw_read(asus, desc, buf, ARRAY_SIZE(buf)))
		return -EIO;

	if (hw_write(asus, desc, buf, touchpad_configuration3,
		     ARRAY_SIZE(touchpad_configuration3)))
		return -EIO;
	return 0;
}

static int asus_touchpad_open(struct input_dev *input)
{
	struct usb_asus *asus = input_get_drvdata(input);

	if (usb_submit_urb(asus->int_in_urb, GFP_ATOMIC))
		return -EIO;

	asus->open = true;
	return 0;
}

static void asus_touchpad_close(struct input_dev *input)
{
	struct usb_asus *asus = input_get_drvdata(input);

	if (asus)
		usb_kill_urb(asus->int_in_urb);
	asus->open = false;
}

static int asus_touchpad_probe(struct usb_interface *intf,
			       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_asus *asus;
	struct usb_endpoint_descriptor *endpoint;
	bool endpoint_found = false;
	int error;
	int i;

	if (intf->cur_altsetting->desc.bInterfaceNumber !=
	    ASUS_T100_TOUCHPAD_INTERFACEID)
		return -ENODEV;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		endpoint = &intf->cur_altsetting->endpoint[i].desc;
		if (((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
		     == USB_DIR_IN)
		    && ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
			== USB_ENDPOINT_XFER_INT)) {
			endpoint_found = true;
			break;
		}
	}
	if (!endpoint_found) {
		dev_err(&intf->dev, "Could not find int-in endpoint\n");
		return -EIO;
	}

	asus = devm_kzalloc(&intf->dev, sizeof(struct usb_asus), GFP_KERNEL);
	if (!asus)
		return -ENOMEM;

	asus->udev = udev;
	asus->intf = intf;

	error = asus_touchpad_hw_cfg(asus, &intf->cur_altsetting->desc);
	if (error)
		return error;

	asus->input = devm_input_allocate_device(&intf->dev);
	if (!asus->input) {
		dev_err(&intf->dev, "Out of memory!\n");
		return -ENOMEM;
	}

	asus->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!asus->int_in_urb) {
		dev_err(&intf->dev, "Out of memory!\n");
		return -ENOMEM;
	}

	asus->alloc_len = le16_to_cpu(endpoint->wMaxPacketSize);

	asus->int_in_buffer = usb_alloc_coherent(asus->udev,
						 asus->alloc_len,
						 GFP_KERNEL,
						 &asus->int_in_urb->transfer_dma);
	if (!asus->int_in_buffer) {
		dev_err(&intf->dev, "Out of memory!\n");
		error = -ENOMEM;
		goto error;
	}

	usb_fill_int_urb(asus->int_in_urb, udev,
			 usb_rcvintpipe(udev,
					endpoint->bEndpointAddress),
			 asus->int_in_buffer,
			 PACKAGE_LEN,
			 asus_touchpad_irq,
			 asus,
			 endpoint->bInterval);

	usb_set_intfdata(intf, asus);

	usb_make_path(udev, asus->phys, sizeof(asus->phys));
	strlcat(asus->phys, "/input0", sizeof(asus->phys));
	asus->input->phys = asus->phys;
	usb_to_input_id(asus->udev, &asus->input->id);
	asus->input->dev.parent = &intf->dev;

	asus->input->name = "Asus T100 multitouch touchpad";

	input_set_abs_params(asus->input, ABS_MT_POSITION_X, 0, 2048, 0, 0);
	input_set_abs_params(asus->input, ABS_MT_POSITION_Y, 0, 1024, 0, 0);
	input_mt_init_slots(asus->input, MAX_TOUCHES,
			    INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED | INPUT_MT_TRACK);
	input_set_drvdata(asus->input, asus);

	asus->input->open = asus_touchpad_open;
	asus->input->close = asus_touchpad_close;

	__set_bit(EV_ABS, asus->input->evbit);
	__set_bit(BTN_TOUCH, asus->input->keybit);
	__set_bit(BTN_TOOL_FINGER, asus->input->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, asus->input->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, asus->input->keybit);
	__set_bit(BTN_TOOL_QUADTAP, asus->input->keybit);
	__set_bit(BTN_TOOL_QUINTTAP, asus->input->keybit);

	__clear_bit(EV_REL, asus->input->evbit);
	__clear_bit(REL_X, asus->input->relbit);
	__clear_bit(REL_Y, asus->input->relbit);

	__set_bit(EV_KEY, asus->input->evbit);
	__set_bit(BTN_LEFT, asus->input->keybit);
	__set_bit(BTN_RIGHT, asus->input->keybit);

	error = input_register_device(asus->input);

	if (error) {
		dev_err(&intf->dev, "Failed to register device!\n");
		goto error;
	}

	return 0;
error:
	if (asus->int_in_buffer)
		usb_free_coherent(asus->udev, asus->alloc_len,
				  asus, asus->int_in_urb->transfer_dma);
	usb_free_urb(asus->int_in_urb);
	usb_set_intfdata(intf, NULL);
	if (asus->input)
		input_set_drvdata(asus->input, NULL);

	return error;
}

static void asus_touchpad_disconnect(struct usb_interface *intf)
{
	struct usb_asus *asus = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (asus) {
		usb_kill_urb(asus->int_in_urb);
		usb_free_coherent(asus->udev, asus->alloc_len,
				  asus, asus->int_in_urb->transfer_dma);
		usb_free_urb(asus->int_in_urb);
		input_set_drvdata(asus->input, NULL);
	}
}

static int asus_touchpad_suspend(struct usb_interface *intf,
				 pm_message_t message)
{
	struct usb_asus *asus = usb_get_intfdata(intf);

	usb_kill_urb(asus->int_in_urb);
	return 0;
}

static int asus_touchpad_resume(struct usb_interface *intf)
{
	struct usb_asus *asus = usb_get_intfdata(intf);

	if (asus->open && usb_submit_urb(asus->int_in_urb, GFP_ATOMIC))
		return -EIO;

	return 0;
}

static int asus_touchpad_reset_resume(struct usb_interface *intf)
{
	struct usb_asus *asus = usb_get_intfdata(intf);
	int error = 0;

	error = asus_touchpad_hw_cfg(asus, &intf->cur_altsetting->desc);
	if (error)
		return error;

	if (asus->open && usb_submit_urb(asus->int_in_urb, GFP_ATOMIC))
		return -EIO;
	return 0;
}

static struct usb_driver asus_touchpad_driver = {
	.name	   = "asus-touchpad-driver",
	.probe	  = asus_touchpad_probe,
	.disconnect	= asus_touchpad_disconnect,
	.suspend	= asus_touchpad_suspend,
	.resume		= asus_touchpad_resume,
	.reset_resume	= asus_touchpad_reset_resume,

	.id_table       = asus_devices,
};

module_usb_driver(asus_touchpad_driver);

MODULE_LICENSE("GPL");
