//========================================================================================
// AthenaPK - a performance portable block structured AMR astrophysical MHD code.
// Copyright (c) 2021, Athena-Parthenon Collaboration. All rights reserved.
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file cluster.cpp
//  \brief Idealized galaxy cluster problem generator
//
// Setups up an idealized galaxy cluster with an ACCEPT-like entropy profile in
// hydrostatic equilbrium with an NFW+BCG+SMBH gravitational profile,
// optionally with an initial magnetic tower field. Includes AGN feedback, AGN
// triggering via cold gas, simple SNIA Feedback
//========================================================================================

// C headers

// C++ headers
#include <algorithm> // min, max
#include <cmath>     // sqrt()
#include <cstdio>    // fopen(), fprintf(), freopen()
#include <iostream>  // endl
#include <limits>
#include <sstream>   // stringstream
#include <stdexcept> // runtime_error
#include <string>    // c_str()

// Parthenon headers
#include <mesh/domain.hpp>
#include <mesh/mesh.hpp>
#include <parthenon/driver.hpp>
#include <parthenon/package.hpp>

// AthenaPK headers
#include "../hydro/hydro.hpp"
#include "../hydro/srcterms/gravitational_field.hpp"
#include "../main.hpp"
#include "../units.hpp"

// Cluster headers
#include "cluster/cluster_gravity.hpp"
#include "cluster/entropy_profiles.hpp"
#include "cluster/hydro_agn_feedback.hpp"
#include "cluster/hydrostatic_equilibrium_sphere.hpp"
#include "cluster/magnetic_tower.hpp"

namespace cluster {
using namespace parthenon;
using namespace parthenon::driver::prelude;
using namespace parthenon::package::prelude;

void ClusterSrcTerm(MeshData<Real> *md, const Real beta_dt,
                    const parthenon::SimTime &tm) {
  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");

  if (hydro_pkg->Param<bool>("gravity_srcterm")) {
    const ClusterGravity &cluster_gravity =
        hydro_pkg->Param<ClusterGravity>("cluster_gravity");

    GravitationalFieldSrcTerm(md, beta_dt, cluster_gravity);
  }

  //Adds magnetic tower feedback as an unsplit term
  //if (hydro_pkg->Param<bool>("enable_feedback_magnetic_tower")) {
  //  const MagneticTower &magnetic_tower =
  //      hydro_pkg->Param<MagneticTower>("feedback_magnetic_tower");
  //  magnetic_tower.MagneticFieldSrcTerm(md, beta_dt, tm);
  //}

  if (hydro_pkg->Param<bool>("enable_hydro_agn_feedback")) {
    const HydroAGNFeedback &hydro_agn_feedback =
        hydro_pkg->Param<HydroAGNFeedback>("hydro_agn_feedback");

    hydro_agn_feedback.FeedbackSrcTerm(md, beta_dt, tm);
  }
}

void ClusterFirstOrderSrcTerm(MeshData<Real> *md, const parthenon::SimTime &tm) {
  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");

  // if( hydro_pkg->Param<bool>("enable_hydro_agn_feedback") ){
  //  const HydroAGNFeedback& hydro_agn_feedback =
  //    hydro_pkg->Param<HydroAGNFeedback>("hydro_agn_feedback");

  //    hydro_agn_feedback.FeedbackSrcTerm(md, tm.dt, tm);
  //}

  //Adds magnetic tower feedback as a first order term
  if (hydro_pkg->Param<bool>("enable_feedback_magnetic_tower")) {
    const MagneticTower &magnetic_tower =
        hydro_pkg->Param<MagneticTower>("feedback_magnetic_tower");
    magnetic_tower.MagneticFieldSrcTerm(md, tm.dt, tm);
  }
}

Real ClusterEstimateTimestep(MeshData<Real> *md) {
  Real min_dt = std::numeric_limits<Real>::max();

  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");

  return min_dt;
}

//========================================================================================
//! \fn void InitUserMeshData(ParameterInput *pin)
//  \brief Function to initialize problem-specific data in mesh class.  Can also be used
//  to initialize variables which are global to (and therefore can be passed to) other
//  functions in this file.  Called in Mesh constructor.
//========================================================================================

void ProblemGenerator(MeshBlock *pmb, parthenon::ParameterInput *pin) {
  auto hydro_pkg = pmb->packages.Get("Hydro");
  if (pmb->lid == 0) {

    /************************************************************
     * Read Unit Parameters
     ************************************************************/
    // CGS unit per code unit, or code unit in cgs
    Units units(pin, hydro_pkg);
    hydro_pkg->AddParam<>("units", units);

    /************************************************************
     * Read Uniform Gas
     ************************************************************/

    const bool init_uniform_gas =
        pin->GetOrAddBoolean("problem/cluster", "init_uniform_gas", false);
    hydro_pkg->AddParam<>("init_uniform_gas", init_uniform_gas);

    if (init_uniform_gas) {
      const Real uniform_gas_rho = pin->GetReal("problem/cluster", "uniform_gas_rho");
      const Real uniform_gas_ux = pin->GetReal("problem/cluster", "uniform_gas_ux");
      const Real uniform_gas_uy = pin->GetReal("problem/cluster", "uniform_gas_uy");
      const Real uniform_gas_uz = pin->GetReal("problem/cluster", "uniform_gas_uz");
      const Real uniform_gas_pres = pin->GetReal("problem/cluster", "uniform_gas_pres");

      hydro_pkg->AddParam<>("uniform_gas_rho", uniform_gas_rho);
      hydro_pkg->AddParam<>("uniform_gas_ux", uniform_gas_ux);
      hydro_pkg->AddParam<>("uniform_gas_uy", uniform_gas_uy);
      hydro_pkg->AddParam<>("uniform_gas_uz", uniform_gas_uz);
      hydro_pkg->AddParam<>("uniform_gas_pres", uniform_gas_pres);
    }

    /************************************************************
     * Read Cluster Gravity Parameters
     ************************************************************/

    // Include gravity as a source term during evolution
    const bool gravity_srcterm = pin->GetBoolean("problem/cluster", "gravity_srcterm");
    hydro_pkg->AddParam<>("gravity_srcterm", gravity_srcterm);

    // TODO(forrestglines):If not uniform gas or gravity_srcterm?
    if (!hydro_pkg->Param<bool>("init_uniform_gas") ||
        hydro_pkg->Param<bool>("gravity_srcterm")) {
      // Build cluster_gravity object
      ClusterGravity cluster_gravity(pin);
      hydro_pkg->AddParam<>("cluster_gravity", cluster_gravity);
    }

    /************************************************************
     * Read Initial Entropy Profile
     ************************************************************/

    if (!hydro_pkg->Param<bool>("init_uniform_gas")) {
      // Build entropy_profile object
    }

    /************************************************************
     * Build Hydrostatic Equilibrium Sphere
     ************************************************************/

    if (!hydro_pkg->Param<bool>("init_uniform_gas")) {

      const ClusterGravity &cluster_gravity =
          hydro_pkg->Param<ClusterGravity>("cluster_gravity");
      ACCEPTEntropyProfile entropy_profile(pin);

      HydrostaticEquilibriumSphere hse_sphere(pin, cluster_gravity, entropy_profile);
      hydro_pkg->AddParam<>("hydrostatic_equilibirum_sphere", hse_sphere);
    }

    /************************************************************
     * Read Initial Magnetic Tower
     ************************************************************/

    // Build Initial Magnetic Tower object
    const bool enable_initial_magnetic_tower =
        pin->GetOrAddBoolean("problem/cluster", "enable_initial_magnetic_tower", false);
    hydro_pkg->AddParam<>("enable_initial_magnetic_tower", enable_initial_magnetic_tower);

    if (hydro_pkg->Param<bool>("enable_initial_magnetic_tower")) {
      if (hydro_pkg->Param<Fluid>("fluid") != Fluid::glmmhd) {
        PARTHENON_FAIL("cluster::ProblemGenerator: Magnetic fields required for initial "
                       "magnetic tower");
      }
      // Build Initial Magnetic Tower object
      InitInitialMagneticTower(hydro_pkg, pin);
    }

    /************************************************************
     * Read Magnetic Tower Feedback
     ************************************************************/

    const bool enable_feedback_magnetic_tower =
        pin->GetOrAddBoolean("problem/cluster", "enable_feedback_magnetic_tower", false);
    hydro_pkg->AddParam<>("enable_feedback_magnetic_tower",
                          enable_feedback_magnetic_tower);

    if (hydro_pkg->Param<bool>("enable_feedback_magnetic_tower")) {
      if (hydro_pkg->Param<Fluid>("fluid") != Fluid::glmmhd) {
        PARTHENON_FAIL("cluster::ProblemGenerator: Magnetic fields required for magnetic "
                       "tower feedback");
      }
      // Build Feedback Magnetic Tower object
      InitFeedbackMagneticTower(hydro_pkg, pin);
    }

    /************************************************************
     * Read Hydro AGN Feedback
     ************************************************************/

    const bool enable_hydro_agn_feedback =
        pin->GetOrAddBoolean("problem/cluster", "enable_hydro_agn_feedback", false);
    hydro_pkg->AddParam<>("enable_hydro_agn_feedback", enable_hydro_agn_feedback);

    if (hydro_pkg->Param<bool>("enable_hydro_agn_feedback")) {
      // Build Feedback Magnetic Tower object
      HydroAGNFeedback hydro_agn_feedback(pin);
      hydro_pkg->AddParam<>("hydro_agn_feedback", hydro_agn_feedback);
    }

  }

  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  // Initialize the conserved variables
  auto &u = pmb->meshblock_data.Get()->Get("cons").data;

  auto &coords = pmb->coords;

  // Get Adiabatic Index
  const Real gam = pin->GetReal("hydro", "gamma");
  const Real gm1 = (gam - 1.0);

  /************************************************************
   * Initialize the initial hydro state
   ************************************************************/
  if (hydro_pkg->Param<bool>("init_uniform_gas")) {
    /************************************************************
     * Initialize with a uniform gas
     ************************************************************/
    const Real rho = hydro_pkg->Param<Real>("uniform_gas_rho");
    const Real ux = hydro_pkg->Param<Real>("uniform_gas_ux");
    const Real uy = hydro_pkg->Param<Real>("uniform_gas_uy");
    const Real uz = hydro_pkg->Param<Real>("uniform_gas_uz");
    const Real pres = hydro_pkg->Param<Real>("uniform_gas_pres");

    const Real Mx = rho * ux;
    const Real My = rho * uy;
    const Real Mz = rho * uz;
    const Real E = rho * (0.5 * (ux * uy + uy * uy + uz * uz) + pres / (gm1 * rho));

    parthenon::par_for(
        DEFAULT_LOOP_PATTERN, "Cluster::ProblemGenerator::UniformGas",
        parthenon::DevExecSpace(), kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
        KOKKOS_LAMBDA(const int &k, const int &j, const int &i) {
          u(IDN, k, j, i) = rho;
          u(IM1, k, j, i) = Mx;
          u(IM2, k, j, i) = My;
          u(IM3, k, j, i) = Mz;
          u(IEN, k, j, i) = E;
        });

  } else {
    /************************************************************
     * Initialize a HydrostaticEquilibriumSphere
     ************************************************************/
    const auto &he_sphere =
        hydro_pkg
            ->Param<HydrostaticEquilibriumSphere<ClusterGravity, ACCEPTEntropyProfile>>(
                "hydrostatic_equilibirum_sphere");

    const auto P_rho_profile = he_sphere.generate_P_rho_profile<
        Kokkos::View<parthenon::Real *, parthenon::LayoutWrapper,
                     parthenon::HostMemSpace>,
        parthenon::UniformCartesian>(ib, jb, kb, coords);

    // initialize conserved variables
    parthenon::par_for(
        DEFAULT_LOOP_PATTERN, "cluster::ProblemGenerator::UniformGas",
        parthenon::DevExecSpace(), kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
        KOKKOS_LAMBDA(const int &k, const int &j, const int &i) {
          // Calculate radius
          const Real r =
              sqrt(coords.x1v(i) * coords.x1v(i) + coords.x2v(j) * coords.x2v(j) +
                   coords.x3v(k) * coords.x3v(k));

          // Get pressure and density from generated profile
          const Real P_r = P_rho_profile.P_from_r(r);
          const Real rho_r = P_rho_profile.rho_from_r(r);

          // Fill conserved states, 0 initial velocity
          u(IDN, k, j, i) = rho_r;
          u(IM1, k, j, i) = 0.0;
          u(IM2, k, j, i) = 0.0;
          u(IM3, k, j, i) = 0.0;
          u(IEN, k, j, i) = P_r / gm1;
        });
  }

  if (hydro_pkg->Param<Fluid>("fluid") == Fluid::glmmhd) {
    /************************************************************
     * Initialize the initial magnetic field state via a vector potential
     ************************************************************/
    ParArray3D<Real> a_x("a_x", pmb->cellbounds.ncellsk(IndexDomain::entire),
                         pmb->cellbounds.ncellsj(IndexDomain::entire),
                         pmb->cellbounds.ncellsi(IndexDomain::entire));
    ParArray3D<Real> a_y("a_y", pmb->cellbounds.ncellsk(IndexDomain::entire),
                         pmb->cellbounds.ncellsj(IndexDomain::entire),
                         pmb->cellbounds.ncellsi(IndexDomain::entire));
    ParArray3D<Real> a_z("a_z", pmb->cellbounds.ncellsk(IndexDomain::entire),
                         pmb->cellbounds.ncellsj(IndexDomain::entire),
                         pmb->cellbounds.ncellsi(IndexDomain::entire));
    IndexRange a_ib = ib;
    a_ib.s -= 1;
    a_ib.e += 1;
    IndexRange a_jb = jb;
    a_jb.s -= 1;
    a_jb.e += 1;
    IndexRange a_kb = kb;
    a_kb.s -= 1;
    a_kb.e += 1;

    if (hydro_pkg->Param<Fluid>("fluid") == Fluid::glmmhd &&
        hydro_pkg->Param<bool>("enable_initial_magnetic_tower")) {
      /************************************************************
       * Initialize an initial magnetic tower
       ************************************************************/
      const auto &magnetic_tower =
          hydro_pkg->Param<MagneticTower>("initial_magnetic_tower");

      magnetic_tower.AddPotential(pmb, a_kb, a_jb, a_ib, a_x, a_y, a_z, 0);
      //magnetic_tower.AddField(pmb, kb, jb, ib, u, 0);
    }

    /************************************************************
     * Apply the potential to the conserved variables
     ************************************************************/
    parthenon::par_for(
        DEFAULT_LOOP_PATTERN, "cluster::ProblemGenerator::ApplyMagneticPotential",
        parthenon::DevExecSpace(), kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
        KOKKOS_LAMBDA(const int &k, const int &j, const int &i) {
          u(IB1, k, j, i) = (a_z(k, j + 1, i) - a_z(k, j - 1, i)) / coords.dx2v(j) / 2.0 -
                            (a_y(k + 1, j, i) - a_y(k - 1, j, i)) / coords.dx3v(k) / 2.0;
          u(IB2, k, j, i) = (a_x(k + 1, j, i) - a_x(k - 1, j, i)) / coords.dx3v(k) / 2.0 -
                            (a_z(k, j, i + 1) - a_z(k, j, i - 1)) / coords.dx1v(i) / 2.0;
          u(IB3, k, j, i) = (a_y(k, j, i + 1) - a_y(k, j, i - 1)) / coords.dx1v(i) / 2.0 -
                            (a_x(k, j + 1, i) - a_x(k, j - 1, i)) / coords.dx2v(j) / 2.0;

          u(IEN, k, j, i) +=
              0.5 * (SQR(u(IB1, k, j, i)) + SQR(u(IB2, k, j, i)) + SQR(u(IB3, k, j, i)));
        });
  }  //END
}

} // namespace cluster
