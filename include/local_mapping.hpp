#ifndef _NESO_PARTICLES_LOCAL_MAPPING
#define _NESO_PARTICLES_LOCAL_MAPPING

#include <CL/sycl.hpp>
#include <memory>
#include <mpi.h>

#include "compute_target.hpp"
#include "particle_dat.hpp"
#include "profiling.hpp"
#include "typedefs.hpp"

using namespace cl;
namespace NESO::Particles {

class ParticleGroup;

/**
 * Abstract class to map positions to owning MPI ranks based on local
 * information, i.e. halo cells. Could also use or set the cell id.
 */
class LocalMapper {
public:
  /**
   * This function maps particle positions to cells on the underlying mesh.
   *
   * @param particle_group ParticleGroup containing particle positions.
   */
  virtual inline void map(ParticleGroup &particle_group,
                          const int map_cell = -1) = 0;

  /**
   * Callback for ParticleGroup to execute for additional setup of the
   * LocalMapper that may involve the ParticleGroup.
   *
   * @param particle_group ParticleGroup instance.
   */
  virtual inline void
  particle_group_callback(ParticleGroup &particle_group) = 0;
};

typedef std::shared_ptr<LocalMapper> LocalMapperShPtr;

/**
 *  Dummy LocalMapper implementation that does nothing to use as a default.
 */
class DummyLocalMapperT : public LocalMapper {
private:
public:
  /**
   *  No-op implementation of map.
   */
  inline void map(ParticleGroup &particle_group, const int map_cell = -1){};

  /**
   *  No-op implementation of callback.
   */
  inline void particle_group_callback(ParticleGroup &particle_group){};
};

inline std::shared_ptr<DummyLocalMapperT> DummyLocalMapper() {
  return std::make_shared<DummyLocalMapperT>();
}

} // namespace NESO::Particles
#endif
