#ifndef _NESO_PARTICLES_COMPUTE_TARGET
#define _NESO_PARTICLES_COMPUTE_TARGET

#include <CL/sycl.hpp>
#include <mpi.h>

#include "communication.hpp"
#include "profiling.hpp"
#include "typedefs.hpp"

using namespace cl;

namespace NESO::Particles {

class SYCLTarget {
private:
public:
  sycl::device device;
  sycl::queue queue;
  MPI_Comm comm;
  CommPair comm_pair;
  ProfileMap profile_map;

  SYCLTarget(){};
  SYCLTarget(const int gpu_device, MPI_Comm comm) : comm_pair(comm) {
    if (gpu_device > 0) {
      try {
        this->device = sycl::device(sycl::gpu_selector());
      } catch (sycl::exception const &e) {
        std::cout << "Cannot select a GPU\n" << e.what() << "\n";
        std::cout << "Using a CPU device\n";
        this->device = sycl::device(sycl::cpu_selector());
      }
    } else if (gpu_device < 0) {
      this->device = sycl::device(sycl::cpu_selector());
    } else {
      this->device = sycl::device(sycl::default_selector());
    }

    if (this->comm_pair.rank_parent == 0) {
      std::cout << "Using " << this->device.get_info<sycl::info::device::name>()
                << std::endl;
      std::cout << "Kernel type: " << NESO_PARTICLES_DEVICE_LABEL << std::endl;
    }

    this->queue = sycl::queue(this->device);
    this->comm = comm;

    this->profile_map.set("MPI", "MPI_COMM_WORLD_rank",
                          this->comm_pair.rank_parent);
    this->profile_map.set("MPI", "MPI_COMM_WORLD_size",
                          this->comm_pair.size_parent);
  }
  ~SYCLTarget() {}

  void free() { comm_pair.free(); }
};

template <typename T> class BufferDevice {
private:
public:
  SYCLTarget &sycl_target;
  T *ptr;
  size_t size;
  BufferDevice(SYCLTarget &sycl_target, size_t size)
      : sycl_target(sycl_target) {
    this->size = size;
    this->ptr = (T *)sycl::malloc_device(size * sizeof(T), sycl_target.queue);
  }
  inline size_t size_bytes() { return this->size * sizeof(T); }
  inline int realloc_no_copy(const size_t size) {
    if (size > this->size) {
      sycl::free(this->ptr, this->sycl_target.queue);
      this->ptr = (T *)sycl::malloc_device(size * sizeof(T), sycl_target.queue);
      this->size = size;
    }
    return this->size;
  }
  ~BufferDevice() {
    if (this->ptr != NULL) {
      sycl::free(this->ptr, sycl_target.queue);
    }
  }
};

template <typename T> class BufferShared {
private:
public:
  SYCLTarget &sycl_target;
  T *ptr;
  size_t size;
  BufferShared(SYCLTarget &sycl_target, size_t size)
      : sycl_target(sycl_target) {
    this->size = size;
    this->ptr = (T *)sycl::malloc_shared(size * sizeof(T), sycl_target.queue);
  }
  inline size_t size_bytes() { return this->size * sizeof(T); }
  inline int realloc_no_copy(const size_t size) {
    if (size > this->size) {
      sycl::free(this->ptr, this->sycl_target.queue);
      this->ptr = (T *)sycl::malloc_shared(size * sizeof(T), sycl_target.queue);
      this->size = size;
    }
    return this->size;
  }
  ~BufferShared() {
    if (this->ptr != NULL) {
      sycl::free(this->ptr, sycl_target.queue);
    }
  }
  // inline T& operator[](const int index) {
  //   return this->ptr[index];
  // };
};

template <typename T> class BufferHost {
private:
public:
  SYCLTarget &sycl_target;
  T *ptr;
  size_t size;
  BufferHost(SYCLTarget &sycl_target, size_t size) : sycl_target(sycl_target) {
    this->size = size;
    this->ptr = (T *)sycl::malloc_host(size * sizeof(T), sycl_target.queue);
  }
  inline size_t size_bytes() { return this->size * sizeof(T); }
  inline int realloc_no_copy(const size_t size) {
    if (size > this->size) {
      sycl::free(this->ptr, this->sycl_target.queue);
      this->ptr = (T *)sycl::malloc_host(size * sizeof(T), sycl_target.queue);
      this->size = size;
    }
    return this->size;
  }
  ~BufferHost() {
    if (this->ptr != NULL) {
      sycl::free(this->ptr, sycl_target.queue);
    }
  }
};

template <typename T> class BufferDeviceHost {
private:
public:
  SYCLTarget &sycl_target;
  size_t size;

  BufferDevice<T> d_buffer;
  BufferHost<T> h_buffer;

  ~BufferDeviceHost(){};
  BufferDeviceHost(SYCLTarget &sycl_target, size_t size)
      : sycl_target(sycl_target), size(size), d_buffer(sycl_target, size),
        h_buffer(sycl_target, size){};

  inline size_t size_bytes() { return this->size * sizeof(T); }
  inline int realloc_no_copy(const size_t size) {
    this->d_buffer.realloc_no_copy(size);
    this->h_buffer.realloc_no_copy(size);
    this->size = size;
    return this->size;
  }

  inline void host_to_device() {
    this->sycl_target.queue
        .memcpy(this->d_buffer.ptr, this->h_buffer.ptr, this->size_bytes())
        .wait();
  }

  inline void device_to_host() {
    this->sycl_target.queue
        .memcpy(this->h_buffer.ptr, this->d_buffer.ptr, this->size_bytes())
        .wait();
  }

  inline sycl::event async_host_to_device() {
    return this->sycl_target.queue.memcpy(
        this->d_buffer.ptr, this->h_buffer.ptr, this->size_bytes());
  }

  inline sycl::event async_device_to_host() {
    return this->sycl_target.queue.memcpy(
        this->h_buffer.ptr, this->d_buffer.ptr, this->size_bytes());
  }
};

} // namespace NESO::Particles

#endif
