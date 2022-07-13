#include <CL/sycl.hpp>
#include <gtest/gtest.h>
#include <neso_particles.hpp>
#include <random>

using namespace NESO::Particles;

TEST(ParticleGroup, global_move_single) {

  const int ndim = 2;
  std::vector<int> dims(ndim);
  dims[0] = 8;
  dims[1] = 8;

  const double cell_extent = 1.0;
  const int subdivision_order = 0;
  CartesianHMesh mesh(MPI_COMM_WORLD, ndim, dims, cell_extent,
                      subdivision_order);

  SYCLTarget sycl_target{GPU_SELECTOR, mesh.get_comm()};

  Domain domain(mesh);

  ParticleSpec particle_spec{ParticleProp(Sym<REAL>("P"), ndim, true),
                             ParticleProp(Sym<REAL>("V"), 3),
                             ParticleProp(Sym<INT>("CELL_ID"), 1, true),
                             ParticleProp(Sym<INT>("ID"), 1)};

  ParticleGroup A(domain, particle_spec, sycl_target);

  A.add_particle_dat(ParticleDat(sycl_target, ParticleProp(Sym<REAL>("FOO"), 3),
                                 domain.mesh.get_cell_count()));

  std::mt19937 rng_pos(52234234);
  std::mt19937 rng_vel(52234231);
  std::mt19937 rng_rank(18241);

  const int N = 1024;

  auto positions =
      uniform_within_extents(N, ndim, mesh.global_extents, rng_pos);
  auto velocities =
      NESO::Particles::normal_distribution(N, 3, 0.0, 1.0, rng_vel);

  std::uniform_int_distribution<int> uniform_dist(
      0, sycl_target.comm_pair.size_parent - 1);

  ParticleSet initial_distribution(N, A.get_particle_spec());

  // determine which particles should end up on which rank
  std::map<int, std::vector<int>> mapping;
  for (int px = 0; px < N; px++) {
    for (int dimx = 0; dimx < ndim; dimx++) {
      initial_distribution[Sym<REAL>("P")][px][dimx] = positions[dimx][px];
    }
    for (int dimx = 0; dimx < 3; dimx++) {
      initial_distribution[Sym<REAL>("V")][px][dimx] = velocities[dimx][px];
    }
    initial_distribution[Sym<INT>("CELL_ID")][px][0] = 0;
    initial_distribution[Sym<INT>("ID")][px][0] = px;
    const auto px_rank = uniform_dist(rng_rank);
    initial_distribution[Sym<INT>("NESO_MPI_RANK")][px][0] = px_rank;
    mapping[px_rank].push_back(px);
  }

  if (sycl_target.comm_pair.rank_parent == 0) {
    A.add_particles_local(initial_distribution);
  }

  A.global_move();

  const int rank = sycl_target.comm_pair.rank_parent;
  const int correct_npart = mapping[rank].size();

  // check the dats hold the correct number of particles in all dats
  for (auto &dat : A.particle_dats_real) {
    ASSERT_EQ(dat.second->s_npart_cell[0], correct_npart);
  }
  for (auto &dat : A.particle_dats_int) {
    ASSERT_EQ(dat.second->s_npart_cell[0], correct_npart);
  }

  // check cell 0 contains the data for the correct particles in the correct
  // rows
  auto cell_ids_dat = A[Sym<INT>("ID")]->cell_dat.get_cell(0);
  auto velocities_dat = A[Sym<REAL>("V")]->cell_dat.get_cell(0);

  // loop over owned particles and check they should be on this rank
  for (int px = 0; px < correct_npart; px++) {
    // find the particle in the dat
    int row = -1;
    const int id = mapping[rank][px];
    for (int rx = 0; rx < correct_npart; rx++) {
      if ((*cell_ids_dat)[0][rx] == id) {
        row = rx;
        break;
      }
    }
    ASSERT_TRUE(row != -1);

    // we use this assumption below - check it actually holds
    ASSERT_EQ(id, initial_distribution[Sym<INT>("ID")][id][0]);

    // check the velocities were copied correctly
    for (int cx = 0; cx < 3; cx++) {
      ASSERT_EQ(initial_distribution[Sym<REAL>("V")][id][cx],
                (*velocities_dat)[cx][row]);
    }
  }

  mesh.free();
}

TEST(ParticleGroup, global_move_multiple) {

  const int ndim = 2;
  std::vector<int> dims(ndim);
  dims[0] = 4;
  dims[1] = 4;

  const double cell_extent = 1.0;
  const int subdivision_order = 0;
  CartesianHMesh mesh(MPI_COMM_WORLD, ndim, dims, cell_extent,
                      subdivision_order);

  SYCLTarget sycl_target{GPU_SELECTOR, mesh.get_comm()};

  Domain domain(mesh);

  ParticleSpec particle_spec{ParticleProp(Sym<REAL>("P"), ndim, true),
                             ParticleProp(Sym<REAL>("P_ORIG"), ndim),
                             ParticleProp(Sym<REAL>("V"), 3),
                             ParticleProp(Sym<INT>("CELL_ID"), 1, true),
                             ParticleProp(Sym<INT>("ID"), 1)};

  ParticleGroup A(domain, particle_spec, sycl_target);

  A.add_particle_dat(ParticleDat(sycl_target, ParticleProp(Sym<REAL>("FOO"), 3),
                                 domain.mesh.get_cell_count()));

  std::mt19937 rng_pos(52234234);
  std::mt19937 rng_vel(52234231);
  std::mt19937 rng_rank(18241);

  const int N = 10;
  const int Ntest = 1;
  const REAL dt = 0.001;

  auto positions =
      uniform_within_extents(N, ndim, mesh.global_extents, rng_pos);
  auto velocities =
      NESO::Particles::normal_distribution(N, 3, 0.0, 1.0, rng_vel);

  std::uniform_int_distribution<int> uniform_dist(
      0, sycl_target.comm_pair.size_parent - 1);

  ParticleSet initial_distribution(N, A.get_particle_spec());

  // determine which particles should end up on which rank
  std::map<int, std::vector<int>> mapping;
  for (int px = 0; px < N; px++) {
    for (int dimx = 0; dimx < ndim; dimx++) {
      initial_distribution[Sym<REAL>("P")][px][dimx] = positions[dimx][px];
      initial_distribution[Sym<REAL>("P_ORIG")][px][dimx] = positions[dimx][px];
    }
    for (int dimx = 0; dimx < 3; dimx++) {
      initial_distribution[Sym<REAL>("V")][px][dimx] = velocities[dimx][px];
    }
    initial_distribution[Sym<INT>("CELL_ID")][px][0] = 0;
    initial_distribution[Sym<INT>("ID")][px][0] = px;
    const auto px_rank = uniform_dist(rng_rank);
    initial_distribution[Sym<INT>("NESO_MPI_RANK")][px][0] = px_rank;
    mapping[px_rank].push_back(px);
  }

  if (sycl_target.comm_pair.rank_parent == 0) {
    A.add_particles_local(initial_distribution);
  }

  MeshHierarchyGlobalMap mesh_heirarchy_global_map(
      sycl_target, domain.mesh, A.position_dat, A.cell_id_dat, A.mpi_rank_dat);

  CartesianPeriodic pbc(sycl_target, mesh, A.position_dat);

  A[Sym<INT>("NESO_MPI_RANK")]->print();

  reset_mpi_ranks(A[Sym<INT>("NESO_MPI_RANK")]);

  auto lambda_advect = [&] { std::cout << " FOO " << std::endl; };

  auto lambda_test = [&] {
    auto k_P = A[Sym<REAL>("P")]->cell_dat.device_ptr();
    auto k_V = A[Sym<REAL>("V")]->cell_dat.device_ptr();
    const auto k_ndim = ndim;
    const auto k_dt = dt;

    const auto pl_iter_range = A.mpi_rank_dat->get_particle_loop_iter_range();
    const auto pl_stride = A.mpi_rank_dat->get_particle_loop_cell_stride();
    const auto pl_npart_cell = A.mpi_rank_dat->get_particle_loop_npart_cell();

    sycl_target.queue
        .submit([&](sycl::handler &cgh) {
          cgh.parallel_for<>(
              sycl::range<1>(pl_iter_range), [=](sycl::id<1> idx) {
                const INT cellx = ((INT)idx) / pl_stride;
                const INT layerx = ((INT)idx) % pl_stride;
                if (layerx < pl_npart_cell[cellx]) {
                  for (int dimx = 0; dimx < k_ndim; dimx++) {
                    k_P[cellx][dimx][layerx] += k_V[cellx][dimx][layerx] * k_dt;
                  }
                }
              });
        })
        .wait_and_throw();
  };

  for (int testx = 0; testx < Ntest; testx++) {
    pbc.execute();
    mesh_heirarchy_global_map.execute();
    A.global_move();
    // would normally bin into local cells here
    // then move particles between cells and compress
    A[Sym<INT>("NESO_MPI_RANK")]->print();

    // test current position
    lambda_test();

    // advect need a particle loop
    lambda_advect();
  }
  mesh.free();
}
