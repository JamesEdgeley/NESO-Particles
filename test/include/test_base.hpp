#ifndef _NESO_PARTICLES_TEST_BASE_H_
#define _NESO_PARTICLES_TEST_BASE_H_

#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <neso_particles.hpp>
#include <numeric>
#include <random>
#include <type_traits>
#include <vector>

namespace NESO::Particles {
template <typename T> struct NPToTestMapper;
template <typename T> struct TestMapperToNP;
} // namespace NESO::Particles

using namespace NESO::Particles;

#endif
