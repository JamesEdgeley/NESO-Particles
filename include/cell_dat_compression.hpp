#ifndef _NESO_PARTICLES_CELL_DAT_COMPRESSION
#define _NESO_PARTICLES_CELL_DAT_COMPRESSION

#include <CL/sycl.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <mpi.h>
#include <string>

#include "cell_dat.hpp"
#include "compute_target.hpp"
#include "particle_dat.hpp"
#include "particle_set.hpp"
#include "particle_spec.hpp"
#include "typedefs.hpp"

using namespace cl;
namespace NESO::Particles {

class LayerCompressor {
private:
  const int ncell;
  BufferDevice<INT> d_remove_cells;
  BufferDevice<INT> d_remove_layers;
  BufferDevice<INT> d_compress_cells_old;
  BufferDevice<INT> d_compress_cells_new;
  BufferDevice<INT> d_compress_layers_old;
  BufferDevice<INT> d_compress_layers_new;

  // these should be INT not int but hipsycl refused to do atomic refs on long
  // int
  BufferDevice<int> d_npart_cell;
  BufferDevice<int> d_move_counters;

  // references to the ParticleGroup methods
  ParticleDatShPtr<INT> cell_id_dat;
  std::map<Sym<REAL>, ParticleDatShPtr<REAL>> &particle_dats_real;
  std::map<Sym<INT>, ParticleDatShPtr<INT>> &particle_dats_int;

public:
  SYCLTarget &sycl_target;

  ~LayerCompressor() {}
  LayerCompressor(
      SYCLTarget &sycl_target, const int ncell,
      std::map<Sym<REAL>, ParticleDatShPtr<REAL>> &particle_dats_real,
      std::map<Sym<INT>, ParticleDatShPtr<INT>> &particle_dats_int)
      : sycl_target(sycl_target), ncell(ncell), d_remove_cells(sycl_target, 1),
        d_remove_layers(sycl_target, 1), d_compress_cells_old(sycl_target, 1),
        d_compress_cells_new(sycl_target, 1),
        d_compress_layers_old(sycl_target, 1),
        d_compress_layers_new(sycl_target, 1), d_npart_cell(sycl_target, ncell),
        d_move_counters(sycl_target, ncell),
        particle_dats_real(particle_dats_real),
        particle_dats_int(particle_dats_int)

  {}

  inline void set_cell_id_dat(ParticleDatShPtr<INT> cell_id_dat) {
    this->cell_id_dat = cell_id_dat;
  }

  template <typename T>
  inline void compute_remove_compress_indicies(const int npart, T *usm_cells,
                                               T *usm_layers) {

    d_compress_cells_old.realloc_no_copy(npart);
    d_compress_layers_old.realloc_no_copy(npart);
    d_compress_layers_new.realloc_no_copy(npart);

    NESOASSERT(this->d_npart_cell.size >= this->ncell,
               "Bad device_npart_cell length");

    const int ncell = this->ncell;

    auto mpi_particle_dat = this->particle_dats_int[Sym<INT>("NESO_MPI_RANK")];
    auto k_npart_cell = mpi_particle_dat->d_npart_cell;

    auto device_npart_cell_ptr = this->d_npart_cell.ptr;
    auto device_move_counters_ptr = this->d_move_counters.ptr;

    auto compress_cells_old_ptr = this->d_compress_cells_old.ptr;
    auto compress_layers_old_ptr = this->d_compress_layers_old.ptr;
    auto compress_layers_new_ptr = this->d_compress_layers_new.ptr;

    INT ***cell_ids_ptr = this->cell_id_dat->cell_dat.device_ptr();
    this->sycl_target.queue
        .submit([&](sycl::handler &cgh) {
          cgh.parallel_for<>(sycl::range<1>(static_cast<size_t>(ncell)),
                             [=](sycl::id<1> idx) {
                               device_npart_cell_ptr[idx] =
                                   static_cast<int>(k_npart_cell[idx]);
                               device_move_counters_ptr[idx] = 0;
                             });
        })
        .wait();

    this->sycl_target.queue
        .submit([&](sycl::handler &cgh) {
          cgh.parallel_for<>(
              sycl::range<1>(static_cast<size_t>(npart)), [=](sycl::id<1> idx) {
                const auto cell = usm_cells[idx];
                const auto layer = usm_layers[idx];

                // Atomically do device_npart_cell_ptr[cell]--
                sycl::atomic_ref<int, sycl::memory_order::relaxed,
                                 sycl::memory_scope::device>
                    element_atomic(device_npart_cell_ptr[cell]);
                auto count = element_atomic.fetch_add(-1);

                //// indicate this particle is removed by setting
                /// the / cell index to -1
                cell_ids_ptr[cell][0][layer] = -1;
              });
        })
        .wait();

    this->sycl_target.queue
        .submit([&](sycl::handler &cgh) {
          cgh.parallel_for<>(
              sycl::range<1>(static_cast<size_t>(npart)), [=](sycl::id<1> idx) {
                const auto cell = usm_cells[idx];
                const auto layer = usm_layers[idx];

                // Is this layer less than the new cell count?
                // If so then there is a particle in a row greater than the cell
                // count to be copied into this layer.
                if (layer < device_npart_cell_ptr[cell]) {

                  // If there are n rows to be filled in row indices less than
                  // the new cell count then there are n rows greater than the
                  // new cell count which are to be copied down. Atomically
                  // compute which one of those rows this index copies.
                  sycl::atomic_ref<int, sycl::memory_order::relaxed,
                                   sycl::memory_scope::device>
                      element_atomic(device_move_counters_ptr[cell]);
                  const INT source_row_offset = element_atomic.fetch_add(1);

                  // find the source row counting up from new_cell_count to
                  // old_cell_count potential source rows will have a cell index
                  // >= 0. This index should pick the i^th source row where i
                  // was computed from the atomic above.
                  INT found_count = 0;
                  INT source_row = -1;
                  for (INT rowx = device_npart_cell_ptr[cell];
                       rowx < k_npart_cell[cell]; rowx++) {
                    // Is this a potential source row?
                    if (cell_ids_ptr[cell][0][rowx] > -1) {
                      if (source_row_offset == found_count++) {
                        source_row = rowx;
                        break;
                      }
                    }
                  }
                  compress_cells_old_ptr[idx] = cell;
                  compress_layers_new_ptr[idx] = layer;
                  compress_layers_old_ptr[idx] = source_row;

                } else {

                  compress_cells_old_ptr[idx] = -1;
                  compress_layers_new_ptr[idx] = -1;
                  compress_layers_old_ptr[idx] = -1;
                }
              });
        })
        .wait_and_throw();
  }

  template <typename T>
  inline void remove_particles(const int npart, T *usm_cells, T *usm_layers) {
    this->compute_remove_compress_indicies(npart, usm_cells, usm_layers);

    auto compress_cells_old_ptr = this->d_compress_cells_old.ptr;
    auto compress_layers_old_ptr = this->d_compress_layers_old.ptr;
    auto compress_layers_new_ptr = this->d_compress_layers_new.ptr;

    for (auto &dat : particle_dats_real) {
      dat.second->copy_particle_data(
          npart, compress_cells_old_ptr, compress_cells_old_ptr,
          compress_layers_old_ptr, compress_layers_new_ptr);
      dat.second->set_npart_cells_device(this->d_npart_cell.ptr);
    }
    for (auto &dat : particle_dats_int) {
      dat.second->copy_particle_data(
          npart, compress_cells_old_ptr, compress_cells_old_ptr,
          compress_layers_old_ptr, compress_layers_new_ptr);
      dat.second->set_npart_cells_device(this->d_npart_cell.ptr);
    }

    // the move calls are async
    sycl_target.queue.wait_and_throw();

    for (auto &dat : particle_dats_real) {
      dat.second->npart_device_to_host();
      dat.second->trim_cell_dat_rows();
    }
    for (auto &dat : particle_dats_int) {
      dat.second->npart_device_to_host();
      dat.second->trim_cell_dat_rows();
    }
  }
};

} // namespace NESO::Particles

#endif
