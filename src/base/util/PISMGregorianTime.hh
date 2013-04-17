// Copyright (C) 2012, 2013 PISM Authors
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#ifndef _PISMGREGORIANTIME_H_
#define _PISMGREGORIANTIME_H_

#include "PISMTime.hh"
#include "PISMUnits.hh"

class PISMGregorianTime : public PISMTime
{
public:
  PISMGregorianTime(MPI_Comm c, const NCConfigVariable &conf,
                    PISMUnitSystem units_system);
  virtual ~PISMGregorianTime();

  virtual PetscErrorCode init();

  virtual PetscErrorCode init_from_file(string filename);

  virtual double mod(double time, double period);

  virtual double year_fraction(double T);

  virtual string date(double T);

  virtual string date();

  virtual string start_date();

  virtual string end_date();

  virtual string units()
  { return CF_units(); }

  virtual bool use_reference_date()
  { return true; }

  virtual double calendar_year_start(double T);

  virtual double increment_date(double T, int years, int months, int days);

protected:
  PISMUnit m_time_units;
private:
  // Hide copy constructor / assignment operator.
  PISMGregorianTime(PISMGregorianTime const &);
  PISMGregorianTime & operator=(PISMGregorianTime const &);
};


#endif /* _PISMGREGORIANTIME_H_ */
