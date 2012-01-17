/*****************************************************************************
* Copyright 2009 - 2010 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#include "interface/vcos/vcos.h"
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/uaccess.h>

struct opaque_vcos_cfg_buf_t
{
    struct seq_file *seq;
    char            *charBuf;
};

struct opaque_vcos_cfg_entry_t
{
    struct proc_dir_entry *pde;
    struct proc_dir_entry *parent_pde;
    VCOS_CFG_SHOW_FPTR     showFunc;
    VCOS_CFG_PARSE_FPTR    parseFunc;
    void                  *data;
    const char            *name;
};

/***************************************************************************** 
* 
*    cfg_proc_show
*  
*****************************************************************************/

static int cfg_proc_show( struct seq_file *s, void *v )
{
    VCOS_CFG_ENTRY_T                entry;
    struct opaque_vcos_cfg_buf_t    buf;

    entry = s->private;

    if ( entry->showFunc )
    {
        memset( &buf, 0, sizeof( buf ));
        buf.seq = s;

        entry->showFunc( &buf, entry->data );
    }

    return 0;
}

/***************************************************************************** 
* 
*    cfg_proc_write
*  
*****************************************************************************/

static ssize_t cfg_proc_write( struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
    VCOS_CFG_ENTRY_T                entry = PDE(file->f_path.dentry->d_inode)->data;
    char                           *charBuf;
    struct opaque_vcos_cfg_buf_t    buf;
    size_t                          len;

    if ( entry->parseFunc != NULL )
    {
        /* The number 4000 is rather arbitrary. It just needs to be bigger than any input
         * string we expect to use.
         */

        len = count;
        if ( count > 4000 )
        {
            len = 4000;
        }

        /* Allocate a kernel buffer to contain the string being written. */

        charBuf = kmalloc( len + 1, GFP_KERNEL );
        if ( copy_from_user( charBuf, buffer, len ))
        {
            kfree( charBuf );
            return -EFAULT;
        }

        /* echo puts a trailing newline in the buffer - strip it out. */

        if (( len > 0 ) && ( charBuf[ len - 1 ] == '\n' ))
        {
            len--;
        }
        charBuf[len] = '\0';

        memset( &buf, 0, sizeof( buf ));
        buf.charBuf = charBuf;

        entry->parseFunc( &buf, entry->data );
        kfree( charBuf );
    }
    return count;
}

/***************************************************************************** 
* 
*    cfg_proc_open
*  
*****************************************************************************/

static int cfg_proc_open( struct inode *inode, struct file *file )
{
    return single_open( file, cfg_proc_show, PDE(inode)->data );
}

static const struct file_operations cfg_proc_fops = 
{
    .open       = cfg_proc_open,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = single_release,
    .write      = cfg_proc_write,
};

/***************************************************************************** 
* 
*    vcos_cfg_mkdir
*  
*****************************************************************************/

VCOS_STATUS_T vcos_cfg_mkdir( VCOS_CFG_ENTRY_T *entryp,
                              VCOS_CFG_ENTRY_T *parent,
                              const char *dirName )
{
    VCOS_CFG_ENTRY_T    entry;

    if (( entry = kzalloc( sizeof( *entry ), GFP_KERNEL )) == NULL )
    {
        return VCOS_ENOMEM;
    }

    if ( parent == NULL )
    {
        entry->pde = proc_mkdir( dirName, NULL );
    }
    else
    {
        entry->pde = proc_mkdir( dirName, (*parent)->pde );
        entry->parent_pde = (*parent)->pde;
    }
    if ( entry->pde == NULL )
    {
        kfree( entry );
        return VCOS_ENOMEM;
    }

    entry->name = dirName;

    *entryp = entry;
    return VCOS_SUCCESS;
}

/***************************************************************************** 
* 
*    vcos_cfg_create_entry
*  
*****************************************************************************/

VCOS_STATUS_T vcos_cfg_create_entry( VCOS_CFG_ENTRY_T *entryp,
                                     VCOS_CFG_ENTRY_T *parent,
                                     const char *entryName,
                                     VCOS_CFG_SHOW_FPTR showFunc,
                                     VCOS_CFG_PARSE_FPTR parseFunc,
                                     void *data )
{
    VCOS_CFG_ENTRY_T    entry;
    mode_t              mode;

    *entryp = NULL;

    if (( entry = kzalloc( sizeof( *entry ), GFP_KERNEL )) == NULL )
    {
        return VCOS_ENOMEM;
    }

    mode = 0;
    if ( showFunc != NULL )
    {
        mode |= 0444;
    }
    if ( parseFunc != NULL )
    {
        mode |= 0200;
    }
    
    if ( parent == NULL )
    {
        entry->pde = create_proc_entry( entryName, mode, NULL );
    }
    else
    {
        entry->pde = create_proc_entry( entryName, mode, (*parent)->pde );
        entry->parent_pde = (*parent)->pde;
    }
    if ( entry->pde == NULL )
    {
        kfree( entry );
        return -ENOMEM;
    }
    entry->showFunc = showFunc;
    entry->parseFunc = parseFunc;
    entry->data = data;
    entry->name = entryName;

    entry->pde->data = entry;
    entry->pde->proc_fops = &cfg_proc_fops;
    
    *entryp = entry;
    return VCOS_SUCCESS;    
}

/***************************************************************************** 
* 
*    vcos_cfg_remove_entry
*  
*****************************************************************************/

VCOS_STATUS_T vcos_cfg_remove_entry( VCOS_CFG_ENTRY_T *entryp )
{
    if (( entryp != NULL ) && ( *entryp != NULL ))
    {
        remove_proc_entry( (*entryp)->name, (*entryp)->parent_pde );

        kfree( *entryp );
        *entryp = NULL;
    }

    return VCOS_SUCCESS;
}

/***************************************************************************** 
* 
*    vcos_cfg_is_entry_created
*  
*****************************************************************************/

int vcos_cfg_is_entry_created( VCOS_CFG_ENTRY_T entry )
{
    return ( entry != NULL ) && ( entry->pde != NULL );
}

/***************************************************************************** 
* 
*    vcos_cfg_buf_printf
*  
*****************************************************************************/

void vcos_cfg_buf_printf( VCOS_CFG_BUF_T buf, const char *fmt, ... )
{
    struct seq_file *m = buf->seq;

    /* Bah - there is no seq_vprintf */

    va_list args;
    int len;

    if (m->count < m->size) {
        va_start(args, fmt);
        len = vsnprintf(m->buf + m->count, m->size - m->count, fmt, args);
        va_end(args);
        if (m->count + len < m->size) {
            m->count += len;
            return;
        }
    }
    m->count = m->size;
}

/***************************************************************************** 
* 
*    vcos_cfg_buf_get_str
*  
*****************************************************************************/

char *vcos_cfg_buf_get_str( VCOS_CFG_BUF_T buf )
{
    return buf->charBuf;
}

/***************************************************************************** 
* 
*    vcos_cfg_get_proc_entry
*  
*    This function is only created for a couple of backwards compatibility '
*    issues and shouldn't normally be used.
*  
*****************************************************************************/

void *vcos_cfg_get_proc_entry( VCOS_CFG_ENTRY_T entry )
{
    return entry->pde;
}

/***************************************************************************** 
* 
*    vcos_cfg_get_entry_name
*  
*****************************************************************************/

const char *vcos_cfg_get_entry_name( VCOS_CFG_ENTRY_T entry )
{
   return entry->pde->name;
}


EXPORT_SYMBOL( vcos_cfg_mkdir );
EXPORT_SYMBOL( vcos_cfg_create_entry );
EXPORT_SYMBOL( vcos_cfg_remove_entry );
EXPORT_SYMBOL( vcos_cfg_get_entry_name );
EXPORT_SYMBOL( vcos_cfg_is_entry_created );
EXPORT_SYMBOL( vcos_cfg_buf_printf );
EXPORT_SYMBOL( vcos_cfg_buf_get_str );

EXPORT_SYMBOL_GPL( vcos_cfg_get_proc_entry );

