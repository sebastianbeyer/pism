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

#include "pism/stressbalance/blatter/Blatter.hh"

namespace pism {
namespace stressbalance {

/*!
 * Implements Dirichlet BC and the source term for a verification test using the following
 * exact solution:
 *
 * u = exp(x) * sin(2 * pi * y)
 * v = exp(x) * cos(2 * pi * y)
 *
 * Domain: [0, 1] * [0, 1] * [0, 1].
 *
 * Ice thickness is equal to 1 everywhere; bed elevation is 0 everywhere.
 *
 * Dirichlet BC are imposed at all the nodes along the lateral boundary.
 *
 * Natural boundary conditions are used on the top and bottom boundaries.
 *
 * Note: KSP iterations seem to stall when the default preconditioner is used. Use
 * "-pc_type gamg" instead.
 */
class BlatterTest1 : public Blatter {
public:
  BlatterTest1(IceGrid::ConstPtr grid, int Mz, int n_levels, int coarsening_factor);

protected:
  bool neumann_bc_face(int face, const int *node_type);

  bool dirichlet_node(const DMDALocalInfo &info, const fem::Element3::GlobalIndex& I);

  Vector2 u_bc(double x, double y, double z);

  void residual_source_term(const fem::Q1Element3 &element,
                            const double *surface,
                            Vector2 *residual);
  //! constant ice hardness
  double m_B;
};

} // end of namespace stressbalance
} // end of namespace pism
