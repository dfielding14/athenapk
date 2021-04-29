#ifndef CLUSTER_CLUSTER_GRAVITY_HPP_
#define CLUSTER_CLUSTER_GRAVITY_HPP_
//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file cluster_gravity.hpp
//  \brief Class for defining gravitational acceleration for a cluster+bcg+smbh

// Parthenon headers
#include <parameter_input.hpp>

// Athena headers
#include "../../physical_constants.hpp"

namespace cluster {

//Types of BCG's
enum class BCG{ NONE,ENZO,MEECE,MATHEWS,HERNQUIST};

/************************************************************
 *  Cluster Gravity Class, for computing gravitational acceleration
 *    Lightweight object for inlined computation within kernels
 ************************************************************/
class ClusterGravity
{

  //Parameters for which gravity sources to include
  bool include_nfw_g_;
  BCG which_bcg_g_;
  bool include_smbh_g_;

  //NFW Parameters
  parthenon::Real R_nfw_s_;
  parthenon::Real GMC_nfw_; //G , Mass, and Constants rolled into one, to minimize footprint

  //BCG Parameters
  parthenon::Real alpha_bcg_s_;
  parthenon::Real beta_bcg_s_;
  parthenon::Real R_bcg_s_;
  parthenon::Real GMC_bcg_; //G , Mass, and Constants rolled into one, to minimize footprint

  //SMBH Parameters
  parthenon::Real GMC_smbh_; //G , Mass, and Constants rolled into one, to minimize footprint

  //Radius underwhich to truncate
  parthenon::Real smoothing_r_;

  // Static Helper functions to calculate constants to minimize in-kernel work
  static parthenon::Real calc_R_nfw_s(const parthenon::Real rho_crit, const parthenon::Real M_nfw_200, const parthenon::Real c_nfw) {
    const parthenon::Real rho_nfw_0 = 200/3.*rho_crit*pow(c_nfw,3.)/(log(1 + c_nfw) - c_nfw/(1 + c_nfw));
    const parthenon::Real R_nfw_s = pow(M_nfw_200/(4*M_PI*rho_nfw_0*(log(1 + c_nfw) - c_nfw/(1 + c_nfw))),1./3.);
    return R_nfw_s;
  }
  static parthenon::Real calc_GMC_nfw(const parthenon::Real gravitational_constant,
                           const parthenon::Real M_nfw_200, const parthenon::Real c_nfw) {
    return gravitational_constant*M_nfw_200/( log(1 + c_nfw) - c_nfw/(1 + c_nfw) );
  }
  static parthenon::Real calc_GMC_bcg(const parthenon::Real gravitational_constant, BCG which_bcg_g,
                           const parthenon::Real M_bcg_s, const parthenon::Real R_bcg_s, 
                           const parthenon::Real alpha_bcg_s, const parthenon::Real beta_bcg_s) {
    switch(which_bcg_g){
      case BCG::NONE:
        return 0;
      case BCG::ENZO:
        return gravitational_constant* M_bcg_s*pow(2,-beta_bcg_s);
      case BCG::MEECE:
        return gravitational_constant*M_bcg_s*pow(2,-beta_bcg_s);
      case BCG::MATHEWS:
        return 1/(R_bcg_s*R_bcg_s);
      case BCG::HERNQUIST:
        return gravitational_constant*M_bcg_s/(R_bcg_s*R_bcg_s);
    }
    return NAN;
  }
  static KOKKOS_INLINE_FUNCTION parthenon::Real calc_GMC_smbh(const parthenon::Real gravitational_constant, 
                                         const parthenon::Real M_smbh) {
    return gravitational_constant*M_smbh;
  }

public:

  ClusterGravity(parthenon::ParameterInput *pin)
  {
    PhysicalConstants constants(pin);

    //Determine which element to include
    include_nfw_g_    = pin->GetOrAddBoolean("problem","include_nfw_g",false);
    const std::string which_bcg_g_str = pin->GetOrAddString("problem","which_bcg_g","NONE");
    if(which_bcg_g_str == "NONE"){
      which_bcg_g_ = BCG::NONE;
    } else if( which_bcg_g_str == "ENZO"){
      which_bcg_g_ = BCG::ENZO;
    } else if( which_bcg_g_str == "MEECE"){
      which_bcg_g_ = BCG::MEECE;
    } else if( which_bcg_g_str == "MATHEWS"){
      which_bcg_g_ = BCG::MATHEWS;
    } else if( which_bcg_g_str == "HERNQUIST"){
      which_bcg_g_ = BCG::HERNQUIST;
    } else {
      std::stringstream msg;
      msg << "### FATAL ERROR in function [InitUserMeshData]" << std::endl 
          << "Unknown BCG type "<< which_bcg_g_str << std::endl;
      throw std::runtime_error(msg.str().c_str());
    }

    include_smbh_g_   = pin->GetOrAddBoolean("problem","include_smbh_g",false);

    //Initialize the NFW Profile
    const parthenon::Real hubble_parameter = pin->GetOrAddReal("problem", "hubble_parameter",70*constants.km_s()/constants.mpc());
    const parthenon::Real rho_crit = 3*hubble_parameter*hubble_parameter/(8*M_PI*constants.gravitational_constant());

    const parthenon::Real M_nfw_200        = pin->GetOrAddReal("problem", "M_nfw_200"  ,8.5e14*constants.msun());
    const parthenon::Real c_nfw            = pin->GetOrAddReal("problem", "c_nfw"      ,6.81);
    R_nfw_s_ = calc_R_nfw_s(rho_crit,M_nfw_200,c_nfw);
    GMC_nfw_ = calc_GMC_nfw(constants.gravitational_constant(),M_nfw_200,c_nfw);

    //Initialize the NFW Profile
    alpha_bcg_s_      = pin->GetOrAddReal("problem", "alpha_bcg_s",0.1);
    beta_bcg_s_       = pin->GetOrAddReal("problem", "beta_bcg_s" ,1.43);
    const parthenon::Real M_bcg_s          = pin->GetOrAddReal("problem", "M_bcg_s"    ,7.5e10*constants.msun());
    R_bcg_s_          = pin->GetOrAddReal("problem", "R_bcg_s"    ,4*constants.kpc());
    GMC_bcg_ = calc_GMC_bcg(constants.gravitational_constant(),
        which_bcg_g_,M_bcg_s,R_bcg_s_,alpha_bcg_s_,beta_bcg_s_);

    const parthenon::Real m_smbh           = pin->GetOrAddReal("problem", "m_smbh"     ,3.4e8*constants.msun());
    GMC_smbh_  = calc_GMC_smbh(constants.gravitational_constant(),m_smbh),

    smoothing_r_ = pin->GetOrAddReal("problem","g_smoothing_radius",0.0);
  }

  //Inline functions to compute gravitational acceleration
  KOKKOS_INLINE_FUNCTION parthenon::Real g_from_r(const parthenon::Real r) const 
    __attribute__((always_inline)){
    return g_from_r(r,r*r);
  }

  KOKKOS_INLINE_FUNCTION parthenon::Real g_from_r(const parthenon::Real r_in, const parthenon::Real r2_in) const 
    __attribute__((always_inline)){

    const parthenon::Real r2 = std::max(r2_in,smoothing_r_*smoothing_r_);
    const parthenon::Real r  = std::max(r_in,smoothing_r_);

    parthenon::Real g_r = 0;

    //Add NFW gravity
    if( include_nfw_g_){
      g_r += GMC_nfw_*( log(1 + r/R_nfw_s_) - r/(r + R_nfw_s_) )/r2;
    }

    //Add BCG gravity
    switch(which_bcg_g_){
      case BCG::NONE:
        break;
      case BCG::ENZO:
        g_r += GMC_bcg_/
          ( r2*pow(r/R_bcg_s_,-alpha_bcg_s_)*pow( 1 + r/R_bcg_s_,beta_bcg_s_ - alpha_bcg_s_));
        break;
      case BCG::MEECE:
        g_r += GMC_bcg_/
          ( r2*pow(r/R_bcg_s_,-alpha_bcg_s_)*pow( 1 + r/R_bcg_s_,beta_bcg_s_ - alpha_bcg_s_));
        break;
      case BCG::MATHEWS:
        {
          const parthenon::Real s_bcg = 0.9;
          g_r += GMC_bcg_* //Note: *cm**3*s**-2 //To make units work
            pow(pow(r/R_bcg_s_,0.5975/3.206e-7*s_bcg) + pow(pow(r/R_bcg_s_,1.849/1.861e-6),s_bcg ),-1/s_bcg);
        }
        break;
      case BCG::HERNQUIST:
        g_r += GMC_bcg_/( ( 1 + r/R_bcg_s_)*( 1 + r/R_bcg_s_) );
        break;
    }

    //Add SMBH, point mass gravity
    if( include_smbh_g_){
      g_r += GMC_smbh_/r2;
    }

    return g_r;
  }
};

} // namespace cluster

#endif // CLUSTER_CLUSTER_GRAVITY_HPP_


