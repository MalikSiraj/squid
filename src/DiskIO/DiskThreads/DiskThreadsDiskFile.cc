
/*
 * $Id: DiskThreadsDiskFile.cc,v 1.1 2004/12/20 16:30:38 robertc Exp $
 *
 * DEBUG: section 79    Disk IO Routines
 * AUTHOR: Robert Collins
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 * Copyright (c) 2003, Robert Collins <robertc@squid-cache.org>
 */


#include "squid.h"
#include "DiskThreadsDiskFile.h"
#include "Store.h"
#include "SwapDir.h"
#include "Generic.h"
#include "DiskIO/IORequestor.h"
#include "DiskIO/ReadRequest.h"
#include "DiskIO/WriteRequest.h"

/* === PUBLIC =========================================================== */

CBDATA_CLASS_INIT(DiskThreadsDiskFile);
void *
DiskThreadsDiskFile::operator new (size_t)
{
    CBDATA_INIT_TYPE(DiskThreadsDiskFile);
    DiskThreadsDiskFile *result = cbdataAlloc(DiskThreadsDiskFile);
    /* Mark result as being owned - we want the refcounter to do the delete
     * call */
    return cbdataReference(result);
}

void
DiskThreadsDiskFile::operator delete (void *address)
{
    DiskThreadsDiskFile *t = static_cast<DiskThreadsDiskFile *>(address);
    cbdataFree(address);
    /* And allow the memory to be freed */
    cbdataReferenceDone (t);
}

DiskThreadsDiskFile::DiskThreadsDiskFile (char const *aPath, DiskThreadsIOStrategy *anIO):fd(-1), errorOccured (false), IO(anIO),
        inProgressIOs (0)
{
    assert (aPath);
    debug (79,3)("UFSFile::UFSFile: %s\n", aPath);
    path_ = xstrdup (aPath);
}

DiskThreadsDiskFile::~DiskThreadsDiskFile()
{
    safe_free (path_);
    doClose();
}

void
DiskThreadsDiskFile::open (int flags, mode_t mode, IORequestor::Pointer callback)
{
    statCounter.syscalls.disk.opens++;
#if !ASYNC_OPEN

    fd = file_open(path_, flags);

    if (fd < 0) {
        debug(79, 3) ("DiskThreadsDiskFile::open: got failure (%d)\n", errno);
        errorOccured = true;
        return;
    }

#endif
    Opening_FD++;

    ioRequestor = callback;

    ++inProgressIOs;

#if ASYNC_OPEN

    aioOpen(path_, flags, mode, DiskThreadsDiskFile::OpenDone, this);

#else

    openDone(fd, NULL, fd, 0);

#endif
}

void
DiskThreadsDiskFile::read(ReadRequest * request)
{
    debugs(79, 3, "DiskThreadsDiskFile::read: " << this << ", size " << request->len);
    assert (fd > -1);
    assert (ioRequestor.getRaw());
    statCounter.syscalls.disk.reads++;
    ++inProgressIOs;
#if ASYNC_READ

    aioRead(fd, request->offset, request->len, ReadDone, new IoResult<ReadRequest>(this, request));
#else

    file_read(fd, request->buf, request->len, request->offset, ReadDone, new IoResult<ReadRequest>(this, request));
#endif
}

void
DiskThreadsDiskFile::create (int flags, mode_t mode, IORequestor::Pointer callback)
{
    statCounter.syscalls.disk.opens++;
#if !ASYNC_CREATE

    int fd = file_open(path_, flags);

    if (fd < 0) {
        debug(79, 3) ("storeAufsCreate: got failure (%d)\n", errno);
        errorOccured = true;
        return;
    }

#endif
    Opening_FD++;

    ioRequestor = callback;

    ++inProgressIOs;

#if ASYNC_CREATE

    aioOpen(path_, flags, mode, DiskThreadsDiskFile::OpenDone, this);

#else

    openDone (fd, NULL, fd, 0);

#endif
}

bool
DiskThreadsDiskFile::error() const
{
    return errorOccured;
}

void
DiskThreadsDiskFile::OpenDone(int fd, void *cbdata, const char *buf, int aio_return, int aio_errno)
{
    DiskThreadsDiskFile *myFile = static_cast<DiskThreadsDiskFile *>(cbdata);
    myFile->openDone (fd, buf, aio_return, aio_errno);
}

void
DiskThreadsDiskFile::openDone(int unused, const char *unused2, int anFD, int errflag)
{
    debug(79, 3) ("DiskThreadsDiskFile::openDone: FD %d, errflag %d\n", anFD, errflag);
    Opening_FD--;

    fd = anFD;

    if (errflag || fd < 0) {
        errno = errflag;
        debug(79, 0) ("DiskThreadsDiskFile::openDone: %s\n", xstrerror());
        debug(79, 1) ("\t%s\n", path_);
        errorOccured = true;
    } else {
        store_open_disk_fd++;
        commSetCloseOnExec(fd);
        fd_open(fd, FD_FILE, path_);
    }

    debug(79, 3) ("DiskThreadsDiskFile::openDone: exiting\n");

    IORequestor::Pointer t = ioRequestor;
    --inProgressIOs;
    t->ioCompletedNotification();
}

void DiskThreadsDiskFile::doClose()
{
    if (fd > -1) {
        statCounter.syscalls.disk.closes++;
        aioClose(fd);
        fd_close(fd);
        store_open_disk_fd--;
        fd = -1;
    }
}

void
DiskThreadsDiskFile::close ()
{
    debug (79,3)("DiskThreadsDiskFile::close: %p closing for %p\n", this, ioRequestor.getRaw());

    if (!ioInProgress()) {
        doClose();
        assert (ioRequestor.getRaw());
        ioRequestor->closeCompleted();
    }
}

bool
DiskThreadsDiskFile::canRead() const
{
    debug (79,3)("DiskThreadsDiskFile::canRead: fd is %d\n",fd);
    return fd > -1;
}

void
DiskThreadsDiskFile::write(WriteRequest * writeRequest)
{
    debug(79, 3) ("storeAufsWrite: FD %d\n", fd);
    statCounter.syscalls.disk.writes++;
    ++inProgressIOs;
#if ASYNC_WRITE

    aioWrite(fd, writeRequest->offset, (char *)writeRequest->buf, writeRequest->len, WriteDone, new IoResult<WriteRequest>(this, writeRequest),
             writeRequest->free_func);
#else

    file_write(fd, writeRequest->offset, (char *)writeRequest->buf, writeRequest->len, WriteDone, new IoResult<WriteRequest>(this, writeRequest),
               writeRequest->free_func);
#endif
}

bool
DiskThreadsDiskFile::canWrite() const
{
    return fd > -1;
}

bool
DiskThreadsDiskFile::ioInProgress()const
{
    return inProgressIOs > 0;
}

/*  === STATIC =========================================================== */

#if ASYNC_READ
void
DiskThreadsDiskFile::ReadDone(int fd, void *my_data, const char *buf, int len, int errflag)
#else
void
DiskThreadsDiskFile::ReadDone(int fd, const char *buf, int len, int errflag, void *my_data)
#endif
{
    IoResult<ReadRequest> * result = static_cast<IoResult<ReadRequest> *>(my_data);
    assert (result);
    result->file->readDone(fd, buf, len, errflag, result->request);
    delete result;
}

void
DiskThreadsDiskFile::readDone(int rvfd, const char *buf, int len, int errflag, ReadRequest::Pointer request)
{
    debug (79,3)("DiskThreadsDiskFile::readDone: FD %d\n",rvfd);
    assert (fd == rvfd);

    ssize_t rlen;

    if (errflag) {
        debug(79, 3) ("DiskThreadsDiskFile::readDone: got failure (%d)\n", errflag);
        rlen = -1;
    } else {
        rlen = (ssize_t) len;
    }

#if ASYNC_READ
    /* translate errflag from errno to Squid disk error */
    errno = errflag;

    if (errflag)
        errflag = DISK_ERROR;
    else
        errflag = DISK_OK;

#else

    if (errflag == DISK_EOF)
        errflag = DISK_OK;	/* EOF is signalled by len == 0, not errors... */

#endif

    --inProgressIOs;

    ioRequestor->readCompleted(buf, rlen, errflag, request);
}

void
DiskThreadsDiskFile::
#if ASYNC_WRITE
WriteDone(int fd, void *my_data, int len, int errflag)
#else
WriteDone(int fd, int errflag, size_t len, void *my_data)
#endif
{
    IoResult<WriteRequest> * result = static_cast<IoResult<WriteRequest> *>(my_data);
    assert (result);
    result->file->writeDone(fd, errflag, len, result->request);
    delete result;
}

void
DiskThreadsDiskFile::writeDone (int rvfd, int errflag, size_t len, WriteRequest::Pointer request)
{
    assert (rvfd == fd);
    static int loop_detect = 0;
    debug(79, 3) ("storeAufsWriteDone: FD %d, len %ld, err=%d\n",
                  fd, (long int) len, errflag);

#if ASYNC_WRITE
    /* Translate from errno to Squid disk error */
    errno = errflag;

    if (errflag)
        errflag = errno == ENOSPC ? DISK_NO_SPACE_LEFT : DISK_ERROR;
    else
        errflag = DISK_OK;

#endif

    assert(++loop_detect < 10);

    --inProgressIOs;

    ioRequestor->writeCompleted(errflag, len, request);

    --loop_detect;
}

template <class RT>
cbdata_type IoResult<RT>::CBDATA_IoResult = CBDATA_UNKNOWN;

template<class RT>
void *
IoResult<RT>::operator new (size_t)
{
    CBDATA_INIT_TYPE(IoResult);
    IoResult<RT> *result = cbdataAlloc(IoResult);
    return result;
}

template <class RT>
void
IoResult<RT>::operator delete (void *address)
{
    cbdataFree(address);
}

