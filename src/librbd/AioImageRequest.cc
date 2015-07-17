// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/AioImageRequest.h"
#include "librbd/AioCompletion.h"
#include "librbd/AioObjectRequest.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageWatcher.h"
#include "librbd/internal.h"
#include "librbd/Journal.h"
#include "librbd/JournalTypes.h"
#include "include/rados/librados.hpp"
#include "osdc/Striper.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::AioImageRequest: "

namespace librbd {

void AioImageRequest::aio_read(
    ImageCtx *ictx, AioCompletion *c,
    const std::vector<std::pair<uint64_t,uint64_t> > &extents,
    char *buf, bufferlist *pbl, int op_flags) {
  AioImageRead req(*ictx, c, extents, buf, pbl, op_flags);
  req.send();
}

void AioImageRequest::aio_read(ImageCtx *ictx, AioCompletion *c, uint64_t off,
                               size_t len, char *buf, bufferlist *pbl,
                               int op_flags) {
  AioImageRead req(*ictx, c, off, len, buf, pbl, op_flags);
  req.send();
}

void AioImageRequest::aio_write(ImageCtx *ictx, AioCompletion *c, uint64_t off,
                                size_t len, const char *buf, int op_flags) {
  AioImageWrite req(*ictx, c, off, len, buf, op_flags);
  req.send();
}

void AioImageRequest::aio_discard(ImageCtx *ictx, AioCompletion *c,
                                  uint64_t off, uint64_t len) {
  AioImageDiscard req(*ictx, c, off, len);
  req.send();
}

void AioImageRequest::aio_flush(ImageCtx *ictx, AioCompletion *c) {
  AioImageFlush req(*ictx, c);
  req.send();
}

void AioImageRequest::send() {
  assert(m_image_ctx.owner_lock.is_locked());

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << get_request_type() << ": ictx=" << &m_image_ctx << ", "
                 << "completion=" << m_aio_comp <<  dendl;

  m_aio_comp->get();
  int r = ictx_check(&m_image_ctx, m_image_ctx.owner_lock);
  if (r < 0) {
    m_aio_comp->fail(cct, r);
    return;
  }

  send_request();
}

void AioImageRead::send_request() {
  CephContext *cct = m_image_ctx.cct;

  if (m_image_ctx.object_cacher && m_image_ctx.readahead_max_bytes > 0 &&
      !(m_op_flags & LIBRADOS_OP_FLAG_FADVISE_RANDOM)) {
    readahead(&m_image_ctx, m_image_extents);
  }

  librados::snap_t snap_id;
  map<object_t,vector<ObjectExtent> > object_extents;
  uint64_t buffer_ofs = 0;
  {
    // prevent image size from changing between computing clip and recording
    // pending async operation
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    snap_id = m_image_ctx.snap_id;

    // map
    for (vector<pair<uint64_t,uint64_t> >::const_iterator p =
           m_image_extents.begin();
         p != m_image_extents.end(); ++p) {
      uint64_t len = p->second;
      int r = clip_io(&m_image_ctx, p->first, &len);
      if (r < 0) {
        m_aio_comp->fail(cct, r);
        return;
      }
      if (len == 0) {
        continue;
      }

      Striper::file_to_extents(cct, m_image_ctx.format_string,
                               &m_image_ctx.layout, p->first, len, 0,
                               object_extents, buffer_ofs);
      buffer_ofs += len;
    }

    m_aio_comp->init_time(&m_image_ctx, AIO_TYPE_READ);
  }

  m_aio_comp->read_buf = m_buf;
  m_aio_comp->read_buf_len = buffer_ofs;
  m_aio_comp->read_bl = m_pbl;

  for (map<object_t,vector<ObjectExtent> >::iterator p = object_extents.begin();
       p != object_extents.end(); ++p) {
    for (vector<ObjectExtent>::iterator q = p->second.begin();
         q != p->second.end(); ++q) {
      ldout(cct, 20) << " oid " << q->oid << " " << q->offset << "~"
                     << q->length << " from " << q->buffer_extents
                     << dendl;

      C_AioRead *req_comp = new C_AioRead(cct, m_aio_comp);
      AioObjectRead *req = new AioObjectRead(&m_image_ctx, q->oid.name,
                                             q->objectno, q->offset, q->length,
                                             q->buffer_extents, snap_id, true,
                                             req_comp, m_op_flags);
      req_comp->set_req(req);

      if (m_image_ctx.object_cacher) {
        C_CacheRead *cache_comp = new C_CacheRead(&m_image_ctx, req);
        m_image_ctx.aio_read_from_cache(q->oid, q->objectno, &req->data(),
                                        q->length, q->offset,
                                        cache_comp, m_op_flags);
      } else {
        req->send();
      }
    }
  }

  m_aio_comp->finish_adding_requests(cct);
  m_aio_comp->put();

  m_image_ctx.perfcounter->inc(l_librbd_rd);
  m_image_ctx.perfcounter->inc(l_librbd_rd_bytes, buffer_ofs);
}

void AbstractAioImageWrite::send_request() {
  assert(!m_image_ctx.image_watcher->is_lock_supported() ||
          m_image_ctx.image_watcher->is_lock_owner());

  CephContext *cct = m_image_ctx.cct;

  RWLock::RLocker md_locker(m_image_ctx.md_lock);

  bool journaling = false;
  uint64_t journal_tid = 0;

  uint64_t clip_len = m_len;
  ObjectExtents object_extents;
  ::SnapContext snapc;
  {
    // prevent image size from changing between computing clip and recording
    // pending async operation
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.snap_id != CEPH_NOSNAP || m_image_ctx.read_only) {
      m_aio_comp->fail(cct, -EROFS);
      return;
    }

    int r = clip_io(&m_image_ctx, m_off, &clip_len);
    if (r < 0) {
      m_aio_comp->fail(cct, r);
      return;
    }

    snapc = m_image_ctx.snapc;
    m_aio_comp->init_time(&m_image_ctx, get_aio_type());

    // map to object extents
    if (clip_len > 0) {
      Striper::file_to_extents(cct, m_image_ctx.format_string,
                               &m_image_ctx.layout, m_off, clip_len, 0,
                               object_extents);
    }

    journaling = (m_image_ctx.journal != NULL);
  }

  AioObjectRequests requests;
  send_object_requests(object_extents, snapc, (journaling ? &requests : NULL));

  if (journaling) {
    // in-flight ops are flushed prior to closing the journal
    assert(m_image_ctx.journal != NULL);
    journal_tid = append_journal_event(requests, m_synchronous);
  }

  if (m_image_ctx.object_cacher != NULL) {
    send_cache_requests(object_extents, snapc, journal_tid);
  }
  update_stats(clip_len);

  m_aio_comp->finish_adding_requests(cct);
  m_aio_comp->put();
}

void AbstractAioImageWrite::send_object_requests(
    const ObjectExtents &object_extents, const ::SnapContext &snapc,
    AioObjectRequests *aio_object_requests) {
  CephContext *cct = m_image_ctx.cct;

  for (ObjectExtents::const_iterator p = object_extents.begin();
       p != object_extents.end(); ++p) {
    ldout(cct, 20) << " oid " << p->oid << " " << p->offset << "~" << p->length
                   << " from " << p->buffer_extents << dendl;
    C_AioRequest *req_comp = new C_AioRequest(cct, m_aio_comp);
    AioObjectRequest *request = send_object_request(*p, snapc, req_comp);

    // if journaling, stash the request for later; otherwise send
    if (request != NULL) {
      if (aio_object_requests != NULL) {
        aio_object_requests->push_back(request);
      } else {
        request->send();
      }
    }
  }
}

void AioImageWrite::assemble_extent(const ObjectExtent &object_extent,
                                    bufferlist *bl) {
  for (Extents::const_iterator q = object_extent.buffer_extents.begin();
       q != object_extent.buffer_extents.end(); ++q) {
    bl->append(m_buf + q->first, q->second);;
  }
}

uint64_t AioImageWrite::append_journal_event(
    const AioObjectRequests &requests, bool synchronous) {
  bufferlist bl;
  bl.append(m_buf, m_len);

  journal::EventEntry event_entry(journal::AioWriteEvent(m_off, m_len, bl));
  return m_image_ctx.journal->append_event(m_aio_comp, event_entry, requests,
                                           synchronous);
}

void AioImageWrite::send_cache_requests(const ObjectExtents &object_extents,
                                        const ::SnapContext &snapc,
                                        uint64_t journal_tid) {
  CephContext *cct = m_image_ctx.cct;

  for (ObjectExtents::const_iterator p = object_extents.begin();
       p != object_extents.end(); ++p) {
    const ObjectExtent &object_extent = *p;

    bufferlist bl;
    assemble_extent(object_extent, &bl);

    // TODO pass journal_tid to object cacher
    C_AioRequest *req_comp = new C_AioRequest(cct, m_aio_comp);
    m_image_ctx.write_to_cache(object_extent.oid, bl, object_extent.length,
                               object_extent.offset, req_comp, m_op_flags);
  }
}

AioObjectRequest *AioImageWrite::send_object_request(
    const ObjectExtent &object_extent, const ::SnapContext &snapc,
    Context *on_finish) {
  if (m_image_ctx.object_cacher != NULL) {
    return NULL;
  }

  bufferlist bl;
  assemble_extent(object_extent, &bl);
  AioObjectWrite *req = new AioObjectWrite(&m_image_ctx,
                                           object_extent.oid.name,
                                           object_extent.objectno,
                                           object_extent.offset, bl,
                                           snapc, on_finish);
  req->set_op_flags(m_op_flags);
  return req;
}

void AioImageWrite::update_stats(size_t length) {
  m_image_ctx.perfcounter->inc(l_librbd_wr);
  m_image_ctx.perfcounter->inc(l_librbd_wr_bytes, length);
}

uint64_t AioImageDiscard::append_journal_event(
    const AioObjectRequests &requests, bool synchronous) {
  journal::EventEntry event_entry(journal::AioDiscardEvent(m_off, m_len));
  return m_image_ctx.journal->append_event(m_aio_comp, event_entry, requests,
                                           synchronous);
}

void AioImageDiscard::send_cache_requests(const ObjectExtents &object_extents,
                                          const ::SnapContext &snapc,
                                          uint64_t journal_tid) {
  // TODO need to have cache flag pending discard for writeback or need
  // to delay cache update until after journal commits
  Mutex::Locker cache_locker(m_image_ctx.cache_lock);

  // TODO pass journal_tid to object cacher
  m_image_ctx.object_cacher->discard_set(m_image_ctx.object_set,
                                         object_extents);
}

AioObjectRequest *AioImageDiscard::send_object_request(
    const ObjectExtent &object_extent, const ::SnapContext &snapc,
    Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;

  AioObjectRequest *req;
  if (object_extent.length == m_image_ctx.layout.fl_object_size) {
    req = new AioObjectRemove(&m_image_ctx, object_extent.oid.name,
                              object_extent.objectno, snapc, on_finish);
  } else if (object_extent.offset + object_extent.length ==
               m_image_ctx.layout.fl_object_size) {
    req = new AioObjectTruncate(&m_image_ctx, object_extent.oid.name,
                                object_extent.objectno, object_extent.offset,
                                snapc, on_finish);
  } else {
    if(cct->_conf->rbd_skip_partial_discard) {
      delete on_finish;
      return NULL;
    }
    req = new AioObjectZero(&m_image_ctx, object_extent.oid.name,
                            object_extent.objectno, object_extent.offset,
                            object_extent.length, snapc, on_finish);
  }
  return req;
}

void AioImageDiscard::update_stats(size_t length) {
  m_image_ctx.perfcounter->inc(l_librbd_discard);
  m_image_ctx.perfcounter->inc(l_librbd_discard_bytes, length);
}

void AioImageFlush::send_request() {
  CephContext *cct = m_image_ctx.cct;

  {
    // journal the flush event
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_image_ctx.journal != NULL) {
      m_image_ctx.journal->append_event(
        m_aio_comp, journal::EventEntry(journal::AioFlushEvent()),
        AioObjectRequests(), true);
    }
  }

  // TODO race condition between registering op and submitting to cache
  //      (might not be flushed -- backport needed)
  C_AioRequest *flush_ctx = new C_AioRequest(cct, m_aio_comp);
  m_image_ctx.flush_async_operations(flush_ctx);

  m_aio_comp->init_time(&m_image_ctx, AIO_TYPE_FLUSH);
  C_AioRequest *req_comp = new C_AioRequest(cct, m_aio_comp);
  if (m_image_ctx.object_cacher != NULL) {
    m_image_ctx.flush_cache_aio(req_comp);
  } else {
    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(req_comp, NULL, rados_ctx_cb);
    m_image_ctx.data_ctx.aio_flush_async(rados_completion);
    rados_completion->release();
  }

  m_aio_comp->finish_adding_requests(cct);
  m_aio_comp->put();

  m_image_ctx.perfcounter->inc(l_librbd_aio_flush);
}

} // namespace librbd
