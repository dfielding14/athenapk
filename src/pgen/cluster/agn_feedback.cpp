//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file magnetic_tower.hpp
//  \brief Class for defining magnetic tower

// Parthenon headers
#include <coordinates/uniform_cartesian.hpp>
#include <globals.hpp>
#include <interface/state_descriptor.hpp>
#include <mesh/domain.hpp>
#include <parameter_input.hpp>
#include <parthenon/package.hpp>

// Athena headers
#include "../../eos/adiabatic_glmmhd.hpp"
#include "../../eos/adiabatic_hydro.hpp"
#include "../../main.hpp"
#include "../../units.hpp"
#include "agn_feedback.hpp"
#include "agn_triggering.hpp"
#include "cluster_utils.hpp"
#include "magnetic_tower.hpp"

namespace cluster {
using namespace parthenon;

AGNFeedback::AGNFeedback(parthenon::ParameterInput *pin,
                         const std::shared_ptr<parthenon::StateDescriptor> &hydro_pkg)
    : fixed_power_(pin->GetOrAddReal("problem/cluster/agn_feedback", "fixed_power", 0.0)),
      efficiency_(pin->GetOrAddReal("problem/cluster/agn_feedback", "efficiency", 1e-3)),
      thermal_fraction_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "thermal_fraction", 0.0)),
      kinetic_fraction_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "kinetic_fraction", 0.0)),
      magnetic_fraction_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "magnetic_fraction", 0.0)),
      thermal_radius_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "thermal_radius", 0.01)),
      kinetic_jet_radius_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "kinetic_jet_radius", 0.01)),
      kinetic_jet_height_(
          pin->GetOrAddReal("problem/cluster/agn_feedback", "kinetic_jet_height", 0.02)),
      disabled_(pin->GetOrAddBoolean("problem/cluster/agn_feedback", "disabled", false)) {

  hydro_pkg->AddParam<>("agn_feedback", *this);
}

void AGNFeedback::FeedbackSrcTerm(parthenon::MeshData<parthenon::Real> *md,
                                  const parthenon::Real beta_dt,
                                  const parthenon::SimTime &tm) const {
  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");
  auto fluid = hydro_pkg->Param<Fluid>("fluid");
  if (fluid == Fluid::euler) {
    FeedbackSrcTerm(md, beta_dt, tm, hydro_pkg->Param<AdiabaticHydroEOS>("eos"));
  } else if (fluid == Fluid::glmmhd) {
    FeedbackSrcTerm(md, beta_dt, tm, hydro_pkg->Param<AdiabaticGLMMHDEOS>("eos"));
  } else {
    PARTHENON_FAIL("AGNFeedback::FeedbackSrcTerm: Unknown EOS");
  }
}
template <typename EOS>
void AGNFeedback::FeedbackSrcTerm(parthenon::MeshData<parthenon::Real> *md,
                                  const parthenon::Real beta_dt,
                                  const parthenon::SimTime &tm, const EOS &eos) const {
  using parthenon::IndexDomain;
  using parthenon::IndexRange;
  using parthenon::Real;

  auto hydro_pkg = md->GetBlockData(0)->GetBlockPointer()->packages.Get("Hydro");
  auto units = hydro_pkg->Param<Units>("units");
  const auto &agn_triggering = hydro_pkg->Param<AGNTriggering>("agn_triggering");

  const Real accretion_rate = agn_triggering.GetAccretionRate(hydro_pkg.get());
  const Real power =
      fixed_power_ + accretion_rate * efficiency_ * pow(units.speed_of_light(), 2);
  const Real mass_rate =
      efficiency_ == 0 ? 0 : power / (efficiency_ * pow(units.speed_of_light(), 2));

  if (power == 0 || disabled_) {
    // No AGN feedback, return
    return;
  }

  if (magnetic_fraction_ == 0 && thermal_fraction_ == 0 && kinetic_fraction_ == 0) {
    PARTHENON_FAIL("AGNFeedback::FeedbackSrcTerm Magnetic, Thermal, and Kinetic "
                   "fractions are all zero");
  }

  // Grab some necessary variables
  const auto &prim_pack = md->PackVariables(std::vector<std::string>{"prim"});
  const auto &cons_pack = md->PackVariables(std::vector<std::string>{"cons"});
  IndexRange ib = cons_pack.cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = cons_pack.cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = cons_pack.cellbounds.GetBoundsK(IndexDomain::interior);

  // Thermal quantities
  const Real thermal_power = power * thermal_fraction_;
  const Real thermal_scaling_factor = 1 / (4. / 3. * M_PI * pow(thermal_radius_, 3));
  const Real thermal_feedback =
      thermal_power * thermal_scaling_factor * beta_dt; // energy/volume
  const Real thermal_density_rate =
      mass_rate * thermal_scaling_factor * thermal_fraction_;
  const Real thermal_radius2 = thermal_radius_ * thermal_radius_;

  // Kinetic Jet Quantities
  const Real kinetic_power = power * kinetic_fraction_;
  const Real kinetic_scaling_factor =
      1 / (2 * kinetic_jet_height_ * M_PI * pow(kinetic_jet_radius_, 2));
  // Matches 1/2.*jet_density*jet_velocity*jet_velocity*beta_dt;
  const Real kinetic_feedback =
      kinetic_power * kinetic_scaling_factor * beta_dt; // energy/volume

  // Note that new mass is injected to create the kinetic power, separate from the
  // existing gas
  const Real kinetic_jet_total_mass = mass_rate * kinetic_fraction_;
  const Real kinetic_jet_density_rate = kinetic_jet_total_mass * kinetic_scaling_factor;
  const Real kinetic_jet_velocity =
      std::sqrt(2. * kinetic_power / kinetic_jet_total_mass);

  const Real kinetic_jet_radius = kinetic_jet_radius_;
  const Real kinetic_jet_height = kinetic_jet_height_;

  const parthenon::Real time = tm.time;

  const auto &jet_coords_factory =
      hydro_pkg->Param<JetCoordsFactory>("jet_coords_factory");
  const JetCoords jet_coords = jet_coords_factory.CreateJetCoords(time);

  // Constant volumetric heating
  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "HydroAGNFeedback::FeedbackSrcTerm",
      parthenon::DevExecSpace(), 0, cons_pack.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s,
      ib.e, KOKKOS_LAMBDA(const int &b, const int &k, const int &j, const int &i) {
        auto &cons = cons_pack(b);
        auto &prim = prim_pack(b);
        const auto &coords = cons_pack.coords(b);

        const Real x = coords.x1v(i);
        const Real y = coords.x2v(j);
        const Real z = coords.x3v(k);

        // Thermal Feedback
        if (thermal_power > 0) {
          const Real r2 = x * x + y * y + z * z;
          // Determine if point is in sphere r<=thermal_radius
          if (r2 <= thermal_radius2) {
            // Apply heating
            cons(IEN, k, j, i) += thermal_feedback;
            // Add density at constant velocity
            AddDensityToConsAtFixedVel(thermal_density_rate, cons, prim, eos, k, j, i);
          }
        }

        // Kinetic Jet Feedback
        if (kinetic_power > 0) {
          // Get position in jet cylindrical coords
          Real r, cos_theta, sin_theta, h;
          jet_coords.SimCartToJetCylCoords(x, y, z, r, cos_theta, sin_theta, h);

          if (r < kinetic_jet_radius && fabs(h) < kinetic_jet_height) {
            // Cell falls inside jet deposition volume

            // Get the vector of the jet axis
            Real jet_axis_x, jet_axis_y, jet_axis_z;
            jet_coords.JetCylToSimCartVector(cos_theta, sin_theta, 0, 0, 1, jet_axis_x,
                                             jet_axis_y, jet_axis_z);

            const int sign_jet = (h > 0) ? 1 : -1; // Above or below jet-disk

            cons(IDN, k, j, i) += kinetic_jet_density_rate * beta_dt; // mass/volume
            cons(IM1, k, j, i) += kinetic_jet_density_rate * sign_jet * jet_axis_x *
                                  kinetic_jet_velocity * beta_dt; // velocity*mass/volume
            cons(IM2, k, j, i) += kinetic_jet_density_rate * sign_jet * jet_axis_y *
                                  kinetic_jet_velocity * beta_dt; // velocity*mass/volume
            cons(IM3, k, j, i) += kinetic_jet_density_rate * sign_jet * jet_axis_z *
                                  kinetic_jet_velocity * beta_dt; // velocity*mass/volume
            cons(IEN, k, j, i) += kinetic_feedback;               // energy/volume
          }
        }
      });

  // Apply magnetic tower feedback
  const auto &magnetic_tower = hydro_pkg->Param<MagneticTower>("magnetic_tower");

  const Real magnetic_power = power * magnetic_fraction_;
  const Real magnetic_mass_rate = mass_rate * magnetic_fraction_;
  magnetic_tower.PowerSrcTerm(magnetic_power, magnetic_mass_rate, md, beta_dt, tm);
}

} // namespace cluster