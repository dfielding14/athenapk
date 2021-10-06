//========================================================================================
// AthenaPK - a performance portable block structured AMR astrophysical MHD code.
// Copyright (c) 2020, Athena-Parthenon Collaboration. All rights reserved.
// Licensed under the BSD 3-Clause License (the "LICENSE").
//========================================================================================

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Parthenon headers
#include "bvals/cc/bvals_cc_in_one.hpp"
#include "diffusion/diffusion.hpp"
#include "interface/update.hpp"
#include "parthenon/driver.hpp"
#include "parthenon/package.hpp"
#include "refinement/refinement.hpp"
#include "tasks/task_id.hpp"
#include "utils/partition_stl_containers.hpp"
// AthenaPK headers
#include "../eos/adiabatic_hydro.hpp"
#include "glmmhd/glmmhd.hpp"
#include "hydro.hpp"
#include "hydro_driver.hpp"

using namespace parthenon::driver::prelude;

namespace Hydro {

HydroDriver::HydroDriver(ParameterInput *pin, ApplicationInput *app_in, Mesh *pm)
    : MultiStageDriver(pin, app_in, pm) {
  // fail if these are not specified in the input file
  pin->CheckRequired("hydro", "eos");

  // warn if these fields aren't specified in the input file
  pin->CheckDesired("parthenon/time", "cfl");
}

// Calculate mininum dx, which is used in calculating the divergence cleaning speed c_h
TaskStatus CalculateGlobalMinDx(MeshData<Real> *md) {
  auto pmb = md->GetBlockData(0)->GetBlockPointer();
  auto hydro_pkg = pmb->packages.Get("Hydro");

  const auto &prim_pack = md->PackVariables(std::vector<std::string>{"prim"});

  IndexRange ib = prim_pack.cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = prim_pack.cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = prim_pack.cellbounds.GetBoundsK(IndexDomain::interior);

  Real mindx = std::numeric_limits<Real>::max();

  bool nx2 = prim_pack.GetDim(2) > 1;
  bool nx3 = prim_pack.GetDim(3) > 1;
  pmb->par_reduce(
      "CalculateGlobalMinDx", 0, prim_pack.GetDim(5) - 1, kb.s, kb.e, jb.s, jb.e, ib.s,
      ib.e,
      KOKKOS_LAMBDA(const int b, const int k, const int j, const int i, Real &lmindx) {
        const auto &coords = prim_pack.coords(b);
        lmindx = fmin(lmindx, coords.dx1v(k, j, i));
        if (nx2) {
          lmindx = fmin(lmindx, coords.dx2v(k, j, i));
        }
        if (nx3) {
          lmindx = fmin(lmindx, coords.dx3v(k, j, i));
        }
      },
      Kokkos::Min<Real>(mindx));

  // Reduction to host var is blocking and only have one of this tasks run at the same
  // time so modifying the package should be safe.
  auto mindx_pkg = hydro_pkg->Param<Real>("mindx");
  if (mindx < mindx_pkg) {
    hydro_pkg->UpdateParam("mindx", mindx);
  }

  return TaskStatus::complete;
}

// Sets all fluxes to 0
TaskStatus ResetFluxes(MeshData<Real> *md) {
  auto pmb = md->GetBlockData(0)->GetBlockPointer();
  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  // In principle, we'd only need to pack Metadata::WithFluxes here, but
  // choosing to mirror other use in the code so that the packs are already cached.
  std::vector<parthenon::MetadataFlag> flags_ind({Metadata::Independent});
  auto cons_pack = md->PackVariablesAndFluxes(flags_ind);

  const int ndim = pmb->pmy_mesh->ndim;
  // Using separate loops for each dim as the launch overhead should be hidden
  // by enough work over the entire pack and it allows to not use any conditionals.
  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "ResetFluxes X1", parthenon::DevExecSpace(), 0,
      cons_pack.GetDim(5) - 1, 0, cons_pack.GetDim(4) - 1, kb.s, kb.e, jb.s, jb.e, ib.s,
      ib.e + 1,
      KOKKOS_LAMBDA(const int b, const int v, const int k, const int j, const int i) {
        auto &cons = cons_pack(b);
        cons.flux(X1DIR, v, k, j, i) = 0.0;
      });

  if (ndim < 2) {
    return TaskStatus::complete;
  }
  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "ResetFluxes X2", parthenon::DevExecSpace(), 0,
      cons_pack.GetDim(5) - 1, 0, cons_pack.GetDim(4) - 1, kb.s, kb.e, jb.s, jb.e + 1,
      ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int v, const int k, const int j, const int i) {
        auto &cons = cons_pack(b);
        cons.flux(X2DIR, v, k, j, i) = 0.0;
      });

  if (ndim < 3) {
    return TaskStatus::complete;
  }
  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "ResetFluxes X3", parthenon::DevExecSpace(), 0,
      cons_pack.GetDim(5) - 1, 0, cons_pack.GetDim(4) - 1, kb.s, kb.e + 1, jb.s, jb.e,
      ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int v, const int k, const int j, const int i) {
        auto &cons = cons_pack(b);
        cons.flux(X3DIR, v, k, j, i) = 0.0;
      });
  return TaskStatus::complete;
}

TaskStatus RKL2Step(MeshData<Real> *md_Y0, MeshData<Real> *md_Yjm1,
                    MeshData<Real> *md_Yjm2, MeshData<Real> *md_MY0, const int j_int,
                    const Real s, const Real tau) {
  auto pmb = md_Y0->GetBlockData(0)->GetBlockPointer();
  IndexRange ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  IndexRange jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  IndexRange kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);

  const Real j = static_cast<Real>(j_int);
  // Compute coefficients. Meyer+2012 eq. (16)
  const Real w1 = 4. / (s * s + s - 2.0);

  const Real b_j = j_int < 2 ? 1. / 3.
                             : ((j - 0.) * (j - 0.) + (j - 0.) - 2.) /
                                   (2 * (j - 0.) * ((j - 0.) + 1.));
  const Real b_jm1 = j_int < 3 ? 1. / 3.
                               : ((j - 1.) * (j - 1.) + (j - 1.) - 2.) /
                                     (2 * (j - 1.) * ((j - 1.) + 1.));
  const Real b_jm2 = j_int < 4 ? 1. / 3.
                               : ((j - 2.) * (j - 2.) + (j - 2.) - 2.) /
                                     (2 * (j - 2.) * ((j - 2.) + 1.));
  Real mu_j = 0.0;
  Real nu_j = 0.0;
  Real mu_tilde_j = 0.0;
  Real gamma_tilde_j = 0.0;

  if (j_int == 1) {
    // technically mu_tilde_1, but in the eqn in the kernel it's effecticely
    // applied to MY0 so we use gamma_tilde_j instead
    gamma_tilde_j = b_j * w1;
  } else {
    mu_j = (2.0 * j - 1.0) / j * b_j / b_jm1;
    nu_j = -(j - 1.0) / j * b_j / b_jm2;
    mu_tilde_j = mu_j * w1;
    gamma_tilde_j = -(1.0 - b_jm1) * mu_tilde_j; // -a_jm1*mu_tilde_j
  }

  // In principle, we'd only need to pack Metadata::WithFluxes here, but
  // choosing to mirror other use in the code so that the packs are already cached.
  std::vector<parthenon::MetadataFlag> flags_ind({Metadata::Independent});
  auto Y0 = md_Y0->PackVariablesAndFluxes(flags_ind);
  auto Yjm1 = md_Yjm1->PackVariablesAndFluxes(flags_ind);
  auto Yjm2 = md_Yjm2->PackVariablesAndFluxes(flags_ind);
  auto MY0 = md_MY0->PackVariablesAndFluxes(flags_ind);

  const int ndim = pmb->pmy_mesh->ndim;
  parthenon::par_for(
      DEFAULT_LOOP_PATTERN, "RKL step", parthenon::DevExecSpace(), 0,
      Y0.GetDim(5) - 1, 0, Y0.GetDim(4) - 1, kb.s, kb.e, jb.s, jb.e, ib.s, ib.e,
      KOKKOS_LAMBDA(const int b, const int v, const int k, const int j, const int i) {
        // First calc this step
        const auto &coords = Yjm1.coords(b);
        const Real MYjm1 =
            parthenon::Update::FluxDivHelper(v, k, j, i, ndim, coords, Yjm1(b));
        const Real Yj = mu_j * Yjm1(b, v, k, j, i) + nu_j * Yjm2(b, v, k, j, i) +
                        (1.0 - mu_j - nu_j) * Y0(b, v, k, j, i) +
                        mu_tilde_j * tau * MYjm1 +
                        gamma_tilde_j * tau * MY0(b, v, k, j, i);
        // Then shuffle vars for next step
        Yjm2(b, v, k, j, i) = Yjm1(b, v, k, j, i);
        Yjm1(b, v, k, j, i) = Yj;
      });

  return TaskStatus::complete;
}

// Assumes that prim and cons are in sync initially.
// Guarantees that prim and cons are in sync at the end.
void AddSTSTasks(TaskCollection *ptask_coll, Mesh *pmesh, BlockList_t &blocks,
                 const Real tau) {

  auto hydro_pkg = blocks[0]->packages.Get("Hydro");
  auto mindt_diff = hydro_pkg->Param<Real>("dt_diff");

  // get number of RKL steps
  // eq (21) using half hyperbolic timestep due to Strang split
  int s_rkl =
      static_cast<int>(0.5 * (std::sqrt(9.0 + 16.0 * tau / mindt_diff) - 1.0)) + 1;
  // ensure odd number of stages
  if (s_rkl % 2 == 0) s_rkl += 1;

  if (parthenon::Globals::my_rank == 0) {
    const auto ratio = 2.0 * tau / mindt_diff;
    std::cout << "STS ratio: " << ratio << " Taking " << s_rkl << " steps." << std::endl;
    if (ratio > 100.0) {
      std::cout << "WARNING: ratio is > 100. Proceed at own risk." << std::endl;
    }
  }

  TaskID none(0);

  TaskRegion &region_init = ptask_coll->AddRegion(blocks.size());
  for (int i = 0; i < blocks.size(); i++) {
    auto &pmb = blocks[i];
    auto &tl = region_init[i];
    auto &u0 = pmb->meshblock_data.Get();

    // Add extra registers. No-op for existing variables so it's safe to call every
    // time.
    // TODO(pgrete) this allocates all Variables, i.e., prim and cons vector, but only a
    // subset is actually needed. Streamline to allocate only required vars.
    pmb->meshblock_data.Add("MY0", u0);
    pmb->meshblock_data.Add("Yjm2", u0);

    // Need to inititalze Yjm2 with Y0 for stage j=2.
    // However, we copy Y0 data to Yjm1 because the first RKL step will copy from Yjm1 to
    // Yjm2.
    auto &Yjm1 = pmb->meshblock_data.Get("u1");
    tl.AddTask(
        none,
        [](MeshBlockData<Real> *u0, MeshBlockData<Real> *u1) {
          // No need for prim here as only cons are used during first RKL step
          u1->Get("cons").data.DeepCopy(u0->Get("cons").data);
          return TaskStatus::complete;
        },
        u0.get(), Yjm1.get());
  }

  const int num_partitions = pmesh->DefaultNumPartitions();
  TaskRegion &region_rkl2_step_init = ptask_coll->AddRegion(num_partitions);
  for (int i = 0; i < num_partitions; i++) {
    auto &tl = region_rkl2_step_init[i];
    auto &Y0 = pmesh->mesh_data.GetOrAdd("base", i);
    auto &MY0 = pmesh->mesh_data.GetOrAdd("MY0", i);
    // Reset flux arrays (not guaranteed to be zero)
    auto reset_fluxes = tl.AddTask(none, ResetFluxes, Y0.get());

    // Calculate the diffusive fluxes for Y0 (here u0) so that we can store the result
    // as MY0 and reuse later (it is used in every subsetp).
    auto hydro_diff_fluxes =
        tl.AddTask(reset_fluxes, CalcDiffFluxes, hydro_pkg.get(), Y0.get());

    auto init_MY0 =
        tl.AddTask(hydro_diff_fluxes, parthenon::Update::FluxDivergence<MeshData<Real>>,
                   Y0.get(), MY0.get());
  }

  // RKL loop
  for (int j = 1; j <= s_rkl; j++) {

    TaskRegion &region_init_other = ptask_coll->AddRegion(blocks.size());
    for (int i = 0; i < blocks.size(); i++) {
      auto &pmb = blocks[i];
      auto &tl = region_init_other[i];
      auto &Yjm1 = pmb->meshblock_data.Get("u1");
      // only need boundaries for Yjm1 (u1 here)
      auto start_recv = tl.AddTask(none, &MeshBlockData<Real>::StartReceiving, Yjm1.get(),
                                   BoundaryCommSubset::all);
    }

    TaskRegion &region_rkl2_step_other = ptask_coll->AddRegion(num_partitions);
    for (int i = 0; i < num_partitions; i++) {
      auto &tl = region_rkl2_step_other[i];
      auto &Y0 = pmesh->mesh_data.GetOrAdd("base", i);
      auto &MY0 = pmesh->mesh_data.GetOrAdd("MY0", i);
      auto &Yjm1 = pmesh->mesh_data.GetOrAdd("u1", i);
      auto &Yjm2 = pmesh->mesh_data.GetOrAdd("Yjm2", i);

      // Reset flux arrays (not guaranteed to be zero)
      auto reset_fluxes = tl.AddTask(none, ResetFluxes, Yjm1.get());

      // Calculate the diffusive fluxes for Yjm1 (here u1)
      auto hydro_diff_fluxes =
          tl.AddTask(reset_fluxes, CalcDiffFluxes, hydro_pkg.get(), Yjm1.get());

      auto rkl2_step =
          tl.AddTask(hydro_diff_fluxes, RKL2Step, Y0.get(), Yjm1.get(), Yjm2.get(),
                     MY0.get(), j, static_cast<Real>(s_rkl), tau);

      // update ghost cells of Yjm1 (currently storing Yj)
      // TODO(pgrete) optimize (in parthenon) to only send subset of updated vars
      auto send = tl.AddTask(rkl2_step,
                             parthenon::cell_centered_bvars::SendBoundaryBuffers, Yjm1);
      auto recv =
          tl.AddTask(send, parthenon::cell_centered_bvars::ReceiveBoundaryBuffers, Yjm1);
      auto fill_from_bufs =
          tl.AddTask(recv, parthenon::cell_centered_bvars::SetBoundaries, Yjm1);
    }
    TaskRegion &region_clear_bnd_other = ptask_coll->AddRegion(blocks.size());
    for (int i = 0; i < blocks.size(); i++) {
      auto &tl = region_clear_bnd_other[i];
      auto &Yjm1 = blocks[i]->meshblock_data.Get("u1");
      auto clear_comm_flags = tl.AddTask(none, &MeshBlockData<Real>::ClearBoundary,
                                         Yjm1.get(), BoundaryCommSubset::all);
    }
    TaskRegion &region_cons_to_prim_other = ptask_coll->AddRegion(num_partitions);
    for (int i = 0; i < num_partitions; i++) {
      auto &tl = region_cons_to_prim_other[i];
      auto &Yjm1 = pmesh->mesh_data.GetOrAdd("u1", i);
      auto fill_derived =
          tl.AddTask(none, parthenon::Update::FillDerived<MeshData<Real>>, Yjm1.get());
    }
  }

  // copy final result back to u0
  TaskRegion &region_copy_out = ptask_coll->AddRegion(blocks.size());
  for (int i = 0; i < blocks.size(); i++) {
    auto &tl = region_copy_out[i];
    auto &u0 = blocks[i]->meshblock_data.Get();
    auto &Yjm1 = blocks[i]->meshblock_data.Get("u1");
    tl.AddTask(
        none,
        [](MeshBlockData<Real> *u0, MeshBlockData<Real> *u1) {
          u0->Get("cons").data.DeepCopy(u1->Get("cons").data);
          u0->Get("prim").data.DeepCopy(u1->Get("prim").data);
          return TaskStatus::complete;
        },
        u0.get(), Yjm1.get());
  }
}

// See the advection.hpp declaration for a description of how this function gets called.
TaskCollection HydroDriver::MakeTaskCollection(BlockList_t &blocks, int stage) {
  TaskCollection tc;
  auto hydro_pkg = blocks[0]->packages.Get("Hydro");

  TaskID none(0);
  // Number of task lists that can be executed indepenently and thus *may*
  // be executed in parallel and asynchronous.
  // Being extra verbose here in this example to highlight that this is not
  // required to be 1 or blocks.size() but could also only apply to a subset of blocks.
  auto num_task_lists_executed_independently = blocks.size();

  TaskRegion &async_region_1 = tc.AddRegion(num_task_lists_executed_independently);
  for (int i = 0; i < blocks.size(); i++) {
    auto &pmb = blocks[i];
    auto &tl = async_region_1[i];
    // Using "base" as u0, which already exists (and returned by using plain Get())
    auto &u0 = pmb->meshblock_data.Get();

    // Create meshblock data for register u1.
    // TODO(pgrete) update to derive from other quanity as u1 does not require fluxes
    if (stage == 1) {
      pmb->meshblock_data.Add("u1", u0);
    }
  }

  const int num_partitions = pmesh->DefaultNumPartitions();

  // Calculate hyperbolic divergence cleaning speed
  // TODO(pgrete) Calculating mindx is only required after remeshing. Need to find a
  // clean solution for this one-off global reduction.
  if (hydro_pkg->Param<bool>("calc_c_h") && (stage == 1)) {
    // need to make sure that there's only one region in order to MPI_reduce to work
    TaskRegion &single_task_region = tc.AddRegion(1);
    auto &tl = single_task_region[0];
    // First globally reset c_h
    auto prev_task = tl.AddTask(
        none,
        [](StateDescriptor *hydro_pkg) {
          hydro_pkg->UpdateParam("mindx", std::numeric_limits<Real>::max());
          return TaskStatus::complete;
        },
        hydro_pkg.get());
    // Adding one task for each partition. Not using a (new) single partition containing
    // all blocks here as this (default) split is also used for the following tasks and
    // thus does not create an overhead (such as creating a new MeshBlockPack that is
    // just used here). Given that all partitions are in one task list they'll be
    // executed sequentially. Given that a par_reduce to a host var is blocking it's
    // also save to store the variable in the Params for now.
    for (int i = 0; i < num_partitions; i++) {
      auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);
      auto new_mindx = tl.AddTask(prev_task, CalculateGlobalMinDx, mu0.get());
      prev_task = new_mindx;
    }
    auto reduce_c_h = prev_task;
#ifdef MPI_PARALLEL
    reduce_c_h = tl.AddTask(
        prev_task,
        [](StateDescriptor *hydro_pkg) {
          Real mins[2];
          mins[0] = hydro_pkg->Param<Real>("mindx");
          mins[1] = hydro_pkg->Param<Real>("dt_hyp");
          PARTHENON_MPI_CHECK(MPI_Allreduce(MPI_IN_PLACE, mins, 2, MPI_PARTHENON_REAL,
                                            MPI_MIN, MPI_COMM_WORLD));

          hydro_pkg->UpdateParam("mindx", mins[0]);
          hydro_pkg->UpdateParam("dt_hyp", mins[1]);
          return TaskStatus::complete;
        },
        hydro_pkg.get());
#endif
    // Finally update c_h
    auto update_c_h = tl.AddTask(
        reduce_c_h,
        [](StateDescriptor *hydro_pkg) {
          const auto &mindx = hydro_pkg->Param<Real>("mindx");
          const auto &cfl_hyp = hydro_pkg->Param<Real>("cfl");
          const auto &dt_hyp = hydro_pkg->Param<Real>("dt_hyp");
          hydro_pkg->UpdateParam("c_h", cfl_hyp * mindx / dt_hyp);
          return TaskStatus::complete;
        },
        hydro_pkg.get());
  }

  // First add split sources before the main time integration
  if (stage == 1) {
    const auto &diffint = hydro_pkg->Param<DiffInt>("diffint");
    if (diffint == DiffInt::rkl2) {
      AddSTSTasks(&tc, pmesh, blocks, 0.5 * tm.dt);
    }
    TaskRegion &strang_init_region = tc.AddRegion(num_partitions);
    for (int i = 0; i < num_partitions; i++) {
      auto &tl = strang_init_region[i];
      auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);

      // Add initial Strang split source terms, i.e., a dt/2 update
      // IMPORTANT 1: This task must also update `prim` and `cons` variables so that
      // the source term is applied to all active registers in the flux calculation.
      // IMPORTANT 2: The tasks should work using `cons` variables as input as in the
      // final step, `prim` are not updated yet from the flux calculation.
      tl.AddTask(none, AddSplitSourcesStrang, mu0.get(), tm);
    }
  }

  // Now start the main time integration by resetting the registers
  TaskRegion &async_region_init_int = tc.AddRegion(num_task_lists_executed_independently);
  for (int i = 0; i < blocks.size(); i++) {
    auto &pmb = blocks[i];
    auto &tl = async_region_init_int[i];
    auto &u0 = pmb->meshblock_data.Get();
    auto start_recv = tl.AddTask(none, &MeshBlockData<Real>::StartReceiving, u0.get(),
                                 BoundaryCommSubset::all);

    // init u1, see (11) in Athena++ method paper
    if (stage == 1) {
      auto &u1 = pmb->meshblock_data.Get("u1");
      auto init_u1 = tl.AddTask(
          none,
          [](MeshBlockData<Real> *u0, MeshBlockData<Real> *u1, bool copy_prim) {
            u1->Get("cons").data.DeepCopy(u0->Get("cons").data);
            if (copy_prim) {
              u1->Get("prim").data.DeepCopy(u0->Get("prim").data);
            }
            return TaskStatus::complete;
          },
          // First order flux correction needs the original prim variables in the
          // during the correction.
          u0.get(), u1.get(), hydro_pkg->Param<bool>("first_order_flux_correct"));
    }
  }

  // note that task within this region that contains one tasklist per pack
  // could still be executed in parallel
  TaskRegion &single_tasklist_per_pack_region = tc.AddRegion(num_partitions);
  for (int i = 0; i < num_partitions; i++) {
    auto &tl = single_tasklist_per_pack_region[i];
    auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);
    auto &mu1 = pmesh->mesh_data.GetOrAdd("u1", i);

    const auto flux_str = (stage == 1) ? "flux_first_stage" : "flux_other_stage";
    FluxFun_t *calc_flux_fun = hydro_pkg->Param<FluxFun_t *>(flux_str);
    auto calc_flux = tl.AddTask(none, calc_flux_fun, mu0);

    // TODO(pgrete) figure out what to do about the sources from the first stage
    // that are potentially disregarded when the (m)hd fluxes are corrected in the
    // second stage.
    if (hydro_pkg->Param<bool>("first_order_flux_correct")) {
      auto *first_order_flux_correct_fun =
          hydro_pkg->Param<FirstOrderFluxCorrectFun_t *>("first_order_flux_correct_fun");
      auto first_order_flux_correct =
          tl.AddTask(calc_flux, first_order_flux_correct_fun, mu0.get(), mu1.get(),
                     integrator->gam0[stage - 1], integrator->gam1[stage - 1],
                     integrator->beta[stage - 1] * integrator->dt);
    }
  }
  TaskRegion &async_region_2 = tc.AddRegion(num_task_lists_executed_independently);
  for (int i = 0; i < blocks.size(); i++) {
    auto &tl = async_region_2[i];
    auto &u0 = blocks[i]->meshblock_data.Get("base");
    auto send_flux = tl.AddTask(none, &MeshBlockData<Real>::SendFluxCorrection, u0.get());
    auto recv_flux =
        tl.AddTask(none, &MeshBlockData<Real>::ReceiveFluxCorrection, u0.get());
  }

  TaskRegion &single_tasklist_per_pack_region_2 = tc.AddRegion(num_partitions);
  for (int i = 0; i < num_partitions; i++) {
    auto &tl = single_tasklist_per_pack_region_2[i];

    auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);
    auto &mu1 = pmesh->mesh_data.GetOrAdd("u1", i);

    // compute the divergence of fluxes of conserved variables
    auto update = tl.AddTask(
        none, parthenon::Update::UpdateWithFluxDivergence<MeshData<Real>>, mu0.get(),
        mu1.get(), integrator->gam0[stage - 1], integrator->gam1[stage - 1],
        integrator->beta[stage - 1] * integrator->dt);

    // Add non-operator split source terms.
    // Note: Directly update the "cons" variables of mu0 based on the "prim" variables
    // of mu0 as the "cons" variables have already been updated in this stage from the
    // fluxes in the previous step.
    auto source_unsplit = tl.AddTask(update, AddUnsplitSources, mu0.get(), tm,
                                     integrator->beta[stage - 1] * integrator->dt);

    auto source_split_first_order = source_unsplit;

    if (stage == integrator->nstages) {
      // Add final Strang split source terms, i.e., a dt/2 update
      // IMPORTANT: The tasks should work using `cons` variables as input as in the
      // final step, `prim` are not updated yet from the flux calculation.
      auto source_split_strang_final =
          tl.AddTask(source_unsplit, AddSplitSourcesStrang, mu0.get(), tm);

      // Add operator split source terms at first order, i.e., full dt update
      // after all stages of the integration.
      // Not recommended for but allows easy "reset" of variable for some
      // problem types, see random blasts.
      source_split_first_order =
          tl.AddTask(source_split_strang_final, AddSplitSourcesFirstOrder, mu0.get(), tm);
    }

    // update ghost cells
    auto send = tl.AddTask(source_split_first_order,
                           parthenon::cell_centered_bvars::SendBoundaryBuffers, mu0);
    auto recv =
        tl.AddTask(send, parthenon::cell_centered_bvars::ReceiveBoundaryBuffers, mu0);
    auto fill_from_bufs =
        tl.AddTask(recv, parthenon::cell_centered_bvars::SetBoundaries, mu0);
  }

  TaskRegion &async_region_3 = tc.AddRegion(num_task_lists_executed_independently);
  for (int i = 0; i < blocks.size(); i++) {
    auto &tl = async_region_3[i];
    auto &u0 = blocks[i]->meshblock_data.Get("base");
    auto clear_comm_flags = tl.AddTask(none, &MeshBlockData<Real>::ClearBoundary,
                                       u0.get(), BoundaryCommSubset::all);
    auto prolongBound = none;
    if (pmesh->multilevel) {
      prolongBound = tl.AddTask(none, parthenon::ProlongateBoundaries, u0);
    }

    // set physical boundaries
    auto set_bc = tl.AddTask(prolongBound, parthenon::ApplyBoundaryConditions, u0);
  }

  TaskRegion &single_tasklist_per_pack_region_3 = tc.AddRegion(num_partitions);
  for (int i = 0; i < num_partitions; i++) {
    auto &tl = single_tasklist_per_pack_region_3[i];
    auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);
    auto fill_derived =
        tl.AddTask(none, parthenon::Update::FillDerived<MeshData<Real>>, mu0.get());
  }
  const auto &diffint = hydro_pkg->Param<DiffInt>("diffint");
  if (diffint == DiffInt::rkl2 && stage == integrator->nstages) {
    AddSTSTasks(&tc, pmesh, blocks, 0.5 * tm.dt);
  }

  if (stage == integrator->nstages) {
    TaskRegion &tr = tc.AddRegion(num_partitions);
    for (int i = 0; i < num_partitions; i++) {
      auto &tl = tr[i];
      auto &mu0 = pmesh->mesh_data.GetOrAdd("base", i);
      auto new_dt = tl.AddTask(none, parthenon::Update::EstimateTimestep<MeshData<Real>>,
                               mu0.get());
    }
  }

  if (stage == integrator->nstages && pmesh->adaptive) {
    TaskRegion &async_region_4 = tc.AddRegion(num_task_lists_executed_independently);
    for (int i = 0; i < blocks.size(); i++) {
      auto &tl = async_region_4[i];
      auto &u0 = blocks[i]->meshblock_data.Get("base");
      auto tag_refine =
          tl.AddTask(none, parthenon::Refinement::Tag<MeshBlockData<Real>>, u0.get());
    }
  }

  return tc;
}
} // namespace Hydro
