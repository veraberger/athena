//========================================================================================
// Athena++ astrophysical MHD code
// Copyright(C) 2014 James M. Stone <jmstone@princeton.edu> and other code contributors
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
//! \file calculate_fluxes.cpp
//  \brief Calculate hydro/MHD fluxes

// C headers

// C++ headers
#include <algorithm>   // min,max

// Athena++ headers
#include "../athena.hpp"
#include "../athena_arrays.hpp"
#include "../coordinates/coordinates.hpp"
#include "../eos/eos.hpp"   // reapply floors to face-centered reconstructed states
#include "../hydro/hydro.hpp"
#include "../reconstruct/reconstruction.hpp"
#include "scalars.hpp"

// OpenMP header
#ifdef OPENMP_PARALLEL
#include <omp.h>
#endif

//----------------------------------------------------------------------------------------
//! \fn  void PassiveScalars::CalculateFluxes
//  \brief Calculate passive scalar fluxes using reconstruction + weighted upwinding rule

void PassiveScalars::CalculateFluxes(AthenaArray<Real> &s, const int order) {
  MeshBlock *pmb = pmy_block;

  // design decision: do not pass Hydro::flux (for mass flux) via function parameters,
  // since 1) it is unlikely that anything else would be passed, 2) the current
  // PassiveScalars class/feature implementation is inherently coupled to Hydro class
  // 3) high-order calculation of scalar fluxes will require other Hydro flux
  // approximations (flux_fc in calculate_fluxes.cpp is currently not saved persistently
  // in Hydro class but each flux dir is temp. stored in 4D scratch array scr1_nkji_)

  // (moved to ComputeUpwindFlux() definition:)--- REVERT
  Hydro &hyd = *(pmb->phydro);

  AthenaArray<Real> &x1flux = s_flux[X1DIR];
  AthenaArray<Real> mass_flux;
  mass_flux.InitWithShallowSlice(hyd.flux[X1DIR], 4, IDN, 1);
  int is = pmb->is; int js = pmb->js; int ks = pmb->ks;
  int ie = pmb->ie; int je = pmb->je; int ke = pmb->ke;
  int il, iu, jl, ju, kl, ku;

  AthenaArray<Real> &flux_fc = scr1_nkji_;
  AthenaArray<Real> &laplacian_all_fc = scr2_nkji_;

  //--------------------------------------------------------------------------------------
  // i-direction

  // set the loop limits
  jl=js, ju=je, kl=ks, ku=ke;
  // TODO(felker): fix loop limits for fourth-order hydro
  //  if (MAGNETIC_FIELDS_ENABLED) {
  if (pmb->block_size.nx2 > 1) {
    if (pmb->block_size.nx3 == 1) // 2D
      jl=js-1, ju=je+1, kl=ks, ku=ke;
    else // 3D
      jl=js-1, ju=je+1, kl=ks-1, ku=ke+1;
  }
  //  }

  for (int k=kl; k<=ku; ++k) {
    for (int j=jl; j<=ju; ++j) {
      // reconstruct L/R states
      if (order == 1) {
        pmb->precon->DonorCellX1(k, j, is-1, ie+1, s, sl_, sr_);
      } else if (order == 2) {
        pmb->precon->PiecewiseLinearX1(k, j, is-1, ie+1, s, sl_, sr_);
      } else {
        pmb->precon->PiecewiseParabolicX1(k, j, is-1, ie+1, s, sl_, sr_);
      }

      pmb->pcoord->CenterWidth1(k, j, is, ie+1, dxw_);

      ComputeUpwindFlux(k, j, is, ie+1, // CoordinateDirection::X1DIR,
                        sl_, sr_, mass_flux, x1flux);

      if (order == 4) {
        for (int n=0; n<NSCALARS; n++) {
          for (int i=is; i<=ie+1; i++) {
            sl3d_(n,k,j,i) = sl_(n,i);
            sr3d_(n,k,j,i) = sr_(n,i);
          }
        }
      }
    }
  }

  if (order == 4) {
    // TODO(felker): assuming uniform mesh with dx1f=dx2f=dx3f, so this should factor out
    // TODO(felker): also, this may need to be dx1v, since Laplacian is cell-centered
    Real h = pmb->pcoord->dx1f(is);  // pco->dx1f(i); inside loop
    Real C = (h*h)/24.0;

    // construct Laplacian from x1flux
    pmb->pcoord->LaplacianX1All(x1flux, laplacian_all_fc, 0, NSCALARS-1,
                                kl, ku, jl, ju, is, ie+1);

    for (int k=kl; k<=ku; ++k) {
      for (int j=jl; j<=ju; ++j) {
        // Compute Laplacian of x1 face states
        for (int n=0; n<NSCALARS; ++n) {
          pmb->pcoord->LaplacianX1(sl3d_, laplacian_l_fc_, n, k, j, is, ie+1);
          pmb->pcoord->LaplacianX1(sr3d_, laplacian_r_fc_, n, k, j, is, ie+1);
#pragma omp simd
          for (int i=is; i<=ie+1; ++i) {
            sl_(n,i) = sl3d_(n,k,j,i) - C*laplacian_l_fc_(i);
            sr_(n,i) = sr3d_(n,k,j,i) - C*laplacian_r_fc_(i);
          }
        }
#pragma omp simd
        for (int i=is; i<=ie+1; ++i) {
          // pmb->peos->ApplyPrimitiveFloors(sl_, k, j, i);
          // pmb->peos->ApplyPrimitiveFloors(sr_, k, j, i);
        }

        // Compute x1 interface fluxes from face-centered primitive variables
        // TODO(felker): check that e3x1,e2x1 arguments added in late 2017 work here
        pmb->pcoord->CenterWidth1(k, j, is, ie+1, dxw_);
        // ComputeUpwindFlux(k, j, il, iu, sl_, sr_, Hydro::x1flux_fc, flux_fc);

        // Apply Laplacian of second-order accurate face-averaged flux on x1 faces
        for (int n=0; n<NSCALARS; ++n) {
#pragma omp simd
          for (int i=is; i<=ie+1; i++)
            x1flux(n,k,j,i) = flux_fc(n,k,j,i) + C*laplacian_all_fc(n,k,j,i);
        }
      }
    }
  } // end if (order == 4)
  //------------------------------------------------------------------------------
  // end x1 fourth-order hydro

  //--------------------------------------------------------------------------------------
  // j-direction

  if (pmb->block_size.nx2 > 1) {
    AthenaArray<Real> &x2flux = s_flux[X2DIR];
    mass_flux.InitWithShallowSlice(hyd.flux[X2DIR], 4, IDN, 1);

    // set the loop limits
    il=is-1, iu=ie+1, kl=ks, ku=ke;
    // TODO(felker): fix loop limits for fourth-order hydro
    //    if (MAGNETIC_FIELDS_ENABLED) {
    if (pmb->block_size.nx3 == 1) // 2D
      kl=ks, ku=ke;
    else // 3D
      kl=ks-1, ku=ke+1;
    //    }

    for (int k=kl; k<=ku; ++k) {
      // reconstruct the first row
      if (order == 1) {
        pmb->precon->DonorCellX2(k, js-1, il, iu, s, sl_, sr_);
      } else if (order == 2) {
        pmb->precon->PiecewiseLinearX2(k, js-1, il, iu, s, sl_, sr_);
      } else {
        pmb->precon->PiecewiseParabolicX2(k, js-1, il, iu, s, sl_, sr_);
      }
      for (int j=js; j<=je+1; ++j) {
        // reconstruct L/R states at j
        if (order == 1) {
          pmb->precon->DonorCellX2(k, j, il, iu, s, slb_, sr_);
        } else if (order == 2) {
          pmb->precon->PiecewiseLinearX2(k, j, il, iu, s, slb_, sr_);
        } else {
          pmb->precon->PiecewiseParabolicX2(k, j, il, iu, s, slb_, sr_);
        }

        // flx(IBY) = (v2*b3 - v3*b2) = -EMFX
        // flx(IBZ) = (v2*b1 - v1*b2) =  EMFZ
        pmb->pcoord->CenterWidth2(k, j, il, iu, dxw_);
        ComputeUpwindFlux(k, j, il, iu, sl_, sr_, mass_flux, x2flux);

        if (order == 4) {
          for (int n=0; n<NSCALARS; n++) {
            for (int i=il; i<=iu; i++) {
              sl3d_(n,k,j,i) = sl_(n,i);
              sr3d_(n,k,j,i) = sr_(n,i);
            }
          }
        }

        // swap the arrays for the next step
        sl_.SwapAthenaArray(slb_);
      }
    }
    if (order == 4) {
      // TODO(felker): assuming uniform mesh with dx1f=dx2f=dx3f, so factor this out
      // TODO(felker): also, this may need to be dx2v, since Laplacian is cell-centered
      Real h = pmb->pcoord->dx2f(js);  // pco->dx2f(j); inside loop
      Real C = (h*h)/24.0;

      // construct Laplacian from x2flux
      pmb->pcoord->LaplacianX2All(x2flux, laplacian_all_fc, 0, NSCALARS-1,
                                  kl, ku, js, je+1, il, iu);

      // Approximate x2 face-centered states
      for (int k=kl; k<=ku; ++k) {
        for (int j=js; j<=je+1; ++j) {
          // Compute Laplacian of x2 face states
          for (int n=0; n<NSCALARS; ++n) {
            pmb->pcoord->LaplacianX2(sl3d_, laplacian_l_fc_, n, k, j, il, iu);
            pmb->pcoord->LaplacianX2(sr3d_, laplacian_r_fc_, n, k, j, il, iu);
#pragma omp simd
            for (int i=il; i<=iu; ++i) {
              sl_(n,i) = sl3d_(n,k,j,i) - C*laplacian_l_fc_(i);
              sr_(n,i) = sr3d_(n,k,j,i) - C*laplacian_r_fc_(i);
            }
          }
#pragma omp simd
          for (int i=il; i<=iu; ++i) {
            // pmb->peos->ApplyPrimitiveFloors(sl_, k, j, i);
            // pmb->peos->ApplyPrimitiveFloors(sr_, k, j, i);
          }

          // Compute x2 interface fluxes from face-centered primitive variables
          // TODO(felker): check that e1x2,e3x2 arguments added in late 2017 work here
          pmb->pcoord->CenterWidth2(k, j, il, iu, dxw_);
          // ComputeUpwindFlux(k, j, il, iu, sl_, sr_, Hydro::x2flux_fc, flux_fc);

          // Apply Laplacian of second-order accurate face-averaged flux on x1 faces
          for (int n=0; n<NSCALARS; ++n) {
#pragma omp simd
            for (int i=il; i<=iu; i++)
              x2flux(n,k,j,i) = flux_fc(n,k,j,i) + C*laplacian_all_fc(n,k,j,i);
          }
        }
      }
    } // end if (order == 4)
  }

  //--------------------------------------------------------------------------------------
  // k-direction

  if (pmb->block_size.nx3 > 1) {
    AthenaArray<Real> &x3flux = s_flux[X3DIR];
    mass_flux.InitWithShallowSlice(hyd.flux[X3DIR], 4, IDN, 1);

    // set the loop limits
    // TODO(felker): fix loop limits for fourth-order hydro
    //    if (MAGNETIC_FIELDS_ENABLED)
    il=is-1, iu=ie+1, jl=js-1, ju=je+1;

    for (int j=jl; j<=ju; ++j) { // this loop ordering is intentional
      // reconstruct the first row
      if (order == 1) {
        pmb->precon->DonorCellX3(ks-1, j, il, iu, s, sl_, sr_);
      } else if (order == 2) {
        pmb->precon->PiecewiseLinearX3(ks-1, j, il, iu, s, sl_, sr_);
      } else {
        pmb->precon->PiecewiseParabolicX3(ks-1, j, il, iu, s, sl_, sr_);
      }
      for (int k=ks; k<=ke+1; ++k) {
        // reconstruct L/R states at k
        if (order == 1) {
          pmb->precon->DonorCellX3(k, j, il, iu, s, slb_, sr_);
        } else if (order == 2) {
          pmb->precon->PiecewiseLinearX3(k, j, il, iu, s, slb_, sr_);
        } else {
          pmb->precon->PiecewiseParabolicX3(k, j, il, iu, s, slb_, sr_);
        }

        // flx(IBY) = (v3*b1 - v1*b3) = -EMFY
        // flx(IBZ) = (v3*b2 - v2*b3) =  EMFX
        pmb->pcoord->CenterWidth3(k, j, il, iu, dxw_);
        ComputeUpwindFlux(k, j, il, iu, sl_, sr_, mass_flux, x3flux);

        if (order == 4) {
          for (int n=0; n<NSCALARS; n++) {
            for (int i=il; i<=iu; i++) {
              sl3d_(n,k,j,i) = sl_(n,i);
              sr3d_(n,k,j,i) = sr_(n,i);
            }
          }
        }

        // swap the arrays for the next step
        sl_.SwapAthenaArray(slb_);
      }
    }
    if (order == 4) {
      // TODO(felker): assuming uniform mesh with dx1f=dx2f=dx3f, so factor this out
      // TODO(felker): also, this may need to be dx3v, since Laplacian is cell-centered
      Real h = pmb->pcoord->dx3f(ks);  // pco->dx3f(j); inside loop
      Real C = (h*h)/24.0;

      // construct Laplacian from x3flux
      pmb->pcoord->LaplacianX3All(x3flux, laplacian_all_fc, 0, NSCALARS-1,
                                  ks, ke+1, jl, ju, il, iu);

      // Approximate x3 face-centered states
      for (int k=ks; k<=ke+1; ++k) {
        for (int j=jl; j<=ju; ++j) {
          // Compute Laplacian of x3 face states
          for (int n=0; n<NSCALARS; ++n) {
            pmb->pcoord->LaplacianX3(sl3d_, laplacian_l_fc_, n, k, j, il, iu);
            pmb->pcoord->LaplacianX3(sr3d_, laplacian_r_fc_, n, k, j, il, iu);
#pragma omp simd
            for (int i=il; i<=iu; ++i) {
              sl_(n,i) = sl3d_(n,k,j,i) - C*laplacian_l_fc_(i);
              sr_(n,i) = sr3d_(n,k,j,i) - C*laplacian_r_fc_(i);
            }
          }
#pragma omp simd
          for (int i=il; i<=iu; ++i) {
            // pmb->peos->ApplyPrimitiveFloors(sl_, k, j, i);
            // pmb->peos->ApplyPrimitiveFloors(sr_, k, j, i);
          }

          // Compute x3 interface fluxes from face-centered primitive variables
          // TODO(felker): check that e2x3,e1x3 arguments added in late 2017 work here
          pmb->pcoord->CenterWidth3(k, j, il, iu, dxw_);
          // ComputeUpwindFlux(k, j, il, iu, sl_, sr_, Hydro::x3flux_fc, flux_fc);

          // Apply Laplacian of second-order accurate face-averaged flux on x3 faces
          for (int n=0; n<NSCALARS; ++n) {
#pragma omp simd
            for (int i=il; i<=iu; i++)
              x3flux(n,k,j,i) = flux_fc(n,k,j,i) + C*laplacian_all_fc(n,k,j,i);
          }
        }
      }
    } // end if (order == 4)
  }
  return;
}

void PassiveScalars::ComputeUpwindFlux(const int k, const int j, const int il,
                                       const int iu, // CoordinateDirection dir,
                                       AthenaArray<Real> &sl, AthenaArray<Real> &sr,
                                       AthenaArray<Real> &mass_flx,  // 3D
                                       AthenaArray<Real> &flx_out) { // 4D
  const int nu = NSCALARS - 1;

  for (int n=0; n<=nu; n++) {
#pragma omp simd
    for (int i=il; i<=iu; i++) {
      Real fluid_flx = mass_flx(k,j,i);
      if (fluid_flx >= 0.0)
        flx_out(n,k,j,i) = fluid_flx*sl_(n,k,j,i);
      else
        flx_out(n,k,j,i) = fluid_flx*sr_(n,k,j,i);
    }
  }
  return;
}
