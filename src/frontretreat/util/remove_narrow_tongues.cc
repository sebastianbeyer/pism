/* Copyright (C) 2016, 2017, 2018, 2019, 2020 PISM Authors
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

#include "remove_narrow_tongues.hh"

#include "pism/util/IceGrid.hh"
#include "pism/geometry/Geometry.hh"

namespace pism {

/** Remove tips of one-cell-wide ice tongues ("noses")..
 *
 * The center icy cell in ice tongues like this one (and equivalent)
 *
 * @code
   O O ?
   X X O
   O O ?
   @endcode
 *
 * where "O" is ice-free and "?" is any mask value, are removed.
 * Ice tongues like this one
 *
 * @code
   # O ?
   X X O
   # O ?
   @endcode
 * where one or two of the "#" cells are ice-filled, are not removed.
 *
 * See the code for the precise rule, which uses `ice_free_ocean()` for the "O"
 * cells if the center cell has grounded ice, and uses `ice_free()` if the
 * center cell has floating ice.
 *
 * @note We use `geometry.cell_type` (and not `ice_thickness`) to make decisions. This
 * means that we can update `ice_thickness` in place without introducing a dependence on
 * the grid traversal order.
 *
 * @param[in,out] mask cell type mask
 * @param[in,out] ice_thickness modeled ice thickness
 *
 * @return 0 on success
 */
void remove_narrow_tongues(const Geometry &geometry,
                           IceModelVec2S &ice_thickness) {

  auto &mask      = geometry.cell_type;
  auto &bed       = geometry.bed_elevation;
  auto &sea_level = geometry.sea_level_elevation;

  IceGrid::ConstPtr grid = mask.grid();

  IceModelVec::AccessList list{&mask, &bed, &sea_level, &ice_thickness};

  for (Points p(*grid); p; p.next()) {
    const int i = p.i(), j = p.j();
    if (mask.ice_free(i, j) or
        (mask.grounded_ice(i, j) and bed(i, j) >= sea_level(i, j))) {
      continue;
    }

    BoxStencil<bool> ice_free;
    auto M = mask.int_box(i, j);

    if (mask::grounded_ice(M.ij)) {
      using mask::ice_free_ocean;
      // if (i,j) is grounded ice then we will remove it if it has
      // exclusively ice-free ocean neighbors
      ice_free.n  = ice_free_ocean(M.n);
      ice_free.e  = ice_free_ocean(M.e);
      ice_free.s  = ice_free_ocean(M.s);
      ice_free.w  = ice_free_ocean(M.w);
      ice_free.ne = ice_free_ocean(M.ne);
      ice_free.nw = ice_free_ocean(M.nw);
      ice_free.se = ice_free_ocean(M.se);
      ice_free.sw = ice_free_ocean(M.sw);
    } else if (mask.floating_ice(i,j)) {
      // if (i,j) is floating then we will remove it if its neighbors are
      // ice-free, whether ice-free ocean or ice-free ground
      ice_free.n  = mask::ice_free(M.n);
      ice_free.e  = mask::ice_free(M.e);
      ice_free.s  = mask::ice_free(M.s);
      ice_free.w  = mask::ice_free(M.w);
      ice_free.ne = mask::ice_free(M.ne);
      ice_free.nw = mask::ice_free(M.nw);
      ice_free.se = mask::ice_free(M.se);
      ice_free.sw = mask::ice_free(M.sw);
    }

    if ((not ice_free.w and
         ice_free.nw    and
         ice_free.sw    and
         ice_free.n     and
         ice_free.s     and
         ice_free.e)    or
        (not ice_free.n and
         ice_free.nw    and
         ice_free.ne    and
         ice_free.w     and
         ice_free.e     and
         ice_free.s)    or
        (not ice_free.e and
         ice_free.ne    and
         ice_free.se    and
         ice_free.w     and
         ice_free.s     and
         ice_free.n)    or
        (not ice_free.s and
         ice_free.sw    and
         ice_free.se    and
         ice_free.w     and
         ice_free.e     and
         ice_free.n)) {
      ice_thickness(i, j) = 0.0;
    }
  }
}

} // end of namespace pism
