/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Liz");
MODULE_DESCRIPTION("AESD character driver");
MODULE_LICENSE("Dual BSD/GPL");

int aesd_open(struct inode *inode, struct file *filp);
int aesd_release(struct inode *inode, struct file *filp);
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
loff_t aesd_llseek(struct file *filp, loff_t off, int whence);
long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset_byte = 0;
    size_t unread_bytes = 0;
    size_t bytes_to_read = 0;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset_byte);
    if (entry == NULL) {
        mutex_unlock(&dev->lock);
        return 0;
    }

    unread_bytes = entry->size - entry_offset_byte;
    bytes_to_read = (count > unread_bytes) ? unread_bytes : count;

    if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_read)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_read;
    retval = bytes_to_read;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *write_data = NULL;
    
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    
    if (count == 0) return 0;
    
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    
    write_data = kmalloc(count, GFP_KERNEL);
    if (!write_data) {
        retval = -ENOMEM;
        goto out;
    }
    
    if (copy_from_user(write_data, buf, count)) {
        retval = -EFAULT;
        kfree(write_data);
        goto out;
    }
    
    if (dev->working_entry.size == 0) {
        dev->working_entry.buffptr = kmalloc(count, GFP_KERNEL);
        if (!dev->working_entry.buffptr) {
            retval = -ENOMEM;
            kfree(write_data);
            goto out;
        }
        memcpy((void *)dev->working_entry.buffptr, write_data, count);
        dev->working_entry.size = count;
    } else {
        char *new_ptr = krealloc((void *)dev->working_entry.buffptr, dev->working_entry.size + count, GFP_KERNEL);
        if (!new_ptr) {
            retval = -ENOMEM;
            kfree(write_data);
            goto out;
        }
        dev->working_entry.buffptr = new_ptr;
        memcpy((void *)(dev->working_entry.buffptr + dev->working_entry.size), write_data, count);
        dev->working_entry.size += count;
    }
    kfree(write_data);
    
    if (memchr(dev->working_entry.buffptr, '\n', dev->working_entry.size)) {
        if (dev->buffer.full) {
            kfree((void *)dev->buffer.entry[dev->buffer.in_offs].buffptr);
        }
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->working_entry);
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }
    
    retval = count;
    
out:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t newpos;
    struct aesd_buffer_entry *entry;
    uint8_t index;
    loff_t size = 0;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry->buffptr != NULL) {
            size += entry->size;
        }
    }

    switch(whence) {
        case 0: /* SEEK_SET */
            newpos = off;
            break;
        case 1: /* SEEK_CUR */
            newpos = filp->f_pos + off;
            break;
        case 2: /* SEEK_END */
            newpos = size + off;
            break;
        default: /* can't happen */
            mutex_unlock(&dev->lock);
            return -EINVAL;
    }
    if (newpos < 0) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    filp->f_pos = newpos;
    mutex_unlock(&dev->lock);
    return newpos;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t new_pos = 0;
    uint8_t i, curr_idx;
    long retval = 0;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;

    switch(cmd) {
        case AESDCHAR_IOCSEEKTO:
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
                return -EFAULT;
            }
            
            if (mutex_lock_interruptible(&dev->lock)) {
                return -ERESTARTSYS;
            }
            
            curr_idx = dev->buffer.out_offs;
            for (i = 0; i < seekto.write_cmd; i++) {
                if (!dev->buffer.full && curr_idx == dev->buffer.in_offs) {
                    retval = -EINVAL;
                    goto ioctl_out;
                }
                new_pos += dev->buffer.entry[curr_idx].size;
                curr_idx = (curr_idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
            }
            
            if (!dev->buffer.full && curr_idx == dev->buffer.in_offs) {
                retval = -EINVAL;
                goto ioctl_out;
            }
            
            if (seekto.write_cmd_offset >= dev->buffer.entry[curr_idx].size) {
                retval = -EINVAL;
                goto ioctl_out;
            }
            
            new_pos += seekto.write_cmd_offset;
            filp->f_pos = new_pos;
            
        ioctl_out:
            mutex_unlock(&dev->lock);
            break;
            
        default:
            retval = -ENOTTY;
            break;
    }
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



static int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /* initialize the AESD specific portion of the device */
    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.working_entry.buffptr = NULL;
    aesd_device.working_entry.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    /* cleanup AESD specific poritions here as necessary */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree((void *)entry->buffptr);
        }
    }
    if (aesd_device.working_entry.buffptr) {
        kfree((void *)aesd_device.working_entry.buffptr);
    }
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
