/*
 * QEMU Block driver for iSCSI images
 *
 * Copyright (c) 2010-2011 Ronnie Sahlberg <ronniesahlberg@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config-host.h"

#include <poll.h>
#include <arpa/inet.h>
#include "qemu-common.h"
#include "qemu-error.h"
#include "block_int.h"
#include "trace.h"
#include "hw/scsi-defs.h"

#include <iscsi/iscsi.h>
#include <iscsi/scsi-lowlevel.h>

#ifdef __linux__
#include <scsi/sg.h>
#include <hw/scsi-defs.h>
#endif

typedef struct IscsiLun {
    struct iscsi_context *iscsi;
    int lun;
    enum scsi_inquiry_peripheral_device_type type;
    int block_size;
    uint64_t num_blocks;
    int events;
} IscsiLun;

typedef struct IscsiAIOCB {
    BlockDriverAIOCB common;
    QEMUIOVector *qiov;
    QEMUBH *bh;
    IscsiLun *iscsilun;
    struct scsi_task *task;
    uint8_t *buf;
    int status;
    int canceled;
    size_t read_size;
    size_t read_offset;
#ifdef __linux__
    sg_io_hdr_t *ioh;
#endif
} IscsiAIOCB;

struct IscsiTask {
    IscsiLun *iscsilun;
    BlockDriverState *bs;
    int status;
    int complete;
};

static void
iscsi_bh_cb(void *p)
{
    IscsiAIOCB *acb = p;

    qemu_bh_delete(acb->bh);

    if (acb->canceled == 0) {
        acb->common.cb(acb->common.opaque, acb->status);
    }

    if (acb->task != NULL) {
        scsi_free_scsi_task(acb->task);
        acb->task = NULL;
    }

    qemu_aio_release(acb);
}

static void
iscsi_schedule_bh(IscsiAIOCB *acb)
{
    if (acb->bh) {
        return;
    }
    acb->bh = qemu_bh_new(iscsi_bh_cb, acb);
    qemu_bh_schedule(acb->bh);
}


static void
iscsi_abort_task_cb(struct iscsi_context *iscsi, int status, void *command_data,
                    void *private_data)
{
    IscsiAIOCB *acb = private_data;

    acb->status = -ECANCELED;
    iscsi_schedule_bh(acb);
}

static void
iscsi_aio_cancel(BlockDriverAIOCB *blockacb)
{
    IscsiAIOCB *acb = (IscsiAIOCB *)blockacb;
    IscsiLun *iscsilun = acb->iscsilun;

    if (acb->status != -EINPROGRESS) {
        return;
    }

    acb->canceled = 1;

    /* send a task mgmt call to the target to cancel the task on the target */
    iscsi_task_mgmt_abort_task_async(iscsilun->iscsi, acb->task,
                                     iscsi_abort_task_cb, acb);

    while (acb->status == -EINPROGRESS) {
        qemu_aio_wait();
    }
}

static const AIOCBInfo iscsi_aiocb_info = {
    .aiocb_size         = sizeof(IscsiAIOCB),
    .cancel             = iscsi_aio_cancel,
};


static void iscsi_process_read(void *arg);
static void iscsi_process_write(void *arg);

static int iscsi_process_flush(void *arg)
{
    IscsiLun *iscsilun = arg;

    return iscsi_queue_length(iscsilun->iscsi) > 0;
}

static void
iscsi_set_events(IscsiLun *iscsilun)
{
    struct iscsi_context *iscsi = iscsilun->iscsi;
    int ev;

    /* We always register a read handler.  */
    ev = POLLIN;
    ev |= iscsi_which_events(iscsi);
    if (ev != iscsilun->events) {
        qemu_aio_set_fd_handler(iscsi_get_fd(iscsi),
                      iscsi_process_read,
                      (ev & POLLOUT) ? iscsi_process_write : NULL,
                      iscsi_process_flush,
                      iscsilun);

    }

    iscsilun->events = ev;
}

static void
iscsi_process_read(void *arg)
{
    IscsiLun *iscsilun = arg;
    struct iscsi_context *iscsi = iscsilun->iscsi;

    iscsi_service(iscsi, POLLIN);
    iscsi_set_events(iscsilun);
}

static void
iscsi_process_write(void *arg)
{
    IscsiLun *iscsilun = arg;
    struct iscsi_context *iscsi = iscsilun->iscsi;

    iscsi_service(iscsi, POLLOUT);
    iscsi_set_events(iscsilun);
}


static void
iscsi_aio_write16_cb(struct iscsi_context *iscsi, int status,
                     void *command_data, void *opaque)
{
    IscsiAIOCB *acb = opaque;

    trace_iscsi_aio_write16_cb(iscsi, status, acb, acb->canceled);

    g_free(acb->buf);

    if (acb->canceled != 0) {
        return;
    }

    acb->status = 0;
    if (status < 0) {
        error_report("Failed to write16 data to iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        acb->status = -EIO;
    }

    iscsi_schedule_bh(acb);
}

static int64_t sector_qemu2lun(int64_t sector, IscsiLun *iscsilun)
{
    return sector * BDRV_SECTOR_SIZE / iscsilun->block_size;
}

static BlockDriverAIOCB *
iscsi_aio_writev(BlockDriverState *bs, int64_t sector_num,
                 QEMUIOVector *qiov, int nb_sectors,
                 BlockDriverCompletionFunc *cb,
                 void *opaque)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;
    IscsiAIOCB *acb;
    size_t size;
    uint32_t num_sectors;
    uint64_t lba;
    struct iscsi_data data;

    acb = qemu_aio_get(&iscsi_aiocb_info, bs, cb, opaque);
    trace_iscsi_aio_writev(iscsi, sector_num, nb_sectors, opaque, acb);

    acb->iscsilun = iscsilun;
    acb->qiov     = qiov;

    acb->canceled   = 0;
    acb->bh         = NULL;
    acb->status     = -EINPROGRESS;

    /* XXX we should pass the iovec to write16 to avoid the extra copy */
    /* this will allow us to get rid of 'buf' completely */
    size = nb_sectors * BDRV_SECTOR_SIZE;
    acb->buf = g_malloc(size);
    qemu_iovec_to_buf(acb->qiov, 0, acb->buf, size);

    acb->task = malloc(sizeof(struct scsi_task));
    if (acb->task == NULL) {
        error_report("iSCSI: Failed to allocate task for scsi WRITE16 "
                     "command. %s", iscsi_get_error(iscsi));
        qemu_aio_release(acb);
        return NULL;
    }
    memset(acb->task, 0, sizeof(struct scsi_task));

    acb->task->xfer_dir = SCSI_XFER_WRITE;
    acb->task->cdb_size = 16;
    acb->task->cdb[0] = 0x8a;
    lba = sector_qemu2lun(sector_num, iscsilun);
    *(uint32_t *)&acb->task->cdb[2]  = htonl(lba >> 32);
    *(uint32_t *)&acb->task->cdb[6]  = htonl(lba & 0xffffffff);
    num_sectors = size / iscsilun->block_size;
    *(uint32_t *)&acb->task->cdb[10] = htonl(num_sectors);
    acb->task->expxferlen = size;

    data.data = acb->buf;
    data.size = size;

    if (iscsi_scsi_command_async(iscsi, iscsilun->lun, acb->task,
                                 iscsi_aio_write16_cb,
                                 &data,
                                 acb) != 0) {
        scsi_free_scsi_task(acb->task);
        g_free(acb->buf);
        qemu_aio_release(acb);
        return NULL;
    }

    iscsi_set_events(iscsilun);

    return &acb->common;
}

static void
iscsi_aio_read16_cb(struct iscsi_context *iscsi, int status,
                    void *command_data, void *opaque)
{
    IscsiAIOCB *acb = opaque;

    trace_iscsi_aio_read16_cb(iscsi, status, acb, acb->canceled);

    if (acb->canceled != 0) {
        return;
    }

    acb->status = 0;
    if (status != 0) {
        error_report("Failed to read16 data from iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        acb->status = -EIO;
    }

    iscsi_schedule_bh(acb);
}

static BlockDriverAIOCB *
iscsi_aio_readv(BlockDriverState *bs, int64_t sector_num,
                QEMUIOVector *qiov, int nb_sectors,
                BlockDriverCompletionFunc *cb,
                void *opaque)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;
    IscsiAIOCB *acb;
    size_t qemu_read_size;
    int i;
    uint64_t lba;
    uint32_t num_sectors;

    qemu_read_size = BDRV_SECTOR_SIZE * (size_t)nb_sectors;

    acb = qemu_aio_get(&iscsi_aiocb_info, bs, cb, opaque);
    trace_iscsi_aio_readv(iscsi, sector_num, nb_sectors, opaque, acb);

    acb->iscsilun = iscsilun;
    acb->qiov     = qiov;

    acb->canceled    = 0;
    acb->bh          = NULL;
    acb->status      = -EINPROGRESS;
    acb->read_size   = qemu_read_size;
    acb->buf         = NULL;

    /* If LUN blocksize is bigger than BDRV_BLOCK_SIZE a read from QEMU
     * may be misaligned to the LUN, so we may need to read some extra
     * data.
     */
    acb->read_offset = 0;
    if (iscsilun->block_size > BDRV_SECTOR_SIZE) {
        uint64_t bdrv_offset = BDRV_SECTOR_SIZE * sector_num;

        acb->read_offset  = bdrv_offset % iscsilun->block_size;
    }

    num_sectors  = (qemu_read_size + iscsilun->block_size
                    + acb->read_offset - 1)
                    / iscsilun->block_size;

    acb->task = malloc(sizeof(struct scsi_task));
    if (acb->task == NULL) {
        error_report("iSCSI: Failed to allocate task for scsi READ16 "
                     "command. %s", iscsi_get_error(iscsi));
        qemu_aio_release(acb);
        return NULL;
    }
    memset(acb->task, 0, sizeof(struct scsi_task));

    acb->task->xfer_dir = SCSI_XFER_READ;
    lba = sector_qemu2lun(sector_num, iscsilun);
    acb->task->expxferlen = qemu_read_size;

    switch (iscsilun->type) {
    case TYPE_DISK:
        acb->task->cdb_size = 16;
        acb->task->cdb[0]  = 0x88;
        *(uint32_t *)&acb->task->cdb[2]  = htonl(lba >> 32);
        *(uint32_t *)&acb->task->cdb[6]  = htonl(lba & 0xffffffff);
        *(uint32_t *)&acb->task->cdb[10] = htonl(num_sectors);
        break;
    default:
        acb->task->cdb_size = 10;
        acb->task->cdb[0]  = 0x28;
        *(uint32_t *)&acb->task->cdb[2] = htonl(lba);
        *(uint16_t *)&acb->task->cdb[7] = htons(num_sectors);
        break;
    }
    
    if (iscsi_scsi_command_async(iscsi, iscsilun->lun, acb->task,
                                 iscsi_aio_read16_cb,
                                 NULL,
                                 acb) != 0) {
        scsi_free_scsi_task(acb->task);
        qemu_aio_release(acb);
        return NULL;
    }

    for (i = 0; i < acb->qiov->niov; i++) {
        scsi_task_add_data_in_buffer(acb->task,
                acb->qiov->iov[i].iov_len,
                acb->qiov->iov[i].iov_base);
    }

    iscsi_set_events(iscsilun);

    return &acb->common;
}


static void
iscsi_synccache10_cb(struct iscsi_context *iscsi, int status,
                     void *command_data, void *opaque)
{
    IscsiAIOCB *acb = opaque;

    if (acb->canceled != 0) {
        return;
    }

    acb->status = 0;
    if (status < 0) {
        error_report("Failed to sync10 data on iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        acb->status = -EIO;
    }

    iscsi_schedule_bh(acb);
}

static BlockDriverAIOCB *
iscsi_aio_flush(BlockDriverState *bs,
                BlockDriverCompletionFunc *cb, void *opaque)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;
    IscsiAIOCB *acb;

    acb = qemu_aio_get(&iscsi_aiocb_info, bs, cb, opaque);

    acb->iscsilun = iscsilun;
    acb->canceled   = 0;
    acb->bh         = NULL;
    acb->status     = -EINPROGRESS;

    acb->task = iscsi_synchronizecache10_task(iscsi, iscsilun->lun,
                                         0, 0, 0, 0,
                                         iscsi_synccache10_cb,
                                         acb);
    if (acb->task == NULL) {
        error_report("iSCSI: Failed to send synchronizecache10 command. %s",
                     iscsi_get_error(iscsi));
        qemu_aio_release(acb);
        return NULL;
    }

    iscsi_set_events(iscsilun);

    return &acb->common;
}

static void
iscsi_unmap_cb(struct iscsi_context *iscsi, int status,
                     void *command_data, void *opaque)
{
    IscsiAIOCB *acb = opaque;

    if (acb->canceled != 0) {
        return;
    }

    acb->status = 0;
    if (status < 0) {
        error_report("Failed to unmap data on iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        acb->status = -EIO;
    }

    iscsi_schedule_bh(acb);
}

static BlockDriverAIOCB *
iscsi_aio_discard(BlockDriverState *bs,
                  int64_t sector_num, int nb_sectors,
                  BlockDriverCompletionFunc *cb, void *opaque)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;
    IscsiAIOCB *acb;
    struct unmap_list list[1];

    acb = qemu_aio_get(&iscsi_aiocb_info, bs, cb, opaque);

    acb->iscsilun = iscsilun;
    acb->canceled   = 0;
    acb->bh         = NULL;
    acb->status     = -EINPROGRESS;

    list[0].lba = sector_qemu2lun(sector_num, iscsilun);
    list[0].num = nb_sectors * BDRV_SECTOR_SIZE / iscsilun->block_size;

    acb->task = iscsi_unmap_task(iscsi, iscsilun->lun,
                                 0, 0, &list[0], 1,
                                 iscsi_unmap_cb,
                                 acb);
    if (acb->task == NULL) {
        error_report("iSCSI: Failed to send unmap command. %s",
                     iscsi_get_error(iscsi));
        qemu_aio_release(acb);
        return NULL;
    }

    iscsi_set_events(iscsilun);

    return &acb->common;
}

#ifdef __linux__
static void
iscsi_aio_ioctl_cb(struct iscsi_context *iscsi, int status,
                     void *command_data, void *opaque)
{
    IscsiAIOCB *acb = opaque;

    if (acb->canceled != 0) {
        return;
    }

    acb->status = 0;
    if (status < 0) {
        error_report("Failed to ioctl(SG_IO) to iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        acb->status = -EIO;
    }

    acb->ioh->driver_status = 0;
    acb->ioh->host_status   = 0;
    acb->ioh->resid         = 0;

#define SG_ERR_DRIVER_SENSE    0x08

    if (status == SCSI_STATUS_CHECK_CONDITION && acb->task->datain.size >= 2) {
        int ss;

        acb->ioh->driver_status |= SG_ERR_DRIVER_SENSE;

        acb->ioh->sb_len_wr = acb->task->datain.size - 2;
        ss = (acb->ioh->mx_sb_len >= acb->ioh->sb_len_wr) ?
             acb->ioh->mx_sb_len : acb->ioh->sb_len_wr;
        memcpy(acb->ioh->sbp, &acb->task->datain.data[2], ss);
    }

    iscsi_schedule_bh(acb);
}

static BlockDriverAIOCB *iscsi_aio_ioctl(BlockDriverState *bs,
        unsigned long int req, void *buf,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;
    struct iscsi_data data;
    IscsiAIOCB *acb;

    assert(req == SG_IO);

    acb = qemu_aio_get(&iscsi_aiocb_info, bs, cb, opaque);

    acb->iscsilun = iscsilun;
    acb->canceled    = 0;
    acb->bh          = NULL;
    acb->status      = -EINPROGRESS;
    acb->buf         = NULL;
    acb->ioh         = buf;

    acb->task = malloc(sizeof(struct scsi_task));
    if (acb->task == NULL) {
        error_report("iSCSI: Failed to allocate task for scsi command. %s",
                     iscsi_get_error(iscsi));
        qemu_aio_release(acb);
        return NULL;
    }
    memset(acb->task, 0, sizeof(struct scsi_task));

    switch (acb->ioh->dxfer_direction) {
    case SG_DXFER_TO_DEV:
        acb->task->xfer_dir = SCSI_XFER_WRITE;
        break;
    case SG_DXFER_FROM_DEV:
        acb->task->xfer_dir = SCSI_XFER_READ;
        break;
    default:
        acb->task->xfer_dir = SCSI_XFER_NONE;
        break;
    }

    acb->task->cdb_size = acb->ioh->cmd_len;
    memcpy(&acb->task->cdb[0], acb->ioh->cmdp, acb->ioh->cmd_len);
    acb->task->expxferlen = acb->ioh->dxfer_len;

    if (acb->task->xfer_dir == SCSI_XFER_WRITE) {
        data.data = acb->ioh->dxferp;
        data.size = acb->ioh->dxfer_len;
    }
    if (iscsi_scsi_command_async(iscsi, iscsilun->lun, acb->task,
                                 iscsi_aio_ioctl_cb,
                                 (acb->task->xfer_dir == SCSI_XFER_WRITE) ?
                                     &data : NULL,
                                 acb) != 0) {
        scsi_free_scsi_task(acb->task);
        qemu_aio_release(acb);
        return NULL;
    }

    /* tell libiscsi to read straight into the buffer we got from ioctl */
    if (acb->task->xfer_dir == SCSI_XFER_READ) {
        scsi_task_add_data_in_buffer(acb->task,
                                     acb->ioh->dxfer_len,
                                     acb->ioh->dxferp);
    }

    iscsi_set_events(iscsilun);

    return &acb->common;
}


static void ioctl_cb(void *opaque, int status)
{
    int *p_status = opaque;
    *p_status = status;
}

static int iscsi_ioctl(BlockDriverState *bs, unsigned long int req, void *buf)
{
    IscsiLun *iscsilun = bs->opaque;
    int status;

    switch (req) {
    case SG_GET_VERSION_NUM:
        *(int *)buf = 30000;
        break;
    case SG_GET_SCSI_ID:
        ((struct sg_scsi_id *)buf)->scsi_type = iscsilun->type;
        break;
    case SG_IO:
        status = -EINPROGRESS;
        iscsi_aio_ioctl(bs, req, buf, ioctl_cb, &status);

        while (status == -EINPROGRESS) {
            qemu_aio_wait();
        }

        return 0;
    default:
        return -1;
    }
    return 0;
}
#endif

static int64_t
iscsi_getlength(BlockDriverState *bs)
{
    IscsiLun *iscsilun = bs->opaque;
    int64_t len;

    len  = iscsilun->num_blocks;
    len *= iscsilun->block_size;

    return len;
}

static void
iscsi_readcapacity16_cb(struct iscsi_context *iscsi, int status,
                        void *command_data, void *opaque)
{
    struct IscsiTask *itask = opaque;
    struct scsi_readcapacity16 *rc16;
    struct scsi_task *task = command_data;

    if (status != 0) {
        error_report("iSCSI: Failed to read capacity of iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    rc16 = scsi_datain_unmarshall(task);
    if (rc16 == NULL) {
        error_report("iSCSI: Failed to unmarshall readcapacity16 data.");
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    itask->iscsilun->block_size = rc16->block_length;
    itask->iscsilun->num_blocks = rc16->returned_lba + 1;
    itask->bs->total_sectors    = itask->iscsilun->num_blocks *
                               itask->iscsilun->block_size / BDRV_SECTOR_SIZE ;

    itask->status   = 0;
    itask->complete = 1;
    scsi_free_scsi_task(task);
}

static void
iscsi_readcapacity10_cb(struct iscsi_context *iscsi, int status,
                        void *command_data, void *opaque)
{
    struct IscsiTask *itask = opaque;
    struct scsi_readcapacity10 *rc10;
    struct scsi_task *task = command_data;

    if (status != 0) {
        error_report("iSCSI: Failed to read capacity of iSCSI lun. %s",
                     iscsi_get_error(iscsi));
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    rc10 = scsi_datain_unmarshall(task);
    if (rc10 == NULL) {
        error_report("iSCSI: Failed to unmarshall readcapacity10 data.");
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    itask->iscsilun->block_size = rc10->block_size;
    if (rc10->lba == 0) {
        /* blank disk loaded */
        itask->iscsilun->num_blocks = 0;
    } else {
        itask->iscsilun->num_blocks = rc10->lba + 1;
    }
    itask->bs->total_sectors    = itask->iscsilun->num_blocks *
                               itask->iscsilun->block_size / BDRV_SECTOR_SIZE ;

    itask->status   = 0;
    itask->complete = 1;
    scsi_free_scsi_task(task);
}

static void
iscsi_inquiry_cb(struct iscsi_context *iscsi, int status, void *command_data,
                 void *opaque)
{
    struct IscsiTask *itask = opaque;
    struct scsi_task *task = command_data;
    struct scsi_inquiry_standard *inq;

    if (status != 0) {
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    inq = scsi_datain_unmarshall(task);
    if (inq == NULL) {
        error_report("iSCSI: Failed to unmarshall inquiry data.");
        itask->status   = 1;
        itask->complete = 1;
        scsi_free_scsi_task(task);
        return;
    }

    itask->iscsilun->type = inq->periperal_device_type;

    scsi_free_scsi_task(task);

    switch (itask->iscsilun->type) {
    case TYPE_DISK:
        task = iscsi_readcapacity16_task(iscsi, itask->iscsilun->lun,
                                   iscsi_readcapacity16_cb, opaque);
        if (task == NULL) {
            error_report("iSCSI: failed to send readcapacity16 command.");
            itask->status   = 1;
            itask->complete = 1;
            return;
        }
        break;
    case TYPE_ROM:
        task = iscsi_readcapacity10_task(iscsi, itask->iscsilun->lun,
                                   0, 0,
                                   iscsi_readcapacity10_cb, opaque);
        if (task == NULL) {
            error_report("iSCSI: failed to send readcapacity16 command.");
            itask->status   = 1;
            itask->complete = 1;
            return;
        }
        break;
    default:
        itask->status   = 0;
        itask->complete = 1;
    }
}

static void
iscsi_connect_cb(struct iscsi_context *iscsi, int status, void *command_data,
                 void *opaque)
{
    struct IscsiTask *itask = opaque;
    struct scsi_task *task;

    if (status != 0) {
        itask->status   = 1;
        itask->complete = 1;
        return;
    }

    task = iscsi_inquiry_task(iscsi, itask->iscsilun->lun,
                              0, 0, 36,
                              iscsi_inquiry_cb, opaque);
    if (task == NULL) {
        error_report("iSCSI: failed to send inquiry command.");
        itask->status   = 1;
        itask->complete = 1;
        return;
    }
}

static int parse_chap(struct iscsi_context *iscsi, const char *target)
{
    QemuOptsList *list;
    QemuOpts *opts;
    const char *user = NULL;
    const char *password = NULL;

    list = qemu_find_opts("iscsi");
    if (!list) {
        return 0;
    }

    opts = qemu_opts_find(list, target);
    if (opts == NULL) {
        opts = QTAILQ_FIRST(&list->head);
        if (!opts) {
            return 0;
        }
    }

    user = qemu_opt_get(opts, "user");
    if (!user) {
        return 0;
    }

    password = qemu_opt_get(opts, "password");
    if (!password) {
        error_report("CHAP username specified but no password was given");
        return -1;
    }

    if (iscsi_set_initiator_username_pwd(iscsi, user, password)) {
        error_report("Failed to set initiator username and password");
        return -1;
    }

    return 0;
}

static void parse_header_digest(struct iscsi_context *iscsi, const char *target)
{
    QemuOptsList *list;
    QemuOpts *opts;
    const char *digest = NULL;

    list = qemu_find_opts("iscsi");
    if (!list) {
        return;
    }

    opts = qemu_opts_find(list, target);
    if (opts == NULL) {
        opts = QTAILQ_FIRST(&list->head);
        if (!opts) {
            return;
        }
    }

    digest = qemu_opt_get(opts, "header-digest");
    if (!digest) {
        return;
    }

    if (!strcmp(digest, "CRC32C")) {
        iscsi_set_header_digest(iscsi, ISCSI_HEADER_DIGEST_CRC32C);
    } else if (!strcmp(digest, "NONE")) {
        iscsi_set_header_digest(iscsi, ISCSI_HEADER_DIGEST_NONE);
    } else if (!strcmp(digest, "CRC32C-NONE")) {
        iscsi_set_header_digest(iscsi, ISCSI_HEADER_DIGEST_CRC32C_NONE);
    } else if (!strcmp(digest, "NONE-CRC32C")) {
        iscsi_set_header_digest(iscsi, ISCSI_HEADER_DIGEST_NONE_CRC32C);
    } else {
        error_report("Invalid header-digest setting : %s", digest);
    }
}

static char *parse_initiator_name(const char *target)
{
    QemuOptsList *list;
    QemuOpts *opts;
    const char *name = NULL;
    const char *iscsi_name = qemu_get_vm_name();

    list = qemu_find_opts("iscsi");
    if (list) {
        opts = qemu_opts_find(list, target);
        if (!opts) {
            opts = QTAILQ_FIRST(&list->head);
        }
        if (opts) {
            name = qemu_opt_get(opts, "initiator-name");
        }
    }

    if (name) {
        return g_strdup(name);
    } else {
        return g_strdup_printf("iqn.2008-11.org.linux-kvm%s%s",
                               iscsi_name ? ":" : "",
                               iscsi_name ? iscsi_name : "");
    }
}

/*
 * We support iscsi url's on the form
 * iscsi://[<username>%<password>@]<host>[:<port>]/<targetname>/<lun>
 */
static int iscsi_open(BlockDriverState *bs, const char *filename, int flags)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = NULL;
    struct iscsi_url *iscsi_url = NULL;
    struct IscsiTask task;
    char *initiator_name = NULL;
    int ret;

    if ((BDRV_SECTOR_SIZE % 512) != 0) {
        error_report("iSCSI: Invalid BDRV_SECTOR_SIZE. "
                     "BDRV_SECTOR_SIZE(%lld) is not a multiple "
                     "of 512", BDRV_SECTOR_SIZE);
        return -EINVAL;
    }

    iscsi_url = iscsi_parse_full_url(iscsi, filename);
    if (iscsi_url == NULL) {
        error_report("Failed to parse URL : %s %s", filename,
                     iscsi_get_error(iscsi));
        ret = -EINVAL;
        goto out;
    }

    memset(iscsilun, 0, sizeof(IscsiLun));

    initiator_name = parse_initiator_name(iscsi_url->target);

    iscsi = iscsi_create_context(initiator_name);
    if (iscsi == NULL) {
        error_report("iSCSI: Failed to create iSCSI context.");
        ret = -ENOMEM;
        goto out;
    }

    if (iscsi_set_targetname(iscsi, iscsi_url->target)) {
        error_report("iSCSI: Failed to set target name.");
        ret = -EINVAL;
        goto out;
    }

    if (iscsi_url->user != NULL) {
        ret = iscsi_set_initiator_username_pwd(iscsi, iscsi_url->user,
                                              iscsi_url->passwd);
        if (ret != 0) {
            error_report("Failed to set initiator username and password");
            ret = -EINVAL;
            goto out;
        }
    }

    /* check if we got CHAP username/password via the options */
    if (parse_chap(iscsi, iscsi_url->target) != 0) {
        error_report("iSCSI: Failed to set CHAP user/password");
        ret = -EINVAL;
        goto out;
    }

    if (iscsi_set_session_type(iscsi, ISCSI_SESSION_NORMAL) != 0) {
        error_report("iSCSI: Failed to set session type to normal.");
        ret = -EINVAL;
        goto out;
    }

    iscsi_set_header_digest(iscsi, ISCSI_HEADER_DIGEST_NONE_CRC32C);

    /* check if we got HEADER_DIGEST via the options */
    parse_header_digest(iscsi, iscsi_url->target);

    task.iscsilun = iscsilun;
    task.status = 0;
    task.complete = 0;
    task.bs = bs;

    iscsilun->iscsi = iscsi;
    iscsilun->lun   = iscsi_url->lun;

    if (iscsi_full_connect_async(iscsi, iscsi_url->portal, iscsi_url->lun,
                                 iscsi_connect_cb, &task)
        != 0) {
        error_report("iSCSI: Failed to start async connect.");
        ret = -EINVAL;
        goto out;
    }

    while (!task.complete) {
        iscsi_set_events(iscsilun);
        qemu_aio_wait();
    }
    if (task.status != 0) {
        error_report("iSCSI: Failed to connect to LUN : %s",
                     iscsi_get_error(iscsi));
        ret = -EINVAL;
        goto out;
    }

    /* Medium changer or tape. We dont have any emulation for this so this must
     * be sg ioctl compatible. We force it to be sg, otherwise qemu will try
     * to read from the device to guess the image format.
     */
    if (iscsilun->type == TYPE_MEDIUM_CHANGER ||
        iscsilun->type == TYPE_TAPE) {
        bs->sg = 1;
    }

    ret = 0;

out:
    if (initiator_name != NULL) {
        g_free(initiator_name);
    }
    if (iscsi_url != NULL) {
        iscsi_destroy_url(iscsi_url);
    }

    if (ret) {
        if (iscsi != NULL) {
            iscsi_destroy_context(iscsi);
        }
        memset(iscsilun, 0, sizeof(IscsiLun));
    }
    return ret;
}

static void iscsi_close(BlockDriverState *bs)
{
    IscsiLun *iscsilun = bs->opaque;
    struct iscsi_context *iscsi = iscsilun->iscsi;

    qemu_aio_set_fd_handler(iscsi_get_fd(iscsi), NULL, NULL, NULL, NULL);
    iscsi_destroy_context(iscsi);
    memset(iscsilun, 0, sizeof(IscsiLun));
}

static BlockDriver bdrv_iscsi = {
    .format_name     = "iscsi",
    .protocol_name   = "iscsi",

    .instance_size   = sizeof(IscsiLun),
    .bdrv_file_open  = iscsi_open,
    .bdrv_close      = iscsi_close,

    .bdrv_getlength  = iscsi_getlength,

    .bdrv_aio_readv  = iscsi_aio_readv,
    .bdrv_aio_writev = iscsi_aio_writev,
    .bdrv_aio_flush  = iscsi_aio_flush,

    .bdrv_aio_discard = iscsi_aio_discard,

#ifdef __linux__
    .bdrv_ioctl       = iscsi_ioctl,
    .bdrv_aio_ioctl   = iscsi_aio_ioctl,
#endif
};

static void iscsi_block_init(void)
{
    bdrv_register(&bdrv_iscsi);
}

block_init(iscsi_block_init);
