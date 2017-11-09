// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_OBJECT_REQUEST_H
#define CEPH_LIBRBD_IO_OBJECT_REQUEST_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "include/rados/librados.hpp"
#include "common/snap_types.h"
#include "common/zipkin_trace.h"
#include "librbd/ObjectMap.h"
#include "librbd/io/Types.h"
#include <map>

class Context;

namespace librbd {

struct ImageCtx;

namespace io {

struct AioCompletion;
template <typename> class CopyupRequest;
template <typename> class ObjectRemoveRequest;
template <typename> class ObjectTruncateRequest;
template <typename> class ObjectWriteRequest;
template <typename> class ObjectZeroRequest;

struct ObjectRequestHandle {
  virtual ~ObjectRequestHandle() {
  }

  virtual void complete(int r) = 0;
  virtual void send() = 0;
};

/**
 * This class represents an I/O operation to a single RBD data object.
 * Its subclasses encapsulate logic for dealing with special cases
 * for I/O due to layering.
 */
template <typename ImageCtxT = ImageCtx>
class ObjectRequest : public ObjectRequestHandle {
public:
  static ObjectRequest* create_write(ImageCtxT *ictx, const std::string &oid,
                                     uint64_t object_no,
                                     uint64_t object_off,
                                     const ceph::bufferlist &data,
                                     const ::SnapContext &snapc, int op_flags,
				     const ZTracer::Trace &parent_trace,
                                     Context *completion);
  static ObjectRequest* create_discard(ImageCtxT *ictx, const std::string &oid,
                                       uint64_t object_no, uint64_t object_off,
                                       uint64_t object_len,
                                       const ::SnapContext &snapc,
                                       bool disable_clone_remove,
                                       bool update_object_map,
                                       const ZTracer::Trace &parent_trace,
                                       Context *completion);
  static ObjectRequest* create_writesame(ImageCtxT *ictx,
                                         const std::string &oid,
                                         uint64_t object_no,
                                         uint64_t object_off,
                                         uint64_t object_len,
                                         const ceph::bufferlist &data,
                                         const ::SnapContext &snapc,
					 int op_flags,
					 const ZTracer::Trace &parent_trace,
                                         Context *completion);
  static ObjectRequest* create_compare_and_write(ImageCtxT *ictx,
                                                 const std::string &oid,
                                                 uint64_t object_no,
                                                 uint64_t object_off,
                                                 const ceph::bufferlist &cmp_data,
                                                 const ceph::bufferlist &write_data,
                                                 const ::SnapContext &snapc,
                                                 uint64_t *mismatch_offset, int op_flags,
                                                 const ZTracer::Trace &parent_trace,
                                                 Context *completion);

  ObjectRequest(ImageCtxT *ictx, const std::string &oid,
                uint64_t objectno, uint64_t off, uint64_t len,
                librados::snap_t snap_id, bool hide_enoent,
		const char *trace_name, const ZTracer::Trace &parent_trace,
		Context *completion);
  ~ObjectRequest() override {
    m_trace.event("finish");
  }

  static void add_write_hint(ImageCtxT& image_ctx,
                             librados::ObjectWriteOperation *wr);

  virtual void complete(int r);

  virtual bool should_complete(int r) = 0;
  void send() override = 0;

  bool has_parent() const {
    return m_has_parent;
  }

  virtual const char *get_op_type() const = 0;

protected:
  bool compute_parent_extents(Extents *parent_extents);

  ImageCtxT *m_ictx;
  std::string m_oid;
  uint64_t m_object_no, m_object_off, m_object_len;
  librados::snap_t m_snap_id;
  Context *m_completion;
  Extents m_parent_extents;
  bool m_hide_enoent;
  ZTracer::Trace m_trace;

  void async_finish(int r);
  void finish(int r);

private:
  bool m_has_parent = false;
};

template <typename ImageCtxT = ImageCtx>
class ObjectReadRequest : public ObjectRequest<ImageCtxT> {
public:
  typedef std::map<uint64_t, uint64_t> ExtentMap;

  static ObjectReadRequest* create(ImageCtxT *ictx, const std::string &oid,
                                   uint64_t objectno, uint64_t offset,
                                   uint64_t len, librados::snap_t snap_id,
                                   int op_flags, bool cache_initiated,
                                   const ZTracer::Trace &parent_trace,
                                   Context *completion) {
    return new ObjectReadRequest(ictx, oid, objectno, offset, len,
                                 snap_id, op_flags, cache_initiated,
                                 parent_trace, completion);
  }

  ObjectReadRequest(ImageCtxT *ictx, const std::string &oid,
                    uint64_t objectno, uint64_t offset, uint64_t len,
                    librados::snap_t snap_id, int op_flags,
                    bool cache_initiated, const ZTracer::Trace &parent_trace,
                    Context *completion);

  bool should_complete(int r) override;
  void send() override;

  inline uint64_t get_offset() const {
    return this->m_object_off;
  }
  inline uint64_t get_length() const {
    return this->m_object_len;
  }
  ceph::bufferlist &data() {
    return m_read_data;
  }
  ExtentMap &get_extent_map() {
    return m_ext_map;
  }

  const char *get_op_type() const override {
    return "read";
  }

private:
  /**
   * @verbatim
   *
   *           <start>
   *              |
   *              |
   *    /--------/ \--------\
   *    |                   |
   *    | (cache            | (cache
   *    v  disabled)        v  enabled)
   * READ_OBJECT      READ_CACHE
   *    |                   |
   *    |/------------------/
   *    |
   *    v (skip if not needed)
   * READ_PARENT
   *    |
   *    v (skip if not needed)
   * COPYUP
   *    |
   *    v
   * <finish>
   *
   * @endverbatim
   */

  int m_op_flags;
  bool m_cache_initiated;

  ceph::bufferlist m_read_data;
  ExtentMap m_ext_map;

  void read_cache();
  void handle_read_cache(int r);

  void read_object();
  void handle_read_object(int r);

  void read_parent();
  void handle_read_parent(int r);

  void copyup();
};

template <typename ImageCtxT = ImageCtx>
class AbstractObjectWriteRequest : public ObjectRequest<ImageCtxT> {
public:
  AbstractObjectWriteRequest(ImageCtxT *ictx, const std::string &oid,
                             uint64_t object_no, uint64_t object_off,
                             uint64_t len, const ::SnapContext &snapc,
			     bool hide_enoent, const char *trace_name,
			     const ZTracer::Trace &parent_trace,
                             Context *completion);

  virtual bool is_empty_write_op() const {
    return false;
  }

  virtual uint8_t get_pre_write_object_map_state() const {
    return OBJECT_EXISTS;
  }

  virtual void add_copyup_ops(librados::ObjectWriteOperation *wr) {
    add_write_ops(wr);
  }

  void handle_copyup(int r);

  bool should_complete(int r) override;
  void send() override;

  /**
   * Writes go through the following state machine to deal with
   * layering and the object map:
   *
   *   <start>
   *      |
   *      |\
   *      | \       -or-
   *      |  ---------------------------------> LIBRBD_AIO_WRITE_PRE
   *      |                          .                            |
   *      |                          .                            |
   *      |                          .                            v
   *      |                          . . .  . > LIBRBD_AIO_WRITE_FLAT. . .
   *      |                                                       |      .
   *      |                                                       |      .
   *      |                                                       |      .
   *      v                need copyup   (copyup performs pre)    |      .
   * LIBRBD_AIO_WRITE_GUARD -----------> LIBRBD_AIO_WRITE_COPYUP  |      .
   *  .       |                               |        .          |      .
   *  .       |                               |        .          |      .
   *  .       |                         /-----/        .          |      .
   *  .       |                         |              .          |      .
   *  .       \-------------------\     |     /-------------------/      .
   *  .                           |     |     |        .                 .
   *  .                           v     v     v        .                 .
   *  .                       LIBRBD_AIO_WRITE_POST    .                 .
   *  .                               |                .                 .
   *  .                               |  . . . . . . . .                 .
   *  .                               |  .                               .
   *  .                               v  v                               .
   *  . . . . . . . . . . . . . . > <finish> < . . . . . . . . . . . . . .
   *
   * The _PRE/_POST states are skipped if the object map is disabled.
   * The write starts in _WRITE_GUARD or _FLAT depending on whether or not
   * there is a parent overlap.
   */
protected:
  enum write_state_d {
    LIBRBD_AIO_WRITE_GUARD,
    LIBRBD_AIO_WRITE_COPYUP,
    LIBRBD_AIO_WRITE_FLAT,
    LIBRBD_AIO_WRITE_PRE,
    LIBRBD_AIO_WRITE_POST,
    LIBRBD_AIO_WRITE_ERROR
  };

  write_state_d m_state;
  librados::ObjectWriteOperation m_write;
  uint64_t m_snap_seq;
  std::vector<librados::snap_t> m_snaps;
  bool m_object_may_exist = false;
  bool m_guard = true;

  virtual void add_write_hint(librados::ObjectWriteOperation *wr);
  virtual void add_write_ops(librados::ObjectWriteOperation *wr) = 0;

  virtual void guard_write();
  virtual bool post_object_map_update() {
    return false;
  }
  virtual void send_write();
  virtual void send_write_op();
  virtual void handle_write_guard();

  void send_pre_object_map_update();

private:
  bool send_post_object_map_update();
  void send_copyup();
};

template <typename ImageCtxT = ImageCtx>
class ObjectWriteRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectWriteRequest(ImageCtxT *ictx, const std::string &oid,
                     uint64_t object_no, uint64_t object_off,
                     const ceph::bufferlist &data, const ::SnapContext &snapc,
                     int op_flags, const ZTracer::Trace &parent_trace,
                     Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            data.length(), snapc, false,
                                            "write", parent_trace, completion),
      m_write_data(data), m_op_flags(op_flags) {
  }

  bool is_empty_write_op() const override {
    return (m_write_data.length() == 0);
  }

  const char *get_op_type() const override {
    return "write";
  }

protected:
  void add_write_ops(librados::ObjectWriteOperation *wr) override;

  void send_write() override;

private:
  ceph::bufferlist m_write_data;
  int m_op_flags;
};

template <typename ImageCtxT = ImageCtx>
class ObjectDiscardRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectDiscardRequest(ImageCtxT *ictx, const std::string &oid,
                       uint64_t object_no, uint64_t object_off,
                       uint64_t object_len, const ::SnapContext &snapc,
                       bool disable_clone_remove, bool update_object_map,
                       const ZTracer::Trace &parent_trace, Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            object_len, snapc, true, "discard",
                                            parent_trace, completion),
      m_update_object_map(update_object_map) {
    if (object_off == 0 && object_len == ictx->layout.object_size) {
      if (disable_clone_remove && this->has_parent()) {
        // need to hide the parent object instead of child object
        m_discard_action = DISCARD_ACTION_REMOVE_TRUNCATE;
        this->m_object_len = 0;
      } else {
        m_discard_action = DISCARD_ACTION_REMOVE;
      }
      this->m_guard = (!snapc.snaps.empty());
    } else if (object_off + object_len == ictx->layout.object_size) {
      m_discard_action = DISCARD_ACTION_TRUNCATE;
    } else {
      m_discard_action = DISCARD_ACTION_ZERO;
    }
  }

  const char* get_op_type() const override {
    switch (m_discard_action) {
    case DISCARD_ACTION_REMOVE:
      return "remove";
    case DISCARD_ACTION_REMOVE_TRUNCATE:
      return "remove (truncate)";
    case DISCARD_ACTION_TRUNCATE:
      return "truncate";
    case DISCARD_ACTION_ZERO:
      return "zero";
    }
    assert(false);
    return nullptr;
  }

  uint8_t get_pre_write_object_map_state() const override {
    if (m_discard_action == DISCARD_ACTION_REMOVE) {
      return OBJECT_PENDING;
    }
    return OBJECT_EXISTS;
  }

protected:
  void add_write_hint(librados::ObjectWriteOperation *wr) override {
    // no hint for discard
  }

  void add_write_ops(librados::ObjectWriteOperation *wr) override {
    switch (m_discard_action) {
    case DISCARD_ACTION_REMOVE:
      wr->remove();
      break;
    case DISCARD_ACTION_REMOVE_TRUNCATE:
    case DISCARD_ACTION_TRUNCATE:
      wr->truncate(this->m_object_off);
      break;
    case DISCARD_ACTION_ZERO:
      wr->zero(this->m_object_off, this->m_object_len);
      break;
    default:
      assert(false);
      break;
    }
  }

  bool post_object_map_update() override {
    // trim operation updates the object map in batches
    return (m_update_object_map && m_discard_action == DISCARD_ACTION_REMOVE);
  }

  void send_write() override;

private:
  enum DiscardAction {
    DISCARD_ACTION_REMOVE,
    DISCARD_ACTION_REMOVE_TRUNCATE,
    DISCARD_ACTION_TRUNCATE,
    DISCARD_ACTION_ZERO
  };

  DiscardAction m_discard_action;
  bool m_update_object_map;

};

template <typename ImageCtxT = ImageCtx>
class ObjectWriteSameRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectWriteSameRequest(ImageCtxT *ictx, const std::string &oid,
			 uint64_t object_no, uint64_t object_off,
			 uint64_t object_len, const ceph::bufferlist &data,
                         const ::SnapContext &snapc, int op_flags,
			 const ZTracer::Trace &parent_trace,
			 Context *completion)
    : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                            object_len, snapc, false,
                                            "writesame", parent_trace,
                                            completion),
      m_write_data(data), m_op_flags(op_flags) {
  }

  const char *get_op_type() const override {
    return "writesame";
  }

protected:
  void add_write_ops(librados::ObjectWriteOperation *wr) override;

  void send_write() override;

private:
  ceph::bufferlist m_write_data;
  int m_op_flags;
};

template <typename ImageCtxT = ImageCtx>
class ObjectCompareAndWriteRequest : public AbstractObjectWriteRequest<ImageCtxT> {
public:
  ObjectCompareAndWriteRequest(ImageCtxT *ictx, const std::string &oid,
                               uint64_t object_no, uint64_t object_off,
                               const ceph::bufferlist &cmp_bl,
                               const ceph::bufferlist &write_bl,
                               const ::SnapContext &snapc,
                               uint64_t *mismatch_offset, int op_flags,
                               const ZTracer::Trace &parent_trace,
                               Context *completion)
   : AbstractObjectWriteRequest<ImageCtxT>(ictx, oid, object_no, object_off,
                                           cmp_bl.length(), snapc, false,
                                           "compare_and_write", parent_trace,
                                           completion),
    m_cmp_bl(cmp_bl), m_write_bl(write_bl),
    m_mismatch_offset(mismatch_offset), m_op_flags(op_flags) {
  }

  const char *get_op_type() const override {
    return "compare_and_write";
  }

  void complete(int r) override;

protected:
  void add_write_ops(librados::ObjectWriteOperation *wr) override;

  void send_write() override;

private:
  ceph::bufferlist m_cmp_bl;
  ceph::bufferlist m_write_bl;
  uint64_t *m_mismatch_offset;
  int m_op_flags;
};

} // namespace io
} // namespace librbd

extern template class librbd::io::ObjectRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectReadRequest<librbd::ImageCtx>;
extern template class librbd::io::AbstractObjectWriteRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectWriteRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectDiscardRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectWriteSameRequest<librbd::ImageCtx>;
extern template class librbd::io::ObjectCompareAndWriteRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IO_OBJECT_REQUEST_H
