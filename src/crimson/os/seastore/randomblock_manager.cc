// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <sys/mman.h>
#include <string.h>

#include "crimson/common/log.h"

#include "include/buffer.h"
#include "randomblock_manager.h"
#include "crimson/os/seastore/nvmedevice/nvmedevice.h"
#include "include/interval_set.h"
#include "include/intarith.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_filestore);
  }
}

namespace crimson::os::seastore {

RandomBlockManager::write_ertr::future<>
RandomBlockManager::rbm_sync_block_bitmap(rbm_bitmap_block_t &block, blk_id_t block_no)
{
  bufferptr bptr;
  try {
    bptr = bufferptr(ceph::buffer::create_page_aligned(block.get_size()));
    bufferlist bl;
    encode(block, bl);
    auto iter = bl.cbegin();
    iter.copy(block.get_size(), bptr.c_str());
  } catch (const std::exception &e) {
    logger().error(
      "rmb_sync_block_bitmap: "
      "exception creating aligned buffer {}",
      e
    );
    ceph_assert(0 == "unhandled exception");
  }
  uint64_t bitmap_block_no = convert_block_no_to_bitmap_block(block_no);
  return device->write(super.start_alloc_area +
		       bitmap_block_no * super.block_size,
		       bptr);
}

RandomBlockManager::mkfs_ertr::future<>
RandomBlockManager::initialize_blk_alloc_area() {
  auto start = super.start_data_area / super.block_size;
  logger().debug("initialize_alloc_area: start to read at {} ", start);

  /* write allocated bitmap info to rbm meta block */
  rbm_bitmap_block_t b_block(super.block_size);
  alloc_rbm_bitmap_block_buf(b_block);
  for (uint64_t i = 0; i < start; i++) {
    b_block.set_bit(i);
  }
  b_block.set_crc();

  return rbm_sync_block_bitmap(b_block,
    super.start_alloc_area / super.block_size
    ).safe_then([this, b_block, start] () mutable {

    /* initialize bitmap blocks as unused */
    auto max = max_block_by_bitmap_block();
    auto max_block = super.size / super.block_size;
    blk_id_t end = round_up_to(max_block, max) - 1;
    logger().debug(" init start {} end {} ", start, end);
    return rbm_sync_block_bitmap_by_range(
            start,
            end,
            bitmap_op_types_t::ALL_CLEAR).safe_then([this, b_block, start]() mutable {
      /*
       * Set rest of the block bitmap, which is not used, to 1
       * To do so, we only mark 1 to empty bitmap blocks
       */
      uint64_t na_block_no = super.size/super.block_size;
      uint64_t remain_block = na_block_no % max_block_by_bitmap_block();
      logger().debug(" na_block_no: {}, remain_block: {} ",
                     na_block_no, remain_block);
      if (remain_block) {
        logger().debug(" try to remained write alloc info ");
        if (na_block_no > max_block_by_bitmap_block()) {
          b_block.buf.clear();
          alloc_rbm_bitmap_block_buf(b_block);
        }
        for (uint64_t i = remain_block; i < max_block_by_bitmap_block(); i++) {
          b_block.set_bit(i);
        }
        b_block.set_crc();
        return rbm_sync_block_bitmap(b_block, na_block_no
            ).handle_error(
                mkfs_ertr::pass_further{},
                crimson::ct_error::assert_all{
                  "Invalid error rbm_sync_block_bitmap to update last bitmap block \
                   in RandomBlockManager::initialize_blk_alloc_area"
                }
              );
      }
      return mkfs_ertr::now();
    }).handle_error(
      mkfs_ertr::pass_further{},
      crimson::ct_error::assert_all{
        "Invalid error rbm_sync_block_bitmap in RandomBlockManager::initialize_blk_alloc_area"
      }
      );
  }).handle_error(
    mkfs_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error rbm_sync_block_bitmap_by_range in RandomBlockManager::initialize_blk_alloc_area"
    }
    );

}

RandomBlockManager::mkfs_ertr::future<>
RandomBlockManager::mkfs(mkfs_config_t config)
{
  logger().debug("path {}", path);
  return _open_device(path).safe_then([this, &config]() {
    return read_rbm_header(config.start
	).safe_then([this, &config](auto super) {
	  logger().debug(" already exists ");
	  return mkfs_ertr::now();
	}).handle_error(
	  crimson::ct_error::enoent::handle([this, &config] (auto) {

	    super.uuid = uuid_d(); // TODO
	    super.magic = 0xFF; // TODO
	    super.start = config.start;
	    super.end = config.end;
	    super.block_size = config.block_size;
	    super.size = config.total_size;
	    super.free_block_count = config.total_size/config.block_size - 2;
	    super.alloc_area_size = get_alloc_area_size();
	    super.start_alloc_area = RBM_SUPERBLOCK_SIZE;
	    super.start_data_area = super.start_alloc_area + super.alloc_area_size;
	    super.crc = 0;
	    super.feature |= RBM_BITMAP_BLOCK_CRC;

	    logger().debug(" super {} ", super);
	    // write super block
	    return write_rbm_header().safe_then([this] {
		return initialize_blk_alloc_area();
	      }).handle_error(
		mkfs_ertr::pass_further{},
		crimson::ct_error::assert_all{
		  "Invalid error write_rbm_header in RandomBlockManager::mkfs"
		});
	    }),
	  mkfs_ertr::pass_further{},
	  crimson::ct_error::assert_all{
	    "Invalid error read_rbm_header in RandomBlockManager::mkfs"
	  }
	);
    }).handle_error(
	mkfs_ertr::pass_further{},
	crimson::ct_error::assert_all{
	  "Invalid error open_device in RandomBlockManager::mkfs"
	}
    ).finally([this] {
      if (device) {
	return device->close().then([]() {
	    return mkfs_ertr::now();
	    });
      }
      return mkfs_ertr::now();
      });

}

RandomBlockManager::find_block_ret
RandomBlockManager::find_free_block(Transaction &t, size_t size)
{
  auto bp = bufferptr(ceph::buffer::create_page_aligned(super.block_size));
  return seastar::do_with(uint64_t(0), uint64_t(super.start_alloc_area), interval_set<blk_id_t>(), bp,
	  [&, this] (auto &allocated, auto &addr, auto &alloc_extent, auto &bp) mutable {
    return crimson::do_until(
	[&, this] () mutable {
	return device->read(
	    addr,
	    bp
	    ).safe_then([&bp, &addr, size, &allocated, &t, &alloc_extent, this]() mutable {
	      logger().debug("find_free_list: allocate {}, addr {}", allocated, addr);
	      rbm_bitmap_block_t b_block(super.block_size);
	      bufferlist bl_bitmap_block;
	      bl_bitmap_block.append(bp);
	      decode(b_block, bl_bitmap_block);
	      auto max = max_block_by_bitmap_block();
	      auto allocated_blocks = t.get_rbm_allocated_blocks();
	      for (uint64_t i = 0; i < max && (uint64_t)size/super.block_size > allocated; i++) {
		auto block_id = convert_bitmap_block_no_to_block_id(i, addr);
		for (auto b : allocated_blocks) {
		  if (b.alloc_blk_ids.intersects(block_id, 1)) {
		    continue;
		  }
		}
		if (b_block.is_allocated(i)) {
		  continue;
		}
		logger().debug("find_free_list: allocated block no {} i {}",
			       convert_bitmap_block_no_to_block_id(i, addr), i);
		if (allocated != 0 && alloc_extent.range_end() != block_id) {
		  /*
		   * if not continous block, just restart to find continuous blocks at the next block.
		   * in-memory allocator can handle this efficiently.
		   */
		  allocated = 0;
		  alloc_extent.clear(); // a range of block allocation
		  logger().debug("find_free_list: rety to find continuous blocks");
		  continue;
		}
		allocated += 1;
		alloc_extent.insert(block_id);
	      }
	      addr += super.block_size;
	      logger().debug("find_free_list: allocated: {} alloc_extent {}", allocated, alloc_extent);
	      if (((uint64_t)size)/super.block_size == allocated) {
		return find_block_ertr::make_ready_future<bool>(true);
	      } else if (addr >= super.start_data_area) {
		alloc_extent.clear();
		return find_block_ertr::make_ready_future<bool>(true);
	      }
	      return find_block_ertr::make_ready_future<bool>(false);
	      });
	}).safe_then([&allocated, &alloc_extent, &t, size, this] () {
	  logger().debug(" allocated: {} size {} ", allocated * super.block_size, size);
	  if (allocated * super.block_size < size) {
	    alloc_extent.clear();
	  }
	  return find_block_ret(
	      find_block_ertr::ready_future_marker{},
	      alloc_extent);
	  }).handle_error(
	      find_block_ertr::pass_further{},
	      crimson::ct_error::assert_all{
		"Invalid error in RandomBlockManager::find_free_block"
	      }
	    );
    });
}

/* TODO : block allocator */
RandomBlockManager::allocate_ertr::future<>
RandomBlockManager::alloc_extent(Transaction &t, size_t size)
{

  /*
   * 1. find free blocks using block allocator
   * 2. add free blocks to transaction (free block is reserved state, but not stored)
   * 3. link free blocks to onode
   * Due to in-memory block allocator is the next work to do,
   * just read the block bitmap directly to find free blocks here.
   *
   */
  return find_free_block(t, size
      ).safe_then([this, &t, size] (auto alloc_extent) mutable -> allocate_ertr::future<> {
	logger().debug("after find_free_block: allocated {}", alloc_extent);
	if (!alloc_extent.empty()) {
	  // add alloc info to delta
	  rbm_alloc_delta_t alloc_info {
	    extent_types_t::RBM_ALLOC_INFO,
	    alloc_extent,
	    rbm_alloc_delta_t::op_types_t::SET
	  };
	  t.add_rbm_allocated_blocks(alloc_info);
	} else {
	  return crimson::ct_error::enospc::make();
	}
	return allocate_ertr::now();
	}
      ).handle_error(
	allocate_ertr::pass_further{},
	crimson::ct_error::assert_all{
	  "Invalid error find_free_block in RandomBlockManager::alloc_extent"
	}
	);
}

RandomBlockManager::free_block_ertr::future<>
RandomBlockManager::free_extent(Transaction &t, blk_paddr_t from, blk_paddr_t to)
{
  blk_id_t blk_id_start = from / super.block_size;
  blk_id_t blk_id_end = to / super.block_size;

  interval_set<blk_id_t> free_extent;
  free_extent.insert(blk_id_start, blk_id_end - blk_id_start + 1);
  rbm_alloc_delta_t alloc_info {
    extent_types_t::RBM_ALLOC_INFO,
    free_extent,
    rbm_alloc_delta_t::op_types_t::CLEAR
  };
  t.add_rbm_allocated_blocks(alloc_info);
  return free_block_ertr::now();
}

RandomBlockManager::write_ertr::future<>
RandomBlockManager::rbm_sync_block_bitmap_by_range(blk_id_t start, blk_id_t end, bitmap_op_types_t op)
{
  auto addr = super.start_alloc_area +
	      (start / max_block_by_bitmap_block())
	      * super.block_size;
  // aligned write
  if (start % max_block_by_bitmap_block() == 0 &&
      end % (max_block_by_bitmap_block() - 1) == 0) {
      auto num_block = num_block_between_blk_ids(start, end);
      bufferlist bl_bitmap_block;
      add_cont_bitmap_blocks_to_buf(bl_bitmap_block, num_block, op);
      return write(
	  addr,
	  bl_bitmap_block);
  }
  auto bp = bufferptr(ceph::buffer::create_page_aligned(super.block_size));
  // try to read first block, then check the block is aligned
  return device->read(
      addr,
      bp).safe_then([bp, start, end, op, addr, this]() {
	rbm_bitmap_block_t b_block(super.block_size);
	bufferlist bl_bitmap_block;
	bl_bitmap_block.append(bp);
	decode(b_block, bl_bitmap_block);
	auto max = max_block_by_bitmap_block();
	auto loop_end = end < (start / max + 1) * max ?
			end % max : max - 1;
	for (uint64_t i = (start % max); i <= loop_end; i++) {
	  if (op == bitmap_op_types_t::ALL_SET) {
	    b_block.set_bit(i);
	  } else {
	    b_block.clear_bit(i);
	  }
	}
	auto num_block = num_block_between_blk_ids(start, end);
	logger().debug("rbm_sync_block_bitmap_by_range: start {}, end {}, loop_end {}, num_block {}",
			start, end, loop_end, num_block);

	bl_bitmap_block.clear();
	encode(b_block, bl_bitmap_block);
	if (num_block == 1) {
	  // | front (unaligned) |
	  return write(
	      addr,
	      bl_bitmap_block);
	} else if (!((end + 1) % max)) {
	  // | front (unaligned) | middle (aligned) |
	  add_cont_bitmap_blocks_to_buf(bl_bitmap_block, num_block - 1, op);
	  logger().debug("partially aligned write: addr {} length {}",
			  addr, bl_bitmap_block.length());
	  return write(
	      addr,
	      bl_bitmap_block);
	} else if (num_block > 2) {
	  // | front (unaligned) | middle | end (unaligned) |
	  // fill up the middle
	  add_cont_bitmap_blocks_to_buf(bl_bitmap_block, num_block - 2, op);
	}

	auto next_addr = super.start_alloc_area +
		    (end / max_block_by_bitmap_block())
		    * super.block_size;
	auto bptr = bufferptr(ceph::buffer::create_page_aligned(super.block_size));
	// | front (unaligned) | middle | end (unaligned) | or | front (unaligned) | end (unaligned) |
	return device->read(
	    next_addr,
	    bptr).safe_then([bptr, bl_bitmap_block, start, end, op, addr, this]() mutable {
	      rbm_bitmap_block_t b_block(super.block_size);
	      bufferlist block;
	      block.append(bptr);
	      decode(b_block, block);
	      auto max = max_block_by_bitmap_block();
	      for (uint64_t i = (end - (end % max)) % max; i <= (end % max); i++) {
		if (op == bitmap_op_types_t::ALL_SET) {
		  b_block.set_bit(i);
		} else {
		  b_block.clear_bit(i);
		}
	      }
	      logger().debug("start {} end {} ", end - (end % max), end);
	      bl_bitmap_block.claim_append(block);
	      return write(
		addr,
		bl_bitmap_block);
	      }).handle_error(
		write_ertr::pass_further{},
		crimson::ct_error::assert_all{
		  "Invalid error in RandomBlockManager::rbm_sync_block_bitmap_by_range"
		}
	      );
	}).handle_error(
	  write_ertr::pass_further{},
	  crimson::ct_error::assert_all{
	    "Invalid error in RandomBlockManager::rbm_sync_block_bitmap_by_range"
	  }
	);
}

RandomBlockManager::abort_allocation_ertr::future<>
RandomBlockManager::abort_allocation(Transaction &t)
{
  /*
   * TODO: clear all allocation infos associated with transaction in in-memory allocator
   */
  t.clear_rbm_allocated_blocks();
  return abort_allocation_ertr::now();
}

RandomBlockManager::write_ertr::future<>
RandomBlockManager::complete_allocation(Transaction &t)
{
  const auto alloc_blocks = t.get_rbm_allocated_blocks();
  if (alloc_blocks.empty()) {
    return write_ertr::now();
  }
  return seastar::do_with(alloc_blocks,
	  [&, this] (auto &alloc_blocks) mutable {
    return crimson::do_for_each(
	alloc_blocks,
	[this, &alloc_blocks](auto &alloc) {
	  return crimson::do_for_each(
	      alloc.alloc_blk_ids,
	      [this, &alloc] (auto &range) -> write_ertr::future<> {
		logger().debug("range {} ~ {}", range.first, range.second);
		bitmap_op_types_t op = (alloc.op == rbm_alloc_delta_t::op_types_t::SET) ?
				    bitmap_op_types_t::ALL_SET : bitmap_op_types_t::ALL_CLEAR;
		return rbm_sync_block_bitmap_by_range(
			range.first,
			range.first + range.second - 1,
			op);
	      });
	}).safe_then([this, &alloc_blocks]() mutable {
	  int alloc_block_count = 0;
	  for (const auto b : alloc_blocks) {
	    for (interval_set<blk_id_t>::const_iterator r = b.alloc_blk_ids.begin();
		 r != b.alloc_blk_ids.end(); ++r) {
	      if (b.op == rbm_alloc_delta_t::op_types_t::SET) {
		alloc_block_count += r.get_len();
		logger().debug(" complete alloc block: start {} len {} ",
				r.get_start(), r.get_len());
	      } else {
		alloc_block_count -= r.get_len();
		logger().debug(" complete free block:  start {} len {} ",
				r.get_start(), r.get_len());
	      }
	    }
	  }
	  logger().debug("complete_alloction: complete to allocate {} blocks", alloc_block_count);
	  if (alloc_block_count > 0) {
	    super.free_block_count -= alloc_block_count;
	  } else {
	    super.free_block_count += alloc_block_count;
	  }
	  return write_ertr::now();
	  });
  });
}

RandomBlockManager::open_ertr::future<>
RandomBlockManager::open(const std::string &path, blk_paddr_t addr)
{
  logger().debug("open: path{}", path);
  return _open_device(path
      ).safe_then([this, addr]() {
      return read_rbm_header(addr).safe_then([&](auto s) -> open_ertr::future<> {
	if (s.magic != 0xFF) {
	  return crimson::ct_error::enoent::make();
	}
	super = s;
	return open_ertr::now();
      }
      ).handle_error(
	open_ertr::pass_further{},
	crimson::ct_error::assert_all{
	  "Invalid error read_rbm_header in RandomBlockManager::open"
	}
      );
    });
}

RandomBlockManager::write_ertr::future<>
RandomBlockManager::write(
  blk_paddr_t addr,
  bufferptr &bptr)
{
  ceph_assert(device);
  if (addr > super.end - super.start) {
    return crimson::ct_error::erange::make();
  }
  return device->write(
    addr,
    bptr);
}

RandomBlockManager::read_ertr::future<>
RandomBlockManager::read(
  blk_paddr_t addr,
  bufferptr &buffer)
{
  ceph_assert(device);
  if (addr > super.end - super.start ||
      buffer.length() > super.end - super.start) {
    return crimson::ct_error::erange::make();
  }
  return device->read(
      addr,
      buffer);
}

RandomBlockManager::close_ertr::future<>
RandomBlockManager::close()
{
  ceph_assert(device);
  return device->close();
}

RandomBlockManager::open_ertr::future<>
RandomBlockManager::_open_device(const std::string path)
{
  ceph_assert(device);
  return device->open(path, seastar::open_flags::rw
      ).safe_then([this] {
	return open_ertr::now();
  });
}

RandomBlockManager::write_ertr::future<>
RandomBlockManager::write_rbm_header()
{
  bufferlist meta_b_header;
  super.crc = 0;
  encode(super, meta_b_header);
  super.crc = meta_b_header.crc32c(-1);

  bufferlist bl;
  encode(super, bl);
  auto iter = bl.begin();
  auto bp = bufferptr(ceph::buffer::create_page_aligned(super.block_size));
  assert(bl.length() < super.block_size);
  iter.copy(bl.length(), bp.c_str());

  return device->write(super.start, bp
      ).safe_then([this]() {
	return write_ertr::now();
	});
}

RandomBlockManager::read_ertr::future<rbm_metadata_header_t>
RandomBlockManager::read_rbm_header(blk_paddr_t addr)
{
  ceph_assert(device);
  bufferptr bptr = bufferptr(ceph::buffer::create_page_aligned(RBM_SUPERBLOCK_SIZE));
  bptr.zero();
  return device->read(
    addr,
    bptr
  ).safe_then([length=bptr.length(), this, bptr]() -> read_ertr::future<rbm_metadata_header_t> {
    bufferlist bl;
    bl.append(bptr);
    auto p = bl.cbegin();
    rbm_metadata_header_t super_block;
    try {
      decode(super_block, p);
    }
    catch (ceph::buffer::error& e) {
      logger().debug(" read_rbm_header: unable to decode rbm super block {}", e.what());
      return crimson::ct_error::enoent::make();
    }
    checksum_t crc = super_block.crc;
    bufferlist meta_b_header;
    super_block.crc = 0;
    encode(super_block, meta_b_header);
    if (meta_b_header.crc32c(-1) != crc) {
      logger().debug(" bad crc on super block, expected {} != actual {} ", meta_b_header.crc32c(-1), crc);
      return crimson::ct_error::input_output_error::make();
    }
    logger().debug(" got {} ", super);
    return read_ertr::future<rbm_metadata_header_t>(
      read_ertr::ready_future_marker{},
      super_block
      );

  }).handle_error(
    read_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in RandomBlockManager::read_rbm_header"
    }
    );
}

RandomBlockManager::write_ertr::future<>
RandomBlockManager::write(
  blk_paddr_t addr,
  bufferlist &bl)
{
  ceph_assert(device);
  bufferptr bptr;
  try {
    bptr = bufferptr(ceph::buffer::create_page_aligned(bl.length()));
    auto iter = bl.cbegin();
    iter.copy(bl.length(), bptr.c_str());
  } catch (const std::exception &e) {
    logger().error(
      "write: "
      "exception creating aligned buffer {}",
      e
    );
    ceph_assert(0 == "unhandled exception");
  }
  return device->write(
      addr,
      bptr);
}

std::ostream &operator<<(std::ostream &out, const rbm_metadata_header_t &header)
{
  out << " rbm_metadata_header_t(size=" << header.size
       << ", block_size=" << header.block_size
       << ", start=" << header.start
       << ", end=" << header.end
       << ", magic=" << header.magic
       << ", uuid=" << header.uuid
       << ", free_block_count=" << header.free_block_count
       << ", alloc_area_size=" << header.alloc_area_size
       << ", start_alloc_area=" << header.start_alloc_area
       << ", start_data_area=" << header.start_data_area
       << ", flag=" << header.flag
       << ", feature=" << header.feature
       << ", crc=" << header.crc;
  return out << ")";
}

std::ostream &operator<<(std::ostream &out, const rbm_bitmap_block_header_t &header)
{
  out << " rbm_bitmap_block_header_t(size=" << header.size
       << ", checksum=" << header.checksum;
  return out << ")";
}

}
