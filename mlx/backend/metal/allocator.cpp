// Copyright © 2023-2024 Apple Inc.
#include "mlx/backend/metal/allocator.h"
#include "mlx/backend/metal/metal.h"
#include "mlx/backend/metal/resident.h"
#include "mlx/memory.h"

#include <mach/vm_page_size.h>
#include <unistd.h>
#include <cstdlib>

namespace mlx::core {

constexpr size_t resource_options =
    MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;

namespace allocator {

Allocator& allocator() {
  return metal::allocator();
}

void* Buffer::raw_ptr() {
  if (!ptr_) {
    return nullptr;
  }
  return static_cast<MTL::Buffer*>(ptr_)->contents();
}

} // namespace allocator

namespace metal {

namespace {

BufferCache::BufferCache(ResidencySet& residency_set)
    : head_(nullptr),
      tail_(nullptr),
      pool_size_(0),
      residency_set_(residency_set) {}

BufferCache::~BufferCache() {
  auto pool = metal::new_scoped_memory_pool();
  clear();
}

int BufferCache::clear() {
  int n_release = 0;
  for (auto& [size, holder] : buffer_pool_) {
    if (holder->buf) {
      if (!holder->buf->heap()) {
        residency_set_.erase(holder->buf);
      }
      holder->buf->release();
      n_release++;
    }
    delete holder;
  }
  buffer_pool_.clear();
  pool_size_ = 0;
  head_ = nullptr;
  tail_ = nullptr;
  return n_release;
}

MTL::Buffer* BufferCache::reuse_from_cache(size_t size) {
  // Find the closest buffer in pool
  MTL::Buffer* pbuf = nullptr;

  auto it = buffer_pool_.lower_bound(size);

  // Make sure we use most of the available memory
  while (!pbuf && it != buffer_pool_.end() &&
         it->first < std::min(2 * size, size + 2 * vm_page_size)) {
    // Collect from the cache
    pbuf = it->second->buf;

    // Remove from cache
    remove_from_list(it->second);
    delete it->second;
    it = buffer_pool_.erase(it);
  }

  if (pbuf) {
    pool_size_ -= pbuf->length();
  }

  return pbuf;
}

void BufferCache::recycle_to_cache(MTL::Buffer* buf) {
  // Add to cache
  if (buf) {
    BufferHolder* bh = new BufferHolder(buf);
    add_at_head(bh);
    pool_size_ += buf->length();
    buffer_pool_.insert({buf->length(), bh});
  }
}

int BufferCache::release_cached_buffers(size_t min_bytes_to_free) {
  if (min_bytes_to_free >= 0.9 * pool_size_) {
    return clear();
  } else {
    int n_release = 0;
    size_t total_bytes_freed = 0;

    while (tail_ && (total_bytes_freed < min_bytes_to_free)) {
      if (tail_->buf) {
        total_bytes_freed += tail_->buf->length();
        if (!tail_->buf->heap()) {
          residency_set_.erase(tail_->buf);
        }
        tail_->buf->release();
        tail_->buf = nullptr;
        n_release++;
      }
      remove_from_list(tail_);
    }
    pool_size_ -= total_bytes_freed;
    return n_release;
  }
}

void BufferCache::add_at_head(BufferCache::BufferHolder* to_add) {
  if (!to_add)
    return;

  if (!head_) {
    head_ = to_add;
    tail_ = to_add;
  } else {
    head_->prev = to_add;
    to_add->next = head_;
    head_ = to_add;
  }
}

void BufferCache::remove_from_list(BufferCache::BufferHolder* to_remove) {
  if (!to_remove) {
    return;
  }

  // If in the middle
  if (to_remove->prev && to_remove->next) {
    to_remove->prev->next = to_remove->next;
    to_remove->next->prev = to_remove->prev;
  } else if (to_remove->prev && to_remove == tail_) { // If tail
    tail_ = to_remove->prev;
    tail_->next = nullptr;
  } else if (to_remove == head_ && to_remove->next) { // If head
    head_ = to_remove->next;
    head_->prev = nullptr;
  } else if (to_remove == head_ && to_remove == tail_) { // If only element
    head_ = nullptr;
    tail_ = nullptr;
  }

  to_remove->prev = nullptr;
  to_remove->next = nullptr;
}

} // namespace

MetalAllocator::MetalAllocator()
    : device_(device(mlx::core::Device::gpu).mtl_device()),
      residency_set_(device_),
      buffer_cache_(residency_set_) {
  auto pool = metal::new_scoped_memory_pool();
  auto memsize = std::get<size_t>(device_info().at("memory_size"));
  auto max_rec_size =
      std::get<size_t>(device_info().at("max_recommended_working_set_size"));
  resource_limit_ = std::get<size_t>(device_info().at("resource_limit"));
  block_limit_ = std::min(1.5 * max_rec_size, 0.95 * memsize);
  gc_limit_ = std::min(static_cast<size_t>(0.95 * max_rec_size), block_limit_);
  max_pool_size_ = block_limit_;
  device(mlx::core::Device::gpu)
      .set_residency_set(residency_set_.mtl_residency_set());
  bool is_vm = std::get<std::string>(device_info().at("device_name")) ==
      "Apple Paravirtual device";
  if (is_vm) {
    return;
  }
  auto heap_desc = MTL::HeapDescriptor::alloc()->init();
  heap_desc->setResourceOptions(resource_options);
  heap_desc->setSize(heap_size_);
  heap_ = device_->newHeap(heap_desc);
  heap_desc->release();
  residency_set_.insert(heap_);
}

MetalAllocator::~MetalAllocator() {
  auto pool = metal::new_scoped_memory_pool();
  if (heap_) {
    heap_->release();
  }
}

size_t MetalAllocator::set_cache_limit(size_t limit) {
  std::unique_lock lk(mutex_);
  std::swap(limit, max_pool_size_);
  return limit;
};

size_t MetalAllocator::set_memory_limit(size_t limit) {
  std::unique_lock lk(mutex_);
  std::swap(limit, block_limit_);
  gc_limit_ = std::min(
      block_limit_,
      static_cast<size_t>(0.95 * device_->recommendedMaxWorkingSetSize()));
  return limit;
};

size_t MetalAllocator::get_memory_limit() {
  return block_limit_;
}

size_t MetalAllocator::set_wired_limit(size_t limit) {
  std::unique_lock lk(mutex_);
  std::swap(limit, wired_limit_);
  residency_set_.resize(wired_limit_);
  return limit;
};

Buffer MetalAllocator::malloc(size_t size) {
  // Metal doesn't like empty buffers
  if (size == 0) {
    return Buffer{nullptr};
  }

  // More helpful message if maximum buffer length is exceeded
  if (size > device_->maxBufferLength()) {
    std::ostringstream msg;
    msg << "[metal::malloc] Attempting to allocate " << size
        << " bytes which is greater than"
        << " the maximum allowed buffer size of " << device_->maxBufferLength()
        << " bytes.";
    throw std::runtime_error(msg.str());
  }

  // Align up memory
  if (size > vm_page_size) {
    size = vm_page_size * ((size + vm_page_size - 1) / vm_page_size);
  }

  // Try the cache
  std::unique_lock lk(mutex_);
  MTL::Buffer* buf = buffer_cache_.reuse_from_cache(size);
  if (!buf) {
    size_t mem_required = get_active_memory() + get_cache_memory() + size;

    auto pool = metal::new_scoped_memory_pool();

    // If we have a lot of memory pressure or are over the maximum cache size,
    // try to reclaim memory from the cache
    if (mem_required >= gc_limit_ || num_resources_ >= resource_limit_) {
      num_resources_ -=
          buffer_cache_.release_cached_buffers(mem_required - gc_limit_);
    }

    // Allocate new buffer if needed
    if (num_resources_ >= resource_limit_) {
      std::ostringstream msg;
      msg << "[metal::malloc] Resource limit (" << resource_limit_
          << ") exceeded.";
      throw std::runtime_error(msg.str());
    }
    lk.unlock();
    if (size < small_size_ && heap_) {
      buf = heap_->newBuffer(size, resource_options);
    }
    if (!buf) {
      buf = device_->newBuffer(size, resource_options);
    }
    if (!buf) {
      return Buffer{nullptr};
    }
    lk.lock();
    num_resources_++;
    if (!buf->heap()) {
      residency_set_.insert(buf);
    }
  }

  active_memory_ += buf->length();
  peak_memory_ = std::max(peak_memory_, active_memory_);

  // Maintain the cache below the requested limit
  if (get_cache_memory() > max_pool_size_) {
    auto pool = metal::new_scoped_memory_pool();
    num_resources_ -= buffer_cache_.release_cached_buffers(
        get_cache_memory() - max_pool_size_);
  }

  return Buffer{static_cast<void*>(buf)};
}

void MetalAllocator::clear_cache() {
  std::unique_lock lk(mutex_);
  auto pool = metal::new_scoped_memory_pool();
  num_resources_ -= buffer_cache_.clear();
}

void MetalAllocator::free(Buffer buffer) {
  auto buf = static_cast<MTL::Buffer*>(buffer.ptr());
  if (buf == nullptr) {
    return;
  }
  std::unique_lock lk(mutex_);
  active_memory_ -= buf->length();
  if (get_cache_memory() < max_pool_size_) {
    buffer_cache_.recycle_to_cache(buf);
  } else {
    num_resources_--;
    if (!buf->heap()) {
      residency_set_.erase(buf);
    }
    lk.unlock();
    auto pool = metal::new_scoped_memory_pool();
    buf->release();
  }
}

size_t MetalAllocator::size(Buffer buffer) const {
  return static_cast<MTL::Buffer*>(buffer.ptr())->length();
}

MetalAllocator& allocator() {
  // By creating the |allocator_| on heap, the destructor of MetalAllocator
  // will not be called on exit and buffers in the cache will be leaked. This
  // can save some time at program exit.
  static MetalAllocator* allocator_ = new MetalAllocator;
  return *allocator_;
}

} // namespace metal

size_t set_cache_limit(size_t limit) {
  return metal::allocator().set_cache_limit(limit);
}
size_t set_memory_limit(size_t limit) {
  return metal::allocator().set_memory_limit(limit);
}
size_t get_memory_limit() {
  return metal::allocator().get_memory_limit();
}
size_t set_wired_limit(size_t limit) {
  if (limit > std::get<size_t>(metal::device_info().at(
                  "max_recommended_working_set_size"))) {
    throw std::invalid_argument(
        "[metal::set_wired_limit] Setting a wired limit larger than "
        "the maximum working set size is not allowed.");
  }
  return metal::allocator().set_wired_limit(limit);
}
size_t get_active_memory() {
  return metal::allocator().get_active_memory();
}
size_t get_peak_memory() {
  return metal::allocator().get_peak_memory();
}
void reset_peak_memory() {
  metal::allocator().reset_peak_memory();
}
size_t get_cache_memory() {
  return metal::allocator().get_cache_memory();
}
void clear_cache() {
  return metal::allocator().clear_cache();
}

} // namespace mlx::core
