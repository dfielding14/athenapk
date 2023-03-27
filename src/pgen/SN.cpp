//========================================================================================
// AthenaPK - a performance portable block structured AMR astrophysical MHD code.
// Copyright (c) 2021-2023, Athena-Parthenon Collaboration. All rights reserved.
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file SN.cpp
//  \brief Problem generator for spherical blast wave problem.  Works in Cartesian,
//         cylindrical, and spherical coordinates.  Contains post-processing code
//         to check whether blast is spherical for regression tests
//
// REFERENCE: P. Londrillo & L. Del Zanna, "High-order upwind schemes for
//   multidimensional MHD", ApJ, 530, 508 (2000), and references therein.

// C headers

// C++ headers
#include <algorithm>
#include <cmath>
#include <cstdio>  // fopen(), fprintf(), freopen()
#include <cstring> // strcmp()
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

// Parthenon headers
#include "basic_types.hpp"
#include "mesh/mesh.hpp"
#include <parthenon/driver.hpp>
#include <parthenon/package.hpp>
#include <vector>

// AthenaPK headers
#include "../main.hpp"
#include "parthenon/prelude.hpp"
#include "parthenon_arrays.hpp"
#include "utils/error_checking.hpp"
#include "../units.hpp"

using namespace parthenon::package::prelude;

namespace SN {

//========================================================================================
//! \fn void ProblemGenerator(MeshBlock &pmb, ParameterInput *pin)
//  \brief Spherical blast wave test problem generator
//========================================================================================

void ProblemInitPackageData(ParameterInput *pin, parthenon::StateDescriptor *pkg) {

  Units units(pin);

  const Real ta = pin->GetReal("problem/blast", "temperature_ambient");
  const Real da = pin->GetReal("problem/blast", "density_ambient") / units.code_density_cgs();
  Real prat = pin->GetReal("problem/blast", "pressure_ratio");
  Real drat = pin->GetOrAddReal("problem/blast", "density_ratio", 1.0);
  const Real gamma = pin->GetOrAddReal("hydro", "gamma", 5. / 3);
  const Real gm1 = gamma - 1.0;
  const Real shvel = pin->GetReal("problem/blast", "shell_velocity") / (units.code_length_cgs() / units.code_time_cgs());

  const auto He_mass_fraction = pin->GetReal("hydro", "He_mass_fraction");
  const auto H_mass_fraction = 1.0 - He_mass_fraction;
  const auto mu = 1 / (He_mass_fraction * 3. / 4. + (1 - He_mass_fraction) * 2);
  const auto mu_m_u_gm1_by_k_B_ = mu * units.atomic_mass_unit() * gm1 / units.k_boltzmann();
  const Real rhoe = ta * da / mu_m_u_gm1_by_k_B_;
  const Real pa = gm1 * rhoe;

  pkg->AddParam<>("temperature_ambient", ta);
  pkg->AddParam<>("pressure_ambient", pa);
  pkg->AddParam<>("density_ambient", da);
  pkg->AddParam<>("pressure_ratio", prat);
  pkg->AddParam<>("density_ratio", drat);
  pkg->AddParam<>("gamma", gamma);
  pkg->AddParam<>("shell_velocity", shvel);

  Real rstar = pin->GetOrAddReal("problem/blast", "radius_star", 0.0) / units.code_length_cgs();
  Real dout = pin->GetOrAddReal("problem/blast", "outflow_density", 0.0) / units.code_density_cgs();;
  Real vout = pin->GetOrAddReal("problem/blast", "outflow_velocity", 0.0) / (units.code_length_cgs() / units.code_time_cgs());

  pkg->AddParam<>("radius_star", rstar);
  pkg->AddParam<>("outflow_density", dout);
  pkg->AddParam<>("outflow_velocity", vout);

  Real rinp = pin->GetOrAddReal("problem/blast", "inner_perturbation", 0.0) / units.code_length_cgs();
  Real routp = pin->GetOrAddReal("problem/blast", "outer_perturbation", 0.0) / units.code_length_cgs();
  const Real denp = pin->GetOrAddReal("problem/blast", "density_perturbation", 0.0) / units.code_density_cgs();

  pkg->AddParam<>("inner_perturbation", rinp);
  pkg->AddParam<>("outer_perturbation", routp);
  pkg->AddParam<>("density_perturbation", denp);

  std::stringstream msg;
  msg << std::setprecision(2);
  msg << "######################################" << std::endl;
  msg << "###### SN problem" << std::endl;
  msg << "#### Input parameters" << std::endl;
  msg << "## Inner perturbation radius: " << 1000 * rinp / units.kpc() << "pc" << std::endl;
  msg << "## Outer perturbation radius: " << 1000 * routp / units.kpc() << "pc" << std::endl;
  msg << "## Star radius: " << 1000 * rstar / units.kpc() << "pc" << std::endl;
  msg << "## Wind density: " << dout / units.g_cm3() << " g/cm^3" << std::endl;
  msg << "## Ambient density: " << da / units.g_cm3() << " g/cm^3" << std::endl;
  msg << "## Perturbation density: " << denp / units.g_cm3() << " g/cm^3" << std::endl;
  msg << "## Ambient temperature: " << ta << " K" << std::endl;
  msg << "## Wind velocity: " << vout / units.km_s() << " km/s" << std::endl;
  msg << "## Shell velocity: " << shvel / units.km_s() << " km/s" << std::endl;
  msg << "#### Derived parameters" << std::endl;
  msg << "## Ambient pressure : " << pa << std::endl;

}

void ProblemGenerator(MeshBlock *pmb, ParameterInput *pin) {  

  auto hydro_pkg = pmb->packages.Get("Hydro");
  Units units(pin);

  const Real da = hydro_pkg->Param<Real>("density_ambient");
  const Real pa = hydro_pkg->Param<Real>("density_ambient");
  Real prat = hydro_pkg->Param<Real>("pressure_ratio");
  Real drat = hydro_pkg->Param<Real>("density_ratio");
  const Real gamma = hydro_pkg->Param<Real>("gamma");
  const Real gm1 = gamma - 1.0;
  const Real sh_vel = hydro_pkg->Param<Real>("shell_velocity");

  Real rinp = hydro_pkg->Param<Real>("inner_perturbation");
  Real routp = hydro_pkg->Param<Real>("outer_perturbation");
  const Real denp = hydro_pkg->Param<Real>("density_perturbation");

  // get coordinates of center of blast, and convert to Cartesian if necessary
  Real x0 = pin->GetOrAddReal("problem/blast", "x1_0", 0.0);
  Real y0 = pin->GetOrAddReal("problem/blast", "x2_0", 0.0);
  Real z0 = pin->GetOrAddReal("problem/blast", "x3_0", 0.0);

  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  // initialize conserved variables
  auto &rc = pmb->meshblock_data.Get();
  auto &u_dev = rc->Get("cons").data;
  auto &coords = pmb->coords;
  // initializing on host
  auto u = u_dev.GetHostMirrorAndCopy();
  ///////auto &u = pmb->meshblock_data.Get()->Get("cons").data;
  // setup uniform ambient medium with spherical over-pressured region
  for (int k = kb.s; k <= kb.e; k++) {
    for (int j = jb.s; j <= jb.e; j++) {
      for (int i = ib.s; i <= ib.e; i++) {
        Real den = da;
        Real x = coords.Xc<1>(i);
        Real y = coords.Xc<2>(j);
        Real z = coords.Xc<3>(k);
        Real ang = atan(y/fabs(x)) ;
        Real fringe = 7;
        Real rad = std::sqrt(SQR(x - x0) + SQR(y - y0) + SQR(z - z0));
        Real mx = 0.0;
        Real my = 0.0;

        if (rad < routp) {
          if (rad > rinp) {
            den = denp * (fabs(sin(fringe * ang))) + da;
            mx = sh_vel * den * x / rad;
            my = sh_vel * den * y / rad;
          }
        }
        u(IDN, k, j, i) = den;
        u(IM1, k, j, i) = mx;
        u(IM2, k, j, i) = my;
        u(IM3, k, j, i) = 0.0;
        u(IEN, k, j, i) = pa/gm1 + 0.5 * (mx * mx + my * my) / den;
      }
    }
  }
  // copy initialized vars to device
  u_dev.DeepCopy(u);
}

void Outflow(MeshData<Real> *md, const parthenon::SimTime, const Real beta_dt) {
  using parthenon::IndexDomain;
  using parthenon::IndexRange;
  using parthenon::Real;
  
  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");

  const Real rstar = hydro_pkg->Param<Real>("radius_star");
  const Real dout = hydro_pkg->Param<Real>("outflow_density");
  const Real pres = hydro_pkg->Param<Real>("pressure_ambient");
  const Real gamma = hydro_pkg->Param<Real>("gamma");
  Real gm1 = gamma - 1.0;
  const Real vout = hydro_pkg->Param<Real>("outflow_velocity");

  const auto &cons_pack = md->PackVariables(std::vector<std::string>{"cons"});
  auto prim_pack = md->PackVariables(std::vector<std::string>{"prim"});
  IndexRange ib = md->GetBlockData(0)->GetBoundsI(IndexDomain::interior);
  IndexRange jb = md->GetBlockData(0)->GetBoundsJ(IndexDomain::interior);
  IndexRange kb = md->GetBlockData(0)->GetBoundsK(IndexDomain::interior);

  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "Outflow", parthenon::DevExecSpace(), 0,
      cons_pack.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int &b, const int &k, const int &j, const int &i) {
        auto &cons = cons_pack(b);
        const auto &coords = cons_pack.GetCoords(b);
        const Real rad =
            sqrt(coords.Xc<1>(i) * coords.Xc<1>(i) + coords.Xc<2>(j) * coords.Xc<2>(j) +
                 coords.Xc<3>(k) * coords.Xc<3>(k));
        
        if (rad < rstar) {
          const Real mout_x = dout * vout * coords.Xc<1>(i) / rad;
          const Real mout_y = dout * vout * coords.Xc<2>(j) / rad;
          cons(IDN, k, j, i) = dout;
          cons(IM1, k, j, i) = mout_x;
          cons(IM2, k, j, i) = mout_y;
          cons(IEN, k, j, i) = pres / gm1 + 0.5*(cons(IM1, k, j, i)*cons(IM1, k, j, i) + cons(IM2, k, j, i)*cons(IM2, k, j, i))/cons(IDN, k, j, i);
        }
      });
}
} // namespace blast