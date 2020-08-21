/* Copyright (C) 2017, 2018, 2020 PISM Authors
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

#include <memory>

#include "StressBalance.hh"
#include "ShallowStressBalance.hh"
#include "ssa/SSAFD.hh"
#include "ssa/SSAFEM.hh"
#include "WeertmanSliding.hh"
#include "SSB_Modifier.hh"
#include "pism/regional/SSAFD_Regional.hh"
#include "pism/regional/SIAFD_Regional.hh"
#include "blatter/Blatter.hh"
#include "blatter/BlatterMod.hh"

#include "pism/util/pism_utilities.hh"
#include "pism/util/Context.hh"

namespace pism {
namespace stressbalance {

std::shared_ptr<StressBalance> create(const std::string &model,
                                      IceGrid::ConstPtr grid,
                                      bool regional) {
  std::shared_ptr<ShallowStressBalance> sliding;

  auto config = grid->ctx()->config();

  if (model == "blatter") {
    int Mz = config->get_number("stress_balance.blatter.Mz");
    int n_levels = config->get_number("stress_balance.blatter.n_levels");
    int coarsening_factor = config->get_number("stress_balance.blatter.coarsening_factor");

    std::shared_ptr<Blatter> blatter(new Blatter(grid, Mz, n_levels, coarsening_factor));
    std::shared_ptr<BlatterMod> mod(new BlatterMod(blatter));

    return std::shared_ptr<StressBalance>(new StressBalance(grid, blatter, mod));
  }

  if (member(model, {"none", "sia"})) {
    sliding.reset(new ZeroSliding(grid));
  } else if (member(model, {"prescribed_sliding", "prescribed_sliding+sia"})) {
    sliding.reset(new PrescribedSliding(grid));
  } else if (member(model, {"weertman_sliding", "weertman_sliding+sia"})) {
    sliding.reset(new WeertmanSliding(grid));
  } else if (member(model, {"ssa", "ssa+sia"})) {
    if (regional) {
      sliding.reset(new SSAFD_Regional(grid));
    } else {
      if (config->get_string("stress_balance.ssa.method") == "fem") {
        sliding.reset(new SSAFEM(grid));
      } else {
        sliding.reset(new SSAFD(grid));
      }
    }
  } else {
    throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                  "invalid stress balance model: %s", model.c_str());
  }

  std::shared_ptr<SSB_Modifier> modifier;

  if (member(model, {"none", "ssa", "prescribed_sliding", "weertman_sliding"})) {
    modifier.reset(new ConstantInColumn(grid));
  } else if (member(model, {"prescribed_sliding+sia", "weertman_sliding+sia", "ssa+sia", "sia"})) {
    if (regional) {
      modifier.reset(new SIAFD_Regional(grid));
    } else {
      modifier.reset(new SIAFD(grid));
    }
  } else {
    throw RuntimeError::formatted(PISM_ERROR_LOCATION,
                                  "invalid stress balance model: %s", model.c_str());
  }

  return std::shared_ptr<StressBalance>(new StressBalance(grid, sliding, modifier));
}

} // end of namespace stressbalance
} // end of namespace pism
