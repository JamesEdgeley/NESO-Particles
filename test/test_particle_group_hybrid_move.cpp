#include "include/test_neso_particles.hpp"

class ParticleGroupHybridMove : public testing::TestWithParam<int> {};

TEST_P(ParticleGroupHybridMove, multiple) {

  const int ndim = GetParam();
  std::vector<int> dims(3);
  dims[0] = (ndim == 2) ? 8 : 5;
  dims[1] = (ndim == 2) ? 5 : 4;
  dims[2] = 3;

  const double cell_extent = 1.0;
  const int subdivision_order = 1;
  const int stencil_width = 1;

  auto mesh =
      std::make_shared<CartesianHMesh>(MPI_COMM_WORLD, ndim, dims, cell_extent,
                                       subdivision_order, stencil_width);

  auto sycl_target =
      std::make_shared<SYCLTarget>(GPU_SELECTOR, mesh->get_comm());

  // create object to map local cells + stencil to ranks
  auto cart_local_mapper = CartesianHMeshLocalMapper(sycl_target, mesh);
  auto domain = std::make_shared<Domain>(mesh, cart_local_mapper);

  ParticleSpec particle_spec{ParticleProp(Sym<REAL>("P"), ndim, true),
                             ParticleProp(Sym<REAL>("P_ORIG"), ndim),
                             ParticleProp(Sym<REAL>("V"), 3),
                             ParticleProp(Sym<INT>("CELL_ID"), 1, true),
                             ParticleProp(Sym<INT>("ID"), 1)};

  auto A = make_test_obj<ParticleGroup>(domain, particle_spec, sycl_target);

  std::mt19937 rng_pos(52234234);
  std::mt19937 rng_vel(52234231);
  std::mt19937 rng_rank(18241);

  const int N = 1024;
  const int Ntest = 1024;
  const REAL dt = 1.0;
  const REAL tol = 1.0e-10;
  const int cell_count = domain->mesh->get_cell_count();

  auto positions =
      uniform_within_extents(N, ndim, mesh->global_extents, rng_pos);
  auto velocities = NESO::Particles::normal_distribution(
      N, 3, 0.0, dims[0] * cell_extent, rng_vel);

  std::uniform_int_distribution<int> uniform_dist(
      0, sycl_target->comm_pair.size_parent - 1);

  ParticleSet initial_distribution(N, A->get_particle_spec());

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
  }

  if (sycl_target->comm_pair.rank_parent == 0) {
    A->add_particles_local(initial_distribution);
  }

  CartesianPeriodic pbc(sycl_target, mesh, A->position_dat);
  CartesianCellBin ccb(sycl_target, mesh, A->position_dat, A->cell_id_dat);

  reset_mpi_ranks(A->mpi_rank_dat);

  const auto k_ndim = ndim;
  const auto k_dt = dt;
  auto advect_loop = particle_loop(
      A,
      [=](auto k_V, auto k_P) {
        for (int dimx = 0; dimx < k_ndim; dimx++) {
          k_P.at(dimx) += k_V.at(dimx) * k_dt;
        }
      },
      Access::read(Sym<REAL>("V")), Access::write(Sym<REAL>("P")));

  REAL T = 0.0;

  auto lambda_test = [&] {
    int npart_found = A->mpi_rank_dat->get_npart_local();
    int global_npart_found = 0;
    MPICHK(MPI_Allreduce(&npart_found, &global_npart_found, 1, MPI_INT, MPI_SUM,
                         sycl_target->comm_pair.comm_parent));
    ASSERT_EQ(global_npart_found, N);

    // for all cells
    for (int cellx = 0; cellx < cell_count; cellx++) {
      auto P = A->get_cell(Sym<REAL>("P"), cellx);
      auto P_ORIG = A->get_cell(Sym<REAL>("P_ORIG"), cellx);
      auto V = A->get_cell(Sym<REAL>("V"), cellx);
      auto C = A->get_cell(Sym<INT>("CELL_ID"), cellx);
      auto MPI_RANK = A->get_cell(Sym<INT>("NESO_MPI_RANK"), cellx);
      auto ID = A->get_cell(Sym<INT>("ID"), cellx);

      const int nrow = P->nrow;

      // for each dimension
      for (int dimx = 0; dimx < ndim; dimx++) {

        // for each particle
        for (int px = 0; px < nrow; px++) {
          // read the original position of the particle and compute the correct
          // current position based on the time T and velocity on the particle
          const REAL P_correct_abs = (*P_ORIG)[dimx][px] + T * (*V)[dimx][px];
          // map the absolute position back into the periodic domain

          const REAL extent = mesh->global_extents[dimx];
          const REAL P_correct =
              std::fmod(std::fmod(P_correct_abs, extent) + extent, extent);

          const REAL P_to_test = (*P)[dimx][px];

          const REAL err0 = ABS(P_correct - P_to_test);
          // case where P_correct is at 0 and P_to_test is at extent - which is
          // the same point in the periodic mapping
          const REAL err1 = ABS(err0 - extent);

          ASSERT_TRUE(((err0 <= tol) || (err1 <= tol)));

          // check that the particle position is actually owned by this MPI
          // rank
          const int particle_cell =
              ((REAL)(P_to_test * mesh->inverse_cell_width_fine));

          ASSERT_TRUE(particle_cell >= mesh->cell_starts[dimx]);
          ASSERT_TRUE(particle_cell < mesh->cell_ends[dimx]);
        }
      }

      // check the particle is in the cell it is binned into
      // for each particle
      for (int px = 0; px < nrow; px++) {
        int index_tuple[3] = {0, 0, 0};
        for (int dimx = 0; dimx < ndim; dimx++) {
          const REAL P_CELL =
              (*P)[dimx][px] - mesh->cell_starts[dimx] * mesh->cell_width_fine;
          index_tuple[dimx] = ((REAL)P_CELL * mesh->inverse_cell_width_fine);
        }
        int index_linear =
            index_tuple[0] +
            mesh->cell_counts_local[0] *
                (index_tuple[1] + mesh->cell_counts_local[1] * index_tuple[2]);

        ASSERT_EQ((*C)[0][px], index_linear);
      }
    }
  };

  for (int testx = 0; testx < Ntest; testx++) {
    pbc.execute();

    if (testx % 20 == 0) {
      A->reset_version_tracker();
    }
    A->hybrid_move();
    if (testx % 20 == 0) {
      A->test_version_different();
      A->test_internal_state();
    }

    ccb.execute();
    A->cell_move();

    lambda_test();
    advect_loop->execute();

    T += dt;
  }
  mesh->free();
}

INSTANTIATE_TEST_SUITE_P(MultipleDim, ParticleGroupHybridMove,
                         testing::Values(2, 3));
