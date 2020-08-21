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

#include <cassert>
#include <cmath>                // std::pow, std::fabs
#include <algorithm>            // std::max

using std::pow;
using std::fabs;

#include "Poisson3.hh"
#include "pism/util/fem/FEM.hh"
#include "pism/util/error_handling.hh"

#include "DataAccess.hh"
#include "grid_hierarchy.hh"
#include "pism/util/node_types.hh"

namespace pism {
namespace stressbalance {

const double u_exterior = 0.0;

struct Parameters {
  // elevation (z coordinate) of the bottom domain boundary
  double bed;
  // thickness of the domain
  double thickness;
  // NodeType stored as double
  double node_type;
};

/*!
 * Compute the padding needed to allow for `n_levels` of coarsening.
 *
 * @param[in] N number of grid points (nodes)
 * @param[in] n_levels number of coarsening levels
 *
 * @return padding amount
 */
int pad(int N, int n_levels) {
  // number of spaces
  int k = N - 1;
  int C = 1;
  for (int n = 0; n < n_levels; ++n) {
    C *= 2;
    k = (k % 2 ? k + 1: k) / 2;
  }
  return (C * k + 1) - N;
}

/*!
 * dot product (used to compute normal derivatives)
 */
static double dot(const Vector3 &a, const Vector3 &b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

/*!
 * x and y coordinates of the nodes
 *
 * @param[in] min minimum coordinate value
 * @param[in] delta grid spacing
 * @param[in] k node index
 */
static double xy(double min, double delta, int k) {
  return min + k * delta;
}

/*!
 * z coordinates of the nodes
 *
 * @param[in] b surface elevation of the bottom of the domain
 * @param[in] H domain thickness
 * @param[in] Mz number of grid points in each vertical column
 * @param[in] k node index in the z direction
 */
static double z(double b, double H, int Mz, int k) {
  return b + H * k / (Mz - 1.0);
}

/*!
 * Returns true if a node is in the Dirichlet part of the boundary, false otherwise.
 */
static bool dirichlet_node(const DMDALocalInfo *info, const fem::Element3::GlobalIndex& I) {
  return (I.k == 0) or (I.k == info->mz - 1);
}

/*! Dirichlet BC and the exact solution

 b : -1 + x + y;
 n_b : [-diff(b, x), -diff(b, y), 1];
 u : x*y*(z+1)^2+(2.0*(y+1))/((y+1)^2+(x+2)^2)$
 grind('u = u);
 grind(F = ratsimp(-(diff(u, x, 2) + diff(u, y, 2) + diff(u, z, 2))));
 grind(u_x = diff(u, x));
 grind(u_y = diff(u, y));
 grind(u_z = diff(u, z));
*/
static double u_exact(double x, double y, double z) {
  return x * y * pow(z + 1, 2.0) + (2.0 * (y + 1)) / (pow(y + 1, 2.0) + pow(x + 2, 2.0));
}

/*!
 * Right hand side
 *
 * F = - (diff(u, x, 2) + diff(u, y, 2) + diff(u, z, 2))
 */
static double F(double x, double y, double z) {
  (void) z;
  return -2.0 * x * y;
}

/*!
 * Neumann BC
 */
static double G(double x, double y, double z, const Vector3 &N) {
  double
    u_x = (y * pow(z + 1, 2.0) -
           (4.0 * (x + 2) * (y + 1)) / pow(pow(y + 1, 2.0) + pow(x + 2, 2.0), 2.0)),
    u_y = (x * pow(z + 1, 2.0) + 2.0 / (pow(y + 1, 2.0) + pow(x + 2, 2.0)) -
           (4.0 * pow(y + 1, 2.0)) / pow(pow(y + 1, 2.0) + pow(x + 2, 2.0), 2.0)),
    u_z = 2 * x * y * (z + 1);

  return dot({u_x, u_y, u_z}, N);
}

static double dirichlet_scale(double dx, double dy, double dz) {
  return dx * dy * dz * (1.0 / (dx * dx) + 1.0 / (dy * dy) + 1.0 / dz * dz);
}

void Poisson3::compute_residual(DMDALocalInfo *info,
                                const double ***x, double ***R) {
  // Stencil width of 1 is not very important, but if info->sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info->sw == 1);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info->mx - 1),
    dy = (y_max - y_min) / (info->my - 1);

  fem::Q1Element3 element(*info, dx, dy, fem::Q13DQuadrature8());
  fem::Q1Element3Face face(dx, dy, fem::Q1Quadrature4());

  DataAccess<Parameters**> P(info->da, 2, GHOSTED);
  DataAccess<double***> F(info->da, 3, GHOSTED);

  // Compute the residual at Dirichlet BC nodes and reset the residual to zero elsewhere.
  //
  // Setting it to zero is necessary because we call DMDASNESSetFunctionLocal() with
  // INSERT_VALUES.
  //
  // here we loop over all the *owned* nodes
  for (int k = info->zs; k < info->zs + info->zm; k++) {
    for (int j = info->ys; j < info->ys + info->ym; j++) {
      for (int i = info->xs; i < info->xs + info->xm; i++) {

        // nodes that don't belong to any icy elements
        if ((int)P[j][i].node_type == NODE_EXTERIOR) {
          double dz = std::max(P[j][i].thickness, m_grid_info.min_thickness) / (info->mz - 1);
          R[k][j][i] = dirichlet_scale(dx, dy, dz) * (x[k][j][i] - u_exterior);
          continue;
        }

        // Dirichlet nodes
        if (dirichlet_node(info, {i, j, k})) {
          double
            xx = xy(x_min, dx, i),
            yy = xy(y_min, dy, j),
            b  = P[j][i].bed,
            H  = P[j][i].thickness,
            zz = z(b, H, info->mz, k),
            dz = std::max(H, m_grid_info.min_thickness) / (info->mz - 1);

          R[k][j][i] = dirichlet_scale(dx, dy, dz) * (x[k][j][i] - u_exact(xx, yy, zz));
        } else {
          R[k][j][i] = 0.0;
        }
      }
    }
  }

  // values at element nodes
  const int Nk_max = 8;
  int Nk = element.n_chi();
  assert(Nk <= Nk_max);

  double
    x_nodal[Nk_max], y_nodal[Nk_max],
    R_nodal[Nk_max], u_nodal[Nk_max], F_nodal[Nk_max];
  int node_type[Nk_max];
  std::vector<double> z_nodal(Nk);

  // values at quadrature points
  const int Nq_max = 16;
  int Nq = element.n_pts();
  assert(Nq <= Nq_max);

  double u[Nq_max], u_x[Nq_max], u_y[Nq_max], u_z[Nq_max];
  double xq[Nq_max], yq[Nq_max], zq[Nq_max], Fq[Nq_max];

  // make sure that xq, yq, zq and big enough for quadrature points on element faces
  assert(face.n_pts() <= Nq_max);

  // loop over all the elements that have at least one owned node
  for (int k = info->gzs; k < info->gzs + info->gzm - 1; k++) {
    for (int j = info->gys; j < info->gys + info->gym - 1; j++) {
      for (int i = info->gxs; i < info->gxs + info->gxm - 1; i++) {

        // Reset element residual to zero in preparation.
        for (int n = 0; n < Nk; ++n) {
          R_nodal[n] = 0.0;
        }

        // Compute coordinates of the nodes of this element and fetch node types.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(i, j, k, n);

          auto p = P[I.j][I.i];

          node_type[n] = p.node_type;

          x_nodal[n] = xy(x_min, dx, I.i);
          y_nodal[n] = xy(y_min, dy, I.j);
          z_nodal[n] = z(p.bed, p.thickness, info->mz, I.k);
        }

        // skip ice-free elements
        {
          // an element is exterior if one or more of its nodes are "exterior"
          bool exterior = false;
          for (int n = 0; n < Nk; ++n) {
            if (node_type[n] == NODE_EXTERIOR) {
              exterior = true;
              break;
            }
          }

          if (exterior) {
            continue;
          }
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        element.reset(i, j, k, z_nodal);

        // Get nodal values of F.
        element.nodal_values((double***)F, F_nodal);

        // Get nodal values of u.
        element.nodal_values(x, u_nodal);

        // Take care of Dirichlet BC: don't contribute to Dirichlet nodes and set nodal
        // values of the current iterate to Dirichlet BC values.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(n);
          if (dirichlet_node(info, I)) {
            element.mark_row_invalid(n);
            u_nodal[n] = u_exact(x_nodal[n], y_nodal[n], z_nodal[n]);
          }
        }

        // evaluate u and its partial derivatives at quadrature points
        element.evaluate(u_nodal, u, u_x, u_y, u_z);

        // evaluate F at quadrature points
        element.evaluate(F_nodal, Fq);

        // loop over all quadrature points
        for (int q = 0; q < Nq; ++q) {
          auto W = element.weight(q);

          // loop over all test functions
          for (int t = 0; t < Nk; ++t) {
            const auto &psi = element.chi(q, t);

            R_nodal[t] += W * (u_x[q] * psi.dx + u_y[q] * psi.dy + u_z[q] * psi.dz
                               - Fq[q] * psi.val);
          }
        }

        // loop over all faces
        for (int f = 0; f < fem::q13d::n_faces; ++f) {
          auto nodes = fem::q13d::incident_nodes[f];
          // Loop over all nodes corresponding to this face. A face is a part of the
          // Neumann boundary if all four nodes are Neumann nodes. If a node is *both* a
          // Neumann and a Dirichlet node (this may happen), then we treat it as a Neumann
          // node here: add_contribution() will do the right thing later.
          bool neumann = true;
          for (int n = 0; n < 4; ++n) {
            if (not (node_type[nodes[n]] == NODE_BOUNDARY)) {
              neumann = false;
            }
          }

          if (neumann) {
            face.reset(f, z_nodal);

            // compute physical coordinates of quadrature points on this face
            face.evaluate(x_nodal, xq);
            face.evaluate(y_nodal, yq);
            face.evaluate(z_nodal.data(), zq);

            // loop over all quadrature points
            for (int q = 0; q < face.n_pts(); ++q) {
              auto W = face.weight(q);
              auto N = face.normal(q);

              // loop over all test functions
              for (int t = 0; t < Nk; ++t) {
                auto psi = face.chi(q, t);

                R_nodal[t] += - W * psi * G(xq[q], yq[q], zq[q], N);
              }
            }
          } // end of "if (neumann)"
        } // end of the loop over element faces

        element.add_contribution(R_nodal, R);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k
}

void Poisson3::compute_jacobian(DMDALocalInfo *info, const double ***x, Mat A, Mat J) {
  (void) x;

  // Zero out the Jacobian in preparation for updating it.
  PetscErrorCode ierr = MatZeroEntries(J);
  PISM_CHK(ierr, "MatZeroEntries");

  // Stencil width of 1 is not very important, but if info->sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info->sw == 1);

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info->mx - 1),
    dy = (y_max - y_min) / (info->my - 1);

  fem::Q1Element3 element(*info, dx, dy, fem::Q13DQuadrature8());

  DataAccess<Parameters**> P(info->da, 2, GHOSTED);

  const int Nk = fem::q13d::n_chi;
  const int Nq = element.n_pts();

  int node_type[Nk];
  std::vector<double> z_nodal(Nk);

  // loop over all the elements that have at least one owned node
  for (int k = info->gzs; k < info->gzs + info->gzm - 1; k++) {
    for (int j = info->gys; j < info->gys + info->gym - 1; j++) {
      for (int i = info->gxs; i < info->gxs + info->gxm - 1; i++) {

        // Element-local Jacobian matrix (there are Nk vector valued degrees of freedom
        // per element, for a total of Nk*Nk = 64 entries in the local Jacobian.
        double K[Nk][Nk];
        ierr = PetscMemzero(K, sizeof(K));
        PISM_CHK(ierr, "PetscMemzero");

        // Compute coordinates of the nodes of this element and fetch node types.
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(i, j, k, n);

          auto p = P[I.j][I.i];

          node_type[n] = p.node_type;

          z_nodal[n] = z(p.bed, p.thickness, info->mz, I.k);
        }

        // skip ice-free elements
        {
          // an element is exterior if one or more of its nodes are "exterior"
          bool exterior = false;
          for (int n = 0; n < Nk; ++n) {
            if (node_type[n] == NODE_EXTERIOR) {
              exterior = true;
              break;
            }
          }

          if (exterior) {
            continue;
          }
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        element.reset(i, j, k, z_nodal);

        // Don't contribute to Dirichlet nodes
        for (int n = 0; n < Nk; ++n) {
          auto I = element.local_to_global(n);
          if (dirichlet_node(info, I)) {
            element.mark_row_invalid(n);
            element.mark_col_invalid(n);
          }
        }

        // loop over all quadrature points
        for (int q = 0; q < Nq; ++q) {
          auto W = element.weight(q);

          // loop over test and trial functions, computing the upper-triangular part of
          // the element Jacobian
          for (int t = 0; t < Nk; ++t) {
            auto psi = element.chi(q, t);
            for (int s = t; s < Nk; ++s) {
              auto phi = element.chi(q, s);

              K[t][s] += W * (phi.dx * psi.dx + phi.dy * psi.dy + phi.dz * psi.dz);
            }
          }
        } // end of the loop over q

        // fill the lower-triangular part using the fact that J is symmetric
        for (int t = 0; t < Nk; ++t) {
          for (int s = 0; s < t; ++s) {
            K[t][s] = K[s][t];
          }
        }

        element.add_contribution(&K[0][0], J);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k

  // take care of Dirichlet nodes (both explicit and grid points outside the domain)
  //
  // here we loop over all the *owned* nodes
  for (int k = info->zs; k < info->zs + info->zm; k++) {
    for (int j = info->ys; j < info->ys + info->ym; j++) {
      for (int i = info->xs; i < info->xs + info->xm; i++) {
        if ((int)P[j][i].node_type == NODE_EXTERIOR or dirichlet_node(info, {i, j, k})) {
          double
            dz = std::max(P[j][i].thickness, m_grid_info.min_thickness) / (info->mz - 1),
            scaling = dirichlet_scale(dx, dy, dz);
          MatStencil row;
          row.k = k;
          row.j = j;
          row.i = i;
          ierr = MatSetValuesBlockedStencil(J, 1, &row, 1, &row, &scaling, ADD_VALUES);
          PISM_CHK(ierr, "MatSetValuesBlockedStencil"); // this may throw
        }
      }
    }
  }

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

PetscErrorCode Poisson3::function_callback(DMDALocalInfo *info,
                                           const double ***x, double ***f,
                                           CallbackData *data) {
  try {
    data->solver->compute_residual(info, x, f);
  } catch (...) {
    MPI_Comm com = MPI_COMM_SELF;
    PetscErrorCode ierr = PetscObjectGetComm((PetscObject)data->da, &com); CHKERRQ(ierr);
    handle_fatal_errors(com);
    SETERRQ(com, 1, "A PISM callback failed");
  }
  return 0;
}

PetscErrorCode Poisson3::jacobian_callback(DMDALocalInfo *info,
                                           const double ***x,
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

Poisson3::Poisson3(IceGrid::ConstPtr grid, int Mz, int n_levels)
  : ShallowStressBalance(grid) {

  auto pism_da = grid->get_dm(1, 0);

  int ierr = setup(*pism_da, Mz, n_levels);
  if (ierr != 0) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "Failed to allocate a Poisson3 instance");
  }

  {
    int mz = Mz + pad(Mz, n_levels);
    std::vector<double> sigma(mz);
    double dz = 1.0 / (mz - 1);
    for (int i = 0; i < mz; ++i) {
      sigma[i] = i * dz;
    }
    sigma.back() = 1.0;

    std::map<std::string,std::string> z_attrs =
      {{"axis", "Z"},
       {"long_name", "scaled Z-coordinate in the ice (z_base=0, z_surface=1)"},
       {"units", "1"},
       {"positive", "up"}};

    m_solution.reset(new IceModelVec3Custom(grid, "solution", "z_sigma", sigma, z_attrs));
    m_solution->set_attrs("diagnostic", "solution", "1", "1", "", 0);

    m_exact.reset(new IceModelVec3Custom(grid, "exact", "z_sigma", sigma, z_attrs));
    m_exact->set_attrs("diagnostic", "exact", "1", "1", "", 0);
  }
}

/*!
 * Compute node type using domain thickness and the thickness threshold `min_thickness`.
 *
 * A node is *interior* if all four elements it belongs to contain ice.
 *
 * A node is *exterior* if it belongs to zero icy elements.
 *
 * A node that is neither interior nor exterior is a *boundary* node.
 */
void compute_node_type(DM da, double min_thickness) {
  // Note that P provides access to a ghosted copy of 2D parameters, so changes to P have
  // no lasting effect.
  DataAccess<Parameters**> P(da, 2, GHOSTED);

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(da, &info); PISM_CHK(ierr, "DMDAGetLocalInfo");

  // loop over all the owned nodes and reset node type
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      P[j][i].node_type = 0;
    }
  }

  // Note that dx, dy, and quadrature don't matter here.
  fem::Q1Element2 E(info, 1.0, 1.0, fem::Q1Quadrature1());

  Parameters p[fem::q1::n_chi];

  // Loop over all the elements with at least one owned node and compute the number of icy
  // elements each node belongs to.
  for (int j = info.gys; j < info.gys + info.gym - 1; j++) {
    for (int i = info.gxs; i < info.gxs + info.gxm - 1; i++) {
      E.reset(i, j);

      E.nodal_values((Parameters**)P, p);

      // An element is "interior" (contains ice) if all of its nodes have thickness above
      // the threshold
      bool interior = true;
      for (int k = 0; k < fem::q1::n_chi; ++k) {
        if (p[k].thickness < min_thickness) {
          interior = false;
          break;
        }
      }

      for (int k = 0; k < fem::q1::n_chi; ++k) {
        int ii, jj;
        E.local_to_global(k, ii, jj);
        P[jj][ii].node_type += interior;
      }
    }
  }

  DataAccess<Parameters**> result(da, 2, NOT_GHOSTED);

  // Loop over all the owned nodes and turn the number of "icy" elements this node belongs
  // to into node type.
  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {

      switch ((int)P[j][i].node_type) {
      case 4:
        result[j][i].node_type = NODE_INTERIOR;
        break;
      case 0:
        result[j][i].node_type = NODE_EXTERIOR;
        break;
      default:
        result[j][i].node_type = NODE_BOUNDARY;
      }
    }
  }
}


/*!
 * Restrict 2D and 3D model parameters from a fine grid to a coarse grid.
 *
 * Re-compute node types from geometry.
 *
 * This hook is called every time SNES needs to update coarse-grid data.
 *
 * FIXME: parameters restricted by this hook do not change from one SNES iteration to the
 * next, so we can return early after the first one.
 */
static PetscErrorCode p3_restriction_hook(DM fine,
                                          Mat mrestrict, Vec rscale, Mat inject,
                                          DM coarse, void *ctx) {
  // Get rid of warnings about unused arguments
  (void) mrestrict;
  (void) rscale;
  (void) inject;
  GridInfo *grid_info = (GridInfo*)ctx;

  PetscErrorCode ierr;
  ierr = restrict_data(fine, coarse, "2D_DM"); CHKERRQ(ierr);
  ierr = restrict_data(fine, coarse, "3D_DM"); CHKERRQ(ierr);

  compute_node_type(coarse, grid_info->min_thickness);

  return 0;
}

PetscErrorCode p3_coarsening_hook(DM dm_fine, DM dm_coarse, void *ctx) {
  PetscErrorCode ierr;

  ierr = setup_level(dm_coarse, *(GridInfo*)ctx); CHKERRQ(ierr);

  ierr = DMCoarsenHookAdd(dm_coarse, p3_coarsening_hook, p3_restriction_hook, ctx); CHKERRQ(ierr);

  // 2D
  ierr = create_restriction(dm_fine, dm_coarse, "2D_DM"); CHKERRQ(ierr);

  // 3D
  ierr = create_restriction(dm_fine, dm_coarse, "3D_DM"); CHKERRQ(ierr);

  return 0;
}

PetscErrorCode Poisson3::setup(DM pism_da, int Mz, int n_levels) {
  PetscErrorCode ierr;
  // DM
  {
    PetscInt dim, Mx, My, Nx, Ny;
    PetscInt
      Nz            = 1,
      dof           = 1,
      stencil_width = 1;

    ierr = DMDAGetInfo(pism_da,
                       &dim,
                       &Mx,
                       &My,
                       NULL,             // Mz
                       &Nx,              // number of processors in y-direction
                       &Ny,              // number of processors in x-direction
                       NULL,             // ditto, z-direction
                       NULL,             // number of degrees of freedom per node
                       NULL,             // stencil width
                       NULL, NULL, NULL, // types of ghost nodes at the boundary
                       NULL);            // stencil width
    CHKERRQ(ierr);

    assert(dim == 2);

    const PetscInt *lx, *ly;
    ierr = DMDAGetOwnershipRanges(pism_da, &lx, &ly, NULL); CHKERRQ(ierr);

    // make copies of lx and ly so that we can pad the domain
    std::vector<PetscInt>  new_lx(Nx), new_ly(Ny);
    {
      for (int k = 0; k < Nx; ++k) {
        new_lx[k] = lx[k];
      }
      for (int k = 0; k < Ny; ++k) {
        new_ly[k] = ly[k];
      }
    }

    double
      x_max = m_grid->Lx(),
      x_min = -x_max,
      y_max = m_grid->Ly(),
      y_min = -y_max;

    // pad the domain and the grid to allow for n_levels multigrid levels
    {
      // x direction
      {
        int pad_x = pad(Mx, n_levels);

        new_lx[Nx - 1] += pad_x;
        Mx             += pad_x;
        x_max          += pad_x * m_grid->dx();
      }

      // y direction
      {
        int pad_y = pad(My, n_levels);

        new_ly[Ny - 1] += pad_y;
        My             += pad_y;
        y_max          += pad_y * m_grid->dy();
      }

      // z direction
      Mz += pad(Mz, n_levels);
    }

    ierr = DMDACreate3d(PETSC_COMM_WORLD,
                        DM_BOUNDARY_NONE, DM_BOUNDARY_NONE, DM_BOUNDARY_NONE,
                        DMDA_STENCIL_BOX,
                        Mx, My, Mz,
                        Nx, Ny, Nz,
                        dof,           // dof
                        stencil_width, // stencil width
                        new_lx.data(), new_ly.data(), NULL,
                        m_da.rawptr()); CHKERRQ(ierr);

    ierr = DMSetFromOptions(m_da); CHKERRQ(ierr);

    ierr = DMSetUp(m_da); CHKERRQ(ierr);

    double min_thickness = 0.1;

    m_grid_info = {x_min, x_max,
                   y_min, y_max,
                   min_thickness,
                   sizeof(Parameters)/sizeof(double)};

    // set up 2D and 3D parameter storage
    ierr = setup_level(m_da, m_grid_info); CHKERRQ(ierr);

    // tell PETSc how to coarsen this grid and how to restrict data to a coarser grid
    ierr = DMCoarsenHookAdd(m_da, p3_coarsening_hook, p3_restriction_hook, &m_grid_info); CHKERRQ(ierr);
  }

  // Vecs, Mat
  {
    ierr = DMCreateGlobalVector(m_da, m_x.rawptr()); CHKERRQ(ierr);

    // ierr = DMCreateMatrix(m_da, m_J.rawptr()); CHKERRQ(ierr);
  }

  // SNES
  {
    ierr = SNESCreate(m_grid->com, m_snes.rawptr()); CHKERRQ(ierr);

    // ierr = SNESSetOptionsPrefix(m_snes, "poisson3_"); CHKERRQ(ierr);

    ierr = SNESSetDM(m_snes, m_da); CHKERRQ(ierr);

    m_callback_data.da = m_da;
    m_callback_data.solver = this;

    ierr = DMDASNESSetFunctionLocal(m_da, INSERT_VALUES,
                                    (DMDASNESFunction)function_callback,
                                    &m_callback_data); CHKERRQ(ierr);

    ierr = DMDASNESSetJacobianLocal(m_da,
                                    (DMDASNESJacobian)jacobian_callback,
                                    &m_callback_data);
    CHKERRQ(ierr);

    ierr = SNESSetFromOptions(m_snes); CHKERRQ(ierr);
  }

  // set the initial guess
  // FIXME: I will need to read this from a file
  ierr = VecSet(m_x, 0.0); CHKERRQ(ierr);

  return 0;
}

/*!
 * Bottom surface elevation
 */
static double b(double x, double y) {
  (void) x;
  (void) y;
  return 0.0;
}

/*!
 * Domain thickness
 */
static double H(double x, double y) {
  double w = 1.0;
  if (fabs(x) <= w and fabs(y) <= w) {
    return 1.0;
  }
  return 0.0;
}

/*!
 * Set 2D parameters on the finest grid.
 */
void Poisson3::init_2d_parameters() {

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(m_da, &info);
  PISM_CHK(ierr, "DMDAGetLocalInfo");

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  DataAccess<Parameters**> P(m_da, 2, NOT_GHOSTED);

  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      double x = xy(x_min, dx, i);
      double y = xy(y_min, dy, j);

      P[j][i].bed = b(x, y);
      P[j][i].thickness = H(x, y);
    }
  }

  compute_node_type(m_da, m_grid_info.min_thickness);
}

/*!
 * Set 3D parameters on the finest grid.
 */
void Poisson3::init_3d_parameters() {

  DMDALocalInfo info;
  int ierr = DMDAGetLocalInfo(m_da, &info);
  PISM_CHK(ierr, "DMDAGetLocalInfo");

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    x_max = m_grid_info.x_max,
    y_min = m_grid_info.y_min,
    y_max = m_grid_info.y_max,
    dx = (x_max - x_min) / (info.mx - 1),
    dy = (y_max - y_min) / (info.my - 1);

  DataAccess<Parameters**> P2(m_da, 2, NOT_GHOSTED);
  DataAccess<double***> P3(m_da, 3, NOT_GHOSTED);

  for (int k = info.zs; k < info.zs + info.zm; k++) {
    for (int j = info.ys; j < info.ys + info.ym; j++) {
      for (int i = info.xs; i < info.xs + info.xm; i++) {
        double
          xx = xy(x_min, dx, i),
          yy = xy(y_min, dy, j),
          b  = P2[j][i].bed,
          H  = P2[j][i].thickness,
          zz = z(b, H, info.mz, k);

        P3[k][j][i] = F(xx, yy, zz);
      }
    }
  }
}

Poisson3::~Poisson3() {
  // empty
}

void Poisson3::exact_solution(IceModelVec3Custom &result) {
  IceModelVec::AccessList list{&result};

  // Compute grid spacing from domain dimensions and the grid size
  double
    x_min = m_grid_info.x_min,
    y_min = m_grid_info.y_min,
    dx = m_grid->dx(),
    dy = m_grid->dy();

  int Mz = result.levels().size();

  DataAccess<Parameters**> P(m_da, 2, NOT_GHOSTED);

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    double
      xx = xy(x_min, dx, i),
      yy = xy(y_min, dy, j),
      b = P[j][i].bed,
      H = P[j][i].thickness;

    if ((int)P[j][i].node_type == NODE_EXTERIOR) {
      result.set_column(i, j, u_exterior);
      continue;
    }

    auto c = result.get_column(i, j);

    for (int k = 0; k < Mz; ++k) {
      double zz = z(b, H, Mz, k);

      c[k] = u_exact(xx, yy, zz);
    }
  }
}

double Poisson3::error() const {
  IceModelVec3Custom difference(m_grid, "difference", "z_sigma",
                                m_exact->levels(), {});

  difference.copy_from(*m_exact);
  difference.add(-1.0, *m_solution);

  return difference.norm(NORM_INFINITY);
}

void Poisson3::update(const Inputs &inputs, bool) {
  (void) inputs;

  init_2d_parameters();
  init_3d_parameters();

  int ierr = SNESSolve(m_snes, NULL, m_x); PISM_CHK(ierr, "SNESSolve");

  exact_solution(*m_exact);

  {
    double ***x = nullptr;
    ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

    int Mz = m_solution->levels().size();

    IceModelVec::AccessList list{m_solution.get()};

    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      auto c = m_solution->get_column(i, j);

      for (int k = 0; k < Mz; ++k) {
        c[k] = x[k][j][i];
      }
    }

    ierr = DMDAVecRestoreArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecRestoreArray");
  }
}

IceModelVec3Custom::Ptr Poisson3::solution() const {
  return m_solution;
}

IceModelVec3Custom::Ptr Poisson3::exact() const {
  return m_exact;
}

} // end of namespace stressbalance
} // end of namespace pism
