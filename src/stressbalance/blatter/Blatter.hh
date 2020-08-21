/* Copyright (C) 2020 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef PISM_BLATTER_H
#define PISM_BLATTER_H

#include "pism/stressbalance/ShallowStressBalance.hh"
#include "pism/util/iceModelVec3Custom.hh"
#include "pism/util/petscwrappers/SNES.hh"
#include "pism/util/petscwrappers/DM.hh"
#include "pism/util/petscwrappers/Vec.hh"

#include "grid_hierarchy.hh"    // GridInfo

namespace pism {
namespace stressbalance {

class Blatter : public ShallowStressBalance {
public:
  Blatter(IceGrid::ConstPtr grid, int Mz, int n_levels, int coarsening_factor);
  virtual ~Blatter();

  void update(const Inputs &inputs, bool);

  IceModelVec3Custom::Ptr velocity_u_sigma() const;
  IceModelVec3Custom::Ptr velocity_v_sigma() const;

protected:
  // u and v components of ice velocity on the sigma grid
  IceModelVec3Custom::Ptr m_u_sigma, m_v_sigma;

  // 3D dof=2 DM used by SNES
  petsc::DM m_da;
  // storage for the solution
  petsc::Vec m_x;

  petsc::SNES m_snes;

  struct CallbackData {
    DM da;
    Blatter *solver;
  };

  CallbackData m_callback_data;
  GridInfo m_grid_info;
  double m_rhog;

  void init_impl();

  void define_model_state_impl(const File &output) const;

  void write_model_state_impl(const File &output) const;

  void compute_jacobian(DMDALocalInfo *info, const Vector2 ***x, Mat A, Mat J);

  void compute_residual(DMDALocalInfo *info, const Vector2 ***xg, Vector2 ***yg);

  static PetscErrorCode jacobian_callback(DMDALocalInfo *info,
                                          const Vector2 ***x,
                                          Mat A, Mat J, CallbackData *data);

  static PetscErrorCode function_callback(DMDALocalInfo *info, const Vector2 ***x, Vector2 ***f,
                                          CallbackData *data);

  void init_2d_parameters(const Inputs &inputs);
  void init_ice_hardness(const Inputs &inputs);

  // Guts of the constructor. This method wraps PETSc calls to simplify error checking.
  PetscErrorCode setup(DM pism_da, int Mz, int n_levels, int coarsening_factor);

  void set_initial_guess(const IceModelVec3Custom &u_sigma, const IceModelVec3Custom &v_sigma);

  void copy_solution();

  void compute_averaged_velocity(IceModelVec2V &result);

  void get_basal_velocity(IceModelVec2V &result);
};

} // end of namespace stressbalance
} // end of namespace pism

#endif /* PISM_BLATTER_H */
