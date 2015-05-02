/*
 * vc_support.c
 *
 *  Created on: 25 Nov 2012
 *      Author: Simon
 */

#include <linux/module.h>
#include <linux/platform_data/mailbox-bcm2708.h>

#ifdef ECLIPSE_IGNORE

#define __user
#define __init
#define __exit
#define __iomem
#define KERN_DEBUG
#define KERN_ERR
#define KERN_WARNING
#define KERN_INFO
#define _IOWR(a, b, c) b
#define _IOW(a, b, c) b
#define _IO(a, b) b

#endif

/****** VC MAILBOX FUNCTIONALITY ******/
unsigned int AllocateVcMemory(unsigned int *pHandle, unsigned int size, unsigned int alignment, unsigned int flags)
{
	struct vc_msg
	{
		unsigned int m_msgSize;
		unsigned int m_response;

		struct vc_tag
		{
			unsigned int m_tagId;
			unsigned int m_sendBufferSize;
			union {
				unsigned int m_sendDataSize;
				unsigned int m_recvDataSize;
			};

			struct args
			{
				union {
					unsigned int m_size;
					unsigned int m_handle;
				};
				unsigned int m_alignment;
				unsigned int m_flags;
			} m_args;
		} m_tag;

		unsigned int m_endTag;
	} msg;
	int s;

	msg.m_msgSize = sizeof(msg);
	msg.m_response = 0;
	msg.m_endTag = 0;

	//fill in the tag for the allocation command
	msg.m_tag.m_tagId = 0x3000c;
	msg.m_tag.m_sendBufferSize = 12;
	msg.m_tag.m_sendDataSize = 12;

	//fill in our args
	msg.m_tag.m_args.m_size = size;
	msg.m_tag.m_args.m_alignment = alignment;
	msg.m_tag.m_args.m_flags = flags;

	//run the command
	s = bcm_mailbox_property(&msg, sizeof(msg));

	if (s == 0 && msg.m_response == 0x80000000 && msg.m_tag.m_recvDataSize == 0x80000004)
	{
		*pHandle = msg.m_tag.m_args.m_handle;
		return 0;
	}
	else
	{
		printk(KERN_ERR "failed to allocate vc memory: s=%d response=%08x recv data size=%08x\n",
				s, msg.m_response, msg.m_tag.m_recvDataSize);
		return 1;
	}
}

unsigned int ReleaseVcMemory(unsigned int handle)
{
	struct vc_msg
	{
		unsigned int m_msgSize;
		unsigned int m_response;

		struct vc_tag
		{
			unsigned int m_tagId;
			unsigned int m_sendBufferSize;
			union {
				unsigned int m_sendDataSize;
				unsigned int m_recvDataSize;
			};

			struct args
			{
				union {
					unsigned int m_handle;
					unsigned int m_error;
				};
			} m_args;
		} m_tag;

		unsigned int m_endTag;
	} msg;
	int s;

	msg.m_msgSize = sizeof(msg);
	msg.m_response = 0;
	msg.m_endTag = 0;

	//fill in the tag for the release command
	msg.m_tag.m_tagId = 0x3000f;
	msg.m_tag.m_sendBufferSize = 4;
	msg.m_tag.m_sendDataSize = 4;

	//pass across the handle
	msg.m_tag.m_args.m_handle = handle;

	s = bcm_mailbox_property(&msg, sizeof(msg));

	if (s == 0 && msg.m_response == 0x80000000 && msg.m_tag.m_recvDataSize == 0x80000004 && msg.m_tag.m_args.m_error == 0)
		return 0;
	else
	{
		printk(KERN_ERR "failed to release vc memory: s=%d response=%08x recv data size=%08x error=%08x\n",
				s, msg.m_response, msg.m_tag.m_recvDataSize, msg.m_tag.m_args.m_error);
		return 1;
	}
}

unsigned int LockVcMemory(unsigned int *pBusAddress, unsigned int handle)
{
	struct vc_msg
	{
		unsigned int m_msgSize;
		unsigned int m_response;

		struct vc_tag
		{
			unsigned int m_tagId;
			unsigned int m_sendBufferSize;
			union {
				unsigned int m_sendDataSize;
				unsigned int m_recvDataSize;
			};

			struct args
			{
				union {
					unsigned int m_handle;
					unsigned int m_busAddress;
				};
			} m_args;
		} m_tag;

		unsigned int m_endTag;
	} msg;
	int s;

	msg.m_msgSize = sizeof(msg);
	msg.m_response = 0;
	msg.m_endTag = 0;

	//fill in the tag for the lock command
	msg.m_tag.m_tagId = 0x3000d;
	msg.m_tag.m_sendBufferSize = 4;
	msg.m_tag.m_sendDataSize = 4;

	//pass across the handle
	msg.m_tag.m_args.m_handle = handle;

	s = bcm_mailbox_property(&msg, sizeof(msg));

	if (s == 0 && msg.m_response == 0x80000000 && msg.m_tag.m_recvDataSize == 0x80000004)
	{
		//pick out the bus address
		*pBusAddress = msg.m_tag.m_args.m_busAddress;
		return 0;
	}
	else
	{
		printk(KERN_ERR "failed to lock vc memory: s=%d response=%08x recv data size=%08x\n",
				s, msg.m_response, msg.m_tag.m_recvDataSize);
		return 1;
	}
}

unsigned int UnlockVcMemory(unsigned int handle)
{
	struct vc_msg
	{
		unsigned int m_msgSize;
		unsigned int m_response;

		struct vc_tag
		{
			unsigned int m_tagId;
			unsigned int m_sendBufferSize;
			union {
				unsigned int m_sendDataSize;
				unsigned int m_recvDataSize;
			};

			struct args
			{
				union {
					unsigned int m_handle;
					unsigned int m_error;
				};
			} m_args;
		} m_tag;

		unsigned int m_endTag;
	} msg;
	int s;

	msg.m_msgSize = sizeof(msg);
	msg.m_response = 0;
	msg.m_endTag = 0;

	//fill in the tag for the unlock command
	msg.m_tag.m_tagId = 0x3000e;
	msg.m_tag.m_sendBufferSize = 4;
	msg.m_tag.m_sendDataSize = 4;

	//pass across the handle
	msg.m_tag.m_args.m_handle = handle;

	s = bcm_mailbox_property(&msg, sizeof(msg));

	//check the error code too
	if (s == 0 && msg.m_response == 0x80000000 && msg.m_tag.m_recvDataSize == 0x80000004 && msg.m_tag.m_args.m_error == 0)
		return 0;
	else
	{
		printk(KERN_ERR "failed to unlock vc memory: s=%d response=%08x recv data size=%08x error%08x\n",
				s, msg.m_response, msg.m_tag.m_recvDataSize, msg.m_tag.m_args.m_error);
		return 1;
	}
}

unsigned int ExecuteVcCode(unsigned int code,
		unsigned int r0, unsigned int r1, unsigned int r2, unsigned int r3, unsigned int r4, unsigned int r5)
{
	struct vc_msg
	{
		unsigned int m_msgSize;
		unsigned int m_response;

		struct vc_tag
		{
			unsigned int m_tagId;
			unsigned int m_sendBufferSize;
			union {
				unsigned int m_sendDataSize;
				unsigned int m_recvDataSize;
			};

			struct args
			{
				union {
					unsigned int m_pCode;
					unsigned int m_return;
				};
				unsigned int m_r0;
				unsigned int m_r1;
				unsigned int m_r2;
				unsigned int m_r3;
				unsigned int m_r4;
				unsigned int m_r5;
			} m_args;
		} m_tag;

		unsigned int m_endTag;
	} msg;
	int s;

	msg.m_msgSize = sizeof(msg);
	msg.m_response = 0;
	msg.m_endTag = 0;

	//fill in the tag for the unlock command
	msg.m_tag.m_tagId = 0x30010;
	msg.m_tag.m_sendBufferSize = 28;
	msg.m_tag.m_sendDataSize = 28;

	//pass across the handle
	msg.m_tag.m_args.m_pCode = code;
	msg.m_tag.m_args.m_r0 = r0;
	msg.m_tag.m_args.m_r1 = r1;
	msg.m_tag.m_args.m_r2 = r2;
	msg.m_tag.m_args.m_r3 = r3;
	msg.m_tag.m_args.m_r4 = r4;
	msg.m_tag.m_args.m_r5 = r5;

	s = bcm_mailbox_property(&msg, sizeof(msg));

	//check the error code too
	if (s == 0 && msg.m_response == 0x80000000 && msg.m_tag.m_recvDataSize == 0x80000004)
		return msg.m_tag.m_args.m_return;
	else
	{
		printk(KERN_ERR "failed to execute: s=%d response=%08x recv data size=%08x\n",
				s, msg.m_response, msg.m_tag.m_recvDataSize);
		return 1;
	}
}
