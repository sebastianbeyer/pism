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

using std::pow;
using std::fabs;

#include "Poisson3.hh"
#include "pism/util/fem/FEM.hh"
#include "pism/util/error_handling.hh"

namespace pism {
namespace stressbalance {

struct Parameters {
  double bed;
  double thickness;
};

enum ArrayType {GHOSTED, NOT_GHOSTED};
/*!
 * This template class manages access to 2D and 3D Vecs stored in a DM using
 * `PetscObjectCompose`. Performs cleanup at the end of scope.
 *
 * @param[in] da SNES DM for the solution containing 2D and 3D DMs and Vecs
 * @param[in] dim number of dimensions (2 or 3)
 * @param[in] type NOT_GHOSTED -- for setting parameters; GHOSTED -- for accessing ghosts
 *                 during residual and Jacobian evaluation
 */
template<typename T>
class DataInput {
public:
  DataInput(DM da, int dim, ArrayType type)
    : m_local(type == GHOSTED) {
    int ierr;

    assert(dim == 2 or dim == 3);

    if (dim == 2) {
      ierr = setup(da, "2D_DM", "2D_Vec");
    } else {
      ierr = setup(da, "3D_DM", "3D_Vec");
    }

    if (ierr != 0) {
      throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                    "Failed to create an DataInput instance");
    }
  }

  int setup(DM da, const char *dm_name, const char *vec_name) {
    PetscErrorCode ierr;

    m_com = MPI_COMM_SELF;
    ierr = PetscObjectGetComm((PetscObject)da, &m_com); CHKERRQ(ierr);

    ierr = PetscObjectQuery((PetscObject)da, dm_name, (PetscObject*)&m_da); CHKERRQ(ierr);

    if (!m_da) {
      SETERRQ(m_com, 1, "Failed to get the inner DM");
    }

    Vec X;
    ierr = PetscObjectQuery((PetscObject)da, vec_name, (PetscObject*)&X); CHKERRQ(ierr);

    if (!X) {
      SETERRQ(m_com, 1, "Failed to get the inner Vec");
    }

    if (m_local) {
      ierr = DMGetLocalVector(m_da, &m_x); CHKERRQ(ierr);

      ierr = DMGlobalToLocalBegin(m_da, X, INSERT_VALUES, m_x); CHKERRQ(ierr);

      ierr = DMGlobalToLocalEnd(m_da, X, INSERT_VALUES, m_x); CHKERRQ(ierr);
    } else {
      m_x = X;
    }

    ierr = DMDAVecGetArray(m_da, m_x, &m_a); CHKERRQ(ierr);

    return 0;
  }

  ~DataInput() {
    try {
      PetscErrorCode ierr;
      ierr = DMDAVecRestoreArray(m_da, m_x, &m_a);
      PISM_CHK(ierr, "DMDAVecRestoreArray");

      if (m_local) {
        ierr = DMRestoreLocalVector(m_da, &m_x);
        PISM_CHK(ierr, "DMRestoreLocalVector");
      }
    } catch (...) {
      handle_fatal_errors(m_com);
    }
  }

  operator T() {
    return m_a;
  }
private:
  MPI_Comm m_com;
  bool m_local;
  DM m_da;
  Vec m_x;
  T m_a;
};

/*!
 * dot product (used to compute normal derivatives)
 */
static double dot(const std::vector<double> &a, const std::vector<double> &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/*!
 * x and y coordinates of the nodes
 *
 * @param[in] L domain half-width
 * @param[in] delta grid spacing
 * @param[in] k node index
 */
static double xy(double L, double delta, int k) {
  return -L + k * delta;
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
  return (I.i == info->mx - 1) or (I.j == info->my - 1) or (I.k == info->mz - 1);
}

/*!
 * Returns true if a node is in the Neumann part of the boundary, false otherwise.
 */
static bool neumann_node(const DMDALocalInfo *info, const fem::Element3::GlobalIndex& I) {
  (void) info;
  return I.i == 0 or I.j == 0 or I.k == 0;
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
static double G(double x, double y, double z, double b) {

  double u_x = (y * pow(z + 1, 2.0) -
                (4.0 * (x + 2) * (y + 1)) / pow(pow(y + 1, 2.0) + pow(x + 2, 2.0), 2.0));
  double u_y = x * pow(z + 1, 2.0) + 2.0 / (pow(y + 1, 2.0) + pow(x + 2, 2.0)) -
    (4.0 * pow(y + 1, 2.0)) / pow(pow(y + 1, 2.0) + pow(x + 2, 2.0), 2.0);
  double u_z = 2 * x * y * (z + 1);

  double eps = 1e-12;
  if (fabs(x - (-1.0)) < eps) {
    return u_x;
  } else if (fabs(y - (-1.0)) < eps) {
    return u_y;
  } else if (fabs(z - b) < eps) {
    // normal to the bottom surface {-b_x, -b_y, 1}
    std::vector<double> n = {-1, -1, 1}; // magnitude: sqrt(3)

    return dot({u_x, u_y, u_z}, n) / sqrt(3); // normalize
  } else {
    // We are not on a Neumann boundary. This value will not be used.
    return 0.0;
  }
}

void Poisson3::compute_residual(DMDALocalInfo *info,
                                const double ***x, double ***R) {
  // Stencil width of 1 is not very important, but if info->sw > 1 will lead to more
  // redundant computation (we would be looping over elements that don't contribute to any
  // owned nodes).
  assert(info->sw == 1);

  // Compute grid spacing from domain dimensions and the grid size
  double
    Lx = m_grid->Lx(),
    Ly = m_grid->Ly(),
    dx = 2.0 * Lx / (info->mx - 1),
    dy = 2.0 * Ly / (info->my - 1);

  fem::Q1Element3 E(*info, dx, dy, fem::Q13DQuadrature8());
  fem::Q1Element3Face E_face(dx, dy, fem::Q1Quadrature4());

  DataInput<Parameters**> P(info->da, 2, GHOSTED);
  DataInput<double***> F(info->da, 3, GHOSTED);

  // Compute the residual at Dirichlet BC nodes and reset the residual to zero elsewhere.
  //
  // Setting it to zero is necessary because we call DMDASNESSetFunctionLocal() with
  // INSERT_VALUES.
  //
  // here we loop over all the *owned* nodes
  for (int k = info->zs; k < info->zs + info->zm; k++) {
    for (int j = info->ys; j < info->ys + info->ym; j++) {
      for (int i = info->xs; i < info->xs + info->xm; i++) {
        if (dirichlet_node(info, {i, j, k})) {
          double
            xx = xy(Lx, dx, i),
            yy = xy(Ly, dy, j),
            b  = P[j][i].bed,
            H  = P[j][i].thickness,
            zz = z(b, H, info->mz, k);

          // FIXME: scaling goes here
          R[k][j][i] = u_exact(xx, yy, zz) - x[k][j][i];
        } else {
          R[k][j][i] = 0.0;
        }
      }
    }
  }

  // values at element nodes
  const int Nk_max = 8;
  int Nk = E.n_chi();
  assert(Nk <= Nk_max);

  double
    x_nodal[Nk_max], y_nodal[Nk_max],
    R_nodal[Nk_max], u_nodal[Nk_max], b_nodal[Nk_max], F_nodal[Nk_max];
  std::vector<double> z_nodal(Nk);

  // values at quadrature points
  const int Nq_max = 16;
  int Nq = E.n_pts();
  assert(Nq <= Nq_max);

  double u[Nq_max], u_x[Nq_max], u_y[Nq_max], u_z[Nq_max];
  double xq[Nq_max], yq[Nq_max], zq[Nq_max], bq[Nq_max], Fq[Nq_max];

  // make sure that xq, yq, zq and big enough for quadrature points on element faces
  assert(E_face.n_pts() <= Nq_max);

  // loop over all the elements that have at least one owned node
  for (int k = info->gzs; k < info->gzs + info->gzm - 1; k++) {
    for (int j = info->gys; j < info->gys + info->gym - 1; j++) {
      for (int i = info->gxs; i < info->gxs + info->gxm - 1; i++) {

        // Reset element residual to zero in preparation.
        for (int n = 0; n < Nk; ++n) {
          R_nodal[n] = 0.0;
        }

        // Compute coordinates of the nodes of this element and fetch nodal values of the
        // bed elevation.
        for (int n = 0; n < Nk; ++n) {
          auto I = E.local_to_global(i, j, k, n);

          double H = P[I.j][I.i].thickness;

          b_nodal[n] = P[I.j][I.i].bed;

          x_nodal[n] = xy(Lx, dx, I.i);
          y_nodal[n] = xy(Ly, dy, I.j);
          z_nodal[n] = z(b_nodal[n], H, info->mz, I.k);
        }

        // compute values of chi, chi_x, chi_y, chi_z and quadrature weights at quadrature
        // points on this physical element
        E.reset(i, j, k, z_nodal);

        // Get nodal values of F.
        E.nodal_values((double***)F, F_nodal);

        // Get nodal values of u.
        E.nodal_values(x, u_nodal);

        // Take care of Dirichlet BC: don't contribute to Dirichlet nodes and set nodal
        // values of the current iterate to Dirichler BC values.
        for (int n = 0; n < Nk; ++n) {
          auto I = E.local_to_global(n);
          if (dirichlet_node(info, I)) {
            E.mark_row_invalid(n);
            u_nodal[n] = u_exact(x_nodal[n], y_nodal[n], z_nodal[n]);
          }
        }

        // evaluate u and its partial derivatives at quadrature points
        E.evaluate(u_nodal, u, u_x, u_y, u_z);

        // coordinates of quadrature points
        E.evaluate(x_nodal, xq);
        E.evaluate(y_nodal, yq);
        E.evaluate(z_nodal.data(), zq);

        // evaluate F at quadrature points
        E.evaluate(F_nodal, Fq);

        // loop over all quadrature points
        for (int q = 0; q < Nq; ++q) {
          auto W = E.weight(q);

          // loop over all test functions
          for (int t = 0; t < Nk; ++t) {
            const auto &psi = E.chi(q, t);

            // FIXME: scaling goes here
            R_nodal[t] += W * (u_x[q] * psi.dx + u_y[q] * psi.dy + u_z[q] * psi.dz
                               - Fq[q] * psi.val);
          }
        }

        // loop over all faces
        for (int face = 0; face < fem::q13d::n_faces; ++face) {
          auto nodes = fem::q13d::incident_nodes[face];
          // loop over all nodes corresponding to this face. A face is a part of the
          // Neumann boundary if all four nodes are Neumann nodes. If a node is *both* a
          // Neumann and a Dirichlet node (this may happen), then we treat it as a Neumann
          // node here: add_contribution() will do the right thing later.
          bool neumann = true;
          for (int n = 0; n < 4; ++n) {
            auto I = E.local_to_global(nodes[n]);
            if (not neumann_node(info, I)) {
              neumann = false;
              break;
            }
          }

          if (not neumann) {
            continue;
          }

          E_face.reset(face, z_nodal);

          // compute physical coordinates of quadrature points on this face
          E_face.evaluate(x_nodal, xq);
          E_face.evaluate(y_nodal, yq);
          E_face.evaluate(z_nodal.data(), zq);

          // Compute the bed elevation at (below) quadrature points. This is needed to
          // compute G() below
          E_face.evaluate(b_nodal, bq);

          // loop over all quadrature points
          for (int q = 0; q < E_face.n_pts(); ++q) {
            auto W = E_face.weight(q);

            // loop over all test functions
            for (int t = 0; t < Nk; ++t) {
              auto psi = E_face.chi(q, t);

              // FIXME: scaling goes here
              R_nodal[t] += W * psi * G(xq[q], yq[q], zq[q], bq[q]);
            }
          }
        } // end of the loop over element faces

        E.add_contribution(R_nodal, R);
      } // end of the loop over i
    } // end of the loop over j
  } // end of the loop over k
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

/*!
 * Set up storage for 2D and 3D data inputs (DMDAs and Vecs)
 */
static PetscErrorCode setup_level(DM dm, double Lx, double Ly) {
  PetscErrorCode ierr;

  MPI_Comm comm;
  ierr = PetscObjectGetComm((PetscObject)dm, &comm); CHKERRQ(ierr);

  // Get grid information
  PetscInt Mx, My, Mz, mx, my, mz, stencil_width;
  DMDAStencilType stencil_type;
  const PetscInt *lx, *ly, *lz;
  {
    ierr = DMDAGetInfo(dm,
                       NULL,          // dimensions
                       &Mx, &My, &Mz, // grid size
                       &mx, &my, &mz, // number of processors in each direction
                       NULL,          // number of degrees of freedom
                       &stencil_width,
                       NULL, NULL, NULL, // types of ghost nodes at the boundary
                       &stencil_type); CHKERRQ(ierr);

    ierr = DMDAGetOwnershipRanges(dm, &lx, &ly, &lz); CHKERRQ(ierr);
  }

  // Create a 2D DMDA and a global Vec, then stash them in dm.
  {
    // compute the number of parameters per map-plane location
    int dof = sizeof(Parameters)/sizeof(double);

    DM  da;
    Vec parameters;

    ierr = DMDACreate2d(comm,
                        DM_BOUNDARY_NONE, DM_BOUNDARY_NONE,
                        stencil_type,
                        Mx, My,
                        mx, my,
                        dof,
                        stencil_width,
                        lx, ly,
                        &da); CHKERRQ(ierr);

    ierr = DMSetUp(da); CHKERRQ(ierr);

    ierr = DMCreateGlobalVector(da, &parameters); CHKERRQ(ierr);

    ierr = PetscObjectCompose((PetscObject)dm, "2D_DM", (PetscObject)da); CHKERRQ(ierr);
    ierr = PetscObjectCompose((PetscObject)dm, "2D_Vec", (PetscObject)parameters); CHKERRQ(ierr);

    ierr = DMDestroy(&da); CHKERRQ(ierr);

    ierr = VecDestroy(&parameters); CHKERRQ(ierr);
  }

  // Create a 3D DMDA and a global Vec, then stash them in dm.
  {
    DM  da;
    Vec parameters;
    int dof = 1;

    ierr = DMDACreate3d(comm,
                        DM_BOUNDARY_NONE, DM_BOUNDARY_NONE, DM_BOUNDARY_NONE,
                        stencil_type,
                        Mx, My, Mz,
                        mx, my, mz,
                        dof,
                        stencil_width,
                        lx, ly, lz,
                        &da); CHKERRQ(ierr);

    ierr = DMSetUp(da); CHKERRQ(ierr);

    ierr = DMCreateGlobalVector(da, &parameters); CHKERRQ(ierr);

    ierr = PetscObjectCompose((PetscObject)dm, "3D_DM", (PetscObject)da); CHKERRQ(ierr);
    ierr = PetscObjectCompose((PetscObject)dm, "3D_Vec", (PetscObject)parameters); CHKERRQ(ierr);

    ierr = DMDestroy(&da); CHKERRQ(ierr);

    ierr = VecDestroy(&parameters); CHKERRQ(ierr);
  }

  // get refinement level
  PetscInt level = 0;
  {
    PetscInt refinelevel, coarsenlevel;
    ierr = DMGetRefineLevel(dm, &refinelevel); CHKERRQ(ierr);
    ierr = DMGetCoarsenLevel(dm, &coarsenlevel); CHKERRQ(ierr);
    level = refinelevel - coarsenlevel;
  }

  // report
  {
    ierr = PetscPrintf(comm,
                       "Level %D domain size (m) %8.2g x %8.2g,"
                       " num elements %3d x %3d x %3d (%8d), size (m) %g x %g\n",
                       level, Lx, Ly,
                       Mx, My, Mz, Mx*My*Mz, Lx / (Mx - 1), Ly / (My - 1)); CHKERRQ(ierr);
  }
  return 0;
}

/*! @brief Create the restriction matrix.
 *
 * The result of this call is attached to `dm_fine` under `mat_name`.
 *
 * @param[in] fine DM corresponding to the fine grid
 * @param[in] coarse DM corresponding to the coarse grid
 * @param[in] dm_name name of the DM for 2D or 3D parameters
 * @param[in] mat_name name to use when attaching the restriction matrix to `fine`
 */
static PetscErrorCode create_restriction(DM fine, DM coarse,
                                         const char *dm_name, const char *mat_name) {
  PetscErrorCode ierr;
  DM da_fine, da_coarse;
  Mat mat;

  /* Get the DM for parameters from the fine grid DM */
  ierr = PetscObjectQuery((PetscObject)fine, dm_name, (PetscObject *)&da_fine); CHKERRQ(ierr);
  if (!da_fine) {
    SETERRQ1(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "No %s composed with given DMDA", dm_name);
  }

  /* 2. get the DM for parameters from the coarse grid DM */
  ierr = PetscObjectQuery((PetscObject)coarse, dm_name, (PetscObject *)&da_coarse); CHKERRQ(ierr);
  if (!da_coarse) {
    SETERRQ1(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "No %s composed with given DMDA", dm_name);
  }

  /* call DMCreateInterpolation */
  ierr = DMCreateInterpolation(da_coarse, da_fine, &mat, PETSC_NULL); CHKERRQ(ierr);

  /* attach to the fine grid DM */
  ierr = PetscObjectCompose((PetscObject)fine, mat_name, (PetscObject)mat); CHKERRQ(ierr);
  ierr = MatDestroy(&mat);
  CHKERRQ(ierr);

  return 0;
}


/*! @brief Restrict model parameters from the `fine` grid onto the `coarse` grid.
 *
 * This function uses the restriction matrix created by coarsening_hook().
 */
static PetscErrorCode restrict_data(DM fine, DM coarse,
                                    const char *dm_name,
                                    const char *mat_name,
                                    const char *vec_name) {
  PetscErrorCode ierr;
  Vec X_fine, X_coarse;
  DM da_fine, da_coarse;
  Mat mat;

  PetscFunctionBegin;

  /* get the restriction matrix from the fine grid DM */
  ierr = PetscObjectQuery((PetscObject)fine, mat_name, (PetscObject *)&mat); CHKERRQ(ierr);
  if (!mat) {
    SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Failed to get the restriction matrix");
  }

  /* get the DMDA from the fine grid DM */
  ierr = PetscObjectQuery((PetscObject)fine, dm_name, (PetscObject *)&da_fine); CHKERRQ(ierr);
  if (!da_fine) {
    SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Failed to get the fine grid DM");
  }

  /* get the storage vector from the fine grid DM */
  ierr = PetscObjectQuery((PetscObject)fine, vec_name, (PetscObject *)&X_fine); CHKERRQ(ierr);
  if (!X_fine) {
    SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Failed to get the fine grid Vec");
  }

  /* get the DMDA from the coarse grid DM */
  ierr = PetscObjectQuery((PetscObject)coarse, dm_name, (PetscObject *)&da_coarse); CHKERRQ(ierr);
  if (!da_coarse) {
    SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Failed to get the coarse grid DM");
  }

  /* get the storage vector from the coarse grid DM */
  ierr = PetscObjectQuery((PetscObject)coarse, vec_name, (PetscObject *)&X_coarse); CHKERRQ(ierr);
  if (!X_coarse) {
    SETERRQ(PETSC_COMM_SELF, PETSC_ERR_ARG_WRONG, "Failed to get the coarse grid Vec");
  }

  ierr = MatRestrict(mat, X_fine, X_coarse); CHKERRQ(ierr);

  return 0;
}

/*!
 * Restrict 2D and 3D model parameters from a fine grid to a coarse grid.
 *
 * This hook is called every time SNES needs to update coarse-grid data.
 */
static PetscErrorCode restriction_hook(DM fine,
                                       Mat mrestrict, Vec rscale, Mat inject,
                                       DM coarse, void *ctx)
{
  PetscErrorCode ierr;

  /* Get rid of "unused argument" warnings: */
  (void) mrestrict;
  (void) rscale;
  (void) inject;
  (void) ctx;

  ierr = restrict_data(fine, coarse,
                       "2D_DM", "2D_Restriction", "2D_Vec"); CHKERRQ(ierr);

  ierr = restrict_data(fine, coarse,
                       "3D_DM", "3D_Restriction", "3D_Vec"); CHKERRQ(ierr);

  return 0;
}

/*! \brief Grid coarsening hook.
 *
 * This hook is called *once* when SNES sets up the next coarse level.
 *
 * This hook does three things:
 * - Set up the DM for the newly created coarse level.
 * - Set up the matrix type on the coarsest level to allow using
 *   direct solvers for the coarse problem.
 * - Set up the interpolation matrix that will be used by the
 *   restriction hook to set model parameters on the new coarse level.
 *
 * See restriction_hook().
 */
static PetscErrorCode coarsening_hook(DM dm_fine, DM dm_coarse, void *ctx) {
  PetscErrorCode ierr;
  Poisson3 *p3 = (Poisson3*)ctx;
  PetscInt rlevel, clevel;

  auto grid = p3->grid();

  ierr = setup_level(dm_coarse, grid->Lx(), grid->Ly()); CHKERRQ(ierr);

  ierr = DMGetRefineLevel(dm_coarse, &rlevel); CHKERRQ(ierr);
  ierr = DMGetCoarsenLevel(dm_coarse, &clevel); CHKERRQ(ierr);
  if (rlevel - clevel == 0) {
    ierr = DMSetMatType(dm_coarse, MATAIJ); CHKERRQ(ierr);
  }

  ierr = DMCoarsenHookAdd(dm_coarse, coarsening_hook, restriction_hook,
                          ctx); CHKERRQ(ierr);

  // 2D
  ierr = create_restriction(dm_fine, dm_coarse, "2D_DM", "2D_Restriction"); CHKERRQ(ierr);

  // 3D
  ierr = create_restriction(dm_fine, dm_coarse, "3D_DM", "3D_Restriction"); CHKERRQ(ierr);

  return 0;
}

Poisson3::Poisson3(IceGrid::ConstPtr grid, int Mz)
  : ShallowStressBalance(grid), m_Mz(Mz) {

  auto pism_da = grid->get_dm(1, 0);

  int ierr = setup(*pism_da);
  if (ierr != 0) {
    throw RuntimeError(PISM_ERROR_LOCATION,
                       "Failed to allocate a Poisson3 instance");
  }

  {
    std::vector<double> sigma(m_Mz);
    double dz = 1.0 / (m_Mz - 1);
    for (int i = 0; i < m_Mz; ++i) {
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

PetscErrorCode Poisson3::setup(DM pism_da) {
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

    ierr = DMDACreate3d(PETSC_COMM_WORLD,
                        DM_BOUNDARY_NONE, DM_BOUNDARY_NONE, DM_BOUNDARY_NONE,
                        DMDA_STENCIL_BOX,
                        Mx, My, m_Mz,
                        Nx, Ny, Nz,
                        dof,           // dof
                        stencil_width, // stencil width
                        lx, ly, NULL,
                        m_da.rawptr()); CHKERRQ(ierr);

    ierr = DMSetFromOptions(m_da); CHKERRQ(ierr);

    ierr = DMSetUp(m_da); CHKERRQ(ierr);

    // set up 2D and 3D parameter storage
    ierr = setup_level(m_da, m_grid->Lx(), m_grid->Ly()); CHKERRQ(ierr);

    // tell PETSc how to coarsen this grid and how to restrict data to a coarser grid
    ierr = DMCoarsenHookAdd(m_da, coarsening_hook, restriction_hook, this); CHKERRQ(ierr);
  }

  // Vecs, Mat
  {
    ierr = DMCreateGlobalVector(m_da, m_x.rawptr()); CHKERRQ(ierr);

    // ierr = DMCreateMatrix(m_da, m_J.rawptr()); CHKERRQ(ierr);
  }

  // SNES
  {
    ierr = SNESCreate(PETSC_COMM_WORLD, m_snes.rawptr()); CHKERRQ(ierr);

    // ierr = SNESSetOptionsPrefix(m_snes, "poisson3_"); CHKERRQ(ierr);

    ierr = SNESSetDM(m_snes, m_da); CHKERRQ(ierr);

    m_callback_data.da = m_da;
    m_callback_data.solver = this;

    ierr = DMDASNESSetFunctionLocal(m_da, INSERT_VALUES,
                                    (DMDASNESFunction)function_callback,
                                    &m_callback_data); CHKERRQ(ierr);

    ierr = SNESSetFromOptions(m_snes); CHKERRQ(ierr);
  }

  // set the initial guess
  ierr = VecSet(m_x, 0.0); CHKERRQ(ierr);

  return 0;
}

/*!
 * Bottom surface elevation
 */
static double b(double x, double y) {
  (void) x;
  (void) y;
  return -1.0 + x + y;
}

/*!
 * Domain thickness
 */
static double H(double x, double y) {
  return 1.0 + x*x + y*y;
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
    Lx = m_grid->Lx(),
    Ly = m_grid->Ly(),
    dx = 2.0 * Lx / (info.mx - 1),
    dy = 2.0 * Ly / (info.my - 1);

  DataInput<Parameters**> P(m_da, 2, NOT_GHOSTED);

  for (int j = info.ys; j < info.ys + info.ym; j++) {
    for (int i = info.xs; i < info.xs + info.xm; i++) {
      double x = xy(Lx, dx, i);
      double y = xy(Ly, dy, j);

      P[j][i].bed = b(x, y);
      P[j][i].thickness = H(x, y);
    }
  }
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
    Lx = m_grid->Lx(),
    Ly = m_grid->Ly(),
    dx = 2.0 * Lx / (info.mx - 1),
    dy = 2.0 * Ly / (info.my - 1);

  DataInput<Parameters**> P2(m_da, 2, NOT_GHOSTED);
  DataInput<double***> P3(m_da, 3, NOT_GHOSTED);

  for (int k = info.zs; k < info.zs + info.zm; k++) {
    for (int j = info.ys; j < info.ys + info.ym; j++) {
      for (int i = info.xs; i < info.xs + info.xm; i++) {
        double
          xx = xy(Lx, dx, i),
          yy = xy(Ly, dy, j),
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
    Lx = m_grid->Lx(),
    Ly = m_grid->Ly(),
    dx = 2.0 * Lx / (m_grid->Mx() - 1),
    dy = 2.0 * Ly / (m_grid->My() - 1);

  DataInput<Parameters**> P(m_da, 2, NOT_GHOSTED);

  for (Points p(*m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    double
      xx = xy(Lx, dx, i),
      yy = xy(Ly, dy, j),
      b = P[j][i].bed,
      H = P[j][i].thickness;

    auto c = result.get_column(i, j);

    for (int k = 0; k < m_Mz; ++k) {
      double zz = z(b, H, m_Mz, k);

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

  int ierr = 0;

  ierr = SNESSolve(m_snes, NULL, m_x); PISM_CHK(ierr, "SNESSolve");

  exact_solution(*m_exact);

  {
    double ***x = nullptr;
    ierr = DMDAVecGetArray(m_da, m_x, &x); PISM_CHK(ierr, "DMDAVecGetArray");

    IceModelVec::AccessList list{m_solution.get()};

    for (Points p(*m_grid); p; p.next()) {
      const int i = p.i(), j = p.j();

      auto c = m_solution->get_column(i, j);

      for (int k = 0; k < m_Mz; ++k) {
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
