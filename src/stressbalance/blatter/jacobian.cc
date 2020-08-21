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

#include "Blatter.hh"

#include "DataAccess.hh"
#include "pism/basalstrength/basal_resistance.hh"
#include "pism/rheology/FlowLaw.hh"
#include "pism/util/node_types.hh"
#include "grid_hierarchy.hh"    // grid_transpose(), grid_z()

namespace pism {
namespace stressbalance {

/*!
 * Computes the Jacobian contribution of the "main" part of the Blatter system.
 */
void Blatter::jacobian_f(const fem::Element3 &element,
                         const Vector2 *velocity,
                         const double *hardness,
                         double K[16][16]) {
  int Nk = fem::q13d::n_chi;

  Vector2
    *u   = m_work2[0],
    *u_x = m_work2[1],
    *u_y = m_work2[2],
    *u_z = m_work2[3];

  double *B = m_work[0];

  element.evaluate(velocity, u, u_x, u_y, u_z);
  element.evaluate(hardness, B);

  // loop over all quadrature points
  for (int q = 0; q < element.n_pts(); ++q) {
    auto W = element.weight(q);

    double
      ux = u_x[q].u,
      uy = u_y[q].u,
      uz = u_z[q].u,
      vx = u_x[q].v,
      vy = u_y[q].v,
      vz = u_z[q].v;

    double gamma = (ux * ux + vy * vy + ux * vy +
                    0.25 * ((uy + vx) * (uy + vx) + uz * uz + vz * vz));

    double eta, deta;
    m_flow_law->effective_viscosity(B[q], gamma, &eta, &deta);

    // loop over test and trial functions, computing the upper-triangular part of
    // the element Jacobian
    for (int t = 0; t < Nk; ++t) {
      auto psi = element.chi(q, t);
      for (int s = t; s < Nk; ++s) {
        auto phi = element.chi(q, s);

        double
          gamma_u = 2.0 * ux * phi.dx + vy * phi.dx + 0.5 * phi.dy * (uy + vx) + 0.5 * uz * phi.dz,
          gamma_v = 2.0 * vy * phi.dy + ux * phi.dy + 0.5 * phi.dx * (uy + vx) + 0.5 * vz * phi.dz;

        double
          eta_u = deta * gamma_u,
          eta_v = deta * gamma_v;

        // Picard part
        K[t * 2 + 0][s * 2 + 0] += W * eta * (4.0 * psi.dx * phi.dx + psi.dy * phi.dy + psi.dz * phi.dz);
        K[t * 2 + 0][s * 2 + 1] += W * eta * (2.0 * psi.dx * phi.dy + psi.dy * phi.dx);
        K[t * 2 + 1][s * 2 + 0] += W * eta * (2.0 * psi.dy * phi.dx + psi.dx * phi.dy);
        K[t * 2 + 1][s * 2 + 1] += W * eta * (4.0 * psi.dy * phi.dy + psi.dx * phi.dx + psi.dz * phi.dz);
        // extra Newton terms
        K[t * 2 + 0][s * 2 + 0] += W * eta_u * (psi.dx * (4.0 * ux + 2.0 * vy) +
                                                psi.dy * (uy + vx) +
                                                psi.dz * uz);
        K[t * 2 + 0][s * 2 + 1] += W * eta_v * (psi.dx * (4.0 * ux + 2.0 * vy) +
                                                psi.dy * (uy + vx) +
                                                psi.dz * uz);
        K[t * 2 + 1][s * 2 + 0] += W * eta_u * (psi.dx * (uy + vx) +
                                                psi.dy * (4.0 * vy + 2.0 * ux) +
                                                psi.dz * vz);
        K[t * 2 + 1][s * 2 + 1] += W * eta_v * (psi.dx * (uy + vx) +
                                                psi.dy * (4.0 * vy + 2.0 * ux) +
                                                psi.dz * vz);
      }
    }
  } // end of the loop over q

}

/*!
 * Compute the Jacobian contribution of the basal boundary condition.
 *
 * This method implements basal sliding.
 */
void Blatter::jacobian_basal(const fem::Q1Element3Face &face,
                             const double *tauc_nodal,
                             const double *f_nodal,
                             const Vector2 *velocity,
                             double K[16][16]) {
  int Nk = fem::q13d::n_chi;

  Vector2 *u = m_work2[0];

  double
    *tauc       = m_work[0],
    *floatation = m_work[1];

  face.evaluate(velocity, u);
  face.evaluate(tauc_nodal, tauc);
  face.evaluate(f_nodal, floatation);

  for (int q = 0; q < face.n_pts(); ++q) {
    auto W = face.weight(q);

    bool grounded = floatation[q] <= 0.0;
    double beta = 0.0, dbeta = 0.0;
    if (grounded) {
      m_basal_sliding_law->drag_with_derivative(tauc[q], u[q].u, u[q].v, &beta, &dbeta);
    }

    // loop over all test functions
    for (int t = 0; t < Nk; ++t) {
      auto psi = face.chi(q, t);
      for (int s = 0; s < Nk; ++s) {
        auto phi = face.chi(q, s);

        double p = psi * phi;

        K[t * 2 + 0][s * 2 + 0] += W * p * (beta + dbeta * u[q].u * u[q].u);
        K[t * 2 + 0][s * 2 + 1] += W * p * dbeta * u[q].u * u[q].v;
        K[t * 2 + 1][s * 2 + 0] += W * p * dbeta * u[q].v * u[q].u;
        K[t * 2 + 1][s * 2 + 1] += W * p * (beta + dbeta * u[q].v * u[q].v);
      }
    }
  }
}

/*!
 * Set the Jacobian to identity at Dirichlet nodes.
 */
void Blatter::jacobian_dirichlet(const DMDALocalInfo &info, Parameters **P, Mat J) {
  PetscErrorCode ierr;

  // Dirichlet scaling
  Vector2 scaling = {1.0, 1.0};

  // take care of Dirichlet nodes (both explicit and grid points outside the domain)
  //
  // here we loop over all the *owned* nodes
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      for (int k = info.zs; k < info.zs + info.zm; k++) {
        if ((int)P[j][i].node_type == NODE_EXTERIOR or dirichlet_node(info, {i, j, k})) {

          double identity[4] = {scaling.u, 0, 0, scaling.v};

          MatStencil row;
          row.i = k;            // STORAGE_ORDER
          row.j = i;            // STORAGE_ORDER
          row.k = j;            // STORAGE_ORDER
          ierr = MatSetValuesBlockedStencil(J, 1, &row, 1, &row, identity, ADD_VALUES);
          PISM_CHK(ierr, "MatSetValuesBlockedStencil"); // this may throw
        }
      }
    }
  }
}

/*!
 * Compute the Jacobian matrix.
 */
void Blatter::compute_jacobian(DMDALocalInfo *petsc_info,
                               const Vector2 ***X, Mat A, Mat J) {
  auto info = grid_transpose(*petsc_info);

  // Zero out the Jacobian in preparation for updating it.
  PetscErrorCode ierr = MatZeroEntries(J);
  PISM_CHK(ierr, "MatZeroEntries");

  // Stencil width of 1 is not very important, but if info.sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info.sw == 1);

  // horizontal grid spacing is the same on all multigrid levels
  double
    x_min = m_grid->x0() - m_grid->Lx(),
    y_min = m_grid->y0() - m_grid->Ly(),
    dx    = m_grid->dx(),
    dy    = m_grid->dy();

  fem::Q1Element3 element(info,
                          fem::Q13DQuadrature8(),
                          dx, dy, x_min, y_min);

  // Maximum number of nodes per element
  const int Nk = fem::q13d::n_chi;
  assert(element.n_chi() <= Nk);
  assert(element.n_pts() <= m_Nq);

  // scalar quantities
  double z[Nk];
  double floatation[Nk], bottom_elevation[Nk], ice_thickness[Nk];
  double B_nodal[Nk], basal_yield_stress[Nk];
  int node_type[Nk];

  // 2D vector quantities
  Vector2 velocity[Nk];

  // FIXME: this communicates ghosts every time the Jacobian is computed, which is excessive.
  //
  // note: we use m_da below because all multigrid levels use the same 2D grid
  DataAccess<Parameters**> P(m_da, 2, GHOSTED);
  // note: we use info.da below because ice hardness is on the grid corresponding to the
  // current multigrid level
  DataAccess<double***> hardness(info.da, 3, GHOSTED);

  // loop over all the elements that have at least one owned node
  for (int j = info.gys; j < info.gys + info.gym - 1; j++) {
    for (int i = info.gxs; i < info.gxs + info.gxm - 1; i++) {

      for (int n = 0; n < Nk; ++n) {
        auto I = element.local_to_global(i, j, 0, n);

        auto p = P[I.j][I.i];

        bottom_elevation[n] = p.bed;
        ice_thickness[n]    = p.thickness;
        node_type[n]        = p.node_type;
      }

      // skip ice-free (exterior) columns
      if (exterior_element(node_type)) {
        continue;
      }

      for (int k = info.gzs; k < info.gzs + info.gzm - 1; k++) {

        // Element-local Jacobian matrix (there are Nk vector valued degrees of freedom
        // per element, for a total of Nk*Nk = 64 entries in the local Jacobian.
        double K[2*Nk][2*Nk];
        memset(K, 0, sizeof(K));

        // Compute coordinates of the nodes of this element and fetch node types.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(i, j, k, n);

          z[n] = grid_z(bottom_elevation[n], ice_thickness[n], info.mz, I.k);
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        element.reset(i, j, k, z);

        // Get nodal values of ice velocity.
        {
          element.nodal_values(X, velocity);

          // Don't contribute to Dirichlet nodes
          for (int n = 0; n < Nk; ++n) {
            auto I = element.local_to_global(n);
            if (dirichlet_node(info, I)) {
              element.mark_row_invalid(n);
              element.mark_col_invalid(n);
              velocity[n] = u_bc(element.x(n), element.y(n), element.z(n));
            }
          }
        }

        element.nodal_values((double***)hardness, B_nodal);

        jacobian_f(element, velocity, B_nodal, K);

        // basal boundary
        if (k == 0) {
          for (int n = 0; n < Nk; ++n) {
            auto I = element.local_to_global(n);

            basal_yield_stress[n] = P[I.j][I.i].tauc;
            floatation[n]         = P[I.j][I.i].floatation;
          }

          fem::Q1Element3Face *face = grounding_line(floatation) ? &m_face100 : &m_face4;

          // face 4 is the bottom face in fem::q13d::incident_nodes
          face->reset(4, z);

          jacobian_basal(*face, basal_yield_stress, floatation, velocity, K);
        }

        // fill the lower-triangular part of the element Jacobian using the fact that J is
        // symmetric
        for (int t = 0; t < Nk; ++t) {
          for (int s = 0; s < t; ++s) {
            K[t * 2 + 0][s * 2 + 0] = K[s * 2 + 0][t * 2 + 0];
            K[t * 2 + 1][s * 2 + 0] = K[s * 2 + 0][t * 2 + 1];
            K[t * 2 + 0][s * 2 + 1] = K[s * 2 + 1][t * 2 + 0];
            K[t * 2 + 1][s * 2 + 1] = K[s * 2 + 1][t * 2 + 1];
          }
        }

        element.add_contribution(&K[0][0], J);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k

  jacobian_dirichlet(info, P, J);

  ierr = MatAssemblyBegin(J, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyBegin");
  ierr = MatAssemblyEnd(J, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyEnd");
  if (A != J) {
    ierr = MatAssemblyBegin(A, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyBegin");
    ierr = MatAssemblyEnd(A, MAT_FINAL_ASSEMBLY); PISM_CHK(ierr, "MatAssemblyEnd");
  }

  ierr = MatSetOption(J, MAT_NEW_NONZERO_LOCATION_ERR, PETSC_TRUE);
  PISM_CHK(ierr, "MatSetOption");

  ierr = MatSetOption(J, MAT_SYMMETRIC, PETSC_TRUE);
  PISM_CHK(ierr, "MatSetOption");
}

PetscErrorCode Blatter::jacobian_callback(DMDALocalInfo *info,
                                           const Vector2 ***x,
                                           Mat A, Mat J, CallbackData *data) {
  try {
    data->solver->compute_jacobian(info, x, A, J);
  } catch (...) {
    MPI_Comm com = MPI_COMM_SELF;
    PetscErrorCode ierr = PetscObjectGetComm((PetscObject)data->da, &com); CHKERRQ(ierr);
    handle_fatal_errors(com);
    SETERRQ(com, 1, "A PISM callback failed");
  }
  return 0;
}

} // end of namespace stressbalance
} // end of namespace pism
