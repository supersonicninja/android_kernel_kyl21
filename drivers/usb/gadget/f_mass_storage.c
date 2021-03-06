/*
 * f_mass_storage.c -- Mass Storage USB Composite Function
 *
 * Copyright (C) 2003-2008 Alan Stern
 * Copyright (C) 2009 Samsung Electronics
 *                    Author: Michal Nazarewicz <m.nazarewicz@samsung.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The Mass Storage Function acts as a USB Mass Storage device,
 * appearing to the host as a disk drive or as a CD-ROM drive.  In
 * addition to providing an example of a genuinely useful composite
 * function for a USB device, it also illustrates a technique of
 * double-buffering for increased throughput.
 *
 * Function supports multiple logical units (LUNs).  Backing storage
 * for each LUN is provided by a regular file or a block device.
 * Access for each LUN can be limited to read-only.  Moreover, the
 * function can indicate that LUN is removable and/or CD-ROM.  (The
 * later implies read-only access.)
 *
 * MSF is configured by specifying a fsg_config structure.  It has the
 * following fields:
 *
 *	nluns		Number of LUNs function have (anywhere from 1
 *				to FSG_MAX_LUNS which is 8).
 *	luns		An array of LUN configuration values.  This
 *				should be filled for each LUN that
 *				function will include (ie. for "nluns"
 *				LUNs).  Each element of the array has
 *				the following fields:
 *	->filename	The path to the backing file for the LUN.
 *				Required if LUN is not marked as
 *				removable.
 *	->ro		Flag specifying access to the LUN shall be
 *				read-only.  This is implied if CD-ROM
 *				emulation is enabled as well as when
 *				it was impossible to open "filename"
 *				in R/W mode.
 *	->removable	Flag specifying that LUN shall be indicated as
 *				being removable.
 *	->cdrom		Flag specifying that LUN shall be reported as
 *				being a CD-ROM.
 *	->nofua		Flag specifying that FUA flag in SCSI WRITE(10,12)
 *				commands for this LUN shall be ignored.
 *
 *	lun_name_format	A printf-like format for names of the LUN
 *				devices.  This determines how the
 *				directory in sysfs will be named.
 *				Unless you are using several MSFs in
 *				a single gadget (as opposed to single
 *				MSF in many configurations) you may
 *				leave it as NULL (in which case
 *				"lun%d" will be used).  In the format
 *				you can use "%d" to index LUNs for
 *				MSF's with more than one LUN.  (Beware
 *				that there is only one integer given
 *				as an argument for the format and
 *				specifying invalid format may cause
 *				unspecified behaviour.)
 *	thread_name	Name of the kernel thread process used by the
 *				MSF.  You can safely set it to NULL
 *				(in which case default "file-storage"
 *				will be used).
 *
 *	vendor_name
 *	product_name
 *	release		Information used as a reply to INQUIRY
 *				request.  To use default set to NULL,
 *				NULL, 0xffff respectively.  The first
 *				field should be 8 and the second 16
 *				characters or less.
 *
 *	can_stall	Set to permit function to halt bulk endpoints.
 *				Disabled on some USB devices known not
 *				to work correctly.  You should set it
 *				to true.
 *
 * If "removable" is not set for a LUN then a backing file must be
 * specified.  If it is set, then NULL filename means the LUN's medium
 * is not loaded (an empty string as "filename" in the fsg_config
 * structure causes error).  The CD-ROM emulation includes a single
 * data track and no audio tracks; hence there need be only one
 * backing file per LUN.
 *
 *
 * MSF includes support for module parameters.  If gadget using it
 * decides to use it, the following module parameters will be
 * available:
 *
 *	file=filename[,filename...]
 *			Names of the files or block devices used for
 *				backing storage.
 *	ro=b[,b...]	Default false, boolean for read-only access.
 *	removable=b[,b...]
 *			Default true, boolean for removable media.
 *	cdrom=b[,b...]	Default false, boolean for whether to emulate
 *				a CD-ROM drive.
 *	nofua=b[,b...]	Default false, booleans for ignore FUA flag
 *				in SCSI WRITE(10,12) commands
 *	luns=N		Default N = number of filenames, number of
 *				LUNs to support.
 *	stall		Default determined according to the type of
 *				USB device controller (usually true),
 *				boolean to permit the driver to halt
 *				bulk endpoints.
 *
 * The module parameters may be prefixed with some string.  You need
 * to consult gadget's documentation or source to verify whether it is
 * using those module parameters and if it does what are the prefixes
 * (look for FSG_MODULE_PARAMETERS() macro usage, what's inside it is
 * the prefix).
 *
 *
 * Requirements are modest; only a bulk-in and a bulk-out endpoint are
 * needed.  The memory requirement amounts to two 16K buffers, size
 * configurable by a parameter.  Support is included for both
 * full-speed and high-speed operation.
 *
 * Note that the driver is slightly non-portable in that it assumes a
 * single memory/DMA buffer will be useable for bulk-in, bulk-out, and
 * interrupt-in endpoints.  With most device controllers this isn't an
 * issue, but there may be some with hardware restrictions that prevent
 * a buffer from being used by more than one endpoint.
 *
 *
 * The pathnames of the backing files and the ro settings are
 * available in the attribute files "file" and "ro" in the lun<n> (or
 * to be more precise in a directory which name comes from
 * "lun_name_format" option!) subdirectory of the gadget's sysfs
 * directory.  If the "removable" option is set, writing to these
 * files will simulate ejecting/loading the medium (writing an empty
 * line means eject) and adjusting a write-enable tab.  Changes to the
 * ro setting are not allowed when the medium is loaded or if CD-ROM
 * emulation is being used.
 *
 * When a LUN receive an "eject" SCSI request (Start/Stop Unit),
 * if the LUN is removable, the backing file is released to simulate
 * ejection.
 *
 *
 * This function is heavily based on "File-backed Storage Gadget" by
 * Alan Stern which in turn is heavily based on "Gadget Zero" by David
 * Brownell.  The driver's SCSI command interface was based on the
 * "Information technology - Small Computer System Interface - 2"
 * document from X3T9.2 Project 375D, Revision 10L, 7-SEP-93,
 * available at <http://www.t10.org/ftp/t10/drafts/s2/s2-r10l.pdf>.
 * The single exception is opcode 0x23 (READ FORMAT CAPACITIES), which
 * was based on the "Universal Serial Bus Mass Storage Class UFI
 * Command Specification" document, Revision 1.0, December 14, 1998,
 * available at
 * <http://www.usb.org/developers/devclass_docs/usbmass-ufi10.pdf>.
 */

/*
 *				Driver Design
 *
 * The MSF is fairly straightforward.  There is a main kernel
 * thread that handles most of the work.  Interrupt routines field
 * callbacks from the controller driver: bulk- and interrupt-request
 * completion notifications, endpoint-0 events, and disconnect events.
 * Completion events are passed to the main thread by wakeup calls.  Many
 * ep0 requests are handled at interrupt time, but SetInterface,
 * SetConfiguration, and device reset requests are forwarded to the
 * thread in the form of "exceptions" using SIGUSR1 signals (since they
 * should interrupt any ongoing file I/O operations).
 *
 * The thread's main routine implements the standard command/data/status
 * parts of a SCSI interaction.  It and its subroutines are full of tests
 * for pending signals/exceptions -- all this polling is necessary since
 * the kernel has no setjmp/longjmp equivalents.  (Maybe this is an
 * indication that the driver really wants to be running in userspace.)
 * An important point is that so long as the thread is alive it keeps an
 * open reference to the backing file.  This will prevent unmounting
 * the backing file's underlying filesystem and could cause problems
 * during system shutdown, for example.  To prevent such problems, the
 * thread catches INT, TERM, and KILL signals and converts them into
 * an EXIT exception.
 *
 * In normal operation the main thread is started during the gadget's
 * fsg_bind() callback and stopped during fsg_unbind().  But it can
 * also exit when it receives a signal, and there's no point leaving
 * the gadget running when the thread is dead.  At of this moment, MSF
 * provides no way to deregister the gadget when thread dies -- maybe
 * a callback functions is needed.
 *
 * To provide maximum throughput, the driver uses a circular pipeline of
 * buffer heads (struct fsg_buffhd).  In principle the pipeline can be
 * arbitrarily long; in practice the benefits don't justify having more
 * than 2 stages (i.e., double buffering).  But it helps to think of the
 * pipeline as being a long one.  Each buffer head contains a bulk-in and
 * a bulk-out request pointer (since the buffer can be used for both
 * output and input -- directions always are given from the host's
 * point of view) as well as a pointer to the buffer and various state
 * variables.
 *
 * Use of the pipeline follows a simple protocol.  There is a variable
 * (fsg->next_buffhd_to_fill) that points to the next buffer head to use.
 * At any time that buffer head may still be in use from an earlier
 * request, so each buffer head has a state variable indicating whether
 * it is EMPTY, FULL, or BUSY.  Typical use involves waiting for the
 * buffer head to be EMPTY, filling the buffer either by file I/O or by
 * USB I/O (during which the buffer head is BUSY), and marking the buffer
 * head FULL when the I/O is complete.  Then the buffer will be emptied
 * (again possibly by USB I/O, during which it is marked BUSY) and
 * finally marked EMPTY again (possibly by a completion routine).
 *
 * A module parameter tells the driver to avoid stalling the bulk
 * endpoints wherever the transport specification allows.  This is
 * necessary for some UDCs like the SuperH, which cannot reliably clear a
 * halt on a bulk endpoint.  However, under certain circumstances the
 * Bulk-only specification requires a stall.  In such cases the driver
 * will halt the endpoint and set a flag indicating that it should clear
 * the halt in software during the next device reset.  Hopefully this
 * will permit everything to work correctly.  Furthermore, although the
 * specification allows the bulk-out endpoint to halt when the host sends
 * too much data, implementing this would cause an unavoidable race.
 * The driver will always use the "no-stall" approach for OUT transfers.
 *
 * One subtle point concerns sending status-stage responses for ep0
 * requests.  Some of these requests, such as device reset, can involve
 * interrupting an ongoing file I/O operation, which might take an
 * arbitrarily long time.  During that delay the host might give up on
 * the original ep0 request and issue a new one.  When that happens the
 * driver should not notify the host about completion of the original
 * request, as the host will no longer be waiting for it.  So the driver
 * assigns to each ep0 request a unique tag, and it keeps track of the
 * tag value of the request associated with a long-running exception
 * (device-reset, interface-change, or configuration-change).  When the
 * exception handler is finished, the status-stage response is submitted
 * only if the current ep0 request tag is equal to the exception request
 * tag.  Thus only the most recently received ep0 request will get a
 * status-stage response.
 *
 * Warning: This driver source file is too long.  It ought to be split up
 * into a header file plus about 3 separate .c files, to handle the details
 * of the Gadget, USB Mass Storage, and SCSI protocols.
 */

/*
 * This software is contributed or developed by KYOCERA Corporation.
 * (C) 2011 KYOCERA Corporation
 * (C) 2012 KYOCERA Corporation
 */

/* #define VERBOSE_DEBUG */
/* #define DUMP_MSGS */

#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/freezer.h>
#include <linux/utsname.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/composite.h>

#include <linux/switch.h>

#include "gadget_chips.h"

#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/usb/f_mass_ioctl.h>

/*------------------------------------------------------------------------*/

#define FSG_DRIVER_DESC		"Mass Storage Function"
#define FSG_DRIVER_VERSION	"2009/09/11"

static const char fsg_string_interface[] = "Mass Storage";

#define VENDOR_NAME		"KYOCERA"
#define PRODUCT_NAME		"KYL21"
#define RELEASE_NO		0x0100
#define KC_VENDOR_NAME		"change_mode"

#define FSG_NO_INTR_EP 1
#define FSG_NO_DEVICE_STRINGS    1
#define FSG_NO_OTG               1
#define FSG_NO_INTR_EP           1

#define FSYNC_CYCLE_MAX	64

#include "storage_common.c"

#ifdef CONFIG_USB_CSW_HACK
static int write_error_after_csw_sent;
static int csw_hack_sent;
#endif
/*-------------------------------------------------------------------------*/

struct fsg_dev;
struct fsg_common;
struct unq_vendor_info;
/* FSF callback functions */
struct fsg_operations {
	/*
	 * Callback function to call when thread exits.  If no
	 * callback is set or it returns value lower then zero MSF
	 * will force eject all LUNs it operates on (including those
	 * marked as non-removable or with prevent_medium_removal flag
	 * set).
	 */
	int (*thread_exits)(struct fsg_common *common);

	/*
	 * Called prior to ejection.  Negative return means error,
	 * zero means to continue with ejection, positive means not to
	 * eject.
	 */
	int (*pre_eject)(struct fsg_common *common,
			 struct fsg_lun *lun, int num);
	/*
	 * Called after ejection.  Negative return means error, zero
	 * or positive is just a success.
	 */
	int (*post_eject)(struct fsg_common *common,
			  struct fsg_lun *lun, int num);
};

/* Data shared by all the FSG instances. */
struct fsg_common {
	struct usb_gadget	*gadget;
	struct usb_composite_dev *cdev;
	struct fsg_dev		*fsg, *new_fsg;
	wait_queue_head_t	fsg_wait;

	/* filesem protects: backing files in use */
	struct rw_semaphore	filesem;

	/* lock protects: state, all the req_busy's */
	spinlock_t		lock;

	struct usb_ep		*ep0;		/* Copy of gadget->ep0 */
	struct usb_request	*ep0req;	/* Copy of cdev->req */
	unsigned int		ep0_req_tag;

	struct fsg_buffhd	*next_buffhd_to_fill;
	struct fsg_buffhd	*next_buffhd_to_drain;
	struct fsg_buffhd	buffhds[FSG_NUM_BUFFERS];

	int			cmnd_size;
	u8			cmnd[MAX_COMMAND_SIZE];

	unsigned int		nluns;
	unsigned int		lun;
	struct fsg_lun		*luns;
	struct fsg_lun		*curlun;

	unsigned int		bulk_out_maxpacket;
	enum fsg_state		state;		/* For exception handling */
	unsigned int		exception_req_tag;

	enum data_direction	data_dir;
	u32			data_size;
	u32			data_size_from_cmnd;
	u32			tag;
	u32			residue;
	u32			usb_amount_left;

	unsigned int		can_stall:1;
	unsigned int		free_storage_on_release:1;
	unsigned int		phase_error:1;
	unsigned int		short_packet_received:1;
	unsigned int		bad_lun_okay:1;
	unsigned int		running:1;

	int			thread_wakeup_needed;
	struct completion	thread_notifier;
	struct task_struct	*thread_task;

	/* Callback functions. */
	const struct fsg_operations	*ops;
	/* Gadget's private data. */
	void			*private_data;

	/*
	 * Vendor (8 chars), product (16 chars), release (4
	 * hexadecimal digits) and NUL byte
	 */
	char inquiry_string[8 + 16 + 4 + 1];

	struct kref		ref;
	struct unq_vendor_info*	unq_vendor;
	/* vendor_cmd (1 char), vendor_data (16 chars) */
	unsigned char vendor_data[1 + 16];
	struct switch_dev	sdev;

	unsigned int		fsync_counter;
};

struct fsg_config {
	unsigned nluns;
	struct fsg_lun_config {
		const char *filename;
		char ro;
		char removable;
		char cdrom;
		char nofua;
	} luns[FSG_MAX_LUNS];

	const char		*lun_name_format;
	const char		*thread_name;

	/* Callback functions. */
	const struct fsg_operations	*ops;
	/* Gadget's private data. */
	void			*private_data;

	const char *vendor_name;		/*  8 characters or less */
	const char *product_name;		/* 16 characters or less */
	u16 release;

	char			can_stall;
};

struct fsg_dev {
	struct usb_function	function;
	struct usb_gadget	*gadget;	/* Copy of cdev->gadget */
	struct fsg_common	*common;

	u16			interface_number;

	unsigned int		bulk_in_enabled:1;
	unsigned int		bulk_out_enabled:1;

	unsigned long		atomic_bitflags;
#define IGNORE_BULK_OUT		0

	struct usb_ep		*bulk_in;
	struct usb_ep		*bulk_out;

};

/* #define VENDOR_UNIQ_DEBUG_ON */
#ifdef VENDOR_UNIQ_DEBUG_ON
#define VUNIQ_ERR(stuff...)	printk("vuniq: "stuff)
#define VUNIQ_DBG(stuff...)	printk("vuniq: "stuff)
#define VUNIQ_DUMP(buf, length) print_hex_dump(KERN_DEBUG, "vuniq: ",  \
					DUMP_PREFIX_OFFSET,         \
					16, 1, buf, length, 0)
#else
#define VUNIQ_ERR(stuff...)	printk("vuniq: "stuff)
#define VUNIQ_DBG(stuff...)     do { } while (0)
#define VUNIQ_DUMP(buf, size)   do { } while (0)

#endif

static int vender_unique_command(struct fsg_common *common,
		struct fsg_buffhd *bh, int32_t* reply_ptr);
static int usbmass_open(struct inode *inode, struct file *filp);
static unsigned int  usbmass_poll(struct file *filp, poll_table *wait);
static long usbmass_ioctl(struct file *filp, uint32_t cmd, u_long arg);
static const struct file_operations usbmass_fops = {
	.owner = THIS_MODULE,
	.poll = usbmass_poll,
	.open = usbmass_open,
	.unlocked_ioctl= usbmass_ioctl,
};
static struct miscdevice usbmass_dev_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "usbmass",
	.fops = &usbmass_fops,
};
struct usbmass_device {
	spinlock_t          lock;
	wait_queue_head_t   rwait;
	wait_queue_head_t   wwait;
	int rcomplete;
	int wcomplete;
	int connect_chg;
};

enum vendor_sequence {
	VSEQ_NONE,
	VSEQ_IO_EVENT_WAIT,
	VSEQ_TX_DATA,
	VSEQ_RX_DATA,
	VSEQ_DO_READ,
	VSEQ_DO_WRITE,
	VSEQ_NO_DATA,
	VSEQ_INVALID_CMD,
	VSEQ_ERROR,
};
struct unq_vendor_info {
	enum vendor_sequence vseq;
	int vendor_exit;
};
struct usbmass_device usbmass_dev;
struct ioc_startinfo_type* g_vendor_info = NULL;

static inline int __fsg_is_set(struct fsg_common *common,
			       const char *func, unsigned line)
{
	if (common->fsg)
		return 1;
	ERROR(common, "common->fsg is NULL in %s at %u\n", func, line);
	WARN_ON(1);
	return 0;
}

#define fsg_is_set(common) likely(__fsg_is_set(common, __func__, __LINE__))

static inline struct fsg_dev *fsg_from_func(struct usb_function *f)
{
	return container_of(f, struct fsg_dev, function);
}

typedef void (*fsg_routine_t)(struct fsg_dev *);
static int send_status(struct fsg_common *common);

static int exception_in_progress(struct fsg_common *common)
{
	return common->state > FSG_STATE_IDLE;
}

/* Make bulk-out requests be divisible by the maxpacket size */
static void set_bulk_out_req_length(struct fsg_common *common,
				    struct fsg_buffhd *bh, unsigned int length)
{
	unsigned int	rem;

	bh->bulk_out_intended_length = length;
	rem = length % common->bulk_out_maxpacket;
	if (rem > 0)
		length += common->bulk_out_maxpacket - rem;
	bh->outreq->length = length;
}


/*-------------------------------------------------------------------------*/

static int fsg_set_halt(struct fsg_dev *fsg, struct usb_ep *ep)
{
	const char	*name;

	if (ep == fsg->bulk_in)
		name = "bulk-in";
	else if (ep == fsg->bulk_out)
		name = "bulk-out";
	else
		name = ep->name;
	DBG(fsg, "%s set halt\n", name);
	return usb_ep_set_halt(ep);
}


/*-------------------------------------------------------------------------*/

/* These routines may be called in process context or in_irq */

/* Caller must hold fsg->lock */
static void wakeup_thread(struct fsg_common *common)
{
	/* Tell the main thread that something has happened */
	common->thread_wakeup_needed = 1;
	if (common->thread_task)
		wake_up_process(common->thread_task);
}

static void raise_exception(struct fsg_common *common, enum fsg_state new_state)
{
	unsigned long		flags;

	/*
	 * Do nothing if a higher-priority exception is already in progress.
	 * If a lower-or-equal priority exception is in progress, preempt it
	 * and notify the main thread by sending it a signal.
	 */
	spin_lock_irqsave(&common->lock, flags);
	if (common->state <= new_state) {
		common->exception_req_tag = common->ep0_req_tag;
		common->state = new_state;
		if (common->thread_task)
			send_sig_info(SIGUSR1, SEND_SIG_FORCED,
				      common->thread_task);
	}
	spin_unlock_irqrestore(&common->lock, flags);
}


/*-------------------------------------------------------------------------*/

static int ep0_queue(struct fsg_common *common)
{
	int	rc;

	rc = usb_ep_queue(common->ep0, common->ep0req, GFP_ATOMIC);
	common->ep0->driver_data = common;
	if (rc != 0 && rc != -ESHUTDOWN) {
		/* We can't do much more than wait for a reset */
		WARNING(common, "error in submission: %s --> %d\n",
			common->ep0->name, rc);
	}
	return rc;
}


/*-------------------------------------------------------------------------*/

/* Completion handlers. These always run in_irq. */

static void bulk_in_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct fsg_common	*common = ep->driver_data;
	struct fsg_buffhd	*bh = req->context;

	if (req->status || req->actual != req->length)
		DBG(common, "%s --> %d, %u/%u\n", __func__,
		    req->status, req->actual, req->length);
	if (req->status == -ECONNRESET)		/* Request was cancelled */
		usb_ep_fifo_flush(ep);

	/* Hold the lock while we update the request and buffer states */
	smp_wmb();
	spin_lock(&common->lock);
	bh->inreq_busy = 0;
	bh->state = BUF_STATE_EMPTY;
	wakeup_thread(common);
	spin_unlock(&common->lock);
}

static void bulk_out_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct fsg_common	*common = ep->driver_data;
	struct fsg_buffhd	*bh = req->context;

	dump_msg(common, "bulk-out", req->buf, req->actual);
	if (req->status || req->actual != bh->bulk_out_intended_length)
		DBG(common, "%s --> %d, %u/%u\n", __func__,
		    req->status, req->actual, bh->bulk_out_intended_length);
	if (req->status == -ECONNRESET)		/* Request was cancelled */
		usb_ep_fifo_flush(ep);

	/* Hold the lock while we update the request and buffer states */
	smp_wmb();
	spin_lock(&common->lock);
	bh->outreq_busy = 0;
	bh->state = BUF_STATE_FULL;
	wakeup_thread(common);
	spin_unlock(&common->lock);
}

static int fsg_setup(struct usb_function *f,
		     const struct usb_ctrlrequest *ctrl)
{
	struct fsg_dev		*fsg = fsg_from_func(f);
	struct usb_request	*req = fsg->common->ep0req;
	u16			w_index = le16_to_cpu(ctrl->wIndex);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u16			w_length = le16_to_cpu(ctrl->wLength);

	if (!fsg_is_set(fsg->common))
		return -EOPNOTSUPP;

	++fsg->common->ep0_req_tag;	/* Record arrival of a new request */
	req->context = NULL;
	req->length = 0;
	dump_msg(fsg, "ep0-setup", (u8 *) ctrl, sizeof(*ctrl));

	switch (ctrl->bRequest) {

	case USB_BULK_RESET_REQUEST:
		if (ctrl->bRequestType !=
		    (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE))
			break;
		if (w_value != 0)
			return -EDOM;

		/*
		 * Raise an exception to stop the current operation
		 * and reinitialize our state.
		 */
		DBG(fsg, "bulk reset request\n");
		raise_exception(fsg->common, FSG_STATE_RESET);
		return DELAYED_STATUS;

	case USB_BULK_GET_MAX_LUN_REQUEST:
		if (ctrl->bRequestType !=
		    (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE))
			break;
		if (w_value != 0)
			return -EDOM;
		VDBG(fsg, "get max LUN\n");
		*(u8 *)req->buf = fsg->common->nluns - 1;

		/* Respond with data/status */
		req->length = min((u16)1, w_length);
		return ep0_queue(fsg->common);
	}

	VDBG(fsg,
	     "unknown class-specific control req %02x.%02x v%04x i%04x l%u\n",
	     ctrl->bRequestType, ctrl->bRequest,
	     le16_to_cpu(ctrl->wValue), w_index, w_length);
	return -EOPNOTSUPP;
}


/*-------------------------------------------------------------------------*/

/* All the following routines run in process context */

/* Use this for bulk or interrupt transfers, not ep0 */
static void start_transfer(struct fsg_dev *fsg, struct usb_ep *ep,
			   struct usb_request *req, int *pbusy,
			   enum fsg_buffer_state *state)
{
	int	rc;

	if (ep == fsg->bulk_in)
		dump_msg(fsg, "bulk-in", req->buf, req->length);

	spin_lock_irq(&fsg->common->lock);
	*pbusy = 1;
	*state = BUF_STATE_BUSY;
	spin_unlock_irq(&fsg->common->lock);
	rc = usb_ep_queue(ep, req, GFP_KERNEL);
	if (rc != 0) {
		*pbusy = 0;
		*state = BUF_STATE_EMPTY;

		/* We can't do much more than wait for a reset */

		/*
		 * Note: currently the net2280 driver fails zero-length
		 * submissions if DMA is enabled.
		 */
		if (rc != -ESHUTDOWN &&
		    !(rc == -EOPNOTSUPP && req->length == 0))
			WARNING(fsg, "error in submission: %s --> %d\n",
				ep->name, rc);
	}
}

static bool start_in_transfer(struct fsg_common *common, struct fsg_buffhd *bh)
{
	if (!fsg_is_set(common))
		return false;
	start_transfer(common->fsg, common->fsg->bulk_in,
		       bh->inreq, &bh->inreq_busy, &bh->state);
	return true;
}

static bool start_out_transfer(struct fsg_common *common, struct fsg_buffhd *bh)
{
	if (!fsg_is_set(common))
		return false;
	start_transfer(common->fsg, common->fsg->bulk_out,
		       bh->outreq, &bh->outreq_busy, &bh->state);
	return true;
}

static int sleep_thread(struct fsg_common *common)
{
	int	rc = 0;

	/* Wait until a signal arrives or we are woken up */
	for (;;) {
		try_to_freeze();
		set_current_state(TASK_INTERRUPTIBLE);
		if (signal_pending(current)) {
			rc = -EINTR;
			break;
		}
		if (common->thread_wakeup_needed)
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	common->thread_wakeup_needed = 0;
	return rc;
}


/*-------------------------------------------------------------------------*/

static int do_read(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	u32			lba;
	struct fsg_buffhd	*bh;
	int			rc;
	u32			amount_left;
	loff_t			file_offset, file_offset_tmp;
	unsigned int		amount;
	ssize_t			nread;
#ifdef CONFIG_USB_MSC_PROFILING
	ktime_t			start, diff;
#endif

	/*
	 * Get the starting Logical Block Address and check that it's
	 * not too big.
	 */
	if (common->cmnd[0] == READ_6)
		lba = get_unaligned_be24(&common->cmnd[1]);
	else {
		lba = get_unaligned_be32(&common->cmnd[2]);

		/*
		 * We allow DPO (Disable Page Out = don't save data in the
		 * cache) and FUA (Force Unit Access = don't read from the
		 * cache), but we don't implement them.
		 */
		if ((common->cmnd[1] & ~0x18) != 0) {
			curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
			return -EINVAL;
		}
	}
	if (lba >= curlun->num_sectors) {
		curlun->sense_data = SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		return -EINVAL;
	}
	file_offset = ((loff_t) lba) << curlun->blkbits;

	/* Carry out the file reads */
	amount_left = common->data_size_from_cmnd;
	if (unlikely(amount_left == 0))
		return -EIO;		/* No default reply */

	for (;;) {
		/*
		 * Figure out how much we need to read:
		 * Try to read the remaining amount.
		 * But don't read more than the buffer size.
		 * And don't try to read past the end of the file.
		 */
		amount = min(amount_left, FSG_BUFLEN);
		amount = min((loff_t)amount,
			     curlun->file_length - file_offset);

		/* Wait for the next buffer to become available */
		bh = common->next_buffhd_to_fill;
		while (bh->state != BUF_STATE_EMPTY) {
			rc = sleep_thread(common);
			if (rc)
				return rc;
		}

		/*
		 * If we were asked to read past the end of file,
		 * end with an empty buffer.
		 */
		if (amount == 0) {
			curlun->sense_data =
					SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
			curlun->sense_data_info =
					file_offset >> curlun->blkbits;
			curlun->info_valid = 1;
			bh->inreq->length = 0;
			bh->state = BUF_STATE_FULL;
			break;
		}

		/* Perform the read */
		file_offset_tmp = file_offset;

#ifdef CONFIG_USB_MSC_PROFILING
		start = ktime_get();
#endif
		nread = vfs_read(curlun->filp,
				 (char __user *)bh->buf,
				 amount, &file_offset_tmp);
		VLDBG(curlun, "file read %u @ %llu -> %d\n", amount,
		     (unsigned long long) file_offset, (int) nread);
#ifdef CONFIG_USB_MSC_PROFILING
		diff = ktime_sub(ktime_get(), start);
		curlun->perf.rbytes += nread;
		curlun->perf.rtime = ktime_add(curlun->perf.rtime, diff);
#endif
		if (signal_pending(current))
			return -EINTR;

		if (nread < 0) {
			LDBG(curlun, "error in file read: %d\n", (int)nread);
			nread = 0;
		} else if (nread < amount) {
			LDBG(curlun, "partial file read: %d/%u\n",
			     (int)nread, amount);
			nread = round_down(nread, curlun->blksize);
		}
		file_offset  += nread;
		amount_left  -= nread;
		common->residue -= nread;

		/*
		 * Except at the end of the transfer, nread will be
		 * equal to the buffer size, which is divisible by the
		 * bulk-in maxpacket size.
		 */
		bh->inreq->length = nread;
		bh->state = BUF_STATE_FULL;

		/* If an error occurred, report it and its position */
		if (nread < amount) {
			curlun->sense_data = SS_UNRECOVERED_READ_ERROR;
			curlun->sense_data_info =
					file_offset >> curlun->blkbits;
			curlun->info_valid = 1;
			break;
		}

		if (amount_left == 0)
			break;		/* No more left to read */

		/* Send this buffer and go read some more */
		bh->inreq->zero = 0;
		if (!start_in_transfer(common, bh))
			/* Don't know what to do if common->fsg is NULL */
			return -EIO;
		common->next_buffhd_to_fill = bh->next;
	}

	return -EIO;		/* No default reply */
}

/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : vender read command */
/*-------------------------------------------------------------------------*/
static int do_read_buffer(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	struct fsg_buffhd	*bh;
	int			rc;
	u32			amount_left;
	loff_t			file_offset;
	unsigned int		amount;
	struct op_desc		*desc = 0;

	file_offset = get_unaligned_be32(&common->cmnd[2]);

	/* Get the starting Logical Block Address and check that it's
	 * not too big */
//	printk("%s: cmd=%d\n", __func__, common->cmnd[0]);
	desc = curlun->op_desc[common->cmnd[0]-SC_VENDOR_START];
	if (!desc->buffer){
		printk("%s: cmd=%d not ready\n", __func__, common->cmnd[0]);
		curlun->sense_data =
				SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		curlun->sense_data_info = file_offset;
		curlun->info_valid = 1;
/* [ADD START] 2011/09/30 KDDI : no response set */
		bh = common->next_buffhd_to_fill;
		bh->inreq->length = 0;
		bh->state = BUF_STATE_FULL;
/* [ADD END] 2011/09/30 KDDI : no responsea set */
		return -EIO;		/* No default reply */
	}


	/* Carry out the file reads */
	amount_left = common->data_size_from_cmnd;

/* [ADD START] 2011/09/30 KDDI : check offset before read data */
	if (file_offset + amount_left > desc->len) {
		printk("[fms_CR7]%s: vendor buffer out of range offset=0x%x read-len=0x%x buf-len=0x%x\n",
		__func__, (unsigned int)file_offset, amount_left, desc->len);
		curlun->sense_data =
				SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		curlun->sense_data_info = file_offset;
		curlun->info_valid = 1;
		bh = common->next_buffhd_to_fill;
		bh->inreq->length = 0;
		bh->state = BUF_STATE_FULL;
		return -EIO;		/* No default reply */
	}
/* [ADD END] 2011/09/30 KDDI : check offset before read data */

//	printk("[fms_CR7]%s: amount_left=%x\n", __func__, amount_left);
	if (unlikely(amount_left == 0))
		return -EIO;		/* No default reply */

//	printk("[fms_CR7]%s: buf_size=%x\n", __func__, common->buf_size);

	for (;;) {
//		printk("[fms_CR7]%s: file_offset=%x\n", __func__, (unsigned int)file_offset);

		/* Figure out how much we need to read:
		 * Try to read the remaining amount.
		 * But don't read more than the buffer size.
		 * And don't try to read past the end of the file.
		 * Finally, if we're not at a page boundary, don't read past
		 *	the next page.
		 * If this means reading 0 then we were asked to read past
		 *	the end of file. */
		amount = min(amount_left, FSG_BUFLEN);
		amount = min((loff_t) amount, desc->len - file_offset);
//		printk("[fms_CR7]%s: amount=%x\n", __func__, amount);

		/* Wait for the next buffer to become available */
		bh = common->next_buffhd_to_fill;
		while (bh->state != BUF_STATE_EMPTY) {
			rc = sleep_thread(common);
			if (rc)
				return rc;
		}
//		printk("[fms_CR7]%s: wait buffer ok\n", __func__);

		/* If we were asked to read past the end of file,
		 * end with an empty buffer. */
		if (amount == 0) {
			curlun->sense_data =
					SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
			curlun->sense_data_info = file_offset;
			curlun->info_valid = 1;
			bh->inreq->length = 0;
			bh->state = BUF_STATE_FULL;
			break;
		}

		memcpy((char __user *) bh->buf, desc->buffer + file_offset, amount);

		file_offset  += amount;
		amount_left  -= amount;
		common->residue -= amount;
		bh->inreq->length = amount;
		bh->state = BUF_STATE_FULL;

		if (amount_left == 0)
			break;		/* No more left to read */

		/* Send this buffer and go read some more */
/* [CHANGE START] 2012/01/17 KDDI : Android ICS */
#if 0
		START_TRANSFER_OR(common, bulk_in, bh->inreq,
				&bh->inreq_busy, &bh->state)
			/* Don't know what to do if
			 * common->fsg is NULL */
#else
		bh->inreq->zero = 0;
		if (!start_in_transfer(common, bh))
			/* Don't know what to do if common->fsg is NULL */
#endif
/* [CHANGE END] 2012/01/17 KDDI : Android ICS */
			return -EIO;
		common->next_buffhd_to_fill = bh->next;
	}
	return -EIO;		/* No default reply */
}
/* [ADD END] 2011/04/15 KDDI : vender read command*/
/* [ADD END] 2012/01/17 KDDI : Android ICS */

/*-------------------------------------------------------------------------*/

static int do_write(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	u32			lba;
	struct fsg_buffhd	*bh;
	int			get_some_more;
	u32			amount_left_to_req, amount_left_to_write;
	loff_t			usb_offset, file_offset, file_offset_tmp;
	unsigned int		amount;
	ssize_t			nwritten;
	int			rc;

#ifdef CONFIG_USB_CSW_HACK
	int			i;
#endif

#ifdef CONFIG_USB_MSC_PROFILING
	ktime_t			start, diff;
#endif
	if (curlun->ro) {
		curlun->sense_data = SS_WRITE_PROTECTED;
		return -EINVAL;
	}
	spin_lock(&curlun->filp->f_lock);
	curlun->filp->f_flags &= ~O_SYNC;	/* Default is not to wait */
	spin_unlock(&curlun->filp->f_lock);

	/*
	 * Get the starting Logical Block Address and check that it's
	 * not too big
	 */
	if (common->cmnd[0] == WRITE_6)
		lba = get_unaligned_be24(&common->cmnd[1]);
	else {
		lba = get_unaligned_be32(&common->cmnd[2]);

		/*
		 * We allow DPO (Disable Page Out = don't save data in the
		 * cache) and FUA (Force Unit Access = write directly to the
		 * medium).  We don't implement DPO; we implement FUA by
		 * performing synchronous output.
		 */
		if (common->cmnd[1] & ~0x18) {
			curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
			return -EINVAL;
		}
		if (!curlun->nofua && (common->cmnd[1] & 0x08)) { /* FUA */
			spin_lock(&curlun->filp->f_lock);
			curlun->filp->f_flags |= O_SYNC;
			spin_unlock(&curlun->filp->f_lock);
		}
	}
	if (lba >= curlun->num_sectors) {
		curlun->sense_data = SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		return -EINVAL;
	}

	/* Carry out the file writes */
	get_some_more = 1;
	file_offset = usb_offset = ((loff_t) lba) << curlun->blkbits;
	amount_left_to_req = common->data_size_from_cmnd;
	amount_left_to_write = common->data_size_from_cmnd;

	while (amount_left_to_write > 0) {

		/* Queue a request for more data from the host */
		bh = common->next_buffhd_to_fill;
		if (bh->state == BUF_STATE_EMPTY && get_some_more) {

			/*
			 * Figure out how much we want to get:
			 * Try to get the remaining amount,
			 * but not more than the buffer size.
			 */
			amount = min(amount_left_to_req, FSG_BUFLEN);

			/* Beyond the end of the backing file? */
			if (usb_offset >= curlun->file_length) {
				get_some_more = 0;
				curlun->sense_data =
					SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
				curlun->sense_data_info =
					usb_offset >> curlun->blkbits;
				curlun->info_valid = 1;
				continue;
			}

			/* Get the next buffer */
			usb_offset += amount;
			common->usb_amount_left -= amount;
			amount_left_to_req -= amount;
			if (amount_left_to_req == 0)
				get_some_more = 0;

			/*
			 * Except at the end of the transfer, amount will be
			 * equal to the buffer size, which is divisible by
			 * the bulk-out maxpacket size.
			 */
			set_bulk_out_req_length(common, bh, amount);
			if (!start_out_transfer(common, bh))
				/* Dunno what to do if common->fsg is NULL */
				return -EIO;
			common->next_buffhd_to_fill = bh->next;
			continue;
		}

		/* Write the received data to the backing file */
		bh = common->next_buffhd_to_drain;
		if (bh->state == BUF_STATE_EMPTY && !get_some_more)
			break;			/* We stopped early */
#ifdef CONFIG_USB_CSW_HACK
		/*
		 * If the csw packet is already submmitted to the hardware,
		 * by marking the state of buffer as full, then by checking
		 * the residue, we make sure that this csw packet is not
		 * written on to the storage media.
		 */
		if (bh->state == BUF_STATE_FULL && common->residue) {
#else
		if (bh->state == BUF_STATE_FULL) {
#endif
			smp_rmb();
			common->next_buffhd_to_drain = bh->next;
			bh->state = BUF_STATE_EMPTY;

			/* Did something go wrong with the transfer? */
			if (bh->outreq->status != 0) {
				curlun->sense_data = SS_COMMUNICATION_FAILURE;
				curlun->sense_data_info =
					file_offset >> curlun->blkbits;
				curlun->info_valid = 1;
				break;
			}

			amount = bh->outreq->actual;
			if (curlun->file_length - file_offset < amount) {
				LERROR(curlun,
				       "write %u @ %llu beyond end %llu\n",
				       amount, (unsigned long long)file_offset,
				       (unsigned long long)curlun->file_length);
				amount = curlun->file_length - file_offset;
			}

			/* Don't accept excess data.  The spec doesn't say
			 * what to do in this case.  We'll ignore the error.
			 */
			amount = min(amount, bh->bulk_out_intended_length);

			/* Don't write a partial block */
			amount = round_down(amount, curlun->blksize);
			if (amount == 0)
				goto empty_write;

			/* Perform the write */
			file_offset_tmp = file_offset;
#ifdef CONFIG_USB_MSC_PROFILING
			start = ktime_get();
#endif
			common->fsync_counter++;
			nwritten = vfs_write(curlun->filp,
					     (char __user *)bh->buf,
					     amount, &file_offset_tmp);
			VLDBG(curlun, "file write %u @ %llu -> %d\n", amount,
			      (unsigned long long)file_offset, (int)nwritten);
			if (common->fsync_counter % FSYNC_CYCLE_MAX == 0) {
				fsg_lun_fsync_sub(curlun);
			}
#ifdef CONFIG_USB_MSC_PROFILING
			diff = ktime_sub(ktime_get(), start);
			curlun->perf.wbytes += nwritten;
			curlun->perf.wtime =
					ktime_add(curlun->perf.wtime, diff);
#endif
			if (signal_pending(current))
				return -EINTR;		/* Interrupted! */

			if (nwritten < 0) {
				LDBG(curlun, "error in file write: %d\n",
				     (int)nwritten);
				nwritten = 0;
			} else if (nwritten < amount) {
				LDBG(curlun, "partial file write: %d/%u\n",
				     (int)nwritten, amount);
				nwritten = round_down(nwritten, curlun->blksize);
			}
			file_offset += nwritten;
			amount_left_to_write -= nwritten;
			common->residue -= nwritten;

			/* If an error occurred, report it and its position */
			if (nwritten < amount) {
				curlun->sense_data = SS_WRITE_ERROR;
				curlun->sense_data_info =
					file_offset >> curlun->blkbits;
				curlun->info_valid = 1;
#ifdef CONFIG_USB_CSW_HACK
				write_error_after_csw_sent = 1;
				goto write_error;
#endif
				break;
			}

#ifdef CONFIG_USB_CSW_HACK
write_error:
			if ((nwritten == amount) && !csw_hack_sent) {
				if (write_error_after_csw_sent)
					break;
				/*
				 * Check if any of the buffer is in the
				 * busy state, if any buffer is in busy state,
				 * means the complete data is not received
				 * yet from the host. So there is no point in
				 * csw right away without the complete data.
				 */
				for (i = 0; i < FSG_NUM_BUFFERS; i++) {
					if (common->buffhds[i].state ==
							BUF_STATE_BUSY)
						break;
				}
				if (!amount_left_to_req && i == FSG_NUM_BUFFERS) {
					csw_hack_sent = 1;
					send_status(common);
				}
			}
#endif

 empty_write:
			/* Did the host decide to stop early? */
			if (bh->outreq->actual < bh->bulk_out_intended_length) {
				common->short_packet_received = 1;
				break;
			}
			continue;
		}

		/* Wait for something to happen */
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}

	return -EIO;		/* No default reply */
}

/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : vender write command */
/* [CHANGE START] 2011/05/27 KDDI : [offset]use change */
/*-------------------------------------------------------------------------*/

static int do_write_buffer(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	struct fsg_buffhd	*bh;
	int			get_some_more;
	u32			amount_left_to_req, amount_left_to_write;
	loff_t			file_offset;
	unsigned int		amount;
	int			rc;
	struct op_desc		*desc = 0;

	get_some_more = 1;
	file_offset = get_unaligned_be32(&common->cmnd[2]);

//	printk("[fms_CR7]%s: cmd=%d\n", __func__, common->cmnd[0]);
	desc = curlun->op_desc[common->cmnd[0]-SC_VENDOR_START];
	if (!desc->buffer){
		printk("[fms_CR7]%s: cmd=%d not ready\n", __func__, common->cmnd[0]);
		curlun->sense_data =
				SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		curlun->sense_data_info = file_offset;
		curlun->info_valid = 1;
		return -EIO;		/* No default reply */
	}

	amount_left_to_req = amount_left_to_write = common->data_size_from_cmnd;
//	printk("[fms_CR7]%s: amount_left_to_write=%d\n", __func__, amount_left_to_write);
//	printk("[fms_CR7]%s: file_offset=%x\n", __func__, (unsigned int)file_offset);
//	printk("[fms_CR7]%s: desc->len=%x\n", __func__, desc->len);
	if (file_offset + amount_left_to_write > desc->len) {
		printk("[fms_CR7]%s: vendor buffer out of range offset=0x%x write-len=0x%x buf-len=0x%x\n",
			__func__, (unsigned int)file_offset, amount_left_to_req, desc->len);
		curlun->sense_data =
				SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		curlun->sense_data_info = file_offset;
		curlun->info_valid = 1;
		return -EIO;		/* No default reply */
	}

	while (amount_left_to_write > 0) {

		/* Queue a request for more data from the host */
		bh = common->next_buffhd_to_fill;
		if (bh->state == BUF_STATE_EMPTY && get_some_more) {

			/* Figure out how much we want to get:
			 * Try to get the remaining amount.
			 * But don't get more than the buffer size.
			 * And don't try to go past the end of the file.
			 * If we're not at a page boundary,
			 *	don't go past the next page.
			 * If this means getting 0, then we were asked
			 *	to write past the end of file.
			 * Finally, round down to a block boundary. */
			amount = min(amount_left_to_req, FSG_BUFLEN);
//	printk("[fms_CR7]%s: (2)amount=0x%x\n", __func__, amount);

			/* Get the next buffer */
			common->usb_amount_left -= amount;
			amount_left_to_req -= amount;
			if (amount_left_to_req == 0)
				get_some_more = 0;

//	printk("[fms_CR7]%s: (3)amount=0x%x\n", __func__, amount);
//	printk("[fms_CR7]%s: (3)amount_left_to_req=0x%x\n", __func__, amount_left_to_req);
//	printk("[fms_CR7]%s: (3)get_some_more=%d bh->state =%d \n", __func__,get_some_more,bh->state);

			/* amount is always divisible by 512, hence by
			 * the bulk-out maxpacket size */
			bh->outreq->length = bh->bulk_out_intended_length =
					amount;
/* [CHANGE START] 2012/01/17 KDDI : Android ICS */
#if 0
			START_TRANSFER_OR(common, bulk_out, bh->outreq,
					&bh->outreq_busy, &bh->state)
				/* Don't know what to do if
				 * common->fsg is NULL */
#else
			if (!start_out_transfer(common, bh))
				/* Dunno what to do if common->fsg is NULL */
#endif
				return -EIO;
			common->next_buffhd_to_fill = bh->next;
			continue;
		}

		/* Write the received data to the backing file */
		bh = common->next_buffhd_to_drain;
		if (bh->state == BUF_STATE_EMPTY && !get_some_more){
			break;			/* We stopped early */
		}
		if (bh->state == BUF_STATE_FULL) {
			smp_rmb();
			common->next_buffhd_to_drain = bh->next;
			bh->state = BUF_STATE_EMPTY;

			/* Did something go wrong with the transfer? */
			if (bh->outreq->status != 0) {
				curlun->sense_data = SS_COMMUNICATION_FAILURE;
				curlun->sense_data_info = file_offset >> 9;
				curlun->info_valid = 1;
				break;
			}

			amount = bh->outreq->actual;
			if (desc->len - file_offset < amount) {
				LERROR(curlun,
	"write %u @ %llu beyond end %llu\n",
	amount, (unsigned long long) file_offset,
	(unsigned long long) desc->len);
				amount = desc->len - file_offset;
			}

			/* Perform the write */
//	printk("[fms_CR7]%s: (4)buf-write offset=0x%x size=0x%x \n", __func__,(unsigned int)file_offset,amount);
			memcpy(desc->buffer + file_offset, (char __user *) bh->buf,amount);
			file_offset += amount;
			amount_left_to_write -= amount;
			common->residue -= amount;

#ifdef MAX_UNFLUSHED_BYTES
			curlun->unflushed_bytes += amount;
			if (curlun->unflushed_bytes >= MAX_UNFLUSHED_BYTES) {
				fsync_sub(curlun);
				curlun->unflushed_bytes = 0;
			}
#endif
			/* Did the host decide to stop early? */
			if (bh->outreq->actual != bh->outreq->length) {
				common->short_packet_received = 1;
				break;
			}
			continue;
		}

		/* Wait for something to happen */
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}

	return -EIO;		/* No default reply */
}
/* [ADD END] 2011/04/15 KDDI : vender write command */
/* [CHANGE END] 2011/05/27 KDDI : [offset]use change */
/* [ADD END] 2012/01/17 KDDI : Android ICS */

/*-------------------------------------------------------------------------*/

static int do_synchronize_cache(struct fsg_common *common)
{
	struct fsg_lun	*curlun = common->curlun;
	int		rc;

	/* We ignore the requested LBA and write out all file's
	 * dirty data buffers. */
	rc = fsg_lun_fsync_sub(curlun);
	if (rc)
		curlun->sense_data = SS_WRITE_ERROR;
	return 0;
}


/*-------------------------------------------------------------------------*/

static void invalidate_sub(struct fsg_lun *curlun)
{
	struct file	*filp = curlun->filp;
	struct inode	*inode = filp->f_path.dentry->d_inode;
	unsigned long	rc;

	rc = invalidate_mapping_pages(inode->i_mapping, 0, -1);
	VLDBG(curlun, "invalidate_mapping_pages -> %ld\n", rc);
}

static int do_verify(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	u32			lba;
	u32			verification_length;
	struct fsg_buffhd	*bh = common->next_buffhd_to_fill;
	loff_t			file_offset, file_offset_tmp;
	u32			amount_left;
	unsigned int		amount;
	ssize_t			nread;

	/*
	 * Get the starting Logical Block Address and check that it's
	 * not too big.
	 */
	lba = get_unaligned_be32(&common->cmnd[2]);
	if (lba >= curlun->num_sectors) {
		curlun->sense_data = SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		return -EINVAL;
	}

	/*
	 * We allow DPO (Disable Page Out = don't save data in the
	 * cache) but we don't implement it.
	 */
	if (common->cmnd[1] & ~0x10) {
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}

	verification_length = get_unaligned_be16(&common->cmnd[7]);
	if (unlikely(verification_length == 0))
		return -EIO;		/* No default reply */

	/* Prepare to carry out the file verify */
	amount_left = verification_length << curlun->blkbits;
	file_offset = ((loff_t) lba) << curlun->blkbits;

	/* Write out all the dirty buffers before invalidating them */
	fsg_lun_fsync_sub(curlun);
	if (signal_pending(current))
		return -EINTR;

	invalidate_sub(curlun);
	if (signal_pending(current))
		return -EINTR;

	/* Just try to read the requested blocks */
	while (amount_left > 0) {
		/*
		 * Figure out how much we need to read:
		 * Try to read the remaining amount, but not more than
		 * the buffer size.
		 * And don't try to read past the end of the file.
		 */
		amount = min(amount_left, FSG_BUFLEN);
		amount = min((loff_t)amount,
			     curlun->file_length - file_offset);
		if (amount == 0) {
			curlun->sense_data =
					SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
			curlun->sense_data_info =
				file_offset >> curlun->blkbits;
			curlun->info_valid = 1;
			break;
		}

		/* Perform the read */
		file_offset_tmp = file_offset;
		nread = vfs_read(curlun->filp,
				(char __user *) bh->buf,
				amount, &file_offset_tmp);
		VLDBG(curlun, "file read %u @ %llu -> %d\n", amount,
				(unsigned long long) file_offset,
				(int) nread);
		if (signal_pending(current))
			return -EINTR;

		if (nread < 0) {
			LDBG(curlun, "error in file verify: %d\n", (int)nread);
			nread = 0;
		} else if (nread < amount) {
			LDBG(curlun, "partial file verify: %d/%u\n",
			     (int)nread, amount);
			nread = round_down(nread, curlun->blksize);
		}
		if (nread == 0) {
			curlun->sense_data = SS_UNRECOVERED_READ_ERROR;
			curlun->sense_data_info =
				file_offset >> curlun->blkbits;
			curlun->info_valid = 1;
			break;
		}
		file_offset += nread;
		amount_left -= nread;
	}
	return 0;
}


/*-------------------------------------------------------------------------*/

static int do_inquiry(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun *curlun = common->curlun;
	u8	*buf = (u8 *) bh->buf;

	if (!curlun) {		/* Unsupported LUNs are okay */
		common->bad_lun_okay = 1;
		memset(buf, 0, 36);
		buf[0] = 0x7f;		/* Unsupported, no device-type */
		buf[4] = 31;		/* Additional length */
		return 36;
	}

	buf[0] = curlun->cdrom ? TYPE_ROM : TYPE_DISK;
	buf[1] = curlun->removable ? 0x80 : 0;
	buf[2] = 2;		/* ANSI SCSI level 2 */
	buf[3] = 2;		/* SCSI-2 INQUIRY data format */
/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [CHANGE START] 2011/07/27 KDDI : inquiry command extend ,[Lun0] only */
	if ( strcmp(dev_name(&curlun->dev),"lun0") == 0 ) {
/* [CHANGE START] 2011/04/15 KDDI : return data size */
		buf[4] = 31 + INQUIRY_VENDOR_SPECIFIC_SIZE;		/* Additional length */
/* [CHANGE END] 2011/04/15 KDDI : return data size */
		buf[5] = 0;		/* No special options */
		buf[6] = 0;
		buf[7] = 0;
		memcpy(buf + 8, common->inquiry_string, sizeof common->inquiry_string);
/* [CHANGE START] 2011/05/26 KDDI : return data set */
		memcpy(buf + 8 + sizeof common->inquiry_string - 1,
			curlun->inquiry_vendor, INQUIRY_VENDOR_SPECIFIC_SIZE);
		if (g_vendor_info) {
			VUNIQ_DBG("%s: KDDI offset[%d] uniq offset[%d]\n", __func__,
				(8 + sizeof common->inquiry_string - 1),
				g_vendor_info->Offset);
			memcpy(&buf[g_vendor_info->Offset],
				g_vendor_info->Data, g_vendor_info->DataSize);
			memcpy(&buf[g_vendor_info->Offset + g_vendor_info->DataSize],
				curlun->inquiry_vendor,
				(INQUIRY_VENDOR_SPECIFIC_SIZE - g_vendor_info->DataSize));
			VUNIQ_DUMP(&buf[16],40);
		}
		return 36 + INQUIRY_VENDOR_SPECIFIC_SIZE;
/* [CHANGE END] 2011/05/26 KDDI : return data set */
	} else {
/* [ADD END] 2012/01/17 KDDI : Android ICS */
		buf[4] = 31;		/* Additional length */
		buf[5] = 0;		/* No special options */
		buf[6] = 0;
		buf[7] = 0;
		memcpy(buf + 8, common->inquiry_string, sizeof common->inquiry_string);
		return 36;
/* [ADD START] 2012/01/17 KDDI : Android ICS */
	}
/* [CHANGE END] 2011/07/27 KDDI : inquiry command extend ,[Lun0] only */
/* [ADD END] 2012/01/17 KDDI : Android ICS */
}

static int do_request_sense(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	u8		*buf = (u8 *) bh->buf;
	u32		sd, sdinfo;
	int		valid;

	/*
	 * From the SCSI-2 spec., section 7.9 (Unit attention condition):
	 *
	 * If a REQUEST SENSE command is received from an initiator
	 * with a pending unit attention condition (before the target
	 * generates the contingent allegiance condition), then the
	 * target shall either:
	 *   a) report any pending sense data and preserve the unit
	 *	attention condition on the logical unit, or,
	 *   b) report the unit attention condition, may discard any
	 *	pending sense data, and clear the unit attention
	 *	condition on the logical unit for that initiator.
	 *
	 * FSG normally uses option a); enable this code to use option b).
	 */
#if 0
	if (curlun && curlun->unit_attention_data != SS_NO_SENSE) {
		curlun->sense_data = curlun->unit_attention_data;
		curlun->unit_attention_data = SS_NO_SENSE;
	}
#endif

	if (!curlun) {		/* Unsupported LUNs are okay */
		common->bad_lun_okay = 1;
		sd = SS_LOGICAL_UNIT_NOT_SUPPORTED;
		sdinfo = 0;
		valid = 0;
	} else {
		sd = curlun->sense_data;
		sdinfo = curlun->sense_data_info;
		valid = curlun->info_valid << 7;
		curlun->sense_data = SS_NO_SENSE;
		curlun->sense_data_info = 0;
		curlun->info_valid = 0;
	}

	memset(buf, 0, 18);
	buf[0] = valid | 0x70;			/* Valid, current error */
	buf[2] = SK(sd);
	put_unaligned_be32(sdinfo, &buf[3]);	/* Sense information */
	buf[7] = 18 - 8;			/* Additional sense length */
	buf[12] = ASC(sd);
	buf[13] = ASCQ(sd);
	return 18;
}

static int do_read_capacity(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	u32		lba = get_unaligned_be32(&common->cmnd[2]);
	int		pmi = common->cmnd[8];
	u8		*buf = (u8 *)bh->buf;

	/* Check the PMI and LBA fields */
	if (pmi > 1 || (pmi == 0 && lba != 0)) {
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}

	put_unaligned_be32(curlun->num_sectors - 1, &buf[0]);
						/* Max logical block */
	put_unaligned_be32(curlun->blksize, &buf[4]);/* Block length */
	return 8;
}

static int do_read_header(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	int		msf = common->cmnd[1] & 0x02;
	u32		lba = get_unaligned_be32(&common->cmnd[2]);
	u8		*buf = (u8 *)bh->buf;

	if (common->cmnd[1] & ~0x02) {		/* Mask away MSF */
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}
	if (lba >= curlun->num_sectors) {
		curlun->sense_data = SS_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE;
		return -EINVAL;
	}

	memset(buf, 0, 8);
	buf[0] = 0x01;		/* 2048 bytes of user data, rest is EC */
	store_cdrom_address(&buf[4], msf, lba);
	return 8;
}

static int do_read_toc(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	int		msf = common->cmnd[1] & 0x02;
	int		start_track = common->cmnd[6];
	u8		*buf = (u8 *)bh->buf;

	if ((common->cmnd[1] & ~0x02) != 0 ||	/* Mask away MSF */
			start_track > 1) {
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}

	memset(buf, 0, 20);
	buf[1] = (20-2);		/* TOC data length */
	buf[2] = 1;			/* First track number */
	buf[3] = 1;			/* Last track number */
	buf[5] = 0x16;			/* Data track, copying allowed */
	buf[6] = 0x01;			/* Only track is number 1 */
	store_cdrom_address(&buf[8], msf, 0);

	buf[13] = 0x16;			/* Lead-out track is data */
	buf[14] = 0xAA;			/* Lead-out track number */
	store_cdrom_address(&buf[16], msf, curlun->num_sectors);
	return 20;
}

static int do_mode_sense(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	int		mscmnd = common->cmnd[0];
	u8		*buf = (u8 *) bh->buf;
	u8		*buf0 = buf;
	int		pc, page_code;
	int		changeable_values, all_pages;
	int		valid_page = 0;
	int		len, limit;

	if ((common->cmnd[1] & ~0x08) != 0) {	/* Mask away DBD */
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}
	pc = common->cmnd[2] >> 6;
	page_code = common->cmnd[2] & 0x3f;
	if (pc == 3) {
		curlun->sense_data = SS_SAVING_PARAMETERS_NOT_SUPPORTED;
		return -EINVAL;
	}
	changeable_values = (pc == 1);
	all_pages = (page_code == 0x3f);

	/*
	 * Write the mode parameter header.  Fixed values are: default
	 * medium type, no cache control (DPOFUA), and no block descriptors.
	 * The only variable value is the WriteProtect bit.  We will fill in
	 * the mode data length later.
	 */
	memset(buf, 0, 8);
	if (mscmnd == MODE_SENSE) {
		buf[2] = (curlun->ro ? 0x80 : 0x00);		/* WP, DPOFUA */
		buf += 4;
		limit = 255;
	} else {			/* MODE_SENSE_10 */
		buf[3] = (curlun->ro ? 0x80 : 0x00);		/* WP, DPOFUA */
		buf += 8;
		limit = 65535;		/* Should really be FSG_BUFLEN */
	}

	/* No block descriptors */

	/*
	 * The mode pages, in numerical order.  The only page we support
	 * is the Caching page.
	 */
	if (page_code == 0x08 || all_pages) {
		valid_page = 1;
		buf[0] = 0x08;		/* Page code */
		buf[1] = 10;		/* Page length */
		memset(buf+2, 0, 10);	/* None of the fields are changeable */

		if (!changeable_values) {
			buf[2] = 0x04;	/* Write cache enable, */
					/* Read cache not disabled */
					/* No cache retention priorities */
			put_unaligned_be16(0xffff, &buf[4]);
					/* Don't disable prefetch */
					/* Minimum prefetch = 0 */
			put_unaligned_be16(0xffff, &buf[8]);
					/* Maximum prefetch */
			put_unaligned_be16(0xffff, &buf[10]);
					/* Maximum prefetch ceiling */
		}
		buf += 12;
	}

	/*
	 * Check that a valid page was requested and the mode data length
	 * isn't too long.
	 */
	len = buf - buf0;
	if (!valid_page || len > limit) {
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}

	/*  Store the mode data length */
	if (mscmnd == MODE_SENSE)
		buf0[0] = len - 1;
	else
		put_unaligned_be16(len - 2, buf0);
	return len;
}

static int do_start_stop(struct fsg_common *common)
{
	struct fsg_lun	*curlun = common->curlun;
	int		loej, start;

	if (!curlun) {
		return -EINVAL;
	} else if (!curlun->removable) {
		curlun->sense_data = SS_INVALID_COMMAND;
		return -EINVAL;
	} else if ((common->cmnd[1] & ~0x01) != 0 || /* Mask away Immed */
		   (common->cmnd[4] & ~0x03) != 0) { /* Mask LoEj, Start */
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}

	loej  = common->cmnd[4] & 0x02;
	start = common->cmnd[4] & 0x01;

	/*
	 * Our emulation doesn't support mounting; the medium is
	 * available for use as soon as it is loaded.
	 */
	if (start) {
		if (!fsg_lun_is_open(curlun)) {
			curlun->sense_data = SS_MEDIUM_NOT_PRESENT;
			return -EINVAL;
		}
		return 0;
	}

	/* Are we allowed to unload the media? */
	if (curlun->prevent_medium_removal) {
		LDBG(curlun, "unload attempt prevented\n");
		curlun->sense_data = SS_MEDIUM_REMOVAL_PREVENTED;
		return -EINVAL;
	}

	if (!loej)
		return 0;

	/* Simulate an unload/eject */
	if (common->ops && common->ops->pre_eject) {
		int r = common->ops->pre_eject(common, curlun,
					       curlun - common->luns);
		if (unlikely(r < 0))
			return r;
		else if (r)
			return 0;
	}

	up_read(&common->filesem);
	down_write(&common->filesem);
	fsg_lun_close(curlun);
	up_write(&common->filesem);
	down_read(&common->filesem);

	return common->ops && common->ops->post_eject
		? min(0, common->ops->post_eject(common, curlun,
						 curlun - common->luns))
		: 0;
}

static int do_prevent_allow(struct fsg_common *common)
{
	struct fsg_lun	*curlun = common->curlun;
	int		prevent;

	if (!common->curlun) {
		return -EINVAL;
	} else if (!common->curlun->removable) {
		common->curlun->sense_data = SS_INVALID_COMMAND;
		return -EINVAL;
	}

	prevent = common->cmnd[4] & 0x01;
	if ((common->cmnd[4] & ~0x01) != 0) {	/* Mask away Prevent */
		curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
		return -EINVAL;
	}
	if (!curlun->nofua && curlun->prevent_medium_removal && !prevent)
		fsg_lun_fsync_sub(curlun);
	curlun->prevent_medium_removal = prevent;
	return 0;
}

static int do_read_format_capacities(struct fsg_common *common,
			struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;
	u8		*buf = (u8 *) bh->buf;

	buf[0] = buf[1] = buf[2] = 0;
	buf[3] = 8;	/* Only the Current/Maximum Capacity Descriptor */
	buf += 4;

	put_unaligned_be32(curlun->num_sectors, &buf[0]);
						/* Number of blocks */
	put_unaligned_be32(curlun->blksize, &buf[4]);/* Block length */
	buf[4] = 0x02;				/* Current capacity */
	return 12;
}

static int do_mode_select(struct fsg_common *common, struct fsg_buffhd *bh)
{
	struct fsg_lun	*curlun = common->curlun;

	/* We don't support MODE SELECT */
	if (curlun)
		curlun->sense_data = SS_INVALID_COMMAND;
	return -EINVAL;
}


/*-------------------------------------------------------------------------*/

static int halt_bulk_in_endpoint(struct fsg_dev *fsg)
{
	int	rc;

	rc = fsg_set_halt(fsg, fsg->bulk_in);
	if (rc == -EAGAIN)
		VDBG(fsg, "delayed bulk-in endpoint halt\n");
	while (rc != 0) {
		if (rc != -EAGAIN) {
			WARNING(fsg, "usb_ep_set_halt -> %d\n", rc);
			rc = 0;
			break;
		}

		/* Wait for a short time and then try again */
		if (msleep_interruptible(100) != 0)
			return -EINTR;
		rc = usb_ep_set_halt(fsg->bulk_in);
	}
	return rc;
}

static int wedge_bulk_in_endpoint(struct fsg_dev *fsg)
{
	int	rc;

	DBG(fsg, "bulk-in set wedge\n");
	rc = usb_ep_set_wedge(fsg->bulk_in);
	if (rc == -EAGAIN)
		VDBG(fsg, "delayed bulk-in endpoint wedge\n");
	while (rc != 0) {
		if (rc != -EAGAIN) {
			WARNING(fsg, "usb_ep_set_wedge -> %d\n", rc);
			rc = 0;
			break;
		}

		/* Wait for a short time and then try again */
		if (msleep_interruptible(100) != 0)
			return -EINTR;
		rc = usb_ep_set_wedge(fsg->bulk_in);
	}
	return rc;
}

static int throw_away_data(struct fsg_common *common)
{
	struct fsg_buffhd	*bh;
	u32			amount;
	int			rc;

	for (bh = common->next_buffhd_to_drain;
	     bh->state != BUF_STATE_EMPTY || common->usb_amount_left > 0;
	     bh = common->next_buffhd_to_drain) {

		/* Throw away the data in a filled buffer */
		if (bh->state == BUF_STATE_FULL) {
			smp_rmb();
			bh->state = BUF_STATE_EMPTY;
			common->next_buffhd_to_drain = bh->next;

			/* A short packet or an error ends everything */
			if (bh->outreq->actual < bh->bulk_out_intended_length ||
			    bh->outreq->status != 0) {
				raise_exception(common,
						FSG_STATE_ABORT_BULK_OUT);
				return -EINTR;
			}
			continue;
		}

		/* Try to submit another request if we need one */
		bh = common->next_buffhd_to_fill;
		if (bh->state == BUF_STATE_EMPTY
		 && common->usb_amount_left > 0) {
			amount = min(common->usb_amount_left, FSG_BUFLEN);

			/*
			 * Except at the end of the transfer, amount will be
			 * equal to the buffer size, which is divisible by
			 * the bulk-out maxpacket size.
			 */
			set_bulk_out_req_length(common, bh, amount);
			if (!start_out_transfer(common, bh))
				/* Dunno what to do if common->fsg is NULL */
				return -EIO;
			common->next_buffhd_to_fill = bh->next;
			common->usb_amount_left -= amount;
			continue;
		}

		/* Otherwise wait for something to happen */
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}
	return 0;
}

static int finish_reply(struct fsg_common *common)
{
	struct fsg_buffhd	*bh = common->next_buffhd_to_fill;
	int			rc = 0;

	switch (common->data_dir) {
	case DATA_DIR_NONE:
		break;			/* Nothing to send */

	/*
	 * If we don't know whether the host wants to read or write,
	 * this must be CB or CBI with an unknown command.  We mustn't
	 * try to send or receive any data.  So stall both bulk pipes
	 * if we can and wait for a reset.
	 */
	case DATA_DIR_UNKNOWN:
		if (!common->can_stall) {
			/* Nothing */
		} else if (fsg_is_set(common)) {
			fsg_set_halt(common->fsg, common->fsg->bulk_out);
			rc = halt_bulk_in_endpoint(common->fsg);
		} else {
			/* Don't know what to do if common->fsg is NULL */
			rc = -EIO;
		}
		break;

	/* All but the last buffer of data must have already been sent */
	case DATA_DIR_TO_HOST:
		if (common->data_size == 0) {
			/* Nothing to send */

		/* Don't know what to do if common->fsg is NULL */
		} else if (!fsg_is_set(common)) {
			rc = -EIO;

		/* If there's no residue, simply send the last buffer */
		} else if (common->residue == 0) {
			bh->inreq->zero = 0;
			if (!start_in_transfer(common, bh))
				return -EIO;
			common->next_buffhd_to_fill = bh->next;

		/*
		 * For Bulk-only, mark the end of the data with a short
		 * packet.  If we are allowed to stall, halt the bulk-in
		 * endpoint.  (Note: This violates the Bulk-Only Transport
		 * specification, which requires us to pad the data if we
		 * don't halt the endpoint.  Presumably nobody will mind.)
		 */
		} else {
			bh->inreq->zero = 1;
			if (!start_in_transfer(common, bh))
				rc = -EIO;
			common->next_buffhd_to_fill = bh->next;
			if (common->can_stall)
				rc = halt_bulk_in_endpoint(common->fsg);
		}
		break;

	/*
	 * We have processed all we want from the data the host has sent.
	 * There may still be outstanding bulk-out requests.
	 */
	case DATA_DIR_FROM_HOST:
		if (common->residue == 0) {
			/* Nothing to receive */

		/* Did the host stop sending unexpectedly early? */
		} else if (common->short_packet_received) {
			raise_exception(common, FSG_STATE_ABORT_BULK_OUT);
			rc = -EINTR;

		/*
		 * We haven't processed all the incoming data.  Even though
		 * we may be allowed to stall, doing so would cause a race.
		 * The controller may already have ACK'ed all the remaining
		 * bulk-out packets, in which case the host wouldn't see a
		 * STALL.  Not realizing the endpoint was halted, it wouldn't
		 * clear the halt -- leading to problems later on.
		 */
#if 0
		} else if (common->can_stall) {
			if (fsg_is_set(common))
				fsg_set_halt(common->fsg,
					     common->fsg->bulk_out);
			raise_exception(common, FSG_STATE_ABORT_BULK_OUT);
			rc = -EINTR;
#endif

		/*
		 * We can't stall.  Read in the excess data and throw it
		 * all away.
		 */
		} else {
			rc = throw_away_data(common);
		}
		break;
	}
	return rc;
}

static int send_status(struct fsg_common *common)
{
	struct fsg_lun		*curlun = common->curlun;
	struct fsg_buffhd	*bh;
	struct bulk_cs_wrap	*csw;
	int			rc;
	u8			status = USB_STATUS_PASS;
	u32			sd, sdinfo = 0;

	/* Wait for the next buffer to become available */
	bh = common->next_buffhd_to_fill;
	while (bh->state != BUF_STATE_EMPTY) {
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}

	if (curlun) {
		sd = curlun->sense_data;
		sdinfo = curlun->sense_data_info;
	} else if (common->bad_lun_okay)
		sd = SS_NO_SENSE;
	else
		sd = SS_LOGICAL_UNIT_NOT_SUPPORTED;

	if (common->phase_error) {
		DBG(common, "sending phase-error status\n");
		status = USB_STATUS_PHASE_ERROR;
		sd = SS_INVALID_COMMAND;
	} else if (sd != SS_NO_SENSE) {
		DBG(common, "sending command-failure status\n");
		status = USB_STATUS_FAIL;
		VDBG(common, "  sense data: SK x%02x, ASC x%02x, ASCQ x%02x;"
				"  info x%x\n",
				SK(sd), ASC(sd), ASCQ(sd), sdinfo);
	}

	/* Store and send the Bulk-only CSW */
	csw = (void *)bh->buf;

	csw->Signature = cpu_to_le32(USB_BULK_CS_SIG);
	csw->Tag = common->tag;
	csw->Residue = cpu_to_le32(common->residue);
#ifdef CONFIG_USB_CSW_HACK
	/* Since csw is being sent early, before
	 * writing on to storage media, need to set
	 * residue to zero,assuming that write will succeed.
	 */
	if (write_error_after_csw_sent) {
		write_error_after_csw_sent = 0;
		csw->Residue = cpu_to_le32(common->residue);
	} else
		csw->Residue = 0;
#else
	csw->Residue = cpu_to_le32(common->residue);
#endif
	csw->Status = status;

	bh->inreq->length = USB_BULK_CS_WRAP_LEN;
	bh->inreq->zero = 0;
	if (!start_in_transfer(common, bh))
		/* Don't know what to do if common->fsg is NULL */
		return -EIO;

	common->next_buffhd_to_fill = bh->next;
	return 0;
}


/*-------------------------------------------------------------------------*/

/*
 * Check whether the command is properly formed and whether its data size
 * and direction agree with the values we already have.
 */
static int check_command(struct fsg_common *common, int cmnd_size,
			 enum data_direction data_dir, unsigned int mask,
			 int needs_medium, const char *name)
{
	int			i;
	int			lun = common->cmnd[1] >> 5;
	static const char	dirletter[4] = {'u', 'o', 'i', 'n'};
	char			hdlen[20];
	struct fsg_lun		*curlun;

	hdlen[0] = 0;
	if (common->data_dir != DATA_DIR_UNKNOWN)
		sprintf(hdlen, ", H%c=%u", dirletter[(int) common->data_dir],
			common->data_size);
	VDBG(common, "SCSI command: %s;  Dc=%d, D%c=%u;  Hc=%d%s\n",
	     name, cmnd_size, dirletter[(int) data_dir],
	     common->data_size_from_cmnd, common->cmnd_size, hdlen);

	/*
	 * We can't reply at all until we know the correct data direction
	 * and size.
	 */
	if (common->data_size_from_cmnd == 0)
		data_dir = DATA_DIR_NONE;
	if (common->data_size < common->data_size_from_cmnd) {
		/*
		 * Host data size < Device data size is a phase error.
		 * Carry out the command, but only transfer as much as
		 * we are allowed.
		 */
		common->data_size_from_cmnd = common->data_size;
		common->phase_error = 1;
	}
	common->residue = common->data_size;
	common->usb_amount_left = common->data_size;

	/* Conflicting data directions is a phase error */
	if (common->data_dir != data_dir && common->data_size_from_cmnd > 0) {
		common->phase_error = 1;
		return -EINVAL;
	}

	/* Verify the length of the command itself */
	if (cmnd_size != common->cmnd_size) {

		/*
		 * Special case workaround: There are plenty of buggy SCSI
		 * implementations. Many have issues with cbw->Length
		 * field passing a wrong command size. For those cases we
		 * always try to work around the problem by using the length
		 * sent by the host side provided it is at least as large
		 * as the correct command length.
		 * Examples of such cases would be MS-Windows, which issues
		 * REQUEST SENSE with cbw->Length == 12 where it should
		 * be 6, and xbox360 issuing INQUIRY, TEST UNIT READY and
		 * REQUEST SENSE with cbw->Length == 10 where it should
		 * be 6 as well.
		 */
		if (cmnd_size <= common->cmnd_size) {
			DBG(common, "%s is buggy! Expected length %d "
			    "but we got %d\n", name,
			    cmnd_size, common->cmnd_size);
			cmnd_size = common->cmnd_size;
		} else {
			common->phase_error = 1;
			return -EINVAL;
		}
	}

	/* Check that the LUN values are consistent */
	if (common->lun != lun)
		DBG(common, "using LUN %d from CBW, not LUN %d from CDB\n",
		    common->lun, lun);

	/* Check the LUN */
	if (common->lun < common->nluns) {
		curlun = &common->luns[common->lun];
		common->curlun = curlun;
		if (common->cmnd[0] != REQUEST_SENSE) {
			curlun->sense_data = SS_NO_SENSE;
			curlun->sense_data_info = 0;
			curlun->info_valid = 0;
		}
	} else {
		common->curlun = NULL;
		curlun = NULL;
		common->bad_lun_okay = 0;

		/*
		 * INQUIRY and REQUEST SENSE commands are explicitly allowed
		 * to use unsupported LUNs; all others may not.
		 */
		if (common->cmnd[0] != INQUIRY &&
		    common->cmnd[0] != REQUEST_SENSE) {
			DBG(common, "unsupported LUN %d\n", common->lun);
			return -EINVAL;
		}
	}

	/*
	 * If a unit attention condition exists, only INQUIRY and
	 * REQUEST SENSE commands are allowed; anything else must fail.
	 */
	if (curlun && curlun->unit_attention_data != SS_NO_SENSE &&
	    common->cmnd[0] != INQUIRY &&
	    common->cmnd[0] != REQUEST_SENSE) {
		curlun->sense_data = curlun->unit_attention_data;
		curlun->unit_attention_data = SS_NO_SENSE;
		return -EINVAL;
	}

	/* Check that only command bytes listed in the mask are non-zero */
	common->cmnd[1] &= 0x1f;			/* Mask away the LUN */
	for (i = 1; i < cmnd_size; ++i) {
		if (common->cmnd[i] && !(mask & (1 << i))) {
			if (curlun)
				curlun->sense_data = SS_INVALID_FIELD_IN_CDB;
			return -EINVAL;
		}
	}

	/* If the medium isn't mounted and the command needs to access
	 * it, return an error. */
	if (curlun && !fsg_lun_is_open(curlun) && needs_medium) {
		curlun->sense_data = SS_MEDIUM_NOT_PRESENT;
		return -EINVAL;
	}

	return 0;
}

static int do_scsi_command(struct fsg_common *common)
{
	struct fsg_buffhd	*bh;
	int			rc;
	int			reply = -EINVAL;
	int			i;
	static char		unknown[16];
/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : for vendor command */
	struct op_desc	*desc;
/* [ADD END] 2011/04/15 KDDI : for vendor command */
/* [ADD END] 2012/01/17 KDDI : Android ICS */

	dump_cdb(common);

	/* Wait for the next buffer to become available for data or status */
	bh = common->next_buffhd_to_fill;
	common->next_buffhd_to_drain = bh;
	while (bh->state != BUF_STATE_EMPTY) {
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}
	common->phase_error = 0;
	common->short_packet_received = 0;

	down_read(&common->filesem);	/* We're using the backing file */
	switch (common->cmnd[0]) {

	case INQUIRY:
		common->data_size_from_cmnd = common->cmnd[4];
		reply = check_command(common, 6, DATA_DIR_TO_HOST,
				      (1<<4), 0,
				      "INQUIRY");
		if (reply == 0)
			reply = do_inquiry(common, bh);
		break;

	case MODE_SELECT:
		common->data_size_from_cmnd = common->cmnd[4];
		reply = check_command(common, 6, DATA_DIR_FROM_HOST,
				      (1<<1) | (1<<4), 0,
				      "MODE SELECT(6)");
		if (reply == 0)
			reply = do_mode_select(common, bh);
		break;

	case MODE_SELECT_10:
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]);
		reply = check_command(common, 10, DATA_DIR_FROM_HOST,
				      (1<<1) | (3<<7), 0,
				      "MODE SELECT(10)");
		if (reply == 0)
			reply = do_mode_select(common, bh);
		break;

	case MODE_SENSE:
		common->data_size_from_cmnd = common->cmnd[4];
		reply = check_command(common, 6, DATA_DIR_TO_HOST,
				      (1<<1) | (1<<2) | (1<<4), 0,
				      "MODE SENSE(6)");
		if (reply == 0)
			reply = do_mode_sense(common, bh);
		break;

	case MODE_SENSE_10:
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]);
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (1<<1) | (1<<2) | (3<<7), 0,
				      "MODE SENSE(10)");
		if (reply == 0)
			reply = do_mode_sense(common, bh);
		break;

	case ALLOW_MEDIUM_REMOVAL:
		common->data_size_from_cmnd = 0;
		reply = check_command(common, 6, DATA_DIR_NONE,
				      (1<<4), 0,
				      "PREVENT-ALLOW MEDIUM REMOVAL");
		if (reply == 0)
			reply = do_prevent_allow(common);
		break;

	case READ_6:
		i = common->cmnd[4];
		common->data_size_from_cmnd = (i == 0 ? 256 : i) <<
				common->curlun->blkbits;
		reply = check_command(common, 6, DATA_DIR_TO_HOST,
				      (7<<1) | (1<<4), 1,
				      "READ(6)");
		if (reply == 0)
			reply = do_read(common);
		break;

	case READ_10:
		common->data_size_from_cmnd =
				get_unaligned_be16(&common->cmnd[7]) <<
						common->curlun->blkbits;
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (1<<1) | (0xf<<2) | (3<<7), 1,
				      "READ(10)");
		if (reply == 0)
			reply = do_read(common);
		break;

	case READ_12:
		common->data_size_from_cmnd =
				get_unaligned_be32(&common->cmnd[6]) <<
						common->curlun->blkbits;
		reply = check_command(common, 12, DATA_DIR_TO_HOST,
				      (1<<1) | (0xf<<2) | (0xf<<6), 1,
				      "READ(12)");
		if (reply == 0)
			reply = do_read(common);
		break;

	case READ_CAPACITY:
		common->data_size_from_cmnd = 8;
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (0xf<<2) | (1<<8), 1,
				      "READ CAPACITY");
		if (reply == 0)
			reply = do_read_capacity(common, bh);
		break;

	case READ_HEADER:
		if (!common->curlun || !common->curlun->cdrom)
			goto unknown_cmnd;
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]);
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (3<<7) | (0x1f<<1), 1,
				      "READ HEADER");
		if (reply == 0)
			reply = do_read_header(common, bh);
		break;

	case READ_TOC:
		if (!common->curlun || !common->curlun->cdrom)
			goto unknown_cmnd;
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]);
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (7<<6) | (1<<1), 1,
				      "READ TOC");
		if (reply == 0)
			reply = do_read_toc(common, bh);
		break;

	case READ_FORMAT_CAPACITIES:
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]);
		reply = check_command(common, 10, DATA_DIR_TO_HOST,
				      (3<<7), 1,
				      "READ FORMAT CAPACITIES");
		if (reply == 0)
			reply = do_read_format_capacities(common, bh);
		break;

	case REQUEST_SENSE:
		common->data_size_from_cmnd = common->cmnd[4];
		reply = check_command(common, 6, DATA_DIR_TO_HOST,
				      (1<<4), 0,
				      "REQUEST SENSE");
		if (reply == 0)
			reply = do_request_sense(common, bh);
		break;

	case START_STOP:
		common->data_size_from_cmnd = 0;
		reply = check_command(common, 6, DATA_DIR_NONE,
				      (1<<1) | (1<<4), 0,
				      "START-STOP UNIT");
		if (reply == 0)
			reply = do_start_stop(common);
		break;

	case SYNCHRONIZE_CACHE:
		common->data_size_from_cmnd = 0;
		reply = check_command(common, 10, DATA_DIR_NONE,
				      (0xf<<2) | (3<<7), 1,
				      "SYNCHRONIZE CACHE");
		if (reply == 0)
			reply = do_synchronize_cache(common);
		break;

	case TEST_UNIT_READY:
		common->data_size_from_cmnd = 0;
		reply = check_command(common, 6, DATA_DIR_NONE,
				0, 1,
				"TEST UNIT READY");
		break;

	/*
	 * Although optional, this command is used by MS-Windows.  We
	 * support a minimal version: BytChk must be 0.
	 */
	case VERIFY:
		common->data_size_from_cmnd = 0;
		reply = check_command(common, 10, DATA_DIR_NONE,
				      (1<<1) | (0xf<<2) | (3<<7), 1,
				      "VERIFY");
		if (reply == 0)
			reply = do_verify(common);
		break;

	case WRITE_6:
		i = common->cmnd[4];
		common->data_size_from_cmnd = (i == 0 ? 256 : i) <<
					common->curlun->blkbits;
		reply = check_command(common, 6, DATA_DIR_FROM_HOST,
				      (7<<1) | (1<<4), 1,
				      "WRITE(6)");
		if (reply == 0)
			reply = do_write(common);
		break;

	case WRITE_10:
		common->data_size_from_cmnd =
				get_unaligned_be16(&common->cmnd[7]) <<
						common->curlun->blkbits;
		reply = check_command(common, 10, DATA_DIR_FROM_HOST,
				      (1<<1) | (0xf<<2) | (3<<7), 1,
				      "WRITE(10)");
		if (reply == 0)
			reply = do_write(common);
		break;

	case WRITE_12:
		common->data_size_from_cmnd =
				get_unaligned_be32(&common->cmnd[6]) <<
						common->curlun->blkbits;
		reply = check_command(common, 12, DATA_DIR_FROM_HOST,
				      (1<<1) | (0xf<<2) | (0xf<<6), 1,
				      "WRITE(12)");
		if (reply == 0)
			reply = do_write(common);
		break;

/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : add case vendor command */
	case SC_VENDOR_START ... SC_VENDOR_END:
/* [ADD START] 2011/05/30 KDDI : mutex_lock */
		mutex_lock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_lock */

/* [CHANGE START] 2011/09/30 KDDI : check[Lun0] BugFix , log add */
/* [ADD START] 2011/07/27 KDDI : inquiry command extend ,[Lun0] only */
		if (common->lun != 0){
			printk("[fms_CR7]%s e4 command receive but not[lun0]! \n", __func__);
			goto cmd_error;
		}
/* [ADD END] 2011/07/27 KDDI : inquiry command extend ,[Lun0] only */
		desc = common->luns[common->lun].op_desc[common->cmnd[0] - SC_VENDOR_START];
		if (!desc){
			printk("[fms_CR7]%s  opcode-%02x not ready! \n", __func__,common->cmnd[0]);
			goto cmd_error;
		}
/* [CHANGE END] 2011/09/30 KDDI : check[Lun0] BugFix , log add */

		common->data_size_from_cmnd = get_unaligned_be32(&common->cmnd[6]);
		if (common->data_size_from_cmnd == 0)
				goto cmd_error;
		if (~common->cmnd[1] & 0x10) {
			if ((reply = check_command(common, 10, DATA_DIR_FROM_HOST,
					(1<<1) | (0xf<<2) | (0xf<<6),
					0, "VENDOR WRITE BUFFER")) == 0) {
				reply = do_write_buffer(common);
				desc->update = jiffies;
				schedule_work(&desc->work);
			} else
				goto cmd_error;
/* [ADD START] 2011/05/30 KDDI : mutex_unlock */
				mutex_unlock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_unlock */
			break;
		} else {
			if ((reply = check_command(common, 10, DATA_DIR_TO_HOST,
					(1<<1) | (0xf<<2) | (0xf<<6),
					0, "VENDOR READ BUFFER")) == 0)
				reply = do_read_buffer(common);
			else
				goto cmd_error;
/* [ADD START] 2011/05/30 KDDI : mutex_unlock */
				mutex_unlock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_unlock */
			break;
		}
		cmd_error:
/* [ADD START] 2011/05/30 KDDI : mutex_unlock */
			mutex_unlock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_unlock */
			common->data_size_from_cmnd = 0;
			sprintf(unknown, "Unknown x%02x", common->cmnd[0]);
			if ((reply = check_command(common, common->cmnd_size,
					DATA_DIR_UNKNOWN, 0x3ff, 0, unknown)) == 0) { /* 2011/04/27 KDDI : ff->3ff(10Byte support) */
				common->curlun->sense_data = SS_INVALID_COMMAND;
				reply = -EINVAL;
			}
		break;
/* [ADD END] 2011/04/15 KDDI : add case vendor command */
/* [ADD END] 2012/01/17 KDDI : Android ICS */
	/*
	 * Some mandatory commands that we recognize but don't implement.
	 * They don't mean much in this setting.  It's left as an exercise
	 * for anyone interested to implement RESERVE and RELEASE in terms
	 * of Posix locks.
	 */
	case FORMAT_UNIT:
	case RELEASE:
	case RESERVE:
	case SEND_DIAGNOSTIC:
		/* Fall through */

	default:
		if (!vender_unique_command(common, bh, &reply)) {
			break;
		}
		if (common->cmnd[0] == common->vendor_data[0]) {
			common->data_size_from_cmnd = 0;
			/* check cmd */
			reply = strncmp(&common->cmnd[1], &common->vendor_data[1], 15);
			if (reply == 0) {
				if (fsg_is_set(common)) {
			                switch_set_state(&common->sdev, 0);
					switch_set_state(&common->sdev, 1);
				}
				else {
					printk(KERN_ERR "fsg is not set\n");
				}
			} else {
				printk(KERN_ERR "check_cmd error!!\n");
			}
			break;
		}
unknown_cmnd:
		common->data_size_from_cmnd = 0;
		sprintf(unknown, "Unknown x%02x", common->cmnd[0]);
		reply = check_command(common, common->cmnd_size,
				      DATA_DIR_UNKNOWN, 0xff, 0, unknown);
		if (reply == 0) {
			common->curlun->sense_data = SS_INVALID_COMMAND;
			reply = -EINVAL;
		}
		break;
	}
	up_read(&common->filesem);

	if (reply == -EINTR || signal_pending(current))
		return -EINTR;

	/* Set up the single reply buffer for finish_reply() */
	if (reply == -EINVAL)
		reply = 0;		/* Error reply length */
	if (reply >= 0 && common->data_dir == DATA_DIR_TO_HOST) {
		reply = min((u32)reply, common->data_size_from_cmnd);
		bh->inreq->length = reply;
		bh->state = BUF_STATE_FULL;
		common->residue -= reply;
	}				/* Otherwise it's already set */

	return 0;
}


/*-------------------------------------------------------------------------*/

static int received_cbw(struct fsg_dev *fsg, struct fsg_buffhd *bh)
{
	struct usb_request	*req = bh->outreq;
	struct fsg_bulk_cb_wrap	*cbw = req->buf;
	struct fsg_common	*common = fsg->common;

	/* Was this a real packet?  Should it be ignored? */
	if (req->status || test_bit(IGNORE_BULK_OUT, &fsg->atomic_bitflags))
		return -EINVAL;

	/* Is the CBW valid? */
	if (req->actual != USB_BULK_CB_WRAP_LEN ||
			cbw->Signature != cpu_to_le32(
				USB_BULK_CB_SIG)) {
		DBG(fsg, "invalid CBW: len %u sig 0x%x\n",
				req->actual,
				le32_to_cpu(cbw->Signature));

		/*
		 * The Bulk-only spec says we MUST stall the IN endpoint
		 * (6.6.1), so it's unavoidable.  It also says we must
		 * retain this state until the next reset, but there's
		 * no way to tell the controller driver it should ignore
		 * Clear-Feature(HALT) requests.
		 *
		 * We aren't required to halt the OUT endpoint; instead
		 * we can simply accept and discard any data received
		 * until the next reset.
		 */
		wedge_bulk_in_endpoint(fsg);
		set_bit(IGNORE_BULK_OUT, &fsg->atomic_bitflags);
		return -EINVAL;
	}

	/* Is the CBW meaningful? */
	if (cbw->Lun >= FSG_MAX_LUNS || cbw->Flags & ~USB_BULK_IN_FLAG ||
			cbw->Length <= 0 || cbw->Length > MAX_COMMAND_SIZE) {
		DBG(fsg, "non-meaningful CBW: lun = %u, flags = 0x%x, "
				"cmdlen %u\n",
				cbw->Lun, cbw->Flags, cbw->Length);

		/*
		 * We can do anything we want here, so let's stall the
		 * bulk pipes if we are allowed to.
		 */
		if (common->can_stall) {
			fsg_set_halt(fsg, fsg->bulk_out);
			halt_bulk_in_endpoint(fsg);
		}
		return -EINVAL;
	}

	/* Save the command for later */
	common->cmnd_size = cbw->Length;
	memcpy(common->cmnd, cbw->CDB, common->cmnd_size);
	if (cbw->Flags & USB_BULK_IN_FLAG)
		common->data_dir = DATA_DIR_TO_HOST;
	else
		common->data_dir = DATA_DIR_FROM_HOST;
	common->data_size = le32_to_cpu(cbw->DataTransferLength);
	if (common->data_size == 0)
		common->data_dir = DATA_DIR_NONE;
	common->lun = cbw->Lun;
	common->tag = cbw->Tag;
	return 0;
}

static int get_next_command(struct fsg_common *common)
{
	struct fsg_buffhd	*bh;
	int			rc = 0;

	/* Wait for the next buffer to become available */
	bh = common->next_buffhd_to_fill;
	while (bh->state != BUF_STATE_EMPTY) {
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}

	/* Queue a request to read a Bulk-only CBW */
	set_bulk_out_req_length(common, bh, USB_BULK_CB_WRAP_LEN);
	if (!start_out_transfer(common, bh))
		/* Don't know what to do if common->fsg is NULL */
		return -EIO;

	/*
	 * We will drain the buffer in software, which means we
	 * can reuse it for the next filling.  No need to advance
	 * next_buffhd_to_fill.
	 */

	/* Wait for the CBW to arrive */
	while (bh->state != BUF_STATE_FULL) {
		rc = sleep_thread(common);
		if (rc)
			return rc;
	}
	smp_rmb();
	rc = fsg_is_set(common) ? received_cbw(common->fsg, bh) : -EIO;
	bh->state = BUF_STATE_EMPTY;

	return rc;
}


/*-------------------------------------------------------------------------*/

static int alloc_request(struct fsg_common *common, struct usb_ep *ep,
		struct usb_request **preq)
{
	*preq = usb_ep_alloc_request(ep, GFP_ATOMIC);
	if (*preq)
		return 0;
	ERROR(common, "can't allocate request for %s\n", ep->name);
	return -ENOMEM;
}

/* Reset interface setting and re-init endpoint state (toggle etc). */
static int do_set_interface(struct fsg_common *common, struct fsg_dev *new_fsg)
{
	struct fsg_dev *fsg;
	int i, rc = 0;

	if (common->running)
		DBG(common, "reset interface\n");

reset:
	/* Deallocate the requests */
	if (common->fsg) {
		fsg = common->fsg;

		for (i = 0; i < FSG_NUM_BUFFERS; ++i) {
			struct fsg_buffhd *bh = &common->buffhds[i];

			if (bh->inreq) {
				usb_ep_free_request(fsg->bulk_in, bh->inreq);
				bh->inreq = NULL;
			}
			if (bh->outreq) {
				usb_ep_free_request(fsg->bulk_out, bh->outreq);
				bh->outreq = NULL;
			}
		}


		common->fsg = NULL;
		wake_up(&common->fsg_wait);
	}

	common->running = 0;
	if (!new_fsg || rc)
		return rc;

	common->fsg = new_fsg;
	fsg = common->fsg;

	/* Allocate the requests */
	for (i = 0; i < FSG_NUM_BUFFERS; ++i) {
		struct fsg_buffhd	*bh = &common->buffhds[i];

		rc = alloc_request(common, fsg->bulk_in, &bh->inreq);
		if (rc)
			goto reset;
		rc = alloc_request(common, fsg->bulk_out, &bh->outreq);
		if (rc)
			goto reset;
		bh->inreq->buf = bh->outreq->buf = bh->buf;
		bh->inreq->context = bh->outreq->context = bh;
		bh->inreq->complete = bulk_in_complete;
		bh->outreq->complete = bulk_out_complete;
	}

	common->running = 1;
	for (i = 0; i < common->nluns; ++i)
		common->luns[i].unit_attention_data = SS_RESET_OCCURRED;
	return rc;
}


/****************************** ALT CONFIGS ******************************/

static int fsg_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct fsg_dev *fsg = fsg_from_func(f);
	struct fsg_common *common = fsg->common;
	int rc;
	unsigned long flags;

	/* Enable the endpoints */
	rc = config_ep_by_speed(common->gadget, &(fsg->function), fsg->bulk_in);
	if (rc)
		return rc;
	rc = usb_ep_enable(fsg->bulk_in);
	if (rc)
		return rc;
	fsg->bulk_in->driver_data = common;
	fsg->bulk_in_enabled = 1;

	rc = config_ep_by_speed(common->gadget, &(fsg->function),
				fsg->bulk_out);
	if (rc)
		goto reset_bulk_int;
	rc = usb_ep_enable(fsg->bulk_out);
	if (rc)
		goto reset_bulk_int;
	fsg->bulk_out->driver_data = common;
	fsg->bulk_out_enabled = 1;
	common->bulk_out_maxpacket = le16_to_cpu(fsg->bulk_in->desc->wMaxPacketSize);
	clear_bit(IGNORE_BULK_OUT, &fsg->atomic_bitflags);
	fsg->common->new_fsg = fsg;
	raise_exception(fsg->common, FSG_STATE_CONFIG_CHANGE);

	if (g_vendor_info) {
		VUNIQ_DBG("%s: set POLLMSG\n", __func__);
		spin_lock_irqsave(&usbmass_dev.lock, flags);
		usbmass_dev.connect_chg = POLLMSG;
		spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
		wake_up_interruptible(&(usbmass_dev.rwait));
	}
	return USB_GADGET_DELAYED_STATUS;

reset_bulk_int:
	usb_ep_disable(fsg->bulk_in);
	fsg->bulk_in_enabled = 0;
	return rc;
}

static void fsg_disable(struct usb_function *f)
{
	struct fsg_dev *fsg = fsg_from_func(f);
	unsigned long flags;

	/* Disable the endpoints */
	if (fsg->bulk_in_enabled) {
		usb_ep_disable(fsg->bulk_in);
		fsg->bulk_in_enabled = 0;
		fsg->bulk_in->driver_data = NULL;
	}
	if (fsg->bulk_out_enabled) {
		usb_ep_disable(fsg->bulk_out);
		fsg->bulk_out_enabled = 0;
		fsg->bulk_out->driver_data = NULL;
	}
	fsg->common->new_fsg = NULL;

	if (g_vendor_info) {
		VUNIQ_DBG("%s: set POLLREMOVE\n", __func__);
		spin_lock_irqsave(&(usbmass_dev.lock), flags);
		usbmass_dev.connect_chg = POLLREMOVE;
		spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
		wake_up_interruptible(&(usbmass_dev.rwait));
	}

	raise_exception(fsg->common, FSG_STATE_CONFIG_CHANGE);
}


/*-------------------------------------------------------------------------*/

static struct fsg_common		*the_common;

static void handle_exception(struct fsg_common *common)
{
	siginfo_t		info;
	int			i;
	struct fsg_buffhd	*bh;
	enum fsg_state		old_state;
	struct fsg_lun		*curlun;
	unsigned int		exception_req_tag;

	/*
	 * Clear the existing signals.  Anything but SIGUSR1 is converted
	 * into a high-priority EXIT exception.
	 */
	for (;;) {
		int sig =
			dequeue_signal_lock(current, &current->blocked, &info);
		if (!sig)
			break;
		if (sig != SIGUSR1) {
			if (common->state < FSG_STATE_EXIT)
				DBG(common, "Main thread exiting on signal\n");
			raise_exception(common, FSG_STATE_EXIT);
		}
	}

	/* Cancel all the pending transfers */
	if (likely(common->fsg)) {
		for (i = 0; i < FSG_NUM_BUFFERS; ++i) {
			bh = &common->buffhds[i];
			if (bh->inreq_busy)
				usb_ep_dequeue(common->fsg->bulk_in, bh->inreq);
			if (bh->outreq_busy)
				usb_ep_dequeue(common->fsg->bulk_out,
					       bh->outreq);
		}

		/* Wait until everything is idle */
		for (;;) {
			int num_active = 0;
			for (i = 0; i < FSG_NUM_BUFFERS; ++i) {
				bh = &common->buffhds[i];
				num_active += bh->inreq_busy + bh->outreq_busy;
			}
			if (num_active == 0)
				break;
			if (sleep_thread(common))
				return;
		}

		/* Clear out the controller's fifos */
		if (common->fsg->bulk_in_enabled)
			usb_ep_fifo_flush(common->fsg->bulk_in);
		if (common->fsg->bulk_out_enabled)
			usb_ep_fifo_flush(common->fsg->bulk_out);
	}

	/*
	 * Reset the I/O buffer states and pointers, the SCSI
	 * state, and the exception.  Then invoke the handler.
	 */
	spin_lock_irq(&common->lock);

	for (i = 0; i < FSG_NUM_BUFFERS; ++i) {
		bh = &common->buffhds[i];
		bh->state = BUF_STATE_EMPTY;
	}
	common->next_buffhd_to_fill = &common->buffhds[0];
	common->next_buffhd_to_drain = &common->buffhds[0];
	exception_req_tag = common->exception_req_tag;
	old_state = common->state;

	if (old_state == FSG_STATE_ABORT_BULK_OUT)
		common->state = FSG_STATE_STATUS_PHASE;
	else {
		for (i = 0; i < common->nluns; ++i) {
			curlun = &common->luns[i];
			curlun->prevent_medium_removal = 0;
			curlun->sense_data = SS_NO_SENSE;
			curlun->unit_attention_data = SS_NO_SENSE;
			curlun->sense_data_info = 0;
			curlun->info_valid = 0;
		}
		common->state = FSG_STATE_IDLE;
	}
	spin_unlock_irq(&common->lock);

	/* Carry out any extra actions required for the exception */
	switch (old_state) {
	case FSG_STATE_ABORT_BULK_OUT:
		send_status(common);
		spin_lock_irq(&common->lock);
		if (common->state == FSG_STATE_STATUS_PHASE)
			common->state = FSG_STATE_IDLE;
		spin_unlock_irq(&common->lock);
		break;

	case FSG_STATE_RESET:
		/*
		 * In case we were forced against our will to halt a
		 * bulk endpoint, clear the halt now.  (The SuperH UDC
		 * requires this.)
		 */
		if (!fsg_is_set(common))
			break;
		if (test_and_clear_bit(IGNORE_BULK_OUT,
				       &common->fsg->atomic_bitflags))
			usb_ep_clear_halt(common->fsg->bulk_in);

		if (common->ep0_req_tag == exception_req_tag)
			ep0_queue(common);	/* Complete the status stage */

		/*
		 * Technically this should go here, but it would only be
		 * a waste of time.  Ditto for the INTERFACE_CHANGE and
		 * CONFIG_CHANGE cases.
		 */
		/* for (i = 0; i < common->nluns; ++i) */
		/*	common->luns[i].unit_attention_data = */
		/*		SS_RESET_OCCURRED;  */
		break;

	case FSG_STATE_CONFIG_CHANGE:
		do_set_interface(common, common->new_fsg);
		if (common->new_fsg)
			usb_composite_setup_continue(common->cdev);
		break;

	case FSG_STATE_EXIT:
	case FSG_STATE_TERMINATED:
		do_set_interface(common, NULL);		/* Free resources */
		spin_lock_irq(&common->lock);
		common->state = FSG_STATE_TERMINATED;	/* Stop the thread */
		spin_unlock_irq(&common->lock);
		break;

	case FSG_STATE_INTERFACE_CHANGE:
	case FSG_STATE_DISCONNECT:
	case FSG_STATE_COMMAND_PHASE:
	case FSG_STATE_DATA_PHASE:
	case FSG_STATE_STATUS_PHASE:
	case FSG_STATE_IDLE:
		break;
	}
}


/*-------------------------------------------------------------------------*/

static int fsg_main_thread(void *common_)
{
	struct fsg_common	*common = common_;

	/*
	 * Allow the thread to be killed by a signal, but set the signal mask
	 * to block everything but INT, TERM, KILL, and USR1.
	 */
	allow_signal(SIGINT);
	allow_signal(SIGTERM);
	allow_signal(SIGKILL);
	allow_signal(SIGUSR1);

	/* Allow the thread to be frozen */
	set_freezable();

	/*
	 * Arrange for userspace references to be interpreted as kernel
	 * pointers.  That way we can pass a kernel pointer to a routine
	 * that expects a __user pointer and it will work okay.
	 */
	set_fs(get_ds());

	/* The main loop */
	while (common->state != FSG_STATE_TERMINATED) {
		if (exception_in_progress(common) || signal_pending(current)) {
			handle_exception(common);
			continue;
		}

		if (!common->running) {
			sleep_thread(common);
			continue;
		}

		if (get_next_command(common))
			continue;

		spin_lock_irq(&common->lock);
		if (!exception_in_progress(common))
			common->state = FSG_STATE_DATA_PHASE;
		spin_unlock_irq(&common->lock);

		if (do_scsi_command(common) || finish_reply(common))
			continue;

		spin_lock_irq(&common->lock);
		if (!exception_in_progress(common))
			common->state = FSG_STATE_STATUS_PHASE;
		spin_unlock_irq(&common->lock);

#ifdef CONFIG_USB_CSW_HACK
		/* Since status is already sent for write scsi command,
		 * need to skip sending status once again if it is a
		 * write scsi command.
		 */
		if (csw_hack_sent) {
			csw_hack_sent = 0;
			continue;
		}
#endif
		if (send_status(common))
			continue;

		spin_lock_irq(&common->lock);
		if (!exception_in_progress(common))
			common->state = FSG_STATE_IDLE;
		spin_unlock_irq(&common->lock);
	}

	spin_lock_irq(&common->lock);
	common->thread_task = NULL;
	spin_unlock_irq(&common->lock);

	if (!common->ops || !common->ops->thread_exits
	 || common->ops->thread_exits(common) < 0) {
		struct fsg_lun *curlun = common->luns;
		unsigned i = common->nluns;

		down_write(&common->filesem);
		for (; i--; ++curlun) {
			if (!fsg_lun_is_open(curlun))
				continue;

			fsg_lun_close(curlun);
			curlun->unit_attention_data = SS_MEDIUM_NOT_PRESENT;
		}
		up_write(&common->filesem);
	}

	/* Let fsg_unbind() know the thread has exited */
	complete_and_exit(&common->thread_notifier, 0);
}


/*************************** DEVICE ATTRIBUTES ***************************/

/* Write permission is checked per LUN in store_*() functions. */
static DEVICE_ATTR(ro, 0644, fsg_show_ro, fsg_store_ro);
static DEVICE_ATTR(nofua, 0644, fsg_show_nofua, fsg_store_nofua);
static DEVICE_ATTR(file, 0644, fsg_show_file, fsg_store_file);
#ifdef CONFIG_USB_MSC_PROFILING
static DEVICE_ATTR(perf, 0644, fsg_show_perf, fsg_store_perf);
#endif

/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : functions to handle vendor command */
/*************************** VENDOR SCSI OPCODE ***************************/

/* setting notify change buffer */
static void buffer_notify_sysfs(struct work_struct *work)
{
	struct op_desc	*desc;
//printk("[fms_CR7]%s\n", __func__);
	desc = container_of(work, struct op_desc, work);
	sysfs_notify_dirent(desc->value_sd);
}

/* check vendor command code */
static int vendor_cmd_is_valid(unsigned cmd)
{
	if(cmd < SC_VENDOR_START)
		return 0;
	if(cmd > SC_VENDOR_END)
		return 0;
	return 1;
}

/* read vendor command buffer */
static ssize_t
vendor_cmd_read_buffer(struct file* f, struct kobject *kobj, struct bin_attribute *attr,
                char *buf, loff_t off, size_t count)
{
	ssize_t	status;
	struct op_desc	*desc = attr->private;

//printk("[fms_CR7]%s: buf=%p off=%lx count=%x\n", __func__, buf, (unsigned long)off, count);
	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else {
		size_t srclen, n;
		void *src;
		size_t nleft = count;
		src = desc->buffer;
		srclen = desc->len;

		if (off < srclen) {
			n = min(nleft, srclen - (size_t) off);
			memcpy(buf, src + off, n);
			nleft -= n;
			buf += n;
			off = 0;
		} else {
			off -= srclen;
		}
		status = count - nleft;
	}

	mutex_unlock(&sysfs_lock);
	return status;
}

/* write vendor commn buffer */
static ssize_t
vendor_cmd_write_buffer(struct file* f, struct kobject *kobj, struct bin_attribute *attr,
                char *buf, loff_t off, size_t count)
{
	ssize_t	status;
	struct op_desc	*desc = attr->private;

//printk("[fms_CR7]%s: buf=%p off=%lx count=%x\n", __func__, buf, (unsigned long)off, count);
	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else {
		size_t dstlen, n;
		size_t nleft = count;
		void *dst;

		dst = desc->buffer;
		dstlen = desc->len;

		if (off < dstlen) {
			n = min(nleft, dstlen - (size_t) off);
			memcpy(dst + off, buf, n);
			nleft -= n;
			buf += n;
			off = 0;
		} else {
			off -= dstlen;
		}
		status = count - nleft;
	}

	desc->update = jiffies;
	schedule_work(&desc->work);

	mutex_unlock(&sysfs_lock);
	return status;
}

/* memory mapping vendor commn buffer */
static int
vendor_cmd_mmap_buffer(struct file *f, struct kobject *kobj, struct bin_attribute *attr,
		struct vm_area_struct *vma)
{
        int rc = -EINVAL;
	unsigned long pgoff, delta;
	ssize_t size = vma->vm_end - vma->vm_start;
	struct op_desc	*desc = attr->private;

printk("[fms_CR7]%s\n", __func__);
/* [ADD START] 2011/05/30 KDDI : mutex_lock */
	mutex_lock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_lock */

	if (vma->vm_pgoff != 0) {
		printk("mmap failed: page offset %lx\n", vma->vm_pgoff);
		goto done;
	}

	pgoff = __pa(desc->buffer);
	delta = PAGE_ALIGN(pgoff) - pgoff;
printk("[fms_CR7]%s size=%x delta=%lx pgoff=%lx\n", __func__, size, delta, pgoff);

        if (size + delta > desc->len) {
                printk("[fms_CR7]%s mmap failed: size %d\n", __func__, size);
		goto done;
        }

        pgoff += delta;
        vma->vm_flags |= VM_RESERVED;

	rc = io_remap_pfn_range(vma, vma->vm_start, pgoff >> PAGE_SHIFT,
		size, vma->vm_page_prot);

	if (rc < 0)
                printk("[fms_CR7]%s mmap failed: remap error %d\n", __func__, rc);
done:
/* [ADD START] 2011/05/30 KDDI : mutex_unlock */
	mutex_unlock(&sysfs_lock);
/* [ADD END] 2011/05/30 KDDI : mutex_unlock */
	return rc;
}

/* set 'size'file */
static ssize_t vendor_size_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct op_desc	*desc = dev_to_desc(dev);
	ssize_t		status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else
		status = sprintf(buf, "%d\n", desc->len);

	mutex_unlock(&sysfs_lock);
	return status;
}

/* when update 'size'file */
static ssize_t vendor_size_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	long len;
	char* buffer;
	struct op_desc	*desc = dev_to_desc(dev);
	ssize_t		status;
/* [ADD START] 2011/08/26 KDDI : check init alloc */
	long cmd;
	char cmd_buf[16]="0x";
	struct fsg_lun	*curlun = fsg_lun_from_dev(&desc->dev);
/* [ADD END] 2011/08/26 KDDI : check init alloc */

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else {
		struct bin_attribute* dev_bin_attr_buffer = &desc->dev_bin_attr_buffer;
		status = strict_strtol(buf, 0, &len);
		if (status < 0) {
			status = -EINVAL;
			goto done;
		}
		if ( desc->len == len ) {
			status = 0;
			printk("[fms_CR7]%s already size setting! size not change\n", __func__);
			goto done;
		}

/* [CHANGE START] 2011/08/26 KDDI : check init alloc */
		status = strict_strtol(strcat(cmd_buf,dev_name(&desc->dev)+7), 0, &cmd);
		if (status < 0) {
			status = -EINVAL;
			goto done;
		}
		printk("[fms_CR7]%s cmd=0x%x old_size=0x%x new_size=0x%x \n", __func__, (unsigned int)cmd, (unsigned int)desc->len, (unsigned int)len);

		if ( cmd-SC_VENDOR_START < ALLOC_CMD_CNT && len == ALLOC_INI_SIZE){
			printk("[fms_CR7]%s buffer alreay malloc \n",__func__);
			buffer = curlun->reserve_buf[cmd-SC_VENDOR_START];
		} else {
			printk("[fms_CR7]%s malloc buffer \n",__func__);
			buffer = kzalloc(len, GFP_KERNEL);
			if(!buffer) {
				status = -ENOMEM;
				goto done;
			}
		}
		if ( cmd-SC_VENDOR_START+1 > ALLOC_CMD_CNT || desc->len != ALLOC_INI_SIZE){
			printk("[fms_CR7]%s free old buffer \n",__func__);
			kfree(desc->buffer);
		}
		desc->len = len;
/* [CHANGE END] 2011/08/26 KDDI : check init alloc */
		desc->buffer = buffer;
		device_remove_bin_file(&desc->dev, dev_bin_attr_buffer);
		dev_bin_attr_buffer->size = len;
		status = device_create_bin_file(&desc->dev, dev_bin_attr_buffer);
	}

done:
	mutex_unlock(&sysfs_lock);
	return status ? : size;
}
/* define 'size'file */
static DEVICE_ATTR(size, 0606, vendor_size_show, vendor_size_store); /* 2011/04/19 KDDI : permission change 0600->0606 */

/* set 'update'file */
static ssize_t vendor_update_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct op_desc	*desc = dev_to_desc(dev);
	ssize_t		status;

	mutex_lock(&sysfs_lock);

	if (!test_bit(FLAG_EXPORT, &desc->flags))
		status = -EIO;
	else
		status = sprintf(buf, "%lu\n", desc->update);

	mutex_unlock(&sysfs_lock);
	return status;
}
/* define 'update'file */
static DEVICE_ATTR(update, 0404, vendor_update_show, 0); /* 2011/04/19 KDDI : permission change 0400->0404 */

/* vendor command create */
static int vendor_cmd_export(struct device *dev, unsigned cmd, int init)
{
	struct fsg_lun	*curlun = fsg_lun_from_dev(dev);
	struct op_desc	*desc;
	int		status = -EINVAL;
	struct bin_attribute* dev_bin_attr_buffer;

	if (!vendor_cmd_is_valid(cmd)){
		printk("[fms_CR7]%s cmd=%02x cmd is invalid\n", __func__, cmd);
		goto done;
	}

	desc = curlun->op_desc[cmd-SC_VENDOR_START];
	if (!desc) {
		desc = kzalloc(sizeof(struct op_desc), GFP_KERNEL);
		if(!desc) {
			printk("[fms_CR7]%s op_desc alloc failed\n", __func__);
			status = -ENOMEM;
			goto done;
		}
		curlun->op_desc[cmd-SC_VENDOR_START] = desc;
	}

	status = 0;
	if (test_bit(FLAG_EXPORT, &desc->flags)) {
		printk("[fms_CR7]%s already exported\n", __func__);
		goto done;
	}

/* [CHANGE START] 2011/08/26 KDDI : check init alloc */
	if ( cmd-SC_VENDOR_START+1 > ALLOC_CMD_CNT ){
		desc->buffer = kzalloc(2048, GFP_KERNEL);
		printk("[fms_CR7]%s opcode:%02x bufalloc size:%08x \n", __func__, cmd, 2048);
		if(!desc->buffer) {
			printk("[fms_CR7]%s buffer alloc failed\n", __func__);
			status = -ENOMEM;
			goto done;
		}
		desc->len = 2048;
	}else{
		desc->buffer = curlun->reserve_buf[cmd-SC_VENDOR_START];
		printk("[fms_CR7]%s opcode:%02x bufcopy bufsize:%08x \n", __func__, cmd, ALLOC_INI_SIZE);
		desc->len = ALLOC_INI_SIZE;
	}
/* [CHANGE END] 2011/08/26 KDDI : check init alloc */

	dev_bin_attr_buffer = &desc->dev_bin_attr_buffer;
	desc->dev.release = op_release;
	desc->dev.parent = &curlun->dev;
	dev_set_drvdata(&desc->dev, curlun);
	dev_set_name(&desc->dev,"opcode-%02x", cmd);
	status = device_register(&desc->dev);
	if (status != 0) {
		printk("[fms_CR7]%s failed to register opcode%d: %d\n", __func__, cmd, status);
		goto done;
	}

	dev_bin_attr_buffer->attr.name = "buffer";
/* [ADD START] 2011/08/26 KDDI : at initialization, change the attributes */
	if (init)
		dev_bin_attr_buffer->attr.mode = 0660;
	else
		dev_bin_attr_buffer->attr.mode = 0606;
/* [ADD END] 2011/08/26 KDDI : at initialization, change the attributes */
	dev_bin_attr_buffer->read = vendor_cmd_read_buffer;
	dev_bin_attr_buffer->write = vendor_cmd_write_buffer;
	dev_bin_attr_buffer->mmap = vendor_cmd_mmap_buffer;
/* [CHANGE START] 2011/08/26 KDDI : check init alloc */
	if ( cmd-SC_VENDOR_START+1 > ALLOC_CMD_CNT ){
		dev_bin_attr_buffer->size = 2048;
	}else{
		dev_bin_attr_buffer->size = ALLOC_INI_SIZE;
	}
/* [CHANGE END] 2011/08/26 KDDI : check init alloc */
	dev_bin_attr_buffer->private = desc;
	status = device_create_bin_file(&desc->dev, dev_bin_attr_buffer);

	if (status != 0) {
		device_remove_bin_file(&desc->dev, dev_bin_attr_buffer);
		kfree(desc->buffer);
		desc->buffer = 0;
		desc->len = 0;
		device_unregister(&desc->dev);
		goto done;
	}

/* [ADD START] 2011/08/26 KDDI : at initialization, change the attributes */
	if (init){
		dev_attr_size.attr.mode = 0660;
		dev_attr_update.attr.mode = 0440;
	} else {
		dev_attr_size.attr.mode = 0606;
		dev_attr_update.attr.mode = 0404;
	}
/* [ADD END] 2011/08/26 KDDI : at initialization, change the attributes */
	status = device_create_file(&desc->dev, &dev_attr_size);
	if (status == 0) status = device_create_file(&desc->dev, &dev_attr_update);
	if (status != 0) {
		printk("[fms_CR7]%s device_create_file failed: %d\n", __func__, status);
		device_remove_file(&desc->dev, &dev_attr_update);
		device_remove_file(&desc->dev, &dev_attr_size);
		device_remove_bin_file(&desc->dev, dev_bin_attr_buffer);
/* [CHANGE START] 2011/08/26 KDDI : check init alloc */
		if ( cmd-SC_VENDOR_START+1 > ALLOC_CMD_CNT )
			kfree(desc->buffer);
/* [CHANGE END] 2011/08/26 KDDI : check init alloc */
		desc->buffer = 0;
		desc->len = 0;
		device_unregister(&desc->dev);
		goto done;
	}

	desc->value_sd = sysfs_get_dirent(desc->dev.kobj.sd, NULL, "update");
	INIT_WORK(&desc->work, buffer_notify_sysfs);

	if (status == 0)
		set_bit(FLAG_EXPORT, &desc->flags);

/* [ADD START] 2011/05/26 KDDI : init 'update' */
	desc->update = 0;
/* [ADD END] 2011/05/26 KDDI : init 'update' */

done:
	if (status)
		pr_debug("%s: opcode%d status %d\n", __func__, cmd, status);
	return status;
}

/* vendor command delete */
static void vendor_cmd_unexport(struct device *dev, unsigned cmd)
{
	struct fsg_lun	*curlun = fsg_lun_from_dev(dev);
	struct op_desc *desc;
	int status = -EINVAL;

	if (!vendor_cmd_is_valid(cmd)){
		printk("[fms_CR7]%s cmd=%02x cmd is invalid\n", __func__, cmd);
		goto done;
	}

	desc = curlun->op_desc[cmd-SC_VENDOR_START];
	if (!desc) {
		printk("[fms_CR7]%s not export\n", __func__);
		status = -ENODEV;
		goto done;
	}

	if (test_bit(FLAG_EXPORT, &desc->flags)) {
		struct bin_attribute* dev_bin_attr_buffer = &desc->dev_bin_attr_buffer;
		clear_bit(FLAG_EXPORT, &desc->flags);
		cancel_work_sync(&desc->work);
		device_remove_file(&desc->dev, &dev_attr_update);
		device_remove_file(&desc->dev, &dev_attr_size);
		device_remove_bin_file(&desc->dev, dev_bin_attr_buffer);
/* [CHANGE START] 2011/08/26 KDDI : check init alloc */
		if ( cmd-SC_VENDOR_START+1 > ALLOC_CMD_CNT || desc->len != ALLOC_INI_SIZE){
			kfree(desc->buffer);
			printk("[fms_CR7]%s opcode:%02x free buff\n", __func__, cmd);
		}else{
			printk("[fms_CR7]%s opcode:%02x not free buff\n", __func__, cmd);
		}
/* [CHANGE END] 2011/08/26 KDDI : check init alloc */
		desc->buffer = 0;
		desc->len = 0;
		status = 0;
		device_unregister(&desc->dev);
		kfree(desc);
		curlun->op_desc[cmd-SC_VENDOR_START] = 0;
	} else
		status = -ENODEV;

done:
	if (status)
		pr_debug("%s: opcode%d status %d\n", __func__, cmd, status);
}


/* when 'export'file update */
static ssize_t vendor_export_store(struct device *dev,
                struct device_attribute *attr, const char *buf, size_t len)
{
	long cmd;
	int status;

	status = strict_strtol(buf, 0, &cmd);
	if (status < 0)
		goto done;

	status = -EINVAL;

	if (!vendor_cmd_is_valid(cmd))
		goto done;

	mutex_lock(&sysfs_lock);

/* [CHANGE START] 2011/08/26 KDDI : at initialization, change the attributes */
	status = vendor_cmd_export(dev, cmd, 0);
/* [CHANGE END] 2011/08/26 KDDI : at initialization, change the attributes */
	if (status < 0)
		vendor_cmd_unexport(dev, cmd);

	mutex_unlock(&sysfs_lock);
done:
	if (status)
		pr_debug(KERN_INFO"%s: status %d\n", __func__, status);
	return status ? : len;
}

/* define 'export'file */
//static DEVICE_ATTR(export, 0202, 0, vendor_export_store); /* 2011/04/19 KDDI : permission change 0200->0202 */
static DEVICE_ATTR(export, 0220, 0, vendor_export_store); /* 2011/08/10 KDDI : permission change 0202->0220 */

/* when 'unexport'file update */
static ssize_t vendor_unexport_store(struct device *dev,
                struct device_attribute *attr, const char *buf, size_t len)
{
	long cmd;
	int status;

	status = strict_strtol(buf, 0, &cmd);
	if (status < 0)
		goto done;

	status = -EINVAL;

	if (!vendor_cmd_is_valid(cmd))
		goto done;

	mutex_lock(&sysfs_lock);

	status = 0;
	vendor_cmd_unexport(dev, cmd);

	mutex_unlock(&sysfs_lock);
done:
	if (status)
		pr_debug(KERN_INFO"%s: status %d\n", __func__, status);
	return status ? : len;
}
/* define 'unexport'file */
//static DEVICE_ATTR(unexport, 0202, 0, vendor_unexport_store); /* 2011/04/19 KDDI : permission change 0200->0202 */
static DEVICE_ATTR(unexport, 0220, 0, vendor_unexport_store); /* 2011/08/10 KDDI : permission change 0202->0220 */

/* set 'inquiry'file */
static ssize_t vendor_inquiry_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct fsg_lun *curlun = fsg_lun_from_dev(dev);
	ssize_t status;

	mutex_lock(&sysfs_lock);
	status = sprintf(buf, "\"%s\"\n", curlun->inquiry_vendor);

	mutex_unlock(&sysfs_lock);
	return status;
}

/* get 'inquiry'file */
static ssize_t vendor_inquiry_store(struct device *dev,
                struct device_attribute *attr, const char *buf, size_t len)
{
	struct fsg_lun *curlun = fsg_lun_from_dev(dev);

	mutex_lock(&sysfs_lock);
	strncpy(curlun->inquiry_vendor, buf,
					sizeof curlun->inquiry_vendor);
	curlun->inquiry_vendor[sizeof curlun->inquiry_vendor - 1] = '\0';

	mutex_unlock(&sysfs_lock);
	return len;
}

/* define 'inquiry'file */
//static DEVICE_ATTR(inquiry, 0606, vendor_inquiry_show, vendor_inquiry_store); /* 2011/04/19 KDDI : permission change 0600->0606 */
static DEVICE_ATTR(inquiry, 0660, vendor_inquiry_show, vendor_inquiry_store); /* 2011/08/10 KDDI : permission change 0606->0660 */

static void op_release(struct device *dev)
{
}
/* [ADD END] 2011/04/15 KDDI : functions to handle vendor command */
/* [ADD END] 2012/01/17 KDDI : Android ICS */

/****************************** FSG COMMON ******************************/

static void fsg_common_release(struct kref *ref);

static void fsg_lun_release(struct device *dev)
{
	/* Nothing needs to be done */
}

static inline void fsg_common_get(struct fsg_common *common)
{
	kref_get(&common->ref);
}

static inline void fsg_common_put(struct fsg_common *common)
{
	kref_put(&common->ref, fsg_common_release);
}

static ssize_t print_switch_name(struct switch_dev *sdev, char *buf);
static ssize_t print_switch_state(struct switch_dev *sdev, char *buf);

static struct fsg_common *fsg_common_init(struct fsg_common *common,
					  struct usb_composite_dev *cdev,
					  struct fsg_config *cfg)
{
	struct usb_gadget *gadget = cdev->gadget;
	struct fsg_buffhd *bh;
	struct fsg_lun *curlun;
	struct fsg_lun_config *lcfg;
	int nluns, i, rc;
/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/10/11 KDDI : "LUN1" is not created */
	int j;
/* [ADD END] 2011/10/11 KDDI : "LUN1" is not created */
/* [ADD END] 2012/01/17 KDDI : Android ICS */
	char *pathbuf;

	cfg->vendor_name = VENDOR_NAME;
	cfg->product_name = PRODUCT_NAME;
	cfg->release = RELEASE_NO;

	/* Find out how many LUNs there should be */
	nluns = cfg->nluns;
	if (nluns < 1 || nluns > FSG_MAX_LUNS) {
		dev_err(&gadget->dev, "invalid number of LUNs: %u\n", nluns);
		return ERR_PTR(-EINVAL);
	}

	/* Allocate? */
	if (!common) {
		common = kzalloc(sizeof *common, GFP_KERNEL);
		if (!common)
			return ERR_PTR(-ENOMEM);
		common->free_storage_on_release = 1;
	} else {
		memset(common, 0, sizeof *common);
		common->free_storage_on_release = 0;
	}

	common->ops = cfg->ops;
	common->private_data = cfg->private_data;

	common->gadget = gadget;
	common->ep0 = gadget->ep0;
	common->ep0req = cdev->req;
	common->cdev = cdev;

#if 1
	fsg_intf_desc.iInterface = 0;
#else
	/* Maybe allocate device-global string IDs, and patch descriptors */
	if (fsg_strings[FSG_STRING_INTERFACE].id == 0) {
		rc = usb_string_id(cdev);
		if (unlikely(rc < 0))
			goto error_release;
		fsg_strings[FSG_STRING_INTERFACE].id = rc;
		fsg_intf_desc.iInterface = rc;
	}
#endif

	common->sdev.name = KC_VENDOR_NAME;
	common->sdev.print_name = print_switch_name;
	common->sdev.print_state = print_switch_state;
	rc = switch_dev_register(&common->sdev);
	if (unlikely(rc)) {
		goto error_release;
	}

	/*
	 * Create the LUNs, open their backing files, and register the
	 * LUN devices in sysfs.
	 */
	curlun = kzalloc(nluns * sizeof *curlun, GFP_KERNEL);
	if (unlikely(!curlun)) {
		rc = -ENOMEM;
		goto error_release;
	}
	common->luns = curlun;

	init_rwsem(&common->filesem);

	for (i = 0, lcfg = cfg->luns; i < nluns; ++i, ++curlun, ++lcfg) {
		curlun->cdrom = !!lcfg->cdrom;
		curlun->ro = lcfg->cdrom || lcfg->ro;
		curlun->initially_ro = curlun->ro;
		curlun->removable = lcfg->removable;
		curlun->nofua = lcfg->nofua;
		curlun->dev.release = fsg_lun_release;
		curlun->dev.parent = &gadget->dev;
		/* curlun->dev.driver = &fsg_driver.driver; XXX */
		dev_set_drvdata(&curlun->dev, &common->filesem);
		dev_set_name(&curlun->dev,
			     cfg->lun_name_format
			   ? cfg->lun_name_format
			   : "lun%d",
			     i);

		rc = device_register(&curlun->dev);
		if (rc) {
			INFO(common, "failed to register LUN%d: %d\n", i, rc);
			common->nluns = i;
			put_device(&curlun->dev);
			goto error_release;
		}

		rc = device_create_file(&curlun->dev, &dev_attr_ro);
		if (rc)
			goto error_luns;
		rc = device_create_file(&curlun->dev, &dev_attr_file);
		if (rc)
			goto error_luns;
		rc = device_create_file(&curlun->dev, &dev_attr_nofua);
		if (rc)
			goto error_luns;
#ifdef CONFIG_USB_MSC_PROFILING
		rc = device_create_file(&curlun->dev, &dev_attr_perf);
		if (rc)
			dev_err(&gadget->dev, "failed to create sysfs entry:"
				"(dev_attr_perf) error: %d\n", rc);
#endif
/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [CHANGE START] 2011/07/27 KDDI : 'export' file create ,[Lun0] only */
		if ( i==0 ){
/* [ADD START] 2011/04/15 KDDI : create file for vendor command */
			rc = device_create_file(&curlun->dev, &dev_attr_export);
			if (rc)
				goto error_luns;
			rc = device_create_file(&curlun->dev, &dev_attr_unexport);
			if (rc)
				goto error_luns;
			rc = device_create_file(&curlun->dev, &dev_attr_inquiry);
			if (rc)
				goto error_luns;
/* [CHANGE START] 2011/05/26 KDDI : initital inquiry response */
			memset(curlun->inquiry_vendor, 0, sizeof curlun->inquiry_vendor);
			strcpy(curlun->inquiry_vendor, INQUIRY_VENDOR_INIT);
/* [CHANGE END] 2011/05/26 KDDI : initital inquiry response */
/* [ADD END] 2011/04/15 KDDI : create file for vendor command */

/* [ADD START] 2011/08/26 KDDI : alloc for commn buffer ,and make e4-buffer*/
/* [CHANGE START] 2011/10/11 KDDI : "LUN1" is not created */
			for (j=0; j < ALLOC_CMD_CNT; j++){
				curlun->reserve_buf[j] = kzalloc(ALLOC_INI_SIZE, GFP_KERNEL);
				printk("[fms_CR7]%s alloc buf[%d]\n", __func__,j);
				if(!curlun->reserve_buf[j]){
					printk("[fms_CR7]%s Error : buffer malloc fail! cmd_idx=%d \n", __func__, j);
/* [CHANGE END] 2011/10/11 KDDI : "LUN1" is not created */
					rc = -ENOMEM;
					goto error_release;
				}
			}

/* [CHANGE START] 2011/08/26 KDDI : at initialization, change the attributes */
			rc = vendor_cmd_export(&curlun->dev, 0xe4, 1);
/* [CHANGE END] 2011/08/26 KDDI : at initialization, change the attributes */
			if (rc < 0){
				vendor_cmd_unexport(&curlun->dev, 0xe4);
				goto error_release;
			}
/* [ADD END] 2011/08/26 KDDI : alloc for commn buffer ,and make e4-buffer*/
		}
/* [CHANGE END] 2011/07/27 KDDI : 'export' file create ,[Lun0] only */
/* [ADD END] 2012/01/17 KDDI : Android ICS */

		if (lcfg->filename) {
			rc = fsg_lun_open(curlun, lcfg->filename);
			if (rc)
				goto error_luns;
		} else if (!curlun->removable) {
			ERROR(common, "no file given for LUN%d\n", i);
			rc = -EINVAL;
			goto error_luns;
		}
	}
	common->nluns = nluns;

	/* Data buffers cyclic list */
	bh = common->buffhds;
	i = FSG_NUM_BUFFERS;
	goto buffhds_first_it;
	do {
		bh->next = bh + 1;
		++bh;
buffhds_first_it:
		bh->buf = kmalloc(FSG_BUFLEN, GFP_KERNEL);
		if (unlikely(!bh->buf)) {
			rc = -ENOMEM;
			goto error_release;
		}
	} while (--i);
	bh->next = common->buffhds;

	/* Prepare inquiryString */
	if (cfg->release != 0xffff) {
		i = cfg->release;
	} else {
		i = usb_gadget_controller_number(gadget);
		if (i >= 0) {
			i = 0x0300 + i;
		} else {
			WARNING(common, "controller '%s' not recognized\n",
				gadget->name);
			i = 0x0399;
		}
	}
	snprintf(common->inquiry_string, sizeof common->inquiry_string,
		 "%-8s%-16s%04x", cfg->vendor_name ?: "Linux",
		 /* Assume product name dependent on the first LUN */
		 cfg->product_name ?: (common->luns->cdrom
				     ? "File-Stor Gadget"
				     : "File-CD Gadget"),
		 i);

	/*
	 * Some peripheral controllers are known not to be able to
	 * halt bulk endpoints correctly.  If one of them is present,
	 * disable stalls.
	 */
	common->can_stall = cfg->can_stall &&
		!(gadget_is_at91(common->gadget));

	spin_lock_init(&common->lock);
	kref_init(&common->ref);

	/* Tell the thread to start working */
	common->thread_task =
		kthread_create(fsg_main_thread, common,
			       cfg->thread_name ?: "file-storage");
	if (IS_ERR(common->thread_task)) {
		rc = PTR_ERR(common->thread_task);
		goto error_release;
	}
	init_completion(&common->thread_notifier);
	init_waitqueue_head(&common->fsg_wait);

	/* Information */
	INFO(common, FSG_DRIVER_DESC ", version: " FSG_DRIVER_VERSION "\n");
	INFO(common, "Number of LUNs=%d\n", common->nluns);

	pathbuf = kmalloc(PATH_MAX, GFP_KERNEL);
	for (i = 0, nluns = common->nluns, curlun = common->luns;
	     i < nluns;
	     ++curlun, ++i) {
		char *p = "(no medium)";
		if (fsg_lun_is_open(curlun)) {
			p = "(error)";
			if (pathbuf) {
				p = d_path(&curlun->filp->f_path,
					   pathbuf, PATH_MAX);
				if (IS_ERR(p))
					p = "(error)";
			}
		}
		LINFO(curlun, "LUN: %s%s%sfile: %s\n",
		      curlun->removable ? "removable " : "",
		      curlun->ro ? "read only " : "",
		      curlun->cdrom ? "CD-ROM " : "",
		      p);
	}
	kfree(pathbuf);

	DBG(common, "I/O thread pid: %d\n", task_pid_nr(common->thread_task));

	wake_up_process(common->thread_task);

	common->unq_vendor =
		kzalloc(sizeof(struct unq_vendor_info), GFP_KERNEL);
	VUNIQ_DBG("%s:unq_vendor = 0x%08x\n", __func__,
			(int)common->unq_vendor);
	if (unlikely(!common->unq_vendor)) {
		rc = -ENOMEM;
		goto error_release;
	}
	rc = misc_register(&usbmass_dev_misc);
	VUNIQ_DBG("%s:misc_register(usbmass_dev_misc):end return %d\n", __func__, rc);
	if (rc) {
		goto error_release;
	}
	init_waitqueue_head(&(usbmass_dev.rwait));
	init_waitqueue_head(&(usbmass_dev.wwait));
	spin_lock_init(&(usbmass_dev.lock));
	/* until power off using ,not call "kfree","misc_deregister" */

	common->fsync_counter = 0;

	the_common = common;

	return common;

error_luns:
	common->nluns = i + 1;
error_release:
	common->state = FSG_STATE_TERMINATED;	/* The thread is dead */
	/* Call fsg_common_release() directly, ref might be not initialised. */
	fsg_common_release(&common->ref);
	return ERR_PTR(rc);
}

static void fsg_common_release(struct kref *ref)
{
	struct fsg_common *common = container_of(ref, struct fsg_common, ref);

	/* If the thread isn't already dead, tell it to exit now */
	if (common->state != FSG_STATE_TERMINATED) {
		raise_exception(common, FSG_STATE_EXIT);
		wait_for_completion(&common->thread_notifier);
	}

	if (likely(common->luns)) {
		struct fsg_lun *lun = common->luns;
		unsigned i = common->nluns;

/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [ADD START] 2011/04/15 KDDI : delete file for vendor command */
		unsigned j;
/* [ADD END] 2011/04/15 KDDI : delete file for vendor command */
/* [ADD END] 2012/01/17 KDDI : Android ICS */
		/* In error recovery common->nluns may be zero. */
		for (; i; --i, ++lun) {
#ifdef CONFIG_USB_MSC_PROFILING
			device_remove_file(&lun->dev, &dev_attr_perf);
#endif
/* [ADD START] 2012/01/17 KDDI : Android ICS */
/* [CHANGE START] 2011/07/27 KDDI : 'export' file create ,[Lun0] only */
			if (i == common->nluns){
/* [ADD START] 2011/04/15 KDDI : delete file for vendor command */
				for (j=SC_VENDOR_START; j < SC_VENDOR_END + 1; j++) {
					vendor_cmd_unexport(&lun->dev, j);
/* [ADD START] 2011/08/26 KDDI : check init alloc */
					if ( j-SC_VENDOR_START < ALLOC_CMD_CNT ){
						printk("[fms_CR7]%s kfree buf[%d]\n", __func__,j-SC_VENDOR_START);
						kfree(lun->reserve_buf[j-SC_VENDOR_START]);
					}
/* [ADD END] 2011/08/26 KDDI : check init alloc */
				}
				device_remove_file(&lun->dev, &dev_attr_export);
				device_remove_file(&lun->dev, &dev_attr_unexport);
				device_remove_file(&lun->dev, &dev_attr_inquiry);
/* [ADD END] 2011/04/15 KDDI : delete file for vendor command */
			}
/* [CHANGE END] 2011/07/27 KDDI : 'export' file create ,[Lun0] only */
/* [ADD END] 2012/01/17 KDDI : Android ICS */
			device_remove_file(&lun->dev, &dev_attr_nofua);
			device_remove_file(&lun->dev, &dev_attr_ro);
			device_remove_file(&lun->dev, &dev_attr_file);
			fsg_lun_close(lun);
			device_unregister(&lun->dev);
		}

		kfree(common->luns);
	}

	{
		struct fsg_buffhd *bh = common->buffhds;
		unsigned i = FSG_NUM_BUFFERS;
		do {
			kfree(bh->buf);
		} while (++bh, --i);
	}

	switch_dev_unregister(&common->sdev);

	if (common->free_storage_on_release)
		kfree(common);
}


/*-------------------------------------------------------------------------*/

static void fsg_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct fsg_dev		*fsg = fsg_from_func(f);
	struct fsg_common	*common = fsg->common;

	DBG(fsg, "unbind\n");
	if (fsg->common->fsg == fsg) {
		fsg->common->new_fsg = NULL;
		raise_exception(fsg->common, FSG_STATE_CONFIG_CHANGE);
		/* FIXME: make interruptible or killable somehow? */
		wait_event(common->fsg_wait, common->fsg != fsg);
	}

	fsg_common_put(common);
	usb_free_descriptors(fsg->function.descriptors);
	usb_free_descriptors(fsg->function.hs_descriptors);
	usb_free_descriptors(fsg->function.ss_descriptors);
	kfree(fsg);
}

static int fsg_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct fsg_dev		*fsg = fsg_from_func(f);
	struct usb_gadget	*gadget = c->cdev->gadget;
	int			i;
	struct usb_ep		*ep;

	fsg->gadget = gadget;

	/* New interface */
	i = usb_interface_id(c, f);
	if (i < 0)
		return i;
	fsg_intf_desc.bInterfaceNumber = i;
	fsg->interface_number = i;

	/* Find all the endpoints we will use */
	ep = usb_ep_autoconfig(gadget, &fsg_fs_bulk_in_desc);
	if (!ep)
		goto autoconf_fail;
	ep->driver_data = fsg->common;	/* claim the endpoint */
	fsg->bulk_in = ep;

	ep = usb_ep_autoconfig(gadget, &fsg_fs_bulk_out_desc);
	if (!ep)
		goto autoconf_fail;
	ep->driver_data = fsg->common;	/* claim the endpoint */
	fsg->bulk_out = ep;

	/* Copy descriptors */
	f->descriptors = usb_copy_descriptors(fsg_fs_function);
	if (unlikely(!f->descriptors))
		return -ENOMEM;

	if (gadget_is_dualspeed(gadget)) {
		/* Assume endpoint addresses are the same for both speeds */
		fsg_hs_bulk_in_desc.bEndpointAddress =
			fsg_fs_bulk_in_desc.bEndpointAddress;
		fsg_hs_bulk_out_desc.bEndpointAddress =
			fsg_fs_bulk_out_desc.bEndpointAddress;
		f->hs_descriptors = usb_copy_descriptors(fsg_hs_function);
		if (unlikely(!f->hs_descriptors)) {
			usb_free_descriptors(f->descriptors);
			return -ENOMEM;
		}
	}

	if (gadget_is_superspeed(gadget)) {
		unsigned	max_burst;

		/* Calculate bMaxBurst, we know packet size is 1024 */
		max_burst = min_t(unsigned, FSG_BUFLEN / 1024, 15);

		fsg_ss_bulk_in_desc.bEndpointAddress =
			fsg_fs_bulk_in_desc.bEndpointAddress;
		fsg_ss_bulk_in_comp_desc.bMaxBurst = max_burst;

		fsg_ss_bulk_out_desc.bEndpointAddress =
			fsg_fs_bulk_out_desc.bEndpointAddress;
		fsg_ss_bulk_out_comp_desc.bMaxBurst = max_burst;

		f->ss_descriptors = usb_copy_descriptors(fsg_ss_function);
		if (unlikely(!f->ss_descriptors)) {
			usb_free_descriptors(f->hs_descriptors);
			usb_free_descriptors(f->descriptors);
			return -ENOMEM;
		}
	}

	return 0;

autoconf_fail:
	ERROR(fsg, "unable to autoconfigure all endpoints\n");
	return -ENOTSUPP;
}


/********************* ADD FUNCTION(HDR Cooperation) ***********************/
/*===========================================================================
FUNCTION do_read_vendor

DESCRIPTION
 vender  write.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static int do_read_vendor(struct fsg_common *common)
{
	int rc = -EIO;
	unsigned long flags;
	struct unq_vendor_info *vendor = common->unq_vendor;
	struct fsg_buffhd *bh = common->next_buffhd_to_fill;

	VUNIQ_DBG("%s:start\n", __func__);

	for (;;) {
		if (vendor->vendor_exit) {
			VUNIQ_DBG("%s:vendor_exit == 1\n", __func__);
			break;
		}
		bh = common->next_buffhd_to_fill;

		VUNIQ_DBG("%s:start_in_transfer\n", __func__);
		/* Send this buffer and go read some more */
		bh->state = BUF_STATE_FULL;
		bh->inreq->zero = 0;
		if (!start_in_transfer(common, bh))
			/* Don't know what to do if
			 * common->fsg is NULL */
			goto vendor_fail;
		common->next_buffhd_to_fill = bh->next;

		while (bh->state != BUF_STATE_EMPTY) {
			rc = sleep_thread(common);
			if (rc) {
				VUNIQ_ERR("%s:err sleep_thread()\n", __func__);
				goto vendor_fail;
			}
		}

		VUNIQ_DBG("%s:(bh->state == %d \n", __func__, bh->state);

		spin_lock_irqsave(&(usbmass_dev.lock), flags);
		usbmass_dev.wcomplete = 1;
		vendor->vseq = VSEQ_IO_EVENT_WAIT;
		spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
		wake_up_interruptible(&(usbmass_dev.wwait));

		while (vendor->vseq == VSEQ_IO_EVENT_WAIT) {
			rc = sleep_thread(common);
			if (rc) {
				VUNIQ_ERR("%s:err sleep_thread()\n", __func__);
				goto vendor_fail;
			}
		}
		VUNIQ_DBG("%s:vendor->vseq == %d\n", __func__, vendor->vseq);
	}
	vendor->vendor_exit = 0;
	VUNIQ_DBG("%s:end return %d\n", __func__, -EIO);
	return -EIO;		/* No default reply */


vendor_fail:
	VUNIQ_ERR("%s:err VSEQ_ERROR\n", __func__);
	spin_lock_irqsave(&(usbmass_dev.lock), flags);
	usbmass_dev.wcomplete = 1;
	vendor->vseq = VSEQ_ERROR;
	vendor->vendor_exit = 0;
	spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
	wake_up_interruptible(&(usbmass_dev.wwait));
	VUNIQ_DBG("%s:end return %d\n", __func__, rc);
	return rc;		/* No default reply */
}


/*===========================================================================
FUNCTION do_write_vendor

DESCRIPTION
 vender  write.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static int do_write_vendor(struct fsg_common *common)
{
	struct fsg_buffhd	*bh;
	int			get_some_more;
	unsigned int		amount;
	int			rc = -EIO;
	struct unq_vendor_info *vendor = common->unq_vendor;
	unsigned long flags;

	VUNIQ_DBG("%s:start\n", __func__);

	get_some_more = 1;
	amount = common->residue;

	for (;;) {
		if (vendor->vendor_exit) {
			break;
		}

		/* Queue a request for more data from the host */
		bh = common->next_buffhd_to_fill;
		if (bh->state == BUF_STATE_EMPTY && get_some_more) {
			bh->outreq->length = amount;
			if (!start_out_transfer(common, bh))
				/* Don't know what to do if
				 * common->fsg is NULL */
				goto vendor_fail;
			common->next_buffhd_to_fill = bh->next;
			get_some_more = 0;
			continue;
		}

		/* Wait for something to happen */
		rc = sleep_thread(common);
		if (rc) {
			VUNIQ_ERR("%s:err sleep_thread()\n", __func__);
			goto vendor_fail;
		}

		/* Write the received data to the backing file */
		bh = common->next_buffhd_to_drain;

		if (bh->state == BUF_STATE_FULL) {
			smp_rmb();

			spin_lock_irqsave(&(usbmass_dev.lock), flags);
			usbmass_dev.rcomplete = 1;
			vendor->vseq = VSEQ_IO_EVENT_WAIT;
			spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
			wake_up_interruptible(&(usbmass_dev.rwait));

			while (vendor->vseq == VSEQ_IO_EVENT_WAIT) {
				rc = sleep_thread(common);
				if (rc) {
					common->next_buffhd_to_drain =
								 bh->next;
					bh->state = BUF_STATE_EMPTY;
					VUNIQ_ERR("%s:err sleep_thread()\n",
								 __func__);
					goto vendor_fail;
				}
			}
			common->next_buffhd_to_drain = bh->next;
			bh->state = BUF_STATE_EMPTY;
			get_some_more = 1;
			amount = common->residue;
		}
	}
	vendor->vendor_exit = 0;
	VUNIQ_DBG("%s:end return %d\n", __func__, -EIO);
	return -EIO;		/* No default reply */

vendor_fail:
	VUNIQ_ERR("%s:err VSEQ_ERROR\n", __func__);
	spin_lock_irqsave(&(usbmass_dev.lock), flags);
	usbmass_dev.rcomplete = 1;
	vendor->vseq = VSEQ_ERROR;
	vendor->vendor_exit = 0;
	spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
	wake_up_interruptible(&(usbmass_dev.rwait));
	VUNIQ_DBG("%s:end return %d\n", __func__, rc);
	return rc;		/* No default reply */
}


/*===========================================================================
FUNCTION vender_unique_command

DESCRIPTION
 vender unique command command transfer.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .
 bh      [out]     buffer pointer.
 reply   [out]     reply status.

RETURN VALUE
       -1:unknown command
        0:OK

SIDE EFFECTS
 none
===========================================================================*/
static int vender_unique_command(struct fsg_common *common,
			struct fsg_buffhd *bh, int32_t* reply_ptr)
{
	int rc = 0;
	unsigned long flags;
	struct unq_vendor_info *vendor;

	VUNIQ_DBG("%s:start\n", __func__);

	if (common->unq_vendor == NULL) {
		VUNIQ_ERR("%s:err common->unq_vendor NULL return %d\n",
				 __func__, -ENOMEM);
		return -ENOMEM;
	}

	if (g_vendor_info == NULL) {
		VUNIQ_ERR("%s:err g_vendor_info NULL return %d\n",
				 __func__, -EFAULT);
		return -EFAULT;
	}

	vendor = common->unq_vendor;

	/* command event */
	spin_lock_irqsave(&(usbmass_dev.lock), flags);
	usbmass_dev.rcomplete = 1;
	vendor->vseq = VSEQ_IO_EVENT_WAIT;
	spin_unlock_irqrestore(&(usbmass_dev.lock), flags);
	wake_up_interruptible(&(usbmass_dev.rwait));

	/* wait event */
	while (vendor->vseq == VSEQ_IO_EVENT_WAIT) {
		rc = sleep_thread(common);
		if (rc) {
			VUNIQ_ERR("%s:err sleep_thread() return %d\n", __func__, rc);
			return rc;
		}
	}

	VUNIQ_DBG("%s:start vendor->vseq[%d]\n", __func__, vendor->vseq);
	switch (vendor->vseq) {
	case VSEQ_TX_DATA :
		*reply_ptr = do_read_vendor(common);
		break;
	case VSEQ_RX_DATA:
		*reply_ptr = do_write_vendor(common);
		break;
	case VSEQ_DO_READ:
		common->data_size_from_cmnd =
			get_unaligned_be16(&common->cmnd[7]) << 9;
		*reply_ptr = check_command(common, 10, DATA_DIR_TO_HOST,
					(1<<1) | (0xf<<2) | (3<<7), 1,
					"READ(10)");
		if (*reply_ptr == 0)
			*reply_ptr = do_read(common);
		break;
	case VSEQ_DO_WRITE:
		common->data_size_from_cmnd =
				get_unaligned_be16(&common->cmnd[7]) << 9;
		*reply_ptr = check_command(common, 10, DATA_DIR_FROM_HOST,
					(1<<1) | (0xf<<2) | (3<<7), 1,
					"WRITE(10)");
		if (*reply_ptr == 0) {
			*reply_ptr = do_write(common);
		}
		break;
	case VSEQ_NO_DATA:
		*reply_ptr = 0;
		break;
	case VSEQ_INVALID_CMD:
	default:
		rc = -EINVAL;
		break;
	}
	if (vendor->vseq != VSEQ_ERROR) {
		vendor->vseq = VSEQ_NONE;
		VUNIQ_DBG("%s:set vendor->vseq = VSEQ_NONE\n", __func__);
	}
	VUNIQ_DBG("%s:end rc %d\n", __func__, rc);
	return rc;
}


/*===========================================================================
FUNCTION usbmass_open

DESCRIPTION
 device file open.

DEPENDENCIES
 inode  [in]      device file inode pointer.
 filp   [in/out]  file status pointer.

RETURN VALUE
 0 on success

SIDE EFFECTS
 none
===========================================================================*/
static int usbmass_open(struct inode *inode, struct file *filp)
{
	VUNIQ_DBG("%s:start\n", __func__);
	/* save device in the file's private structure */
	usbmass_dev.connect_chg = 0;
	usbmass_dev.rcomplete = 0;
	usbmass_dev.wcomplete = 0;
	filp->private_data = &usbmass_dev;
	VUNIQ_DBG("%s:end\n", __func__);
	return 0;
}


/*===========================================================================
FUNCTION usbmass_poll

DESCRIPTION
 event polling.

DEPENDENCIES
 filp  [in]    file status pointer.
 wait  [in]    poll table pointer.

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static unsigned int usbmass_poll(struct file *filp, poll_table *wait)
{
	int ret = 0;
	unsigned long flags;
	struct usbmass_device *dev = filp->private_data;
	struct fsg_common *common = the_common;
	struct unq_vendor_info *vendor =  common->unq_vendor;
	vendor = common->unq_vendor;

	VUNIQ_DBG("%s:start\n", __func__);

	poll_wait(filp, &(dev->rwait), wait);

	poll_wait(filp, &(dev->wwait), wait);

	spin_lock_irqsave(&(dev->lock), flags);

	VUNIQ_DBG("%s:dev->connect_chg = 0x%04x \n",
			__func__, dev->connect_chg);
	VUNIQ_DBG("%s:dev->rcomplete = %d, dev->wcomplete =%d \n",
			__func__, dev->rcomplete, dev->wcomplete);

	if (vendor->vseq == VSEQ_ERROR) {
		VUNIQ_ERR("%s:err POLLERR\n", __func__);
		ret = POLLERR;
		vendor->vseq = VSEQ_NONE;
		VUNIQ_DBG("%s:set vendor->vseq = VSEQ_NONE\n", __func__);
	}

	if (dev->connect_chg) {
		ret |= (POLLIN | POLLOUT | dev->connect_chg);
		dev->connect_chg = 0;
	} else if ((dev->rcomplete) ) {
		ret |= (POLLIN  | POLLRDNORM);
		dev->rcomplete = 0;
	} else if (dev->wcomplete) {
		ret |= (POLLOUT | POLLWRNORM);
		dev->wcomplete = 0;
	}
	spin_unlock_irqrestore(&(dev->lock), flags);
	VUNIQ_DBG("%s:end ret 0x%04x\n", __func__, ret);
	return ret;
}


/*===========================================================================
FUNCTION ioc_get_data

DESCRIPTION
 vender  write.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .
 data    [in]      user data pointer .

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static int ioc_get_data(struct fsg_common *common,
			struct ioc_data_type* data)
{
	int rc = 0;
	struct fsg_buffhd *bh = common->next_buffhd_to_drain;

	VUNIQ_DBG("%s:start\n", __func__);
	/* Did the host decide to stop early? */
	if (bh->outreq->actual != bh->outreq->length) {
		common->short_packet_received = 1;
		VUNIQ_ERR("%s:err bh->outreq->actual\n", __func__);
		return -EFAULT;
	}

	rc = put_user(bh->outreq->actual, &data->BuffSize);
	if (rc) {
		VUNIQ_ERR("%s:err put_user()\n", __func__);
		return -EFAULT;
	}
	VUNIQ_DUMP(bh->buf, 16);
	rc = copy_to_user(data->Buff, bh->buf, bh->outreq->actual);
	if (rc) {
		VUNIQ_ERR("%s:err copy_to_user()\n", __func__);
		return -EFAULT;
	}
	VUNIQ_DBG("%s:end rc %d\n", __func__, rc);
	return rc;
}


/*===========================================================================
FUNCTION ioc_tx_data

DESCRIPTION
 vender  write.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .
 data    [in]      user data pointer .

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static int ioc_tx_data(struct fsg_common *common,
			struct ioc_data_type* data)
{
	int rc = 0;
	struct ioc_data_type st_data;
	struct unq_vendor_info *vendor = common->unq_vendor;
	struct fsg_buffhd *bh = common->next_buffhd_to_fill;
	struct fsg_lun *curlun = common->curlun;
	int flg_data = 0;

	VUNIQ_DBG("%s:start\n", __func__);
	vendor->vendor_exit = 0;
	rc = copy_from_user(&st_data, data, sizeof(struct ioc_data_type));
	if (rc) {
		st_data.CSW.SenseData = SS_UNRECOVERED_READ_ERROR;
		vendor->vendor_exit = 1;
		VUNIQ_ERR("%s:err copy_to_user()\n", __func__);
		rc = -EFAULT;
	} else {
		VUNIQ_DBG("%s:Request[%d]\n",
			 __func__, st_data.Request);
		switch (st_data.Request){
		case USBMAS_REQUEST_DATA_LAST:
			vendor->vendor_exit = 1;
		case USBMAS_REQUEST_DATA:
			if (st_data.BuffSize) {
				flg_data = 1;
			} else {
				/* not set vendor_exit because...secure_mass modules */
				/*  ioctl USBMAS_REQUEST_DATA_ERROR                  */
				/* vendor->vendor_exit = 1; */
				st_data.CSW.SenseData = SS_UNRECOVERED_READ_ERROR;
				VUNIQ_ERR("%s:err BuffSize 0 \n", __func__);
				rc = -EINVAL;
			}
			break;
		case USBMAS_REQUEST_DATA_ERROR:
			vendor->vendor_exit = 1;
			break;
		}
	}

	if (flg_data) {
		VUNIQ_DBG("%s:bh->state[%d]\n", __func__, bh->state);
		/* wait buffer empty */
		while (bh->state != BUF_STATE_EMPTY) {
			rc = sleep_thread(common);
			if (rc) {
				st_data.CSW.SenseData = SS_UNRECOVERED_READ_ERROR;
				VUNIQ_ERR("%s:err sleep_thread() return %d\n", __func__, -EFAULT);
				return -EFAULT;
			}
		}

		rc = copy_from_user(bh->buf, st_data.Buff, st_data.BuffSize);
		if (rc) {
			st_data.CSW.SenseData = SS_UNRECOVERED_READ_ERROR;
			VUNIQ_ERR("%s:err copy_from_user()\n", __func__);
			rc = -EFAULT;
		} else {
			bh->inreq->length = st_data.BuffSize;
		}
		VUNIQ_DUMP(bh->buf, 16);
		VUNIQ_DBG("%s:bh->inreq->length[%d] BuffSize[%d] \n",
			 __func__, bh->inreq->length,  st_data.BuffSize);
	}
	if (vendor->vendor_exit) {
		common->residue = st_data.CSW.Residue;
		curlun->sense_data = st_data.CSW.SenseData;
		VUNIQ_DBG("%s:set CSW residue[%d] sense_data[0x%06X]\n",
			__func__, common->residue, curlun->sense_data);
		if (st_data.CSW.Status == USB_STATUS_PHASE_ERROR) {
			VUNIQ_DBG("%s:set phase_error\n", __func__);
			common->phase_error = 1;
		}
	}
	VUNIQ_DBG("%s:end rc %d\n", __func__, rc);
	return rc;
}


/*===========================================================================
FUNCTION ioc_rx_data

DESCRIPTION
 vender  write.

DEPENDENCIES
 common  [in/out]  pointer Data shared by all the FSG instances .
 data    [in]      user data pointer .

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS
 none
===========================================================================*/
static int ioc_rx_data(struct fsg_common *common,
			struct ioc_data_type* data)
{
	int rc = 0;
	struct ioc_data_type st_data;
	struct unq_vendor_info *vendor = common->unq_vendor;
	struct fsg_lun *curlun = common->curlun;

	VUNIQ_DBG("%s:start\n", __func__);
	vendor->vendor_exit = 0;

	rc = copy_from_user(&st_data, data, sizeof(struct ioc_data_type));
	if (rc) {
		st_data.CSW.SenseData = SS_WRITE_ERROR;
		vendor->vendor_exit = 1;
		VUNIQ_ERR("%s:err copy_from_user()\n", __func__);
		rc = -EFAULT;
	} else {
		VUNIQ_DBG("%s:Request[%d]\n",
			 __func__, st_data.Request);
		switch (st_data.Request){
		case USBMAS_REQUEST_DATA:
			common->residue = st_data.BuffSize;
			break;
		case USBMAS_REQUEST_DATA_LAST:
		case USBMAS_REQUEST_DATA_ERROR:
		default:
			vendor->vendor_exit = 1;
			break;
		}
	}
	if (vendor->vendor_exit) {
		common->residue = st_data.CSW.Residue;
		curlun->sense_data = st_data.CSW.SenseData;
		VUNIQ_DBG("%s:set CSW residue[%d] sense_data[0x%06X]\n",
			__func__, common->residue, curlun->sense_data);
		if (st_data.CSW.Status == USB_STATUS_PHASE_ERROR) {
			VUNIQ_DBG("%s:set phase_error\n", __func__);
			common->phase_error = 1;
		}
	}
	VUNIQ_DBG("%s:end rc %d\n", __func__, rc);
	return rc;
}


/*===========================================================================
FUNCTION usbmass_ioctl

DESCRIPTION
 event ioctl.

DEPENDENCIES
 filp   [in]       file status pointer.
 cmd    [in/out]   command pointer.
 arg    [in/out]   paramater pointer.

RETURN VALUE
 0 on success, otherwise an error code

SIDE EFFECTS

===========================================================================*/
static long usbmass_ioctl(struct file *filp, unsigned cmd, unsigned long arg)
{
	long rc = 0;
	void __user *u_arg = (void __user *)arg;
	struct usbmass_device *dev = filp->private_data;
	struct fsg_common *common = the_common;
	struct unq_vendor_info *vendor =  common->unq_vendor;
	enum vendor_sequence vseq = VSEQ_NONE;

	VUNIQ_DBG("%s:start cmd = 0x%04X\n", __func__, cmd);
	switch (cmd) {
	case USBMAS_IOC_START_NOTIFY:
		if (g_vendor_info == NULL) {
			g_vendor_info =
				kzalloc(sizeof(struct ioc_startinfo_type),
				GFP_KERNEL);
			VUNIQ_DBG("%s:g_vendor_info = 0x%08x\n",
					 __func__, (int)g_vendor_info);
		}
		if (g_vendor_info) {
			rc = copy_from_user(g_vendor_info, u_arg, 
					sizeof(struct ioc_startinfo_type));
			if (rc) {
				VUNIQ_ERR("%s:err copy_from_user()\n", __func__);
				rc = -EFAULT;
			}
			VUNIQ_DBG("%s:Offset[%d] DataSize[%d]\n",
				__func__, g_vendor_info->Offset,
				g_vendor_info->DataSize);
			VUNIQ_DUMP(g_vendor_info->Data,
					USBMAS_MAX_COMMAND_SIZE);
		}
		break;
	case USBMAS_IOC_GET_COMMAND:
		{
		struct ioc_bulk_cb_wrap_type ioc_cbw;
		memset(&ioc_cbw, 0, sizeof(ioc_cbw));
		ioc_cbw.TransferLength = common->data_size;
		ioc_cbw.DataDir = (uint8_t)(common->data_dir);
		ioc_cbw.Lun = common->lun;
		ioc_cbw.CmdLength = common->cmnd_size;
		memcpy(ioc_cbw.Cmd, common->cmnd, common->cmnd_size);
		rc = copy_to_user(u_arg, &ioc_cbw, sizeof(ioc_cbw));
		if (rc) {
			rc = -EFAULT;
			dev->rcomplete = 0;
			VUNIQ_ERR("%s:err copy_to_user()\n", __func__);
		}
		VUNIQ_DUMP(&ioc_cbw, sizeof(ioc_cbw));
		break;
		}
	case USBMAS_IOC_GET_FSGLUN:
		{
		struct fsg_lun *curlun;
		struct ioc_fsg_lun_type ioc_fsg;
		memset(&ioc_fsg, 0, sizeof(ioc_fsg));
		ioc_fsg.Luns = common->nluns;
		if ((common->lun >= 0) && (common->lun < common->nluns)) {
			curlun = &common->luns[common->lun];
			common->curlun = curlun;
			curlun->sense_data = SS_NO_SENSE;
			curlun->sense_data_info = 0;
			curlun->info_valid = 0;
			common->data_size_from_cmnd = common->data_size;
			ioc_fsg.FileSts = (curlun->filp) ? 1: 0 ;
			ioc_fsg.NumSectors = curlun->num_sectors;
			ioc_fsg.Ro = curlun->ro;
			ioc_fsg.SenseData = curlun->sense_data;
			ioc_fsg.UnitAttentionData =
					curlun->unit_attention_data;
			ioc_fsg.FsgBuffLen = FSG_BUFLEN;
		} else {
			common->curlun = NULL;
			common->bad_lun_okay = 0;
		}
		VUNIQ_DBG("%s: Luns[%d] FileSts[%d] NumSectors[%d] Ro[%d] "
		"SenseData[0x%06X] SenseDataInfo[%d] UnitAttentionData[%d]"
		" FsgBuffLen[%d] sizeof(%d)\n",
		__func__, ioc_fsg.Luns, ioc_fsg.FileSts, ioc_fsg.NumSectors,
		ioc_fsg.Ro, ioc_fsg.SenseData, ioc_fsg.SenseDataInfo,
		ioc_fsg.UnitAttentionData, ioc_fsg.FsgBuffLen,
		sizeof(ioc_fsg));
		rc = copy_to_user(u_arg, &ioc_fsg, sizeof(ioc_fsg));
		if (rc) {
			VUNIQ_ERR("%s:err copy_to_user()\n", __func__);
			rc = -EFAULT;
		}
		break;
		}
	case USBMAS_IOC_GET_DATA:
		rc  = ioc_get_data(common, (struct ioc_data_type*)arg);
		break;
	case USBMAS_IOC_TX_DATA:
		rc = ioc_tx_data(common, (struct ioc_data_type*)arg);
		vseq = VSEQ_TX_DATA;
		break;
	case USBMAS_IOC_RX_DATA:
		rc = ioc_rx_data(common, (struct ioc_data_type*)arg);
		vseq = VSEQ_RX_DATA;
		break;
	case USBMAS_IOC_DO_READ:
		vseq = VSEQ_DO_READ;
		break;
	case USBMAS_IOC_DO_WRITE:
		vseq = VSEQ_DO_WRITE;
		break;
	case USBMAS_IOC_NO_DATA:
		{
		struct fsg_lun *curlun;
		struct ioc_data_type st_data;
		curlun = common->curlun;
		rc = copy_from_user(&st_data, u_arg,
					sizeof(struct ioc_data_type));
		if (rc) {
			st_data.CSW.Residue = 0;
			st_data.CSW.SenseData = SS_COMMUNICATION_FAILURE;
			rc = -EFAULT;
			VUNIQ_ERR("%s:err copy_from_user()\n", __func__);
		}
		common->residue = st_data.CSW.Residue;
		curlun->sense_data = st_data.CSW.SenseData;
		VUNIQ_DBG("%s:set CSW residue[%d] sense_data[0x%06X]\n",
			__func__, common->residue, curlun->sense_data);
		if (st_data.CSW.Status == USB_STATUS_PHASE_ERROR) {
			VUNIQ_DBG("%s:set phase_error\n", __func__);
			common->phase_error = 1;
		}
		if (curlun->unit_attention_data != SS_NO_SENSE) {
			curlun->unit_attention_data = SS_NO_SENSE;
		}
		vseq = VSEQ_NO_DATA;
		break;
		}
	case USBMAS_IOC_INVALID_COMMAND:
		vseq = VSEQ_INVALID_CMD;
		break;
	default:
		rc = -EINVAL;
	}
	VUNIQ_DBG("%s:vseq = %d\n", __func__, vseq);
	if (vseq != VSEQ_NONE) {
		spin_lock(&common->lock);
		vendor->vseq = vseq;
		VUNIQ_DBG("%s:wakeup_thread\n", __func__);
		wakeup_thread(common);
		spin_unlock(&common->lock);
	}
	VUNIQ_DBG("%s:end rc %ld\n", __func__, rc);
	return rc;
}



/****************************** ADD FUNCTION ******************************/

#if 0
static struct usb_gadget_strings *fsg_strings_array[] = {
	&fsg_stringtab,
	NULL,
};
#endif

static ssize_t print_switch_name(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", KC_VENDOR_NAME);
}

static ssize_t print_switch_state(struct switch_dev *sdev, char *buf)
{
	return sprintf(buf, "%s\n", sdev->state ? "1" : "0");
}

static int fsg_bind_config(struct usb_composite_dev *cdev,
			   struct usb_configuration *c,
			   struct fsg_common *common)
{
	struct fsg_dev *fsg;
	int rc;

	fsg = kzalloc(sizeof *fsg, GFP_KERNEL);
	if (unlikely(!fsg))
		return -ENOMEM;

	fsg->function.name        = "mass_storage";
#if 0
	fsg->function.strings     = fsg_strings_array;
#endif
	fsg->function.bind        = fsg_bind;
	fsg->function.unbind      = fsg_unbind;
	fsg->function.setup       = fsg_setup;
	fsg->function.set_alt     = fsg_set_alt;
	fsg->function.disable     = fsg_disable;

	fsg->common               = common;
	/*
	 * Our caller holds a reference to common structure so we
	 * don't have to be worry about it being freed until we return
	 * from this function.  So instead of incrementing counter now
	 * and decrement in error recovery we increment it only when
	 * call to usb_add_function() was successful.
	 */

	rc = usb_add_function(c, &fsg->function);
	if (unlikely(rc))
		kfree(fsg);
	else
		fsg_common_get(fsg->common);
	return rc;
}

static inline int __deprecated __maybe_unused
fsg_add(struct usb_composite_dev *cdev, struct usb_configuration *c,
	struct fsg_common *common)
{
	return fsg_bind_config(cdev, c, common);
}


/************************* Module parameters *************************/

struct fsg_module_parameters {
	char		*file[FSG_MAX_LUNS];
	int		ro[FSG_MAX_LUNS];
	int		removable[FSG_MAX_LUNS];
	int		cdrom[FSG_MAX_LUNS];
	int		nofua[FSG_MAX_LUNS];

	unsigned int	file_count, ro_count, removable_count, cdrom_count;
	unsigned int	nofua_count;
	unsigned int	luns;	/* nluns */
	int		stall;	/* can_stall */
};

#define _FSG_MODULE_PARAM_ARRAY(prefix, params, name, type, desc)	\
	module_param_array_named(prefix ## name, params.name, type,	\
				 &prefix ## params.name ## _count,	\
				 S_IRUGO);				\
	MODULE_PARM_DESC(prefix ## name, desc)

#define _FSG_MODULE_PARAM(prefix, params, name, type, desc)		\
	module_param_named(prefix ## name, params.name, type,		\
			   S_IRUGO);					\
	MODULE_PARM_DESC(prefix ## name, desc)

#define FSG_MODULE_PARAMETERS(prefix, params)				\
	_FSG_MODULE_PARAM_ARRAY(prefix, params, file, charp,		\
				"names of backing files or devices");	\
	_FSG_MODULE_PARAM_ARRAY(prefix, params, ro, bool,		\
				"true to force read-only");		\
	_FSG_MODULE_PARAM_ARRAY(prefix, params, removable, bool,	\
				"true to simulate removable media");	\
	_FSG_MODULE_PARAM_ARRAY(prefix, params, cdrom, bool,		\
				"true to simulate CD-ROM instead of disk"); \
	_FSG_MODULE_PARAM_ARRAY(prefix, params, nofua, bool,		\
				"true to ignore SCSI WRITE(10,12) FUA bit"); \
	_FSG_MODULE_PARAM(prefix, params, luns, uint,			\
			  "number of LUNs");				\
	_FSG_MODULE_PARAM(prefix, params, stall, bool,			\
			  "false to prevent bulk stalls")

static void
fsg_config_from_params(struct fsg_config *cfg,
		       const struct fsg_module_parameters *params)
{
	struct fsg_lun_config *lun;
	unsigned i;

	/* Configure LUNs */
	cfg->nluns =
		min(params->luns ?: (params->file_count ?: 1u),
		    (unsigned)FSG_MAX_LUNS);
	for (i = 0, lun = cfg->luns; i < cfg->nluns; ++i, ++lun) {
		lun->ro = !!params->ro[i];
		lun->cdrom = !!params->cdrom[i];
		lun->removable = /* Removable by default */
			params->removable_count <= i || params->removable[i];
		lun->filename =
			params->file_count > i && params->file[i][0]
			? params->file[i]
			: 0;
	}

	/* Let MSF use defaults */
	cfg->lun_name_format = 0;
	cfg->thread_name = 0;
	cfg->vendor_name = 0;
	cfg->product_name = 0;
	cfg->release = 0xffff;

	cfg->ops = NULL;
	cfg->private_data = NULL;

	/* Finalise */
	cfg->can_stall = params->stall;
}

static inline struct fsg_common *
fsg_common_from_params(struct fsg_common *common,
		       struct usb_composite_dev *cdev,
		       const struct fsg_module_parameters *params)
	__attribute__((unused));
static inline struct fsg_common *
fsg_common_from_params(struct fsg_common *common,
		       struct usb_composite_dev *cdev,
		       const struct fsg_module_parameters *params)
{
	struct fsg_config cfg;
	fsg_config_from_params(&cfg, params);
	return fsg_common_init(common, cdev, &cfg);
}

